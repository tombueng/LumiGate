/*
 * LumiGate — Art-Net / sACN → DMX Gateway
 * ESP32 / ESP32-S3 / WT32-ETH01 + Waveshare RS485 (C)
 *
 * Default pins: DMX TX=17, RX=16 (compile-time: DEF_DMX_TX_PIN/DEF_DMX_RX_PIN; runtime: web /config)
 * WT32-ETH01:   DMX TX=4, RX=5  (GPIO16 used by LAN8720 power)
 */

#include <Arduino.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>   // for esp_wifi_get/set_config (BSSID lock clearing)
#ifdef USE_ETHERNET
#include <ETH.h>
#else
#include <WiFiManager.h>
#endif
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <ArtnetWifi.h>
#include <esp_dmx.h>
#include <rdm/controller.h>             // RDM controller: discovery + GET/SET
#include <rdm/controller/include/utils.h>  // rdm_send_request() for sensor PIDs
#include <rdm/include/uid.h>            // rdm_uid_is_eq() and friends

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
#ifndef DEF_DMX_RTS_PIN
#define DEF_DMX_RTS_PIN -1
#endif
#ifndef DEF_DMX_PORT
#define DEF_DMX_PORT 1
#endif

// ---------------------------------------------------------------------------
// DMX outputs — up to MAX_OUTPUTS independent universes, each driven by its own
// hardware UART + RS485 transceiver. Hardware ceiling is 2: the ESP32 / ESP32-S3
// expose 3 UARTs and UART0 is the serial console, leaving UART1 + UART2.
// ---------------------------------------------------------------------------
static constexpr int MAX_OUTPUTS = 2;

struct DmxOutput {
    bool enabled;
    int  universe;   // Art-Net universe; sACN listens on (universe + 1)
    int  port;       // dmx_port_t: 1 or 2
    int  txPin;
    int  rxPin;      // -1 = output only (no RDM)
    int  rtsPin;     // -1 = auto-direction module / no RDM
};

static constexpr int BOOT_PIN = 0;
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
    uint8_t  uni;     // Art-Net universe this frame targeted
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
Preferences       prefs;
AsyncWebServer    http(80);
AsyncWebSocket    ws("/ws");
ArtnetWifi        artnet;
static WiFiUDP   sacnUdp[MAX_OUTPUTS];   // one multicast socket per output universe
static Adafruit_NeoPixel neoPixel(1, 0, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static uint8_t  dmxBuf[MAX_OUTPUTS][DMX_PACKET_SIZE] = {{0}};
static bool     outReady[MAX_OUTPUTS] = {false};   // per-output DMX driver installed
static int      monitorOut   = 0;                  // output shown/controlled by the web UI
static int      rdmOut       = -1;                 // output RDM runs on, -1 = none
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
static String   otaTarget       = "latest";   // release tag to install
static String   latestVersion   = "";
static bool     updateAvailable = false;

// Identify: temporarily force one channel to full on the wire to locate a fixture
static constexpr uint32_t IDENTIFY_MS = 1500;
static uint16_t identifyCh      = 0;       // 1-512, 0 = inactive
static uint32_t identifyUntil   = 0;
static uint32_t pendingRebootAt = 0;       // 0 = none; loop() reboots when due
static bool     pendingWifiReset = false;  // clear WiFi creds before reboot

// WS binary frame: fps(2) rssi(2) heap(4) uptime(4) senders(1) conflict(1) jitter(2) dmx(512) = 528
static uint8_t wsBuf[528];

// sACN receive buffer
static uint8_t sacnBuf[638];

struct Config {
    String hostname;
    String otaPassword;
    int    protocol;
    int    ledPin;
    int    ledType;
    DmxOutput outputs[MAX_OUTPUTS];
    bool   staticIp;       // false = DHCP
    String ip;             // dotted-quad strings; empty when unused
    String gateway;
    String subnet;
    String dns;
    bool   autoUpdate;     // auto-install newer firmware when detected
} cfg;

// Channel labels — stored verbatim as a JSON blob. The browser owns the
// structure (now per output: {"0":{"1":"Front L"},"1":{...}}); the device just
// persists what it receives. Sized for labels across all outputs.
static constexpr size_t LABELS_MAX = 6000;
static String g_labels = "{}";

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------
// Per-output NVS keys are "o<i>_<field>" (e.g. "o0_tx", "o1_uni").
static String okey(int i, const char* field) {
    return String('o') + i + '_' + field;
}

// An output with no TX GPIO can't drive a line and crashes esp_dmx on init
// (tx=-1 is "no change", so the UART is left half-configured). Force any such
// "enabled but pin-less" output off so it can never brick the device.
static void sanitizeOutputs() {
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (cfg.outputs[i].enabled && cfg.outputs[i].txPin < 0)
            cfg.outputs[i].enabled = false;
}

static void loadConfig() {
    prefs.begin(PREF_NS, false);
    cfg.hostname    = prefs.getString("hostname", DEF_HOSTNAME);
    cfg.otaPassword = prefs.getString("otapw",   DEF_OTA_PW);
    cfg.protocol    = prefs.getInt("protocol",  DEF_PROTOCOL);
    cfg.ledPin      = prefs.getInt("ledpin",    DEF_LED_PIN);
    cfg.ledType     = prefs.getInt("ledtype",   DEF_LED_TYPE);

    // Output 0 falls back to the legacy single-universe keys, so devices
    // updated from an older firmware keep their existing DMX setup untouched.
    // Output 1 ships disabled. Once saved, the new o<i>_* keys take over.
    cfg.outputs[0].enabled  = prefs.getBool(okey(0,"en").c_str(),  true);
    cfg.outputs[0].universe = constrain(prefs.getInt(okey(0,"uni").c_str(),
                                  prefs.getInt("universe", DEF_UNIVERSE)), 0, 15);
    cfg.outputs[0].port     = constrain(prefs.getInt(okey(0,"port").c_str(),
                                  prefs.getInt("dmxport", DEF_DMX_PORT)), 1, 2);
    cfg.outputs[0].txPin    = constrain(prefs.getInt(okey(0,"tx").c_str(),
                                  prefs.getInt("dmxtx", DEF_DMX_TX_PIN)), -1, 48);
    cfg.outputs[0].rxPin    = constrain(prefs.getInt(okey(0,"rx").c_str(),
                                  prefs.getInt("dmxrx", DEF_DMX_RX_PIN)), -1, 48);
    cfg.outputs[0].rtsPin   = constrain(prefs.getInt(okey(0,"rts").c_str(),
                                  prefs.getInt("dmxrts", DEF_DMX_RTS_PIN)), -1, 48);

    cfg.outputs[1].enabled  = prefs.getBool(okey(1,"en").c_str(),  false);
    cfg.outputs[1].universe = constrain(prefs.getInt(okey(1,"uni").c_str(),  DEF_UNIVERSE + 1), 0, 15);
    cfg.outputs[1].port     = constrain(prefs.getInt(okey(1,"port").c_str(), 2), 1, 2);
    cfg.outputs[1].txPin    = constrain(prefs.getInt(okey(1,"tx").c_str(),  -1), -1, 48);
    cfg.outputs[1].rxPin    = constrain(prefs.getInt(okey(1,"rx").c_str(),  -1), -1, 48);
    cfg.outputs[1].rtsPin   = constrain(prefs.getInt(okey(1,"rts").c_str(), -1), -1, 48);

    cfg.staticIp    = prefs.getBool("staticip", false);
    cfg.ip          = prefs.getString("ip",      "");
    cfg.gateway     = prefs.getString("gateway", "");
    cfg.subnet      = prefs.getString("subnet",  "255.255.255.0");
    cfg.dns         = prefs.getString("dns",     "");
    cfg.autoUpdate  = prefs.getBool("autoupd",   false);
    g_labels        = prefs.getString("labels",  "{}");
    prefs.end();
    sanitizeOutputs();
}

static void saveConfig() {
    prefs.begin(PREF_NS, false);
    prefs.putString("hostname", cfg.hostname);
    prefs.putString("otapw",    cfg.otaPassword);
    prefs.putInt("protocol",    cfg.protocol);
    prefs.putInt("ledpin",      cfg.ledPin);
    prefs.putInt("ledtype",     cfg.ledType);
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        prefs.putBool(okey(i,"en").c_str(),   cfg.outputs[i].enabled);
        prefs.putInt(okey(i,"uni").c_str(),   cfg.outputs[i].universe);
        prefs.putInt(okey(i,"port").c_str(),  cfg.outputs[i].port);
        prefs.putInt(okey(i,"tx").c_str(),    cfg.outputs[i].txPin);
        prefs.putInt(okey(i,"rx").c_str(),    cfg.outputs[i].rxPin);
        prefs.putInt(okey(i,"rts").c_str(),   cfg.outputs[i].rtsPin);
    }
    prefs.putBool("staticip",   cfg.staticIp);
    prefs.putString("ip",       cfg.ip);
    prefs.putString("gateway",  cfg.gateway);
    prefs.putString("subnet",   cfg.subnet);
    prefs.putString("dns",      cfg.dns);
    prefs.putBool("autoupd",    cfg.autoUpdate);
    prefs.end();
}

