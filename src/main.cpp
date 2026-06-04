/*
 * LumiGate — Art-Net / sACN → DMX Gateway
 * ESP32 / ESP32-S3 / WT32-ETH01 + Waveshare RS485 (C)
 *
 * Default pins: DMX TX=DEF_DMX_TX_PIN(17), RX=DEF_DMX_RX_PIN(16)
 * WT32-ETH01:   DMX TX=4, RX=5  (GPIO16 used by LAN8720 power)
 */

#include <Arduino.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <WiFi.h>
#ifdef USE_ETHERNET
#include <ETH.h>
#else
#include <WiFiManager.h>
#endif
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <ArtnetWifi.h>
#include <esp_dmx.h>

// Auto-generated asset headers (produced by extra_scripts.py before each build)
#include "generated/version.h"
#include "generated/index_html.h"
#include "generated/config_html.h"
#include "generated/config_saved_html.h"
#include "generated/reset_html.h"
#include "generated/reset_done_html.h"
#include "generated/ota_progress_html.h"
#include "generated/ota_done_html.h"
#include "generated/logo_png.h"
#include "generated/bootstrap_min_css.h"

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
#ifndef DEF_DMX_TX_PIN
#define DEF_DMX_TX_PIN 17
#endif
#ifndef DEF_DMX_RX_PIN
#define DEF_DMX_RX_PIN 16
#endif

static constexpr int        DMX_TX_PIN  = DEF_DMX_TX_PIN;
static constexpr int        DMX_RX_PIN  = DEF_DMX_RX_PIN;
static constexpr int        DMX_RTS_PIN = -1;
static constexpr dmx_port_t DMX_PORT    = DMX_NUM_1;
static constexpr int        BOOT_PIN    = 0;
static constexpr uint32_t   HOLD_MS     = 3000;

#ifndef DEF_LED_PIN
#define DEF_LED_PIN  2
#endif
#ifndef DEF_LED_TYPE
#define DEF_LED_TYPE 1   // 0=off, 1=plain GPIO, 2=WS2812
#endif

// ---------------------------------------------------------------------------
// Defaults / NVS keys
// ---------------------------------------------------------------------------
static const char* DEF_HOSTNAME = "dmx-gateway";
static const char* DEF_OTA_PW   = "dmxota";
static constexpr int DEF_UNIVERSE = 0;
static constexpr int DEF_PROTOCOL = 2;
static const char* PREF_NS = "dmxgw";
static const char* AP_SSID = "DMX-Gateway";

// ---------------------------------------------------------------------------
// Sender tracking
// ---------------------------------------------------------------------------
static constexpr int MAX_SENDERS = 8;

struct Sender {
    uint32_t ip;       // 0 = empty slot
    uint8_t  proto;    // 0=ArtNet, 1=sACN
    uint32_t lastMs;
    uint32_t winMs;    // fps window start
    uint16_t winCnt;
    float    fps;
};
static Sender senders[MAX_SENDERS] = {};

// ---------------------------------------------------------------------------
// Change log
// ---------------------------------------------------------------------------
static constexpr int LOG_SIZE = 50;
static constexpr int LOG_TOP  = 6;   // top changed channels stored per entry

struct LogEntry {
    uint32_t ms;
    uint32_t ip;
    uint8_t  proto;
    uint8_t  topN;    // valid entries in top[]
    uint16_t total;   // total channels changed
    struct { uint16_t ch; uint8_t val; } top[LOG_TOP];
};
static LogEntry dmxLog[LOG_SIZE] = {};
static uint8_t  logHead  = 0;
static uint8_t  logCount = 0;
static uint32_t lastLogMs = 0;

// ---------------------------------------------------------------------------
// Network abstraction (WiFi vs Ethernet)
// ---------------------------------------------------------------------------
#ifdef USE_ETHERNET
static bool      netConnected() { return ETH.linkUp() && ETH.localIP() != IPAddress(0,0,0,0); }
static IPAddress netLocalIP()   { return ETH.localIP(); }
static String    netSSID()      { return "Ethernet"; }
static int       netRSSI()      { return 0; }
#else
static bool      netConnected() { return WiFi.status() == WL_CONNECTED; }
static IPAddress netLocalIP()   { return WiFi.localIP(); }
static String    netSSID()      { return WiFi.SSID(); }
static int       netRSSI()      { return (int)WiFi.RSSI(); }
#endif

// Parse a dotted-quad into IPAddress; returns false (and 0.0.0.0) if invalid/empty
static bool parseIp(const String& s, IPAddress& out) {
    if (s.length() == 0) { out = IPAddress(0,0,0,0); return false; }
    return out.fromString(s);
}

