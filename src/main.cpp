/*
 * LumiGate — Art-Net → DMX Gateway
 * ESP32 + Waveshare RS485 (C) — galvanically isolated, auto-direction
 *
 * Pins: DMX TX=17, RX=16, DE/RE=auto (Waveshare), LED=2, BOOT=0
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

// Auto-generated asset headers (produced by extra_scripts.py before each build)
#include "generated/index_html.h"
#include "generated/config_html.h"
#include "generated/config_saved_html.h"
#include "generated/reset_html.h"
#include "generated/reset_done_html.h"
#include "generated/logo_png.h"

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
static constexpr int        DMX_TX_PIN  = 17;
static constexpr int        DMX_RX_PIN  = 16;
static constexpr int        DMX_RTS_PIN = -1;
static constexpr dmx_port_t DMX_PORT    = DMX_NUM_1;
static constexpr int        BOOT_PIN    = 0;
static constexpr uint32_t   HOLD_MS     = 3000;
static constexpr int        LED_PIN     = 2;

// ---------------------------------------------------------------------------
// Defaults / NVS keys
// ---------------------------------------------------------------------------
static const char* DEF_HOSTNAME = "dmx-gateway";
static const char* DEF_OTA_PW   = "dmxota";
static constexpr int DEF_UNIVERSE = 0;
static const char* PREF_NS = "dmxgw";
static const char* AP_SSID = "DMX-Gateway";

// ---------------------------------------------------------------------------
// Global objects
// ---------------------------------------------------------------------------
Preferences      prefs;
WebServer        http(80);
WebSocketsServer ws(81);
ArtnetWifi       artnet;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static uint8_t  dmxBuf[DMX_PACKET_SIZE] = {0};
static uint32_t lastFrameMs  = 0;
static uint32_t frameCount   = 0;
static float    fps          = 0.0f;
static uint32_t startMs      = 0;
static bool     dmxReady     = false;
static bool     manualMode   = false;
static uint32_t lastWsPush   = 0;
static uint32_t lastArtNetMs = 0;

// Binary WS frame: fps(2) rssi(2) heap(4) uptime(4) dmx(512) = 524 bytes
static uint8_t wsBuf[524];

struct Config {
    int    universe;
    String hostname;
    String otaPassword;
} cfg;

// ---------------------------------------------------------------------------
// Config persistence
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
    prefs.putInt("universe",    cfg.universe);
    prefs.putString("hostname", cfg.hostname);
    prefs.putString("otapw",    cfg.otaPassword);
    prefs.end();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
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
// WebSocket push (binary)
// ---------------------------------------------------------------------------
static void wsPush() {
    if (ws.connectedClients() == 0) return;
    uint16_t fpsI = (uint16_t)(fps * 10.0f);
    int16_t  rssi = (int16_t)WiFi.RSSI();
    uint32_t heap = ESP.getFreeHeap();
    uint32_t upS  = uptimeSec();
    wsBuf[0] = fpsI >> 8;           wsBuf[1] = fpsI & 0xFF;
    wsBuf[2] = (uint8_t)((uint16_t)rssi >> 8); wsBuf[3] = rssi & 0xFF;
    wsBuf[4] = heap >> 24;          wsBuf[5] = (heap>>16)&0xFF;
    wsBuf[6] = (heap>>8)&0xFF;      wsBuf[7] = heap & 0xFF;
    wsBuf[8] = upS >> 24;           wsBuf[9] = (upS>>16)&0xFF;
    wsBuf[10]= (upS>>8)&0xFF;       wsBuf[11]= upS & 0xFF;
    memcpy(&wsBuf[12], &dmxBuf[1], 512);
    ws.broadcastBIN(wsBuf, 524);
}

// ---------------------------------------------------------------------------
// WebSocket event (browser → ESP)
// ---------------------------------------------------------------------------
static void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
    if (type != WStype_TEXT || len < 2) return;
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
// Art-Net callback
// ---------------------------------------------------------------------------
static void onArtDmx(uint16_t universe, uint16_t length, uint8_t, uint8_t* data) {
    if ((int)universe != cfg.universe) return;
    if (!manualMode) {
        memcpy(&dmxBuf[1], data, min((uint16_t)512, length));
        sendDmx();
    }
    lastArtNetMs = millis();

    uint32_t now = millis();
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
// HTTP handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
    String p = FPSTR(INDEX_HTML);
    p.replace("{{SSID}}",     WiFi.SSID());
    p.replace("{{IP}}",       WiFi.localIP().toString());
    p.replace("{{UNIVERSE}}", String(cfg.universe));
    p.replace("{{HOSTNAME}}", cfg.hostname);
    http.send(200, "text/html", p);
}

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

static void handleConfigGet() {
    String p = FPSTR(CONFIG_HTML);
    p.replace("{{UNIVERSE}}", String(cfg.universe));
    p.replace("{{HOSTNAME}}", cfg.hostname);
    p.replace("{{OTAPW}}",    cfg.otaPassword);
    http.send(200, "text/html", p);
}

static void handleConfigPost() {
    if (http.hasArg("universe"))
        cfg.universe = constrain(http.arg("universe").toInt(), 0, 15);
    if (http.hasArg("hostname") && http.arg("hostname").length() > 0)
        cfg.hostname = http.arg("hostname");
    if (http.hasArg("otapw") && http.arg("otapw").length() > 0)
        cfg.otaPassword = http.arg("otapw");
    saveConfig();
    http.send(200, "text/html", FPSTR(CONFIG_SAVED_HTML));
    delay(400);
    ESP.restart();
}

static void handleResetGet() {
    http.send(200, "text/html", FPSTR(RESET_HTML));
}

static void handleResetPost() {
    http.send(200, "text/html", FPSTR(RESET_DONE_HTML));
    delay(400);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
}

static void handleLogo() {
    http.sendHeader("Cache-Control", "max-age=86400");
    http.send_P(200, "image/png", (const char*)LOGO_PNG, LOGO_PNG_LEN);
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
    Serial.begin(115200);
    startMs = millis();
    Serial.println("\n[BOOT] LumiGate — Art-Net DMX Gateway");

    loadConfig();

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    pinMode(BOOT_PIN, INPUT_PULLUP);

    bool forcePortal = false;
    if (digitalRead(BOOT_PIN) == LOW) {
        Serial.print("[BOOT] button held, waiting...");
        uint32_t t = millis();
        while (digitalRead(BOOT_PIN) == LOW && millis()-t < HOLD_MS) delay(50);
        forcePortal = (digitalRead(BOOT_PIN) == LOW);
        Serial.println(forcePortal ? " → config portal" : " released");
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
    Serial.printf("[ArtNet] universe %d\n", cfg.universe);

    http.on("/logo.png", HTTP_GET,  handleLogo);
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
    Serial.println("[BOOT] ready.");
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
    artnet.read();
    ArduinoOTA.handle();
    http.handleClient();
    ws.loop();

    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED_PIN, LOW);
    } else if (now - lastArtNetMs < 300) {
        digitalWrite(LED_PIN, HIGH);
    } else {
        digitalWrite(LED_PIN, (now % 2000) < 100 ? HIGH : LOW);
    }

    if (now - lastWsPush >= 100) {
        wsPush();
        lastWsPush = now;
    }
}