// ---------------------------------------------------------------------------
// LED helpers
// ---------------------------------------------------------------------------
static constexpr uint32_t NEO_OFF    = 0x000000;
static constexpr uint32_t NEO_GREEN  = 0x002200;
static constexpr uint32_t NEO_AMBER  = 0x221000;
static constexpr uint32_t NEO_RED    = 0x220000;
static constexpr uint32_t NEO_BLUE   = 0x000022;   // connecting to WiFi
static constexpr uint32_t NEO_PURPLE = 0x180018;   // AP / config portal active
static constexpr uint32_t NEO_WHITE  = 0x0a0a0a;   // booting

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
    // Skip redundant updates — repeatedly clocking the WS2812 (it sits next to
    // the antenna on the S3 DevKitC-1) injects RF noise and weakens WiFi.
    static uint32_t lastNeo = 0xFFFFFFFF;
    static int8_t   lastGpio = -1;
    if (cfg.ledType == 1 && cfg.ledPin >= 0) {
        if ((int8_t)gpioOn == lastGpio) return;
        lastGpio = gpioOn;
        digitalWrite(cfg.ledPin, gpioOn ? HIGH : LOW);
    } else if (cfg.ledType == 2 && cfg.ledPin >= 0) {
        if (neoColor == lastNeo) return;
        lastNeo = neoColor;
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

// The web monitor views/controls one output. monitorOut can go stale (e.g. its
// output was later disabled), so always resolve to a currently-enabled output —
// the monitor must never show or drive an empty/disabled buffer.
static int viewOutput() {
    if (monitorOut >= 0 && monitorOut < MAX_OUTPUTS && cfg.outputs[monitorOut].enabled)
        return monitorOut;
    for (int i = 0; i < MAX_OUTPUTS; i++) if (cfg.outputs[i].enabled) return i;
    return 0;
}

static void sendDmx() {
    if (!dmxReady) return;
    bool ovActive = identifyCh && millis() < identifyUntil;
    int  vo = viewOutput();
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (!outReady[i]) continue;
        dmx_port_t port = (dmx_port_t)cfg.outputs[i].port;
        // Identify override: force one channel to full on the wire only (on the
        // monitored output), without corrupting the stored value the UI sees.
        bool ov = ovActive && i == vo;
        uint8_t saved = 0;
        if (ov) { saved = dmxBuf[i][identifyCh]; dmxBuf[i][identifyCh] = 255; }
        dmx_write(port, dmxBuf[i], DMX_PACKET_SIZE);
        dmx_send(port);
        dmx_wait_sent(port, DMX_TIMEOUT_TICK);
        if (ov) dmxBuf[i][identifyCh] = saved;
    }
}

// ---------------------------------------------------------------------------
// RDM (E1.20) controller — discovery + GET/SET on the physical DMX line.
// Bound to a single output (rdmOut): the first enabled output with a direction-
// enable pin set (the esp_dmx "enable" line). All bus access runs on loop()'s
// thread — the only owner of the DMX port — so the async web/WS task just sets
// request flags.
// ---------------------------------------------------------------------------
static constexpr int RDM_MAX_SENSORS = 4;        // sensors stored per fixture
struct RdmSensor {
    char    name[20];   // SENSOR_DEFINITION description, or a type label
    char    unit[8];    // SI unit string derived from the definition
    int16_t value;      // SENSOR_VALUE present value
    bool    valid;      // a value was read
};
struct RdmDevice {
    rdm_uid_t uid;
    uint16_t  startAddr;
    uint16_t  footprint;
    uint16_t  modelId;
    uint16_t  subDeviceCount;
    uint8_t   personality;
    uint8_t   personalityCount;
    bool      identifying;
    char      swLabel[33];
    uint8_t   sensorCount;
    RdmSensor sensors[RDM_MAX_SENSORS];
};
static constexpr int RDM_MAX_DEVICES = 32;
static RdmDevice rdmDevices[RDM_MAX_DEVICES];
static int       rdmCount      = 0;
static bool      rdmScanned    = false;       // a discovery has completed at least once
static volatile bool rdmBusy   = false;       // discovery in progress
static uint32_t  rdmLastScanMs = 0;

// Single-slot request mailboxes: set by the async WS task, consumed in loop().
static volatile bool rdmDiscoverReq = false;
static volatile bool rdmSetAddrReq  = false;
static volatile bool rdmIdentifyReq = false;
static rdm_uid_t     rdmSetUid      = {0, 0};
static rdm_uid_t     rdmIdentUid    = {0, 0};
static volatile uint16_t rdmReqAddr = 1;
static volatile bool rdmReqOn       = false;

// RDM only works when the DMX driver is up AND a direction-enable pin is set.
static bool rdmAvailable() { return dmxReady && rdmOut >= 0 && outReady[rdmOut]; }

// Map E1.20 sensor unit / type enums to short display strings.
static const char* rdmUnitStr(uint8_t u) {
    switch (u) {
        case RDM_UNITS_CENTIGRADE:    return "C";
        case RDM_UNITS_VOLTS_DC:
        case RDM_UNITS_VOLTS_AC_PEAK:
        case RDM_UNITS_VOLTS_AC_RMS:  return "V";
        case RDM_UNITS_AMPERE_DC:
        case RDM_UNITS_AMPERE_AC_PEAK:
        case RDM_UNITS_AMPERE_AC_RMS: return "A";
        case RDM_UNITS_HERTZ:         return "Hz";
        case RDM_UNITS_OHM:           return "ohm";
        case RDM_UNITS_WATT:          return "W";
        case RDM_UNITS_KILOGRAM:      return "kg";
        case RDM_UNITS_METERS:        return "m";
        case RDM_UNITS_SECOND:        return "s";
        case RDM_UNITS_DEGREE:        return "deg";
        case RDM_UNITS_LUX:           return "lux";
        case RDM_UNITS_BYTE:          return "B";
        default:                      return "";
    }
}
static const char* rdmTypeStr(uint8_t t) {
    switch (t) {
        case RDM_SENSOR_TYPE_TEMPERATURE:      return "Temperature";
        case RDM_SENSOR_TYPE_VOLTAGE:          return "Voltage";
        case RDM_SENSOR_TYPE_CURRENT:          return "Current";
        case RDM_SENSOR_TYPE_FREQUENCY:        return "Frequency";
        case RDM_SENSOR_TYPE_POWER:            return "Power";
        case RDM_SENSOR_TYPE_ANGULAR_VELOCITY: return "Fan";
        case RDM_SENSOR_TYPE_TIME:             return "Time";
        case RDM_SENSOR_TYPE_HUMIDITY:         return "Humidity";
        default:                               return "Sensor";
    }
}

