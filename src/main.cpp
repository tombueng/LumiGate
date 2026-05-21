/*
 * Art-Net → DMX Gateway
 * ESP32 + Waveshare RS485-Modul (Auto-Direction)
 *
 * Features:
 *   - Art-Net Unicast → DMX512 (512 Kanäle)
 *   - WebSocket Push (binary, Port 81) für Live-UI
 *   - Manuelles Überschreiben einzelner Kanäle per Browser
 *   - WiFiManager Config-Portal
 *   - OTA, mDNS, REST (/dmx.json)
 *   - Persistente Konfiguration in NVS
 *
 * Pins: DMX TX=17, RX=16, DE/RE=Auto (Waveshare)
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ArtnetWifi.h>
#include <esp_dmx.h>

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
static constexpr int         DMX_TX_PIN  = 17;
static constexpr int         DMX_RX_PIN  = 16;
static constexpr int         DMX_RTS_PIN = -1;
static constexpr dmx_port_t  DMX_PORT    = DMX_NUM_1;
static constexpr int         BOOT_PIN    = 0;
static constexpr uint32_t    HOLD_MS     = 3000;
static constexpr int         LED_PIN     = 2;

// ---------------------------------------------------------------------------
// Defaults / NVS
// ---------------------------------------------------------------------------
static const char* DEF_HOSTNAME = "dmx-gateway";
static const char* DEF_OTA_PW   = "dmxota";
static constexpr int DEF_UNIVERSE = 0;
static const char* PREF_NS = "dmxgw";
static const char* AP_SSID = "DMX-Gateway";

// ---------------------------------------------------------------------------
// Globale Objekte
// ---------------------------------------------------------------------------
Preferences      prefs;
WebServer        http(80);
WebSocketsServer ws(81);
ArtnetWifi       artnet;

// ---------------------------------------------------------------------------
// Laufzeit-Zustand
// ---------------------------------------------------------------------------
static uint8_t  dmxBuf[DMX_PACKET_SIZE] = {0}; // [0]=start code, [1..512]=Kanäle
static uint32_t lastFrameMs  = 0;
static uint32_t frameCount   = 0;
static float    fps          = 0.0f;
static uint32_t startMs      = 0;
static bool     dmxReady     = false;
static bool     manualMode   = false; // wenn true: Art-Net wird ignoriert
static uint32_t lastWsPush   = 0;
static uint32_t lastArtNetMs = 0; // letzter empfangener Art-Net Frame

// Binary WS-Frame: fps(2) rssi(2) heap(4) uptime(4) dmx(512) = 524 Byte
static uint8_t wsBuf[524];

// Konfiguration
struct Config {
    int    universe;
    String hostname;
    String otaPassword;
} cfg;

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------
static void loadConfig() {
    prefs.begin(PREF_NS, false);
    cfg.universe    = prefs.getInt("universe",  DEF_UNIVERSE);
    cfg.hostname    = prefs.getString("hostname", DEF_HOSTNAME);
    cfg.otaPassword = prefs.getString("otapw",   DEF_OTA_PW);
    prefs.end();
}

static void saveConfig() {
    prefs.begin(PREF_NS, false);
    prefs.putInt("universe", cfg.universe);
    prefs.putString("hostname",  cfg.hostname);
    prefs.putString("otapw",     cfg.otaPassword);
    prefs.end();
}

static uint32_t uptimeSec() { return (millis() - startMs) / 1000; }

static String uptimeStr() {
    uint32_t s = uptimeSec();
    char buf[32];
    snprintf(buf, sizeof(buf), "%02ud %02u:%02u:%02u",
             s/86400, (s%86400)/3600, (s%3600)/60, s%60);
    return String(buf);
}

static void sendDmx() {
    if (!dmxReady) return;
    dmx_write(DMX_PORT, dmxBuf, DMX_PACKET_SIZE);
    dmx_send(DMX_PORT);
    dmx_wait_sent(DMX_PORT, DMX_TIMEOUT_TICK);
}

// ---------------------------------------------------------------------------
// WebSocket Push (binary)
// ---------------------------------------------------------------------------
static void wsPush() {
    if (ws.connectedClients() == 0) return;
    uint16_t fpsI  = (uint16_t)(fps * 10.0f);
    int16_t  rssi  = (int16_t)WiFi.RSSI();
    uint32_t heap  = ESP.getFreeHeap();
    uint32_t upS   = uptimeSec();
    wsBuf[0] = fpsI >> 8;          wsBuf[1] = fpsI & 0xFF;
    wsBuf[2] = (uint8_t)((uint16_t)rssi >> 8); wsBuf[3] = rssi & 0xFF;
    wsBuf[4] = heap >> 24;         wsBuf[5] = (heap>>16)&0xFF;
    wsBuf[6] = (heap>>8)&0xFF;     wsBuf[7] = heap & 0xFF;
    wsBuf[8] = upS >> 24;          wsBuf[9] = (upS>>16)&0xFF;
    wsBuf[10]= (upS>>8)&0xFF;      wsBuf[11]= upS & 0xFF;
    memcpy(&wsBuf[12], &dmxBuf[1], 512);
    ws.broadcastBIN(wsBuf, 524);
}

// ---------------------------------------------------------------------------
// WebSocket Event (Browser → ESP)
// ---------------------------------------------------------------------------
static void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
    if (type != WStype_TEXT || len < 2) return;

    // Einfaches JSON-Parsing ohne Library
    // Erwartete Formate:
    //   {"type":"set","ch":1,"val":200}     → einen Kanal setzen
    //   {"type":"mode","manual":true/false} → Modus umschalten
    //   {"type":"blackout"}                 → alle Kanäle auf 0
    String msg((char*)payload, len);

    if (msg.indexOf("\"blackout\"") >= 0) {
        memset(&dmxBuf[1], 0, 512);
        sendDmx();
        return;
    }
    if (msg.indexOf("\"mode\"") >= 0) {
        manualMode = (msg.indexOf("true") >= 0);
        return;
    }
    if (msg.indexOf("\"set\"") >= 0) {
        int chIdx  = msg.indexOf("\"ch\":");
        int valIdx = msg.indexOf("\"val\":");
        if (chIdx < 0 || valIdx < 0) return;
        int ch  = msg.substring(chIdx  + 5).toInt();
        int val = msg.substring(valIdx + 6).toInt();
        if (ch < 1 || ch > 512) return;
        dmxBuf[ch] = (uint8_t)constrain(val, 0, 255);
        sendDmx();
    }
}

// ---------------------------------------------------------------------------
// Art-Net Callback
// ---------------------------------------------------------------------------
static void onArtDmx(uint16_t universe, uint16_t length, uint8_t /*seq*/, uint8_t* data) {
    if ((int)universe != cfg.universe) return;
    if (!manualMode) {
        uint16_t copyLen = min((uint16_t)512, length);
        memcpy(&dmxBuf[1], data, copyLen);
        sendDmx();
    }

    lastArtNetMs = millis();

    // FPS messen
    uint32_t now = millis();
    frameCount++;
    if (now - lastFrameMs >= 1000) {
        fps         = (float)frameCount * 1000.0f / (float)(now - lastFrameMs);
        frameCount  = 0;
        lastFrameMs = now;
    }

    // WS-Push max 25 fps
    if (now - lastWsPush >= 40) {
        wsPush();
        lastWsPush = now;
    }
}