// ---------------------------------------------------------------------------
// Global objects
// ---------------------------------------------------------------------------
Preferences      prefs;
WebServer        http(80);
WebSocketsServer ws(81);
ArtnetWifi       artnet;
static WiFiUDP   sacnUdp;
static Adafruit_NeoPixel neoPixel(1, 0, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static uint8_t  dmxBuf[DMX_PACKET_SIZE] = {0};
static uint32_t lastFrameMs  = 0;
static uint32_t frameCount   = 0;
static float    fps          = 0.0f;
static float    jitterMs     = 0.0f;
static uint32_t prevFrameMs  = 0;
static uint32_t startMs      = 0;
static bool     dmxReady     = false;
static bool     manualMode   = false;
static uint32_t lastWsPush   = 0;
static uint32_t lastDmxMs    = 0;
static bool     pendingGithubOta = false;
static String   latestVersion   = "";
static String   latestNotes     = "";
static bool     updateAvailable = false;

// Identify: temporarily force one channel to full on the wire to locate a fixture
static constexpr uint32_t IDENTIFY_MS = 1500;
static uint16_t identifyCh      = 0;       // 1-512, 0 = inactive
static uint32_t identifyUntil   = 0;
static uint32_t lastIdentifyTx  = 0;

// WS binary frame: fps(2) rssi(2) heap(4) uptime(4) senders(1) conflict(1) jitter(2) dmx(512) = 528
static uint8_t wsBuf[528];

// sACN receive buffer
static uint8_t sacnBuf[638];

struct Config {
    int    universe;
    String hostname;
    String otaPassword;
    int    protocol;
    int    ledPin;
    int    ledType;
    bool   staticIp;       // false = DHCP
    String ip;             // dotted-quad strings; empty when unused
    String gateway;
    String subnet;
    String dns;
} cfg;

// Channel labels — stored verbatim as a JSON object string {"1":"Front L",...}
// The browser owns editing; the device just persists the blob it receives.
static constexpr size_t LABELS_MAX = 3000;
static String g_labels = "{}";

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------
static void loadConfig() {
    prefs.begin(PREF_NS, false);
    cfg.universe    = prefs.getInt("universe",  DEF_UNIVERSE);
    cfg.hostname    = prefs.getString("hostname", DEF_HOSTNAME);
    cfg.otaPassword = prefs.getString("otapw",   DEF_OTA_PW);
    cfg.protocol    = prefs.getInt("protocol",  DEF_PROTOCOL);
    cfg.ledPin      = prefs.getInt("ledpin",    DEF_LED_PIN);
    cfg.ledType     = prefs.getInt("ledtype",   DEF_LED_TYPE);
    cfg.staticIp    = prefs.getBool("staticip", false);
    cfg.ip          = prefs.getString("ip",      "");
    cfg.gateway     = prefs.getString("gateway", "");
    cfg.subnet      = prefs.getString("subnet",  "255.255.255.0");
    cfg.dns         = prefs.getString("dns",     "");
    g_labels        = prefs.getString("labels",  "{}");
    prefs.end();
}

static void saveConfig() {
    prefs.begin(PREF_NS, false);
    prefs.putInt("universe",    cfg.universe);
    prefs.putString("hostname", cfg.hostname);
    prefs.putString("otapw",    cfg.otaPassword);
    prefs.putInt("protocol",    cfg.protocol);
    prefs.putInt("ledpin",      cfg.ledPin);
    prefs.putInt("ledtype",     cfg.ledType);
    prefs.putBool("staticip",   cfg.staticIp);
    prefs.putString("ip",       cfg.ip);
    prefs.putString("gateway",  cfg.gateway);
    prefs.putString("subnet",   cfg.subnet);
    prefs.putString("dns",      cfg.dns);
    prefs.end();
}

// ---------------------------------------------------------------------------
// LED helpers
// ---------------------------------------------------------------------------
static constexpr uint32_t NEO_OFF   = 0x000000;
static constexpr uint32_t NEO_GREEN = 0x002200;
static constexpr uint32_t NEO_AMBER = 0x221000;
static constexpr uint32_t NEO_RED   = 0x220000;

static void initLed() {
    if (cfg.ledType == 1 && cfg.ledPin >= 0) {
        pinMode(cfg.ledPin, OUTPUT);
        digitalWrite(cfg.ledPin, LOW);
    } else if (cfg.ledType == 2 && cfg.ledPin >= 0) {
        neoPixel.setPin((uint16_t)cfg.ledPin);
        neoPixel.begin();
        neoPixel.setPixelColor(0, NEO_OFF);
        neoPixel.show();
    }
}

static void setLedColor(uint32_t neoColor, bool gpioOn) {
    if (cfg.ledType == 1 && cfg.ledPin >= 0)
        digitalWrite(cfg.ledPin, gpioOn ? HIGH : LOW);
    else if (cfg.ledType == 2 && cfg.ledPin >= 0) {
        neoPixel.setPixelColor(0, neoColor);
        neoPixel.show();
    }
}
static void setLed(bool on) { setLedColor(on ? NEO_GREEN : NEO_OFF, on); }

// ---------------------------------------------------------------------------
// General helpers
// ---------------------------------------------------------------------------
static uint32_t uptimeSec() { return (millis() - startMs) / 1000; }

static String uptimeStr() {
    uint32_t s = uptimeSec();
    char buf[32];
    snprintf(buf, sizeof(buf), "%02ud %02u:%02u:%02u",
             s/86400, (s%86400)/3600, (s%3600)/60, s%60);
    return String(buf);
}

static String ipStr(uint32_t ip) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             ip & 0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);
    return String(buf);
}