static RdmDevice* rdmFind(const rdm_uid_t& uid) {
    for (int i = 0; i < rdmCount; i++)
        if (rdm_uid_is_eq(&rdmDevices[i].uid, &uid)) return &rdmDevices[i];
    return nullptr;
}

// Parse a "uid":"MMMM:DDDDDDDD" field out of a small JSON control message.
static bool rdmParseUid(const String& msg, rdm_uid_t& out) {
    int k = msg.indexOf("\"uid\":\"");
    if (k < 0) return false;
    k += 7;
    int colon = msg.indexOf(':', k);
    int end   = msg.indexOf('"', k);
    if (colon < 0 || end < 0 || colon > end) return false;
    out.man_id = (uint16_t)strtoul(msg.substring(k, colon).c_str(), nullptr, 16);
    out.dev_id = (uint32_t)strtoul(msg.substring(colon + 1, end).c_str(), nullptr, 16);
    return true;
}

// Full discovery sweep + per-device GET device-info & software-version label.
// Blocks the bus for the duration (~hundreds of ms) — DMX output pauses briefly.
static void rdmDoDiscover() {
    dmx_port_t port = (dmx_port_t)cfg.outputs[rdmOut].port;
    rdmBusy = true;
    rdm_uid_t uids[RDM_MAX_DEVICES];
    int n = rdm_discover_devices_simple(port, uids, RDM_MAX_DEVICES);
    if (n > RDM_MAX_DEVICES) n = RDM_MAX_DEVICES;
    rdmCount = 0;
    for (int i = 0; i < n; i++) {
        RdmDevice d = {};
        d.uid = uids[i];
        rdm_ack_t ack;
        rdm_device_info_t info;
        if (rdm_send_get_device_info(port, &uids[i], RDM_SUB_DEVICE_ROOT, &info, &ack)
            && ack.type == RDM_RESPONSE_TYPE_ACK) {
            d.startAddr        = info.dmx_start_address;
            d.footprint        = info.footprint;
            d.modelId          = info.model_id;
            d.subDeviceCount   = info.sub_device_count;
            d.personality      = info.personality.current;
            d.personalityCount = info.personality.count;
            d.sensorCount      = info.sensor_count > RDM_MAX_SENSORS
                                     ? RDM_MAX_SENSORS : info.sensor_count;
        }
        rdm_send_get_software_version_label(port, &uids[i], RDM_SUB_DEVICE_ROOT,
                                            d.swLabel, sizeof(d.swLabel), &ack);

        // Sensors (E1.20): per sensor read its definition (name/unit) then value.
        // Sensors are numbered 0..count-1; definition and value are independent —
        // tolerate either being unsupported.
        for (uint8_t s = 0; s < d.sensorCount; s++) {
            RdmSensor& sen = d.sensors[s];
            rdm_ack_t sack;
            uint8_t   sn = s;

            rdm_sensor_definition_t def = {};
            rdm_request_t dreq = { &uids[i], RDM_SUB_DEVICE_ROOT, RDM_CC_GET_COMMAND,
                                   RDM_PID_SENSOR_DEFINITION, "b$", &sn, 1 };
            if (rdm_send_request(port, &dreq, "bbbbwwwwba$", &def, sizeof(def), &sack)
                && sack.type == RDM_RESPONSE_TYPE_ACK) {
                strlcpy(sen.name, def.description[0] ? def.description : rdmTypeStr(def.type),
                        sizeof(sen.name));
                strlcpy(sen.unit, rdmUnitStr(def.unit), sizeof(sen.unit));
            }

            rdm_sensor_value_t val = {};
            rdm_request_t vreq = { &uids[i], RDM_SUB_DEVICE_ROOT, RDM_CC_GET_COMMAND,
                                   RDM_PID_SENSOR_VALUE, "b$", &sn, 1 };
            if (rdm_send_request(port, &vreq, "bwwww$", &val, sizeof(val), &sack)
                && sack.type == RDM_RESPONSE_TYPE_ACK) {
                sen.value = val.present_value;
                sen.valid = true;
                if (!sen.name[0]) strlcpy(sen.name, "Sensor", sizeof(sen.name));
            }
        }
        rdmDevices[rdmCount++] = d;
    }
    rdmScanned    = true;
    rdmLastScanMs = millis();
    rdmBusy       = false;
    Serial.printf("[RDM] discovery: %d device(s)\n", rdmCount);
}

// Called once per loop() iteration; does work only when a request is queued.
static void rdmService() {
    if (!rdmAvailable()) return;
    dmx_port_t port = (dmx_port_t)cfg.outputs[rdmOut].port;
    rdm_ack_t ack;

    if (rdmSetAddrReq) {
        rdmSetAddrReq = false;
        if (rdm_send_set_dmx_start_address(port, &rdmSetUid, RDM_SUB_DEVICE_ROOT,
                                           rdmReqAddr, &ack)) {
            RdmDevice* d = rdmFind(rdmSetUid);
            if (d) d->startAddr = rdmReqAddr;
            Serial.printf("[RDM] set " UIDSTR " addr=%u\n", UID2STR(rdmSetUid), rdmReqAddr);
        }
    }
    if (rdmIdentifyReq) {
        rdmIdentifyReq = false;
        if (rdm_send_set_identify_device(port, &rdmIdentUid, RDM_SUB_DEVICE_ROOT,
                                         rdmReqOn ? 1 : 0, &ack)) {
            RdmDevice* d = rdmFind(rdmIdentUid);
            if (d) d->identifying = rdmReqOn;
        }
    }
    if (rdmDiscoverReq) {
        rdmDiscoverReq = false;
        rdmDoDiscover();
    }
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
static void maybeLog(int outIdx, const uint8_t* data, uint16_t len, uint32_t ip, uint8_t proto) {
    uint32_t now = millis();
    if (now - lastLogMs < 200) return;

    LogEntry e;
    e.ms    = now;
    e.ip    = ip;
    e.proto = proto;
    e.uni   = (uint8_t)cfg.outputs[outIdx].universe;
    e.total = 0;
    e.topN  = 0;
    uint16_t lim = len < 512 ? len : 512;
    for (int i = 0; i < lim; i++) {
        if (data[i] != dmxBuf[outIdx][i + 1]) {
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
    if (ws.count() == 0) return;
    if (ESP.getFreeHeap() < 40000) return;   // never push under heap pressure
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
    memcpy(&wsBuf[16], &dmxBuf[viewOutput()][1], 512);   // stream the viewed output
    // Only push if the async TCP queues have room, so a slow client never
    // backs up memory or blocks.
    if (ws.availableForWriteAll()) ws.binaryAll(wsBuf, 528);
}

// ---------------------------------------------------------------------------
// WebSocket event (browser → ESP). Runs in the AsyncTCP task, so it only
// updates dmxBuf/flags — loop() performs the actual DMX send.
// ---------------------------------------------------------------------------
static void handleWsText(const char* payload, size_t len) {
    String msg(payload, len);
    // Manual control + the live monitor act on the viewed output (monitorOut);
    // loop()'s 40 Hz refresh outputs every buffer.
    if (msg.indexOf("\"viewout\"") >= 0) {
        int k = msg.indexOf("\"out\":");
        if (k >= 0) {
            int o = msg.substring(k + 6).toInt();
            if (o >= 0 && o < MAX_OUTPUTS && cfg.outputs[o].enabled) monitorOut = o;
        }
        return;
    }
    if (msg.indexOf("\"blackout\"") >= 0) {
        memset(&dmxBuf[viewOutput()][1], 0, 512); return;
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
        return;
    }
    if (msg.indexOf("\"set\"") >= 0) {
        int chIdx  = msg.indexOf("\"ch\":");
        int valIdx = msg.indexOf("\"val\":");
        if (chIdx < 0 || valIdx < 0) return;
        int ch  = msg.substring(chIdx  + 5).toInt();
        int val = msg.substring(valIdx + 6).toInt();
        if (ch < 1 || ch > 512) return;
        dmxBuf[viewOutput()][ch] = (uint8_t)constrain(val, 0, 255);
        return;
    }
    // RDM control — only set request flags here; loop() owns the bus and runs them.
    if (msg.indexOf("\"rdm_discover\"") >= 0) { rdmDiscoverReq = true; return; }
    if (msg.indexOf("\"rdm_setaddr\"") >= 0) {
        rdm_uid_t u;
        int k = msg.indexOf("\"addr\":");
        if (rdmParseUid(msg, u) && k >= 0) {
            int a = msg.substring(k + 7).toInt();
            if (a >= 1 && a <= 512) { rdmSetUid = u; rdmReqAddr = (uint16_t)a; rdmSetAddrReq = true; }
        }
        return;
    }
    if (msg.indexOf("\"rdm_identify\"") >= 0) {
        rdm_uid_t u;
        if (rdmParseUid(msg, u)) {
            rdmIdentUid = u;
            rdmReqOn    = (msg.indexOf("\"on\":true") >= 0);
            rdmIdentifyReq = true;
        }
        return;
    }
}

static volatile uint32_t httpReqCount = 0;   // [DEBUG] HTTP requests served
static volatile uint32_t wsConnCount  = 0;   // [DEBUG] WS connects
static volatile uint32_t wsDiscCount  = 0;   // [DEBUG] WS disconnects

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType type,
                      void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT)    { wsConnCount++; return; }
    if (type == WS_EVT_DISCONNECT) { wsDiscCount++; return; }
    if (type != WS_EVT_DATA) return;
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    // Only handle complete, single-frame text messages (our control msgs are tiny)
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        handleWsText((const char*)data, len);
    }
}