// ---------------------------------------------------------------------------
// Webserver
// ---------------------------------------------------------------------------

// GET /dmx.json  – maschinenlesbare API
static void handleDmxJson() {
    String j;
    j.reserve(2300);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", fps);
    j  = "{\"fps\":";    j += buf;
    j += ",\"rssi\":";   j += WiFi.RSSI();
    j += ",\"up\":\"";   j += uptimeStr();
    j += "\",\"heap\":"; j += ESP.getFreeHeap();
    j += ",\"manual\":"; j += manualMode ? "true" : "false";
    j += ",\"ch\":[";
    for (int i = 1; i <= 512; i++) {
        j += dmxBuf[i];
        if (i < 512) j += ',';
    }
    j += "]}";
    http.send(200, "application/json", j);
}

// GET /
static void handleRoot() {
    String ssid = WiFi.SSID();
    String ip   = WiFi.localIP().toString();

    // Statisches HTML – JS macht alle Live-Updates per WebSocket
    String p;
    p.reserve(5000);
    p = R"html(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DMX Gateway</title>
<script src="https://cdn.tailwindcss.com"></script>
</head>
<body class="bg-gray-950 text-gray-100 min-h-screen font-mono text-sm">
<nav class="bg-gray-900 border-b border-gray-800 px-4 py-3 flex flex-wrap items-center gap-4">
  <span class="text-cyan-400 font-bold text-base">&#9728; DMX Gateway</span>
  <a href="/" class="text-cyan-400 border-b border-cyan-400 pb-0.5">Status</a>
  <a href="/config" class="text-gray-400 hover:text-cyan-400">Konfig</a>
  <a href="/reset"  class="text-gray-400 hover:text-cyan-400">Reset</a>
  <span class="ml-auto flex items-center gap-2">
    <span id="ws-dot" class="w-2 h-2 rounded-full bg-red-500"></span>
    <span id="ws-lbl" class="text-xs text-gray-500">Offline</span>
  </span>
</nav>
<main class="p-4 max-w-5xl mx-auto space-y-4">

<!-- Stats -->
<div class="grid grid-cols-2 sm:grid-cols-4 gap-3">
  <div class="bg-gray-800 rounded-lg p-3">
    <div class="text-xs text-gray-400 mb-1">Framerate</div>
    <div id="fps" class="text-xl text-cyan-400">– fps</div>
  </div>
  <div class="bg-gray-800 rounded-lg p-3">
    <div class="text-xs text-gray-400 mb-1">RSSI</div>
    <div id="rssi" class="text-xl">–</div>
  </div>
  <div class="bg-gray-800 rounded-lg p-3">
    <div class="text-xs text-gray-400 mb-1">Uptime</div>
    <div id="up" class="text-xl text-gray-300">–</div>
  </div>
  <div class="bg-gray-800 rounded-lg p-3">
    <div class="text-xs text-gray-400 mb-1">Heap frei</div>
    <div id="heap" class="text-xl text-gray-300">–</div>
  </div>
</div>

<!-- Info -->
<div class="bg-gray-800 rounded-lg p-3 flex flex-wrap gap-x-6 gap-y-1 text-xs text-gray-400">
  <span>SSID: <span class="text-gray-200">)html";
    p += ssid;
    p += R"html(</span></span>
  <span>IP: <span class="text-gray-200">)html";
    p += ip;
    p += R"html(</span></span>
  <span>Universe: <span class="text-gray-200">)html";
    p += cfg.universe;
    p += R"html(</span></span>
  <span>Hostname: <span class="text-gray-200">)html";
    p += cfg.hostname;
    p += R"html(.local</span></span>