static void sendDmx() {
    if (!dmxReady) return;
    // Identify override: force one channel to full on the wire only,
    // without corrupting the stored value the UI/Art-Net see.
    bool ov = identifyCh && millis() < identifyUntil;
    uint8_t saved = 0;
    if (ov) { saved = dmxBuf[identifyCh]; dmxBuf[identifyCh] = 255; }
    dmx_write(DMX_PORT, dmxBuf, DMX_PACKET_SIZE);
    dmx_send(DMX_PORT);
    dmx_wait_sent(DMX_PORT, DMX_TIMEOUT_TICK);
    if (ov) dmxBuf[identifyCh] = saved;
}

// ---------------------------------------------------------------------------
// Sender tracking
// ---------------------------------------------------------------------------
static void updateSender(uint32_t ip, uint8_t proto) {
    uint32_t now = millis();
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders[i].ip == ip && senders[i].proto == proto) {
            senders[i].lastMs = now;
            senders[i].winCnt++;
            if (now - senders[i].winMs >= 1000) {
                senders[i].fps   = (float)senders[i].winCnt * 1000.0f
                                   / (float)(now - senders[i].winMs);
                senders[i].winCnt = 0;
                senders[i].winMs  = now;
            }
            return;
        }
    }
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders[i].ip == 0) {
            senders[i] = {ip, proto, now, now, 0, 0.0f};
            Serial.printf("[SND] new sender %s proto=%d\n", ipStr(ip).c_str(), proto);
            return;
        }
    }
    // evict least-recently-seen
    int oldest = 0;
    for (int i = 1; i < MAX_SENDERS; i++)
        if (senders[i].lastMs < senders[oldest].lastMs) oldest = i;
    senders[oldest] = {ip, proto, now, now, 0, 0.0f};
}

static uint8_t activeSenderCount() {
    uint32_t now = millis();
    uint8_t n = 0;
    for (int i = 0; i < MAX_SENDERS; i++)
        if (senders[i].ip != 0 && now - senders[i].lastMs < 5000) n++;
    return n;
}

static bool hasConflict() { return activeSenderCount() > 1; }

// ---------------------------------------------------------------------------
// Change log
// ---------------------------------------------------------------------------
static void maybeLog(const uint8_t* data, uint16_t len, uint32_t ip, uint8_t proto) {
    uint32_t now = millis();
    if (now - lastLogMs < 200) return;

    LogEntry e;
    e.ms    = now;
    e.ip    = ip;
    e.proto = proto;
    e.total = 0;
    e.topN  = 0;
    uint16_t lim = len < 512 ? len : 512;
    for (int i = 0; i < lim; i++) {
        if (data[i] != dmxBuf[i + 1]) {
            e.total++;
            if (e.topN < LOG_TOP) {
                e.top[e.topN].ch  = (uint16_t)(i + 1);
                e.top[e.topN].val = data[i];
                e.topN++;
            }
        }
    }
    if (e.total == 0) return;

    dmxLog[logHead] = e;
    logHead  = (logHead + 1) % LOG_SIZE;
    if (logCount < LOG_SIZE) logCount++;
    lastLogMs = now;
}

// ---------------------------------------------------------------------------
// WebSocket push (binary, 528 bytes)
// frame: fps(2) rssi(2) heap(4) uptime(4) senders(1) conflict(1) jitter(2) dmx(512)
// ---------------------------------------------------------------------------
static void wsPush() {
    if (ws.connectedClients() == 0) return;
    uint16_t fpsI  = (uint16_t)(fps * 10.0f);
    int16_t  rssi  = (int16_t)netRSSI();
    uint32_t heap  = ESP.getFreeHeap();
    uint32_t upS   = uptimeSec();
    uint16_t jitI  = (uint16_t)(jitterMs * 10.0f < 65535.0f ? jitterMs * 10.0f : 65535.0f);
    wsBuf[0]  = fpsI >> 8;                       wsBuf[1]  = fpsI & 0xFF;
    wsBuf[2]  = (uint8_t)((uint16_t)rssi >> 8);  wsBuf[3]  = rssi & 0xFF;
    wsBuf[4]  = heap >> 24;  wsBuf[5]  = (heap>>16)&0xFF;
    wsBuf[6]  = (heap>>8)&0xFF; wsBuf[7] = heap & 0xFF;
    wsBuf[8]  = upS >> 24;   wsBuf[9]  = (upS>>16)&0xFF;
    wsBuf[10] = (upS>>8)&0xFF; wsBuf[11] = upS & 0xFF;
    wsBuf[12] = activeSenderCount();
    wsBuf[13] = hasConflict() ? 1 : 0;
    wsBuf[14] = jitI >> 8;  wsBuf[15] = jitI & 0xFF;
    memcpy(&wsBuf[16], &dmxBuf[1], 512);
    ws.broadcastBIN(wsBuf, 528);
}