// ---------------------------------------------------------------------------
// Shared DMX frame handler
// ---------------------------------------------------------------------------
// Copy an incoming frame into one output's buffer. The monitored output is
// frozen while manual mode is on (the web UI owns it then).
static void applyToOutput(int outIdx, const uint8_t* data, uint16_t length) {
    if (manualMode && outIdx == viewOutput()) return;
    memcpy(&dmxBuf[outIdx][1], data, min((uint16_t)512, length));
}

// Route one received universe to every enabled output mapped to it (so the
// same universe on both outputs acts as a 1-in-2-out splitter), then update
// the aggregate stats/sender tracking once per input frame.
static void routeFrame(int artUniverse, const uint8_t* data, uint16_t length,
                       uint32_t senderIp, uint8_t proto) {
    bool matched = false;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (!cfg.outputs[i].enabled || cfg.outputs[i].universe != artUniverse) continue;
        // Log changes for the viewed output before its buffer is overwritten.
        if (i == viewOutput()) maybeLog(i, data, length, senderIp, proto);
        applyToOutput(i, data, length);
        matched = true;
    }
    if (!matched) return;

    uint32_t now = millis();

    // [DEBUG] flag DMX reception gaps (input stalled)
    if (lastDmxMs && now - lastDmxMs > 300)
        Serial.printf("[GAP] dmx input gap=%lums proto=%d up=%lus\n",
            (unsigned long)(now - lastDmxMs), proto, (unsigned long)uptimeSec());

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
    routeFrame((int)universe, data, length, (uint32_t)artnet.getSenderIp(), 0);
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
    // One multicast socket per enabled output, each joined to its universe's
    // group (sACN universe = Art-Net universe + 1). Sockets share port 5568
    // (WiFiUDP sets SO_REUSEADDR); lwip delivers each group to its joiner.
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        sacnUdp[i].stop();
        if (!cfg.outputs[i].enabled) continue;
        uint16_t sacnUniverse = (uint16_t)(cfg.outputs[i].universe + 1);
        uint8_t  univHigh     = (uint8_t)((sacnUniverse >> 8) & 0xFF);
        uint8_t  univLow      = (uint8_t)(sacnUniverse & 0xFF);
        IPAddress mcast(239, 255, univHigh, univLow);
        sacnUdp[i].beginMulticast(mcast, 5568);
        Serial.printf("[sACN] out%d universe %u  multicast 239.255.%u.%u:5568\n",
                      i, sacnUniverse, univHigh, univLow);
    }
}

// Validate + dispatch one sACN socket's pending packets to its output.
static void readSacnSocket(int outIdx) {
    WiFiUDP& udp = sacnUdp[outIdx];
    // Drain all packets buffered since the last call (catches up after any gap)
    for (int guard = 0; guard < 16; guard++) {
        int pktLen = udp.parsePacket();
        if (pktLen < SACN_MIN_LEN) return;
        uint32_t senderIp = (uint32_t)udp.remoteIP();
        int n = udp.read(sacnBuf, sizeof(sacnBuf));
        if (n < SACN_MIN_LEN) continue;
        if (memcmp(sacnBuf + SACN_ACN_ID_OFF, ACN_PACKET_ID, 12) != 0) continue;
        uint32_t rootVec = ((uint32_t)sacnBuf[SACN_ROOT_VEC_OFF    ] << 24)
                         | ((uint32_t)sacnBuf[SACN_ROOT_VEC_OFF + 1] << 16)
                         | ((uint32_t)sacnBuf[SACN_ROOT_VEC_OFF + 2] <<  8)
                         |  (uint32_t)sacnBuf[SACN_ROOT_VEC_OFF + 3];
        if (rootVec != 0x00000004u) continue;
        uint32_t frameVec = ((uint32_t)sacnBuf[SACN_FRAME_VEC_OFF    ] << 24)
                          | ((uint32_t)sacnBuf[SACN_FRAME_VEC_OFF + 1] << 16)
                          | ((uint32_t)sacnBuf[SACN_FRAME_VEC_OFF + 2] <<  8)
                          |  (uint32_t)sacnBuf[SACN_FRAME_VEC_OFF + 3];
        if (frameVec != 0x00000002u) continue;
        uint16_t universe = ((uint16_t)sacnBuf[SACN_UNIVERSE_OFF] << 8)
                           | sacnBuf[SACN_UNIVERSE_OFF + 1];
        if ((int)universe != cfg.outputs[outIdx].universe + 1) continue;
        if (sacnBuf[SACN_STARTCODE_OFF] != 0x00) continue;
        routeFrame((int)universe - 1, sacnBuf + SACN_DATA_OFF, 512, senderIp, 1);
    }
}