</div>

<!-- Controls -->
<div class="bg-gray-800 rounded-lg p-3 flex flex-wrap items-center gap-3">
  <button id="btn-mode"
    onclick="toggleMode()"
    class="px-3 py-1.5 rounded text-xs font-bold bg-gray-700 hover:bg-gray-600 text-gray-200">
    Art-Net aktiv
  </button>
  <button onclick="sendCmd({type:'blackout'})"
    class="px-3 py-1.5 rounded text-xs font-bold bg-red-900 hover:bg-red-800 text-red-200">
    Blackout
  </button>
  <span id="sel-label" class="text-xs text-gray-400 ml-2"></span>
  <div id="ch-ctrl" class="hidden flex items-center gap-2 w-full mt-2">
    <span id="ch-name" class="w-16 text-xs text-gray-300"></span>
    <input id="ch-slider" type="range" min="0" max="255" value="0"
      oninput="onSlider(this.value)"
      class="flex-1 accent-cyan-400">
    <span id="ch-val" class="w-8 text-right text-cyan-400">0</span>
  </div>
</div>

<!-- DMX Grid -->
<div class="bg-gray-800 rounded-lg p-3">
  <div class="text-xs text-gray-400 mb-2">DMX Kanäle 1–512 &nbsp;
    <span class="text-gray-600">· Klicken zum Editieren</span>
  </div>
  <div id="grid" style="display:grid;grid-template-columns:repeat(32,1fr);border-top:1px solid #374151;border-left:1px solid #374151"></div>