// ---------------------------------------------------------------------------
// WebSocket event (browser → ESP)
// ---------------------------------------------------------------------------
static void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
    if (type != WStype_TEXT || len < 2) return;
    String msg((char*)payload, len);
    if (msg.indexOf("\"blackout\"") >= 0) {
        memset(&dmxBuf[1], 0, 512); sendDmx(); return;
    }
    if (msg.indexOf("\"mode\"") >= 0) {
        manualMode = (msg.indexOf("true") >= 0); return;
    }
    if (msg.indexOf("\"identify\"") >= 0) {
        int chIdx = msg.indexOf("\"ch\":");
        if (chIdx < 0) return;
        int ch = msg.substring(chIdx + 5).toInt();
        if (ch < 1 || ch > 512) return;
        identifyCh    = (uint16_t)ch;
        identifyUntil = millis() + IDENTIFY_MS;
        lastIdentifyTx = 0;
        sendDmx();
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
// Shared DMX frame handler
// ---------------------------------------------------------------------------
static void onDmxFrame(const uint8_t* data, uint16_t length, uint32_t senderIp, uint8_t proto) {
    uint32_t now = millis();

    // Log changes before dmxBuf is overwritten (need old values for comparison)
    maybeLog(data, length, senderIp, proto);

    if (!manualMode) {
        memcpy(&dmxBuf[1], data, min((uint16_t)512, length));
        sendDmx();
    }

    updateSender(senderIp, proto);

    // Jitter: deviation from expected inter-frame interval
    if (prevFrameMs > 0 && fps > 1.0f) {
        float interval = (float)(now - prevFrameMs);
        float expected = 1000.0f / fps;
        float dev = interval > expected ? interval - expected : expected - interval;
        jitterMs = jitterMs * 0.85f + dev * 0.15f;
    }
    prevFrameMs = now;

    lastDmxMs = now;
    frameCount++;
    if (now - lastFrameMs >= 1000) {
        fps         = (float)frameCount * 1000.0f / (float)(now - lastFrameMs);
        frameCount  = 0;
        lastFrameMs = now;
    }
    if (now - lastWsPush >= 40) {
        wsPush();
        lastWsPush = now;
    }
}

// ---------------------------------------------------------------------------
// Art-Net callback
// ---------------------------------------------------------------------------
static void onArtDmx(uint16_t universe, uint16_t length, uint8_t, uint8_t* data) {
    if ((int)universe != cfg.universe) return;
    onDmxFrame(data, length, (uint32_t)artnet.getSenderIp(), 0);
}

// ---------------------------------------------------------------------------
// sACN / E1.31
// ---------------------------------------------------------------------------
static constexpr int SACN_ACN_ID_OFF    = 4;
static constexpr int SACN_ROOT_VEC_OFF  = 18;
static constexpr int SACN_FRAME_VEC_OFF = 40;
static constexpr int SACN_UNIVERSE_OFF  = 113;
static constexpr int SACN_STARTCODE_OFF = 125;
static constexpr int SACN_DATA_OFF      = 126;
static constexpr int SACN_MIN_LEN       = 638;

static const uint8_t ACN_PACKET_ID[12] = {
    0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31,
    0x37, 0x00, 0x00, 0x00
};

static void startSacn() {
    sacnUdp.stop();
    uint16_t sacnUniverse = (uint16_t)(cfg.universe + 1);
    uint8_t  univHigh     = (uint8_t)((sacnUniverse >> 8) & 0xFF);
    uint8_t  univLow      = (uint8_t)(sacnUniverse & 0xFF);
    IPAddress mcast(239, 255, univHigh, univLow);
    sacnUdp.beginMulticast(mcast, 5568);
    Serial.printf("[sACN] universe %u  multicast 239.255.%u.%u:5568\n",
                  sacnUniverse, univHigh, univLow);
}

static void readSacn() {
    int pktLen = sacnUdp.parsePacket();
    if (pktLen < SACN_MIN_LEN) return;
    uint32_t senderIp = (uint32_t)sacnUdp.remoteIP();
    int n = sacnUdp.read(sacnBuf, sizeof(sacnBuf));
    if (n < SACN_MIN_LEN) return;
    if (memcmp(sacnBuf + SACN_ACN_ID_OFF, ACN_PACKET_ID, 12) != 0) return;
    uint32_t rootVec = ((uint32_t)sacnBuf[SACN_ROOT_VEC_OFF    ] << 24)
                     | ((uint32_t)sacnBuf[SACN_ROOT_VEC_OFF + 1] << 16)
                     | ((uint32_t)sacnBuf[SACN_ROOT_VEC_OFF + 2] <<  8)
                     |  (uint32_t)sacnBuf[SACN_ROOT_VEC_OFF + 3];
    if (rootVec != 0x00000004u) return;
    uint32_t frameVec = ((uint32_t)sacnBuf[SACN_FRAME_VEC_OFF    ] << 24)
                      | ((uint32_t)sacnBuf[SACN_FRAME_VEC_OFF + 1] << 16)
                      | ((uint32_t)sacnBuf[SACN_FRAME_VEC_OFF + 2] <<  8)
                      |  (uint32_t)sacnBuf[SACN_FRAME_VEC_OFF + 3];
    if (frameVec != 0x00000002u) return;
    uint16_t universe = ((uint16_t)sacnBuf[SACN_UNIVERSE_OFF] << 8)
                       | sacnBuf[SACN_UNIVERSE_OFF + 1];
    if ((int)universe != cfg.universe + 1) return;
    if (sacnBuf[SACN_STARTCODE_OFF] != 0x00) return;
    onDmxFrame(sacnBuf + SACN_DATA_OFF, 512, senderIp, 1);
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void handleVersionJson() {
    String j = "{\"current\":\"";
    j += FIRMWARE_VERSION;
    j += "\",\"latest\":\"";
    j += latestVersion.length() > 0 ? latestVersion : String(FIRMWARE_VERSION);
    j += "\",\"update\":";
    j += updateAvailable ? "true" : "false";
    j += ",\"notes\":\"";
    // Escape the notes for JSON: replace \ " and newlines
    for (int i = 0; i < (int)latestNotes.length(); i++) {
        char c = latestNotes[i];
        if      (c == '"')  j += "\\\"";
        else if (c == '\\') j += "\\\\";
        else if (c == '\n') j += "\\n";
        else if (c == '\r') {}
        else                j += c;
    }
    j += "\"}";
    http.send(200, "application/json", j);
}

static void handleSendersJson() {
    String j = "[";
    bool first = true;
    uint32_t now = millis();
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders[i].ip == 0) continue;
        uint32_t ago = now - senders[i].lastMs;
        if (ago > 30000) continue;
        if (!first) j += ",";
        first = false;
        char buf[72];
        snprintf(buf, sizeof(buf),
            "{\"ip\":\"%s\",\"p\":%d,\"fps\":%.1f,\"ago\":%lu}",
            ipStr(senders[i].ip).c_str(),
            (int)senders[i].proto,
            senders[i].fps,
            (unsigned long)(ago / 1000));
        j += buf;
    }
    j += "]";
    http.send(200, "application/json", j);
}