static void readSacn() {
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (cfg.outputs[i].enabled) readSacnSocket(i);
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
// Fetch a request parameter from POST body or query string
static bool argStr(AsyncWebServerRequest* req, const char* n, String& out) {
    if (req->hasParam(n, true)) { out = req->getParam(n, true)->value(); return true; }
    if (req->hasParam(n))       { out = req->getParam(n)->value();       return true; }
    return false;
}

static void handleVersionJson(AsyncWebServerRequest* req) {
    String j = "{\"current\":\"";
    j += FIRMWARE_VERSION;
    j += "\",\"latest\":\"";
    j += latestVersion.length() > 0 ? latestVersion : String(FIRMWARE_VERSION);
    j += "\",\"update\":";
    j += updateAvailable ? "true" : "false";
    j += "}";
    req->send(200, "application/json", j);
}

static String sendersJson() {
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
    return j;
}

static String logJson() {
    String j = "[";
    bool first = true;
    for (int k = 0; k < logCount; k++) {
        // Iterate newest → oldest
        int idx = ((int)logHead - 1 - k + LOG_SIZE * 2) % LOG_SIZE;
        LogEntry& e = dmxLog[idx];
        if (!first) j += ",";
        first = false;
        char buf[80];
        snprintf(buf, sizeof(buf),
            "{\"ms\":%lu,\"ip\":\"%s\",\"p\":%d,\"u\":%d,\"n\":%d,\"ch\":[",
            (unsigned long)e.ms, ipStr(e.ip).c_str(), (int)e.proto, (int)e.uni, (int)e.total);
        j += buf;
        for (int t = 0; t < e.topN; t++) {
            if (t > 0) j += ",";
            snprintf(buf, sizeof(buf), "[%d,%d]", (int)e.top[t].ch, (int)e.top[t].val);
            j += buf;
        }
        j += "]}";
    }
    j += "]";
    return j;
}

static void handleSendersJson(AsyncWebServerRequest* req) { req->send(200, "application/json", sendersJson()); }
static void handleLogJson(AsyncWebServerRequest* req)     { req->send(200, "application/json", logJson()); }

// Push senders + log over the WebSocket (one persistent connection) so the
// browser doesn't have to poll two HTTP endpoints every 2 s.
static void wsPushMeta() {
    if (ws.count() == 0 || !ws.availableForWriteAll()) return;
    if (ESP.getFreeHeap() < 40000) return;   // never push under heap pressure
    String m = "{\"meta\":1,\"senders\":";
    m += sendersJson();
    m += ",\"log\":";
    m += logJson();
    m += "}";
    ws.textAll(m);
}

// Static pages are served straight from PROGMEM (zero heap). Dynamic values
// are fetched client-side from /info.json.
static void handleRoot(AsyncWebServerRequest* req) {
    httpReqCount++;
    AsyncWebServerResponse* r = req->beginResponse_P(200, "text/html", INDEX_HTML, INDEX_HTML_LEN);
    r->addHeader("Content-Encoding", "gzip");
    req->send(r);
}

static void handleInfoJson(AsyncWebServerRequest* req) {
    String j = "{";
    j += "\"ssid\":\"";     j += netSSID();              j += "\",";
    j += "\"ip\":\"";       j += netLocalIP().toString(); j += "\",";
    j += "\"hostname\":\""; j += cfg.hostname;           j += "\",";
    j += "\"version\":\"";  j += FIRMWARE_VERSION;       j += "\",";
    j += "\"otapw\":\"";    j += cfg.otaPassword;        j += "\",";
    j += "\"universe\":";   j += cfg.outputs[0].universe; j += ",";   // legacy/back-compat
    j += "\"protocol\":";   j += cfg.protocol;           j += ",";
    j += "\"ledType\":";    j += cfg.ledType;            j += ",";
    j += "\"ledPin\":";     j += cfg.ledPin;             j += ",";
    j += "\"rdmOut\":";     j += rdmOut;                 j += ",";
    j += "\"outputs\":[";
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        const DmxOutput& o = cfg.outputs[i];
        if (i) j += ",";
        j += "{\"en\":";   j += o.enabled ? "true" : "false";
        j += ",\"uni\":";  j += o.universe;
        j += ",\"port\":"; j += o.port;
        j += ",\"tx\":";   j += o.txPin;
        j += ",\"rx\":";   j += o.rxPin;
        j += ",\"rts\":";  j += o.rtsPin;
        j += "}";
    }
    j += "],";
    j += "\"staticIp\":";   j += cfg.staticIp ? "true" : "false"; j += ",";
    j += "\"sip\":\"";      j += cfg.ip;                 j += "\",";
    j += "\"gateway\":\"";  j += cfg.gateway;            j += "\",";
    j += "\"subnet\":\"";   j += cfg.subnet;             j += "\",";
    j += "\"dns\":\"";      j += cfg.dns;                j += "\",";
    j += "\"autoUpdate\":"; j += cfg.autoUpdate ? "true" : "false";
    j += "}";
    req->send(200, "application/json", j);
}

static void handleDmxJson(AsyncWebServerRequest* req) {
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
        j += dmxBuf[viewOutput()][i];
        if (i < 512) j += ',';
    }
    j += "]}";
    req->send(200, "application/json", j);
}

// Escape a fixture-supplied string for safe inclusion in JSON.
static String rdmJsonEsc(const char* s) {
    String o;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c >= 0x20)  o += c;
    }
    return o;
}

static void handleRdmJson(AsyncWebServerRequest* req) {
    String j;
    j.reserve(96 + rdmCount * 280);
    j  = "{\"available\":"; j += rdmAvailable() ? "true" : "false";
    j += ",\"busy\":";      j += rdmBusy ? "true" : "false";
    j += ",\"scanned\":";   j += rdmScanned ? "true" : "false";
    j += ",\"devices\":[";
    for (int i = 0; i < rdmCount; i++) {
        const RdmDevice& d = rdmDevices[i];
        char uid[20];
        snprintf(uid, sizeof(uid), "%04X:%08lX", d.uid.man_id, (unsigned long)d.uid.dev_id);
        if (i) j += ',';
        j += "{\"uid\":\"";     j += uid;          j += "\"";
        j += ",\"addr\":";      j += d.startAddr;
        j += ",\"footprint\":"; j += d.footprint;
        j += ",\"model\":";     j += d.modelId;
        j += ",\"pers\":";      j += d.personality;
        j += ",\"persCount\":"; j += d.personalityCount;
        j += ",\"subs\":";      j += d.subDeviceCount;
        j += ",\"identify\":";  j += d.identifying ? "true" : "false";
        j += ",\"sw\":\"";      j += rdmJsonEsc(d.swLabel); j += "\"";
        j += ",\"sensors\":[";
        bool firstSen = true;
        for (int s = 0; s < d.sensorCount; s++) {
            const RdmSensor& sn = d.sensors[s];
            if (!sn.valid) continue;
            if (!firstSen) j += ',';
            firstSen = false;
            j += "{\"name\":\"";  j += rdmJsonEsc(sn.name);
            j += "\",\"value\":"; j += sn.value;
            j += ",\"unit\":\"";  j += rdmJsonEsc(sn.unit); j += "\"}";
        }
        j += "]}";
    }
    j += "]}";
    req->send(200, "application/json", j);
}

static void handleConfigGet(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse_P(200, "text/html", CONFIG_HTML, CONFIG_HTML_LEN);
    r->addHeader("Content-Encoding", "gzip");
    req->send(r);
}