</div>

</main>

<script>
// --- Build grid ---
const N=512, cells=[], vals=new Uint8Array(512);
const grid=document.getElementById('grid');
for(let i=1;i<=N;i++){
  const d=document.createElement('div');
  d.style.cssText=[
    'height:28px',
    'background:#111827',
    'cursor:pointer',
    'border-right:1px solid #374151',
    'border-bottom:1px solid #374151',
    'display:flex',
    'flex-direction:column',
    'align-items:center',
    'justify-content:center',
    'gap:1px',
    'overflow:hidden',
    'position:relative'
  ].join(';');
  // channel number label (top)
  const cn=document.createElement('span');
  cn.textContent=i;
  cn.style.cssText='font-size:6px;color:#6b7280;line-height:1;pointer-events:none;user-select:none';
  // value label (bottom)
  const vl=document.createElement('span');
  vl.textContent='0';
  vl.style.cssText='font-size:7px;color:#9ca3af;line-height:1;pointer-events:none;user-select:none';
  d.appendChild(cn);
  d.appendChild(vl);
  d.dataset.ch=i;
  d.addEventListener('click',()=>selectCh(i));
  grid.appendChild(d);
  cells.push({el:d, vl});
}

// --- State ---
let manual=false, selCh=null;
const slider=document.getElementById('ch-slider');
const chVal =document.getElementById('ch-val');
const chName=document.getElementById('ch-name');
const chCtrl=document.getElementById('ch-ctrl');
const selLbl=document.getElementById('sel-label');
const btnMode=document.getElementById('btn-mode');

function selectCh(i){
  if(selCh){cells[selCh-1].el.style.outline='';}
  selCh=i;
  cells[i-1].el.style.outline='2px solid #22d3ee';
  chCtrl.classList.remove('hidden');
  const v=vals[i-1];
  slider.value=v; chVal.textContent=v;
  chName.textContent='Ch '+i;
  selLbl.textContent='';
}

function onSlider(v){
  chVal.textContent=v;
  if(selCh) sendCmd({type:'set',ch:selCh,val:parseInt(v)});
}

function toggleMode(){
  manual=!manual;
  sendCmd({type:'mode',manual:manual});
  btnMode.textContent=manual?'Manuell aktiv':'Art-Net aktiv';
  btnMode.className=manual
    ?'px-3 py-1.5 rounded text-xs font-bold bg-cyan-900 hover:bg-cyan-800 text-cyan-200'
    :'px-3 py-1.5 rounded text-xs font-bold bg-gray-700 hover:bg-gray-600 text-gray-200';
}