static void handleLogJson() {
    String j = "[";
    bool first = true;
    for (int k = 0; k < logCount; k++) {
        // Iterate newest → oldest
        int idx = ((int)logHead - 1 - k + LOG_SIZE * 2) % LOG_SIZE;
        LogEntry& e = dmxLog[idx];
        if (!first) j += ",";
        first = false;
        char buf[64];
        snprintf(buf, sizeof(buf),
            "{\"ms\":%lu,\"ip\":\"%s\",\"p\":%d,\"n\":%d,\"ch\":[",
            (unsigned long)e.ms, ipStr(e.ip).c_str(), (int)e.proto, (int)e.total);
        j += buf;
        for (int t = 0; t < e.topN; t++) {
            if (t > 0) j += ",";
            snprintf(buf, sizeof(buf), "[%d,%d]", (int)e.top[t].ch, (int)e.top[t].val);
            j += buf;
        }
        j += "]}";
    }
    j += "]";
    http.send(200, "application/json", j);
}

static void handleRoot() {
    String p = FPSTR(INDEX_HTML);
    p.replace("{{SSID}}",     netSSID());
    p.replace("{{IP}}",       netLocalIP().toString());
    p.replace("{{UNIVERSE}}", String(cfg.universe));
    p.replace("{{HOSTNAME}}", cfg.hostname);
    p.replace("{{VERSION}}",  FIRMWARE_VERSION);
    http.send(200, "text/html", p);
}