static void handleConfigPost(AsyncWebServerRequest* req) {
    String s;
    if (argStr(req, "hostname", s) && s.length() > 0) cfg.hostname = s;
    if (argStr(req, "otapw", s)    && s.length() > 0) cfg.otaPassword = s;
    if (argStr(req, "protocol", s)) cfg.protocol = constrain(s.toInt(), 0, 2);
    if (argStr(req, "ledtype", s))  cfg.ledType   = constrain(s.toInt(), 0, 2);
    if (argStr(req, "ledpin", s))   cfg.ledPin    = constrain(s.toInt(), -1, 48);

    // Per-output DMX config: o<i>_en / _uni / _port / _tx / _rx / _rts.
    // A missing o<i>_en checkbox means that output is disabled.
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        DmxOutput& o = cfg.outputs[i];
        o.enabled = req->hasParam(okey(i,"en"), true) || req->hasParam(okey(i,"en"));
        if (argStr(req, okey(i,"uni").c_str(),  s)) o.universe = constrain(s.toInt(), 0, 15);
        if (argStr(req, okey(i,"port").c_str(), s)) o.port     = constrain(s.toInt(), 1, 2);
        if (argStr(req, okey(i,"tx").c_str(),   s)) o.txPin    = constrain(s.toInt(), -1, 48);
        if (argStr(req, okey(i,"rx").c_str(),   s)) o.rxPin    = constrain(s.toInt(), -1, 48);
        if (argStr(req, okey(i,"rts").c_str(),  s)) o.rtsPin   = constrain(s.toInt(), -1, 48);
    }
    cfg.staticIp = req->hasParam("staticip", true) || req->hasParam("staticip");
    if (argStr(req, "ip", s))      cfg.ip      = s;
    if (argStr(req, "gateway", s)) cfg.gateway = s;
    if (argStr(req, "subnet", s))  cfg.subnet  = s;
    if (argStr(req, "dns", s))     cfg.dns     = s;
    sanitizeOutputs();   // never persist an enabled output with no TX pin
    dmxReady = false;
    saveConfig();
    req->send_P(200, "text/html", CONFIG_SAVED_HTML);
    pendingRebootAt = millis() + 600;
}

// ---------------------------------------------------------------------------
// Channel labels — browser owns the JSON object, device just persists it
// ---------------------------------------------------------------------------
static void handleLabelsGet(AsyncWebServerRequest* req) {
    req->send(200, "application/json", g_labels);
}

// Body handler for POST /labels (raw JSON). Accumulates chunks then persists.
static void handleLabelsBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                             size_t index, size_t total) {
    static String buf;
    if (index == 0) { buf = ""; buf.reserve(total + 1); }
    if (total <= LABELS_MAX) buf.concat((const char*)data, len);
    if (index + len != total) return;   // wait for the full body
    if (buf.length() == 0 || buf.length() > LABELS_MAX || buf[0] != '{') {
        req->send(400, "text/plain", "Invalid labels payload");
        buf = "";
        return;
    }
    g_labels = buf;
    buf = "";
    prefs.begin(PREF_NS, false);
    prefs.putString("labels", g_labels);
    prefs.end();
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleAutoUpdatePost(AsyncWebServerRequest* req) {
    String s;
    cfg.autoUpdate = argStr(req, "enabled", s) && s == "1";
    saveConfig();
    req->send(200, "application/json",
        String("{\"autoUpdate\":") + (cfg.autoUpdate ? "true" : "false") + "}");
}

static void handleResetGet(AsyncWebServerRequest* req)  { req->send_P(200, "text/html", RESET_HTML); }

static void handleResetPost(AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", RESET_DONE_HTML);
    // Also drop static IP so recovery always comes back up on DHCP
    cfg.staticIp = false;
    saveConfig();
    pendingWifiReset = true;
    pendingRebootAt  = millis() + 600;
}

static void handleLogo(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse_P(200, "image/png", LOGO_PNG, LOGO_PNG_LEN);
    r->addHeader("Cache-Control", "max-age=86400");
    req->send(r);
}

static void handleBootstrapCss(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse_P(200, "text/css", BOOTSTRAP_MIN_CSS, BOOTSTRAP_MIN_CSS_LEN);
    r->addHeader("Content-Encoding", "gzip");
    r->addHeader("Cache-Control", "max-age=604800");
    req->send(r);
}

// ---------------------------------------------------------------------------
// Version check (FreeRTOS task, runs once 8s after boot)
// ---------------------------------------------------------------------------
static int parseBuild(const String& v) {
    int dot = v.lastIndexOf('.');
    return dot >= 0 ? v.substring(dot + 1).toInt() : 0;
}

static bool httpsGet(const char* url, String& out, size_t maxLen) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient h;
    h.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!h.begin(client, url)) return false;
    bool ok = false;
    if (h.GET() == 200) {
        String s = h.getString();
        s.trim();
        if (s.length() > 0 && s.length() <= maxLen) { out = s; ok = true; }
    }
    h.end();
    return ok;
}

static void checkForUpdate() {
    String v;
    if (!httpsGet("https://github.com/tombueng/LumiGate/releases/download/latest/version.txt", v, 24)) return;
    latestVersion   = v;
    updateAvailable = parseBuild(v) > parseBuild(String(FIRMWARE_VERSION));
    Serial.printf("[VER] latest=%s current=%s update=%s\n",
        v.c_str(), FIRMWARE_VERSION, updateAvailable ? "yes" : "no");
}

static void versionCheckTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(8000));
    for (;;) {
        checkForUpdate();
        if (cfg.autoUpdate && updateAvailable) {
            Serial.println("[OTA] auto-update enabled, installing latest...");
            otaTarget = "latest";
            pendingGithubOta = true;   // loop() performs the update
        }
        vTaskDelay(pdMS_TO_TICKS(6UL * 3600UL * 1000UL));  // re-check every 6 h
    }
}

// ---------------------------------------------------------------------------
// OTA handlers
// ---------------------------------------------------------------------------
// Firmware asset name for this build target
#if defined(USE_ETHERNET)
#define OTA_BIN "firmware-wt32eth01.bin"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define OTA_BIN "firmware-esp32s3.bin"
#else
#define OTA_BIN "firmware.bin"
#endif

static void doGithubOta() {
    String otaUrl = "https://github.com/tombueng/LumiGate/releases/download/"
                    + otaTarget + "/" + OTA_BIN;
    Serial.printf("[OTA] Starting update from %s\n", otaUrl.c_str());
    dmxReady = false;
    WiFiClientSecure client;
    client.setInsecure();
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true);
    httpUpdate.update(client, otaUrl);
    Serial.printf("[OTA] Failed (%d): %s\n",
        httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    dmxReady = true;
    delay(2000);
    ESP.restart();
}

static void handleOtaGithub(AsyncWebServerRequest* req) {
    // Optional version=1.0.N (POST/query) selects a specific release; default latest
    String v;
    argStr(req, "version", v);
    v.trim();
    if (v.length() == 0 || v == "latest") {
        otaTarget = "latest";
    } else {
        if (v[0] == 'v' || v[0] == 'V') v = v.substring(1);
        otaTarget = "v" + v;
    }
    Serial.printf("[OTA] Target requested: %s\n", otaTarget.c_str());
    req->send_P(200, "text/html", OTA_PROGRESS_HTML);
    pendingGithubOta = true;
}

static void handleOtaUploadDone(AsyncWebServerRequest* req) {
    bool ok = !Update.hasError();
    String p = FPSTR(OTA_DONE_HTML);
    p.replace("{{OTA_ICON}}",  ok ? "&#10003;" : "&#10007;");
    p.replace("{{OTA_CLASS}}", ok ? "text-success" : "text-danger");
    p.replace("{{OTA_TITLE}}", ok ? "Firmware updated" : "Update failed");
    p.replace("{{OTA_MSG}}",   ok ? "Rebooting&hellip;" :
                                    String("Error: ") + Update.errorString());
    req->send(200, "text/html", p);
    if (ok) pendingRebootAt = millis() + 800;
}