// --- WebSocket ---
let sock=null;
function connect(){
  sock=new WebSocket('ws://'+location.hostname+':81');
  sock.binaryType='arraybuffer';
  sock.onopen=()=>{
    document.getElementById('ws-dot').className='w-2 h-2 rounded-full bg-green-400';
    document.getElementById('ws-lbl').textContent='Live';
  };
  sock.onclose=()=>{
    document.getElementById('ws-dot').className='w-2 h-2 rounded-full bg-red-500';
    document.getElementById('ws-lbl').textContent='Offline';
    setTimeout(connect,2000);
  };
  sock.onmessage=e=>{
    if(!(e.data instanceof ArrayBuffer)) return;
    const v=new DataView(e.data);
    const fps  = v.getUint16(0)/10;
    const rssi = v.getInt16(2);
    const heap = v.getUint32(4);
    const upS  = v.getUint32(8);
    const ch   = new Uint8Array(e.data,12,512);

    document.getElementById('fps').textContent=fps.toFixed(1)+' fps';
    const rssiEl=document.getElementById('rssi');
    rssiEl.textContent=rssi+' dBm';
    rssiEl.className='text-xl '+(rssi>-70?'text-green-400':rssi>-85?'text-yellow-400':'text-red-400');
    document.getElementById('heap').textContent=Math.round(heap/1024)+' KB';

    const d=Math.floor(upS/86400), h=Math.floor((upS%86400)/3600),
          m=Math.floor((upS%3600)/60), s=upS%60;
    document.getElementById('up').textContent=
      String(d).padStart(2,'0')+'d '+
      String(h).padStart(2,'0')+':'+
      String(m).padStart(2,'0')+':'+
      String(s).padStart(2,'0');

    for(let i=0;i<512;i++){
      const val=ch[i];
      if(vals[i]===val) continue; // skip unchanged
      vals[i]=val;
      const c=cells[i];
      const a=(val/255).toFixed(3);
      c.el.style.background=val>0?`rgba(34,211,238,${a})`:'#111827';
      c.vl.textContent=val;
      c.vl.style.color=val>0?'#e5e7eb':'#6b7280';
      if(selCh===i+1){slider.value=val; chVal.textContent=val;}
    }
  };
}

function sendCmd(obj){
  if(sock && sock.readyState===1) sock.send(JSON.stringify(obj));
}

connect();
</script>
</body></html>
)html";

    http.send(200, "text/html", p);
}

// GET /config
static void handleConfigGet() {
    String p = R"html(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DMX Gateway – Konfig</title>
<script src="https://cdn.tailwindcss.com"></script>
</head>
<body class="bg-gray-950 text-gray-100 min-h-screen font-mono text-sm">
<nav class="bg-gray-900 border-b border-gray-800 px-4 py-3 flex flex-wrap items-center gap-4">
  <span class="text-cyan-400 font-bold text-base">&#9728; DMX Gateway</span>
  <a href="/" class="text-gray-400 hover:text-cyan-400">Status</a>
  <a href="/config" class="text-cyan-400 border-b border-cyan-400">Konfig</a>
  <a href="/reset"  class="text-gray-400 hover:text-cyan-400">Reset</a>
</nav>
<main class="p-4 max-w-md mx-auto">
<div class="bg-gray-800 rounded-lg p-5 space-y-4">
<h2 class="text-cyan-400 font-bold">Konfiguration</h2>
<form method="POST" action="/config" class="space-y-3">
<label class="block">
  <span class="text-xs text-gray-400">Art-Net Universe (0–15)</span>
  <input name="universe" type="number" min="0" max="15" value=")html";
    p += cfg.universe;
    p += R"html(" class="mt-1 w-full bg-gray-900 border border-gray-700 rounded px-3 py-2 text-gray-100 focus:outline-none focus:border-cyan-500">
</label>
<label class="block">
  <span class="text-xs text-gray-400">Hostname (ohne .local)</span>
  <input name="hostname" type="text" maxlength="32" value=")html";
    p += cfg.hostname;
    p += R"html(" class="mt-1 w-full bg-gray-900 border border-gray-700 rounded px-3 py-2 text-gray-100 focus:outline-none focus:border-cyan-500">
</label>
<label class="block">
  <span class="text-xs text-gray-400">OTA-Passwort</span>
  <input name="otapw" type="password" maxlength="32" value=")html";
    p += cfg.otaPassword;
    p += R"html(" class="mt-1 w-full bg-gray-900 border border-gray-700 rounded px-3 py-2 text-gray-100 focus:outline-none focus:border-cyan-500">
</label>
<button type="submit"
  class="w-full py-2 rounded bg-cyan-500 hover:bg-cyan-400 text-gray-950 font-bold">
  Speichern &amp; Neustart
</button>
</form>
</div>
</main></body></html>)html";
    http.send(200, "text/html", p);
}