static void handleDmxJson() {
    String j;
    j.reserve(2300);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", fps);
    j  = "{\"fps\":";    j += buf;
    j += ",\"rssi\":";   j += netRSSI();
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

static void handleConfigGet() {
    String p = FPSTR(CONFIG_HTML);
    p.replace("{{UNIVERSE}}", String(cfg.universe));
    p.replace("{{HOSTNAME}}", cfg.hostname);
    p.replace("{{OTAPW}}",    cfg.otaPassword);
    p.replace("{{VERSION}}",  FIRMWARE_VERSION);
    p.replace("{{PROTOCOL}}", String(cfg.protocol));
    p.replace("{{LED_PIN}}",  String(cfg.ledPin));
    p.replace("{{LED_TYPE}}", String(cfg.ledType));
    p.replace("{{STATIC_IP}}", cfg.staticIp ? "1" : "0");
    p.replace("{{IP}}",        cfg.ip);
    p.replace("{{GATEWAY}}",   cfg.gateway);
    p.replace("{{SUBNET}}",    cfg.subnet);
    p.replace("{{DNS}}",       cfg.dns);
    http.send(200, "text/html", p);
}

static void handleConfigPost() {
    if (http.hasArg("universe"))
        cfg.universe = constrain(http.arg("universe").toInt(), 0, 15);
    if (http.hasArg("hostname") && http.arg("hostname").length() > 0)
        cfg.hostname = http.arg("hostname");
    if (http.hasArg("otapw") && http.arg("otapw").length() > 0)
        cfg.otaPassword = http.arg("otapw");
    if (http.hasArg("protocol"))
        cfg.protocol = constrain(http.arg("protocol").toInt(), 0, 2);
    if (http.hasArg("ledtype"))
        cfg.ledType = constrain(http.arg("ledtype").toInt(), 0, 2);
    if (http.hasArg("ledpin"))
        cfg.ledPin = constrain(http.arg("ledpin").toInt(), -1, 48);
    cfg.staticIp = http.hasArg("staticip");
    if (http.hasArg("ip"))      cfg.ip      = http.arg("ip");
    if (http.hasArg("gateway")) cfg.gateway = http.arg("gateway");
    if (http.hasArg("subnet"))  cfg.subnet  = http.arg("subnet");
    if (http.hasArg("dns"))     cfg.dns     = http.arg("dns");
    saveConfig();
    http.send(200, "text/html", FPSTR(CONFIG_SAVED_HTML));
    delay(400);
    ESP.restart();
}

// ---------------------------------------------------------------------------
// Channel labels — browser owns the JSON object, device just persists it
// ---------------------------------------------------------------------------
static void handleLabelsGet() {
    http.send(200, "application/json", g_labels);
}

static void handleLabelsPost() {
    String body = http.arg("plain");
    if (body.length() == 0 || body.length() > LABELS_MAX || body[0] != '{') {
        http.send(400, "text/plain", "Invalid labels payload");
        return;
    }
    g_labels = body;
    prefs.begin(PREF_NS, false);
    prefs.putString("labels", g_labels);
    prefs.end();
    http.send(200, "application/json", "{\"ok\":true}");
}

static void handleResetGet()  { http.send(200, "text/html", FPSTR(RESET_HTML)); }

static void handleResetPost() {
    http.send(200, "text/html", FPSTR(RESET_DONE_HTML));
    delay(400);
    // Also drop static IP so recovery always comes back up on DHCP
    cfg.staticIp = false;
    saveConfig();
#ifndef USE_ETHERNET
    WiFiManager wm;
    wm.resetSettings();
#endif
    ESP.restart();
}

static void handleLogo() {
    http.sendHeader("Cache-Control", "max-age=86400");
    http.send_P(200, "image/png", (const char*)LOGO_PNG, LOGO_PNG_LEN);
}

static void handleBootstrapCss() {
    http.sendHeader("Cache-Control", "max-age=604800");
    http.send_P(200, "text/css", (const char*)BOOTSTRAP_MIN_CSS, BOOTSTRAP_MIN_CSS_LEN);
}

// ---------------------------------------------------------------------------
// Version check (FreeRTOS task, runs once 8s after boot)
// ---------------------------------------------------------------------------
static int parseBuild(const String& v) {
    int dot = v.lastIndexOf('.');
    return dot >= 0 ? v.substring(dot + 1).toInt() : 0;
}

static void checkForUpdate() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http2;
    http2.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    if (!http2.begin(client, "https://github.com/tombueng/LumiGate/releases/download/latest/version.txt")) return;
    int code = http2.GET();
    if (code == 200) {
        String v = http2.getString();
        v.trim();
        if (v.length() > 0 && v.length() < 24) {
            latestVersion   = v;
            updateAvailable = parseBuild(v) > parseBuild(String(FIRMWARE_VERSION));
            Serial.printf("[VER] latest=%s current=%s update=%s\n",
                v.c_str(), FIRMWARE_VERSION, updateAvailable ? "yes" : "no");
        }
    }
    http2.end();

    if (!updateAvailable) return;
    if (!http2.begin(client, "https://github.com/tombueng/LumiGate/releases/download/latest/changes.txt")) return;
    code = http2.GET();
    if (code == 200) {
        String n = http2.getString();
        n.trim();
        if (n.length() > 0 && n.length() < 2000) latestNotes = n;
    }
    http2.end();
}