static void handleOtaUploadChunk(AsyncWebServerRequest* req, const String& filename,
                                 size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        Serial.printf("[OTA] Upload: %s\n", filename.c_str());
        dmxReady = false;
        Update.begin(UPDATE_SIZE_UNKNOWN);
    }
    if (len) Update.write(data, len);
    if (final) {
        Update.end(true);
        Serial.printf("[OTA] Upload done: %u bytes\n", (unsigned)(index + len));
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
    wm.setAPCallback([](WiFiManager*) { setLedColor(NEO_PURPLE, true); }); // portal open
    wm.setConnectTimeout(60);
    wm.setConfigPortalTimeout(180);
    if (cfg.staticIp) {
        IPAddress ip, gw, sn, dns;
        parseIp(cfg.ip, ip); parseIp(cfg.gateway, gw);
        parseIp(cfg.subnet, sn); parseIp(cfg.dns, dns);
        wm.setSTAStaticIPConfig(ip, gw, sn, dns);
        Serial.printf("[WiFi] static IP %s\n", cfg.ip.c_str());
    }
    snprintf(wm_universeStr, sizeof(wm_universeStr), "%d", cfg.outputs[0].universe);
    WiFiManagerParameter param_universe("universe", "Art-Net Universe (0-15)", wm_universeStr, 3);
    wm.addParameter(&param_universe);
    bool connected = forcePortal ? wm.startConfigPortal(AP_SSID)
                                 : wm.autoConnect(AP_SSID);
    if (!connected) ESP.restart();
    if (wm_shouldSave) {
        cfg.outputs[0].universe = constrain(atoi(param_universe.getValue()), 0, 15);
        saveConfig();
        // The captive-portal ran its own web server on port 80; it may not
        // release the socket cleanly, which would stop our AsyncWebServer from
        // binding (HTTP "connection refused" until reboot). Reboot for a clean
        // start where auto-connect succeeds and the portal never runs.
        Serial.println("[WiFi] credentials saved — rebooting for a clean start");
        delay(400);
        ESP.restart();
    }
}

// Scan every AP for our SSID, log them, and (re)connect to the strongest one.
// On mesh/multi-AP networks the ESP32's auto-connect often sticks to a distant
// node; this guarantees the closest one. Also a diagnostic: the [SCAN] log
// reveals whether the SSID has multiple BSSIDs (per-node) or a single shared
// BSSID (seamless mesh — in which case the AP, not us, chooses the node).
static void connectStrongestAP() {
    String ssid = WiFi.SSID();
    if (ssid.length() == 0) return;
    String pass = WiFi.psk();
    int curRssi = (int)WiFi.RSSI();

    int n = WiFi.scanNetworks(false /*async*/, true /*hidden*/);
    int bestIdx = -1, bestRssi = -999, bestCh = 0, matches = 0;
    uint8_t bestBssid[6] = {0};
    Serial.printf("[SCAN] %d networks total. APs for '%s':\n", n, ssid.c_str());
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != ssid) continue;
        matches++;
        Serial.printf("   bssid=%s rssi=%d ch=%d\n",
            WiFi.BSSIDstr(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
        if (WiFi.RSSI(i) > bestRssi) {
            bestRssi = WiFi.RSSI(i); bestIdx = i; bestCh = WiFi.channel(i);
            memcpy(bestBssid, WiFi.BSSID(i), 6);
        }
    }
    Serial.printf("[SCAN] %d AP(s) for SSID, current rssi=%d, best rssi=%d\n",
        matches, curRssi, bestRssi);
    WiFi.scanDelete();

    // Only switch if a meaningfully stronger distinct AP exists
    if (bestIdx >= 0 && bestRssi > curRssi + 6) {
        Serial.printf("[SCAN] switching to stronger AP (rssi %d -> %d)\n", curRssi, bestRssi);
        WiFi.begin(ssid.c_str(), pass.c_str(), bestCh, bestBssid, true);
        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) {
            setLedColor((millis() % 400) < 200 ? NEO_BLUE : NEO_OFF, (millis() % 400) < 200);
            delay(100);
        }
        Serial.printf("[SCAN] reconnected rssi=%d bssid=%s\n",
            (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str());
    }
}
#endif

// ---------------------------------------------------------------------------
// Peripheral init
// ---------------------------------------------------------------------------
static void initDmx() {
    dmxReady = false;
    rdmOut   = -1;
    monitorOut = 0;
    bool firstEnabled = true;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        outReady[i] = false;
        if (!cfg.outputs[i].enabled) continue;

        // A DMX output must have a real TX GPIO. esp_dmx treats tx=-1 as
        // "no change", so installing a driver with no TX pin half-configures
        // the UART and crashes on the first send — a boot loop. Skip it.
        if (cfg.outputs[i].txPin < 0) {
            Serial.printf("[DMX] out%d skipped: enabled but no TX pin (tx=%d)\n",
                          i, cfg.outputs[i].txPin);
            continue;
        }

        // Two outputs cannot share a UART port; skip the colliding one.
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (outReady[j] && cfg.outputs[j].port == cfg.outputs[i].port) dup = true;
        if (dup) {
            Serial.printf("[DMX] out%d skipped: port %d already in use\n", i, cfg.outputs[i].port);
            continue;
        }

        dmx_port_t port = (dmx_port_t)cfg.outputs[i].port;
        dmx_config_t config = DMX_CONFIG_DEFAULT;
        dmx_driver_install(port, &config, nullptr, 0);
        dmx_set_pin(port, cfg.outputs[i].txPin, cfg.outputs[i].rxPin, cfg.outputs[i].rtsPin);
        dmx_write(port, dmxBuf[i], DMX_PACKET_SIZE);
        dmx_send(port);
        dmx_wait_sent(port, DMX_TIMEOUT_TICK);
        outReady[i] = true;
        dmxReady    = true;
        if (firstEnabled) { monitorOut = i; firstEnabled = false; }
        // RDM binds to the first enabled output with a direction-enable pin.
        if (rdmOut < 0 && cfg.outputs[i].rtsPin >= 0) rdmOut = i;
        Serial.printf("[DMX] out%d ready: uni=%d port=%d tx=%d rx=%d rts=%d\n",
            i, cfg.outputs[i].universe, cfg.outputs[i].port,
            cfg.outputs[i].txPin, cfg.outputs[i].rxPin, cfg.outputs[i].rtsPin);
    }
    if (!dmxReady) Serial.println("[DMX] no outputs enabled");
    else Serial.printf("[DMX] ready (monitor=out%d rdm=out%d)\n", monitorOut, rdmOut);
}