// POST /config
static void handleConfigPost() {
    if (http.hasArg("universe"))  cfg.universe    = constrain(http.arg("universe").toInt(), 0, 15);
    if (http.hasArg("hostname") && http.arg("hostname").length() > 0)
                                  cfg.hostname    = http.arg("hostname");
    if (http.hasArg("otapw") && http.arg("otapw").length() > 0)
                                  cfg.otaPassword = http.arg("otapw");
    saveConfig();
    http.send(200, "text/html",
        String(R"html(<!DOCTYPE html><html><head><meta charset="utf-8">
<script src="https://cdn.tailwindcss.com"></script></head>
<body class="bg-gray-950 text-gray-100 flex items-center justify-center h-screen font-mono">
<div class="text-center space-y-3">
  <div class="text-green-400 text-xl">Gespeichert.</div>
  <div class="text-gray-400 text-sm">Neustart...</div>
</div>
<script>setTimeout(()=>location.href='/',3500)</script>
</body></html>)html"));
    delay(400);
    ESP.restart();
}

// GET /reset
static void handleResetGet() {
    http.send(200, "text/html", R"html(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DMX Gateway – Reset</title>
<script src="https://cdn.tailwindcss.com"></script>
</head>
<body class="bg-gray-950 text-gray-100 min-h-screen font-mono text-sm">
<nav class="bg-gray-900 border-b border-gray-800 px-4 py-3 flex flex-wrap items-center gap-4">
  <span class="text-cyan-400 font-bold text-base">&#9728; DMX Gateway</span>
  <a href="/" class="text-gray-400 hover:text-cyan-400">Status</a>
  <a href="/config" class="text-gray-400 hover:text-cyan-400">Konfig</a>
  <a href="/reset"  class="text-cyan-400 border-b border-cyan-400">Reset</a>
</nav>
<main class="p-4 max-w-md mx-auto">
<div class="bg-gray-800 rounded-lg p-5 space-y-4">
<h2 class="text-red-400 font-bold">WiFi-Reset</h2>
<p class="text-gray-400 text-xs">Löscht WLAN-Zugangsdaten und öffnet das Config-Portal.
Andere Einstellungen bleiben erhalten.</p>
<form method="POST" action="/reset">
<button type="submit"
  class="w-full py-2 rounded bg-red-700 hover:bg-red-600 text-white font-bold">
  Jetzt zurücksetzen
</button>
</form>
</div>
</main></body></html>)html");
}

// POST /reset
static void handleResetPost() {
    http.send(200, "text/html", R"html(<!DOCTYPE html><html><head><meta charset="utf-8">
<script src="https://cdn.tailwindcss.com"></script></head>
<body class="bg-gray-950 text-gray-100 flex items-center justify-center h-screen font-mono">
<div class="text-center space-y-3">
  <div class="text-yellow-400 text-xl">WiFi-Daten gelöscht.</div>
  <div class="text-gray-400 text-sm">Neustart in AP-Mode...</div>
</div></body></html>)html");
    delay(400);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
}

// ---------------------------------------------------------------------------
// WiFiManager
// ---------------------------------------------------------------------------
static bool wm_shouldSave = false;
static char wm_universeStr[4] = "0";

static void wmSaveCallback() { wm_shouldSave = true; }

static void startWiFiManager(bool forcePortal) {
    WiFiManager wm;
    wm.setSaveConfigCallback(wmSaveCallback);
    wm.setConfigPortalTimeout(180);

    snprintf(wm_universeStr, sizeof(wm_universeStr), "%d", cfg.universe);
    WiFiManagerParameter param_universe("universe", "Art-Net Universe (0-15)", wm_universeStr, 3);
    wm.addParameter(&param_universe);

    bool connected = forcePortal ? wm.startConfigPortal(AP_SSID)
                                 : wm.autoConnect(AP_SSID);
    if (!connected) ESP.restart();

    if (wm_shouldSave) {
        cfg.universe = constrain(atoi(param_universe.getValue()), 0, 15);
        saveConfig();
    }
}

// ---------------------------------------------------------------------------
// Peripherie-Init
// ---------------------------------------------------------------------------
static void initDmx() {
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    dmx_driver_install(DMX_PORT, &config, nullptr, 0);
    dmx_set_pin(DMX_PORT, DMX_TX_PIN, DMX_RX_PIN, DMX_RTS_PIN);
    dmx_write(DMX_PORT, dmxBuf, DMX_PACKET_SIZE);
    dmx_send(DMX_PORT);
    dmx_wait_sent(DMX_PORT, DMX_TIMEOUT_TICK);
    dmxReady = true;
    Serial.println("[DMX] bereit");
}

static void initOTA() {
    ArduinoOTA.setHostname(cfg.hostname.c_str());
    ArduinoOTA.setPassword(cfg.otaPassword.c_str());
    ArduinoOTA.onStart([]() { dmxReady = false; Serial.println("[OTA] Start"); });
    ArduinoOTA.onEnd([]()   { Serial.println("[OTA] Fertig"); });
    ArduinoOTA.onError([](ota_error_t e) { dmxReady = true; Serial.printf("[OTA] Fehler[%u]\n", e); });
    ArduinoOTA.begin();
    Serial.printf("[OTA] %s.local pw:%s\n", cfg.hostname.c_str(), cfg.otaPassword.c_str());
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    startMs = millis();
    Serial.println("\n[BOOT] Art-Net DMX Gateway");

    loadConfig();

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    pinMode(BOOT_PIN, INPUT_PULLUP);
    bool forcePortal = false;
    if (digitalRead(BOOT_PIN) == LOW) {
        Serial.print("[BOOT] Taste gehalten, warte...");
        uint32_t t = millis();
        while (digitalRead(BOOT_PIN) == LOW && millis()-t < HOLD_MS) delay(50);
        forcePortal = (digitalRead(BOOT_PIN) == LOW);
        Serial.println(forcePortal ? " → Config-Portal" : " losgelassen");
    }

    startWiFiManager(forcePortal);
    Serial.printf("[WiFi] %s / %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

    if (MDNS.begin(cfg.hostname.c_str())) {
        MDNS.addService("http",   "tcp", 80);
        MDNS.addService("artnet", "udp", 6454);
        Serial.printf("[mDNS] %s.local\n", cfg.hostname.c_str());
    }

    initDmx();
    initOTA();

    artnet.setArtDmxCallback(onArtDmx);
    artnet.begin();
    Serial.printf("[ArtNet] Universe %d\n", cfg.universe);

    http.on("/",         HTTP_GET,  handleRoot);
    http.on("/dmx.json", HTTP_GET,  handleDmxJson);
    http.on("/config",   HTTP_GET,  handleConfigGet);
    http.on("/config",   HTTP_POST, handleConfigPost);
    http.on("/reset",    HTTP_GET,  handleResetGet);
    http.on("/reset",    HTTP_POST, handleResetPost);
    http.begin();

    ws.begin();
    ws.onEvent(wsEvent);

    lastFrameMs = millis();
    Serial.println("[BOOT] Bereit.");
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
    artnet.read();
    ArduinoOTA.handle();
    http.handleClient();
    ws.loop();

    // LED-Zustand:
    //   kein WiFi       → aus
    //   Art-Net aktiv   → an
    //   Idle (kein Data)→ langsamer Herzschlag (100ms an, alle 2s)
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED_PIN, LOW);
    } else if (now - lastArtNetMs < 300) {
        digitalWrite(LED_PIN, HIGH);
    } else {
        // Herzschlag: 100ms an, 1900ms aus
        digitalWrite(LED_PIN, (now % 2000) < 100 ? HIGH : LOW);
    }

    // WS-Push auch wenn kein Art-Net kommt (z.B. im Manual-Mode)
    if (now - lastWsPush >= 100) {
        wsPush();
        lastWsPush = now;
    }
}