static void versionCheckTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(8000));
    checkForUpdate();
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// OTA handlers
// ---------------------------------------------------------------------------
static void doGithubOta() {
    Serial.println("[OTA] Starting GitHub update...");
    dmxReady = false;
#ifdef CONFIG_IDF_TARGET_ESP32S3
    const char* otaUrl = "https://github.com/tombueng/LumiGate/releases/download/latest/firmware-esp32s3.bin";
#else
    const char* otaUrl = "https://github.com/tombueng/LumiGate/releases/download/latest/firmware.bin";
#endif
    WiFiClientSecure client;
    client.setInsecure();
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true);
    t_httpUpdate_return ret = httpUpdate.update(client, otaUrl);
    Serial.printf("[OTA] Failed (%d): %s\n",
        httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    dmxReady = true;
    delay(2000);
    ESP.restart();
}

static void handleOtaGithub() {
    http.send(200, "text/html", FPSTR(OTA_PROGRESS_HTML));
    pendingGithubOta = true;
}

static void handleOtaUploadDone() {
    bool ok = !Update.hasError();
    String p = FPSTR(OTA_DONE_HTML);
    p.replace("{{OTA_ICON}}",  ok ? "&#10003;" : "&#10007;");
    p.replace("{{OTA_CLASS}}", ok ? "text-success" : "text-danger");
    p.replace("{{OTA_TITLE}}", ok ? "Firmware updated" : "Update failed");
    p.replace("{{OTA_MSG}}",   ok ? "Rebooting&hellip;" :
                                    String("Error: ") + Update.errorString());
    http.send(200, "text/html", p);
    if (ok) { delay(500); ESP.restart(); }
}

static void handleOtaUploadChunk() {
    HTTPUpload& up = http.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Upload: %s\n", up.filename.c_str());
        dmxReady = false;
        Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        Update.end(true);
        Serial.printf("[OTA] Upload done: %u bytes\n", up.totalSize);
    }
}

// ---------------------------------------------------------------------------
// WiFiManager (WiFi builds only)
// ---------------------------------------------------------------------------
#ifndef USE_ETHERNET
static bool wm_shouldSave = false;
static char wm_universeStr[4] = "0";
static void wmSaveCallback() { wm_shouldSave = true; }