// ---------------------------------------------------------------------------
// Safe-boot guard around DMX init
// A bad port/pin can make esp_dmx panic *inside* driver install — an
// uncatchable CPU exception that would otherwise boot-loop forever. We persist
// a crash counter before touching the UART and clear it only after init
// returns. If a previous boot died mid-init the counter survives the reboot, so
// we progressively disable outputs until the device always reaches the web UI:
//   >=2 consecutive  -> disable the extra output(s), keep output A
//   >=4 consecutive  -> disable all DMX outputs
// A single transient crash is tolerated (counter clears on the next good boot).
// ---------------------------------------------------------------------------
static void dmxInitGuardBegin() {
    prefs.begin(PREF_NS, false);
    int crashes = prefs.getInt("dmxcrash", 0);
    prefs.putInt("dmxcrash", crashes + 1);   // committed before the UART is touched
    prefs.end();
    if (crashes >= 4) {
        for (int i = 0; i < MAX_OUTPUTS; i++) cfg.outputs[i].enabled = false;
        Serial.printf("[SAFE] %d DMX-init crashes — all outputs disabled\n", crashes);
        saveConfig();
    } else if (crashes >= 2) {
        for (int i = 1; i < MAX_OUTPUTS; i++) cfg.outputs[i].enabled = false;
        Serial.printf("[SAFE] %d DMX-init crashes — extra outputs disabled, keeping output A\n", crashes);
        saveConfig();
    }
}
static void dmxInitGuardEnd() {
    prefs.begin(PREF_NS, false);
    prefs.putInt("dmxcrash", 0);             // init survived — clear the counter
    prefs.end();
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
// Status LED task
// LED runs independently of loop() so network blocking (serving the web UI)
// never freezes it. Very light (one pixel update every 50 ms) so it doesn't
// compete with loop() for CPU. DMX output stays in loop()/callbacks.
// ---------------------------------------------------------------------------
static void ledTask(void*) {
    const TickType_t period = pdMS_TO_TICKS(50);
    for (;;) {
        uint32_t now = millis();
        if (!netConnected()) {
            setLedColor((now % 1000) < 120 ? NEO_RED : NEO_OFF, (now % 1000) < 120);
        } else if (now - lastDmxMs < 1500) {
            // Hold "active" green through brief input gaps (lost multicast)
            setLedColor(NEO_GREEN, true);
        } else {
            setLedColor((now % 1000) < 500 ? NEO_AMBER : NEO_OFF, (now % 1000) < 500);
        }
        vTaskDelay(period);
    }
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
    setLedColor(NEO_WHITE, true);   // booting
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
      while (!netConnected() && millis() - t < 15000) {
          setLedColor((millis() % 600) < 300 ? NEO_BLUE : NEO_OFF, (millis() % 600) < 300);
          delay(200); Serial.print(".");
      } }
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
    WiFi.mode(WIFI_STA);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    setLedColor(NEO_BLUE, true);   // connecting to stored WiFi
    startWiFiManager(forcePortal);
    // The ESP32's auto-connect reliably sticks to whichever AP it used before,
    // even a distant one on a mesh. Explicitly scan and hop to the strongest
    // AP for our SSID on every boot (also logs all APs for diagnostics).
    connectStrongestAP();
    // Disable WiFi power save: with modem-sleep the station misses buffered
    // multicast (sACN) and IGMP queries, causing periodic ~0.3-0.5s reception
    // gaps. WIFI_PS_NONE keeps the radio awake for reliable multicast.
    WiFi.setSleep(WIFI_PS_NONE);
    Serial.printf("[WiFi] %s / %s  rssi=%d  bssid=%s\n",
        netSSID().c_str(), netLocalIP().toString().c_str(),
        (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str());
#endif

    if (MDNS.begin(cfg.hostname.c_str())) {
        MDNS.addService("http",   "tcp", 80);
        if (cfg.protocol != 1) MDNS.addService("artnet", "udp", 6454);
        if (cfg.protocol != 0) MDNS.addService("e131",   "udp", 5568);
        Serial.printf("[mDNS] %s.local\n", cfg.hostname.c_str());
    }

    dmxInitGuardBegin();   // recover automatically if a bad output config panics init
    initDmx();
    dmxInitGuardEnd();
    initOTA();

    if (cfg.protocol != 1) {
        artnet.setArtDmxCallback(onArtDmx);
        artnet.begin();
        Serial.printf("[ArtNet] out0 universe %d%s\n", cfg.outputs[0].universe,
            cfg.outputs[1].enabled ? " (+out1)" : "");
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
    http.on("/info.json",         HTTP_GET,  handleInfoJson);
    http.on("/rdm.json",          HTTP_GET,  handleRdmJson);
    http.on("/labels.json",       HTTP_GET,  handleLabelsGet);
    http.on("/labels",            HTTP_POST, [](AsyncWebServerRequest*){}, NULL, handleLabelsBody);
    http.on("/autoupdate",        HTTP_POST, handleAutoUpdatePost);
    http.onNotFound([](AsyncWebServerRequest* req) { req->send(404, "text/plain", "Not found"); });

    ws.onEvent(onWsEvent);
    http.addHandler(&ws);
    http.begin();

    lastFrameMs = millis();
    // LED on its own low-priority task so web traffic can't freeze it.
    xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
    xTaskCreate(versionCheckTask, "ver_chk", 12288, nullptr, 1, nullptr);
    Serial.println("[BOOT] ready.");
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
    // AsyncWebServer + AsyncWebSocket run in their own task, so loop() never
    // blocks on the network — only Art-Net/sACN input and DMX output here.
    if (cfg.protocol != 1) artnet.read();
    if (cfg.protocol != 0) readSacn();
    ArduinoOTA.handle();

    uint32_t now = millis();

    // Continuous DMX output at ~40 Hz: holds the last frame as a failsafe so
    // brief input gaps (lost multicast on a weak link) never interrupt the
    // lights. sendDmx() applies any identify override and manual changes too.
    static uint32_t lastDmxTx = 0;
    if (identifyCh && now >= identifyUntil) identifyCh = 0;
    if (now - lastDmxTx >= 25) { sendDmx(); lastDmxTx = now; }

    // RDM discovery / GET-SET on the bus (no-op unless a request is queued and
    // the direction-enable pin is configured). A full discovery briefly pauses
    // DMX output while it sweeps the line.
    rdmService();

    if (now - lastWsPush >= 100) {
        wsPush();
        lastWsPush = now;
    }

    static uint32_t lastMetaPush = 0;
    if (now - lastMetaPush >= 2000) {
        wsPushMeta();
        lastMetaPush = now;
    }

    static uint32_t lastWsClean = 0;
    if (now - lastWsClean >= 1000) {
        ws.cleanupClients(4);   // cap clients; drop oldest beyond 4
        lastWsClean = now;
    }

    if (pendingGithubOta) {
        pendingGithubOta = false;
        doGithubOta();
    }

    // Deferred reboot (config save / reset / OTA done) so the HTTP response
    // can flush from the async task before we restart.
    if (pendingRebootAt && now >= pendingRebootAt) {
#ifndef USE_ETHERNET
        if (pendingWifiReset) { WiFiManager wm; wm.resetSettings(); }
#endif
        ESP.restart();
    }

#ifndef USE_ETHERNET
    // WiFi link watchdog: if the association drops, force a reconnect so the
    // device comes back without waiting/rebooting (DMX output keeps running).
    static uint32_t lastWifiOk = 0;
    static uint32_t lastReconnect = 0;
    if (WiFi.status() == WL_CONNECTED) {
        lastWifiOk = now;
    } else if (now - lastWifiOk > 5000 && now - lastReconnect > 10000) {
        Serial.printf("[WiFi] link down (status=%d), re-scanning for strongest AP... up=%lus\n",
            (int)WiFi.status(), (unsigned long)uptimeSec());
        WiFi.disconnect();
        { wifi_config_t c;   // clear BSSID lock before reconnect too
          if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK)
              { c.sta.bssid_set = 0; esp_wifi_set_config(WIFI_IF_STA, &c); } }
        WiFi.begin();   // re-scans all channels, reconnects to strongest BSSID
        lastReconnect = now;
    }
#endif

    // Periodic health line (leaks/uptime visible on the serial console)
    static uint32_t lastHeapLog = 0;
    if (now - lastHeapLog >= 15000) {
        lastHeapLog = now;
        Serial.printf("[HEALTH] up=%lus heap=%u minFree=%u fps=%.1f rssi=%d st=%d ws=%u req=%u wsc=%u/%u\n",
            (unsigned long)uptimeSec(), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
            fps, netRSSI(), (int)WiFi.status(), ws.count(), (unsigned)httpReqCount,
            (unsigned)wsConnCount, (unsigned)wsDiscCount);
    }
}