static void startWiFiManager(bool forcePortal) {
    WiFiManager wm;
    wm.setSaveConfigCallback(wmSaveCallback);
    wm.setConnectTimeout(60);
    wm.setConfigPortalTimeout(180);
    if (cfg.staticIp) {
        IPAddress ip, gw, sn, dns;
        parseIp(cfg.ip, ip); parseIp(cfg.gateway, gw);
        parseIp(cfg.subnet, sn); parseIp(cfg.dns, dns);
        wm.setSTAStaticIPConfig(ip, gw, sn, dns);
        Serial.printf("[WiFi] static IP %s\n", cfg.ip.c_str());
    }
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
#endif

// ---------------------------------------------------------------------------
// Peripheral init
// ---------------------------------------------------------------------------
static void initDmx() {
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    dmx_driver_install(DMX_PORT, &config, nullptr, 0);
    dmx_set_pin(DMX_PORT, DMX_TX_PIN, DMX_RX_PIN, DMX_RTS_PIN);
    dmx_write(DMX_PORT, dmxBuf, DMX_PACKET_SIZE);
    dmx_send(DMX_PORT);
    dmx_wait_sent(DMX_PORT, DMX_TIMEOUT_TICK);
    dmxReady = true;
    Serial.println("[DMX] ready");
}

static void initOTA() {
    ArduinoOTA.setHostname(cfg.hostname.c_str());
    ArduinoOTA.setPassword(cfg.otaPassword.c_str());
    ArduinoOTA.onStart([]() { dmxReady = false; Serial.println("[OTA] start"); });
    ArduinoOTA.onEnd([]()   { Serial.println("[OTA] done"); });
    ArduinoOTA.onError([](ota_error_t e) { dmxReady = true; Serial.printf("[OTA] error[%u]\n", e); });
    ArduinoOTA.begin();
    Serial.printf("[OTA] %s.local pw:%s\n", cfg.hostname.c_str(), cfg.otaPassword.c_str());
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    startMs = millis();
    Serial.println("\n[BOOT] LumiGate — Art-Net / sACN DMX Gateway");

    loadConfig();
    initLed();
    pinMode(BOOT_PIN, INPUT_PULLUP);

#ifdef USE_ETHERNET
    ETH.begin(1, 16, 23, 18, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
    if (cfg.staticIp) {
        IPAddress ip, gw, sn, dns;
        parseIp(cfg.ip, ip); parseIp(cfg.gateway, gw);
        parseIp(cfg.subnet, sn); parseIp(cfg.dns, dns);
        ETH.config(ip, gw, sn, dns);
        Serial.printf("[ETH] static IP %s\n", cfg.ip.c_str());
    }
    Serial.print("[ETH] waiting for link");
    { uint32_t t = millis();
      while (!netConnected() && millis() - t < 15000) { delay(200); Serial.print("."); } }
    Serial.println();
    Serial.printf("[ETH] %s\n", netLocalIP().toString().c_str());
#else
    bool forcePortal = false;
    if (digitalRead(BOOT_PIN) == LOW) {
        Serial.print("[BOOT] button held, waiting...");
        uint32_t t = millis();
        while (digitalRead(BOOT_PIN) == LOW && millis()-t < HOLD_MS) delay(50);
        forcePortal = (digitalRead(BOOT_PIN) == LOW);
        Serial.println(forcePortal ? " → config portal" : " released");
    }
    if (forcePortal && cfg.staticIp) {   // recovery: come back on DHCP
        cfg.staticIp = false;
        saveConfig();
    }
    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    startWiFiManager(forcePortal);
    Serial.printf("[WiFi] %s / %s\n", netSSID().c_str(), netLocalIP().toString().c_str());
#endif

    if (MDNS.begin(cfg.hostname.c_str())) {
        MDNS.addService("http",   "tcp", 80);
        if (cfg.protocol != 1) MDNS.addService("artnet", "udp", 6454);
        if (cfg.protocol != 0) MDNS.addService("e131",   "udp", 5568);
        Serial.printf("[mDNS] %s.local\n", cfg.hostname.c_str());
    }

    initDmx();
    initOTA();

    if (cfg.protocol != 1) {
        artnet.setArtDmxCallback(onArtDmx);
        artnet.begin();
        Serial.printf("[ArtNet] universe %d\n", cfg.universe);
    }
    if (cfg.protocol != 0) startSacn();

    http.on("/logo.png",          HTTP_GET,  handleLogo);
    http.on("/bootstrap.min.css", HTTP_GET,  handleBootstrapCss);
    http.on("/",                  HTTP_GET,  handleRoot);
    http.on("/dmx.json",          HTTP_GET,  handleDmxJson);
    http.on("/senders.json",      HTTP_GET,  handleSendersJson);
    http.on("/log.json",          HTTP_GET,  handleLogJson);
    http.on("/config",            HTTP_GET,  handleConfigGet);
    http.on("/config",            HTTP_POST, handleConfigPost);
    http.on("/reset",             HTTP_GET,  handleResetGet);
    http.on("/reset",             HTTP_POST, handleResetPost);
    http.on("/ota/github",        HTTP_POST, handleOtaGithub);
    http.on("/ota/upload",        HTTP_POST, handleOtaUploadDone, handleOtaUploadChunk);
    http.on("/version.json",      HTTP_GET,  handleVersionJson);
    http.on("/labels.json",       HTTP_GET,  handleLabelsGet);
    http.on("/labels",            HTTP_POST, handleLabelsPost);
    http.onNotFound([]() { http.send(404, "text/plain", "Not found"); });
    http.begin();

    ws.begin();
    ws.onEvent(wsEvent);

    lastFrameMs = millis();
    xTaskCreate(versionCheckTask, "ver_chk", 8192, nullptr, 1, nullptr);
    Serial.println("[BOOT] ready.");
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
    if (cfg.protocol != 1) artnet.read();
    if (cfg.protocol != 0) readSacn();
    ArduinoOTA.handle();
    http.handleClient();
    ws.loop();

    uint32_t now = millis();
    if (!netConnected()) {
        setLedColor((now % 1000) < 100 ? NEO_RED   : NEO_OFF, (now % 1000) < 100);
    } else if (now - lastDmxMs < 300) {
        setLedColor(NEO_GREEN, true);
    } else {
        setLedColor((now % 2000) < 100 ? NEO_AMBER : NEO_OFF, (now % 2000) < 100);
    }

    if (now - lastWsPush >= 100) {
        wsPush();
        lastWsPush = now;
    }

    // Identify: keep refreshing the wire so the fixture stays lit even with
    // no incoming Art-Net; on expiry, send one frame with the real value back.
    if (identifyCh) {
        if (now < identifyUntil) {
            if (now - lastIdentifyTx >= 40) { sendDmx(); lastIdentifyTx = now; }
        } else {
            identifyCh = 0;
            sendDmx();
        }
    }

    if (pendingGithubOta) {
        pendingGithubOta = false;
        doGithubOta();
    }
}
