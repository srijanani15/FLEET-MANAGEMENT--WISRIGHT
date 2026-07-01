/*
  ╔═══════════════════════════════════════════════════════════════╗
  ║          BusTracker — Production ESP32 Firmware              ║
  ║   NEO-6M GPS + SIMCom A7670C 4G LTE + VI Vodafone SIM       ║
  ╠═══════════════════════════════════════════════════════════════╣
  ║  WIRING                                                       ║
  ║  ─────────────────────────────────────────────────────────   ║
  ║  NEO-6M  TX  →  ESP32 GPIO 16  (GPS_RX_PIN)                 ║
  ║  NEO-6M  RX  →  ESP32 GPIO 17  (GPS_TX_PIN, optional)       ║
  ║  NEO-6M  VCC →  3.3 V                                        ║
  ║  NEO-6M  GND →  GND                                          ║
  ║                                                               ║
  ║  A7670C  TX  →  ESP32 GPIO 26  (SIM_RX_PIN)                 ║
  ║  A7670C  RX  →  ESP32 GPIO 27  (SIM_TX_PIN)                 ║
  ║  A7670C  RST →  ESP32 GPIO  4  (SIM_RST_PIN, optional)      ║
  ║  A7670C  VCC →  5 V  (dedicated 2 A supply recommended)      ║
  ║  A7670C  GND →  GND  (shared with ESP32)                     ║
  ║                                                               ║
  ║  SOS Button: GPIO 0 (Boot button) → hold to trigger SOS      ║
  ╠═══════════════════════════════════════════════════════════════╣
  ║  Libraries (Arduino IDE → Library Manager):                   ║
  ║    TinyGPSPlus  by Mikal Hart       (>= 1.0.3)              ║
  ║    ArduinoJson  by Benoit Blanchon  (v6 or v7)               ║
  ╠═══════════════════════════════════════════════════════════════╣
  ║  Board: ESP32 Dev Module  |  Upload Speed: 921600            ║
  ║  CPU: 240 MHz  |  Flash: 4 MB  |  Partition: Default         ║
  ╚═══════════════════════════════════════════════════════════════╝

  NGROK SETUP (one-time):
  ──────────────────────────────────────────────────────────────
  1. Sign up free at https://ngrok.com
  2. Copy your Authtoken from https://dashboard.ngrok.com/auth
  3. Paste it in telematics_backend/.env  →  NGROK_AUTHTOKEN=xxx
  4. Run:  python telematics_backend/app.py
  5. The terminal will print:
       ESP32 endpoint: https://XXXX.ngrok-free.app/telemetry
  6. Copy ONLY the hostname (e.g. "abc123.ngrok-free.app")
     and paste it below as NGROK_HOST.
  7. Free tier: hostname changes on every server restart — repeat step 6.
     Paid tier ($10/mo): reserve a static domain — set once, never change.

  Data flow:
    NEO-6M GPS --> ESP32 UART2 --> build JSON --> UART1 --> A7670C 4G
    --> VI Vodafone LTE --> Internet --> ngrok HTTPS --> Flask /telemetry
    --> MySQL --> Dashboard Leaflet map
*/

#include <TinyGPSPlus.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>

// ─────────────────────────────────────────────────────────────────────────────
//  USER CONFIGURATION  <-- Edit these before uploading
// ─────────────────────────────────────────────────────────────────────────────

// Paste the ngrok hostname here AFTER starting the Flask server.
// Example: "a1b2c3d4.ngrok-free.app"
// HOW TO FIND IT: run  python app.py  and look for the line:
//   ESP32 endpoint: https://<THIS_PART>.ngrok-free.app/telemetry
#define NGROK_HOST    "haphazard-lanky-oxidant.ngrok-free.dev"

// Backend route — must match Flask endpoint
#define SERVER_PATH   "/telemetry"

// Auth token — must match DEVICE_TOKEN in server .env
#define DEVICE_TOKEN  "fleet-secret-2024"

// Unique ID shown on the dashboard for this physical bus
#define BUS_ID        "BUS01"

// VI (Vodafone Idea) India APN (fallbacks tried automatically)
#define SIM_APN    "www.vodafone.net.in"

// Local Wi-Fi AP for the test panel (connect phone/PC to this network)
// Open http://192.168.4.1 in browser after connecting
#define AP_SSID   "BusTracker-Panel"
#define AP_PASS   "bustrack8"       // min 8 chars

// ─────────────────────────────────────────────────────────────────────────────
//  HARDWARE PINS
// ─────────────────────────────────────────────────────────────────────────────

#define GPS_RX_PIN    16   // ESP32 <- NEO-6M TX
#define GPS_TX_PIN    17   // ESP32 -> NEO-6M RX
#define SIM_RX_PIN    26   // ESP32 <- A7670C TX
#define SIM_TX_PIN    27   // ESP32 -> A7670C RX
// SIM_RST_PIN and SOS_BTN_PIN not connected — both disabled

// ─────────────────────────────────────────────────────────────────────────────
//  BAUD RATES
// ─────────────────────────────────────────────────────────────────────────────

#define GPS_BAUD      9600
#define SIM_BAUD      115200
#define MONITOR_BAUD  115200

// ─────────────────────────────────────────────────────────────────────────────
//  TIMING (milliseconds)
// ─────────────────────────────────────────────────────────────────────────────

#define SEND_INTERVAL_MS      5000UL   // POST every 5 seconds
#define GPS_FIX_TIMEOUT_MS   120000UL  // Wait up to 2 min for first fix
#define AT_TIMEOUT_MS          5000UL  // Generic AT command timeout
#define HTTP_TIMEOUT_MS       35000UL  // AT+HTTPACTION wait timeout
#define SIM_REINIT_DELAY_MS   15000UL  // Pause before re-init on failure
#define SOS_AUTO_CLEAR_MS     30000UL  // Auto-clear SOS after 30 s
#define STATUS_PRINT_MS        5000UL  // How often to print live status line

// ─────────────────────────────────────────────────────────────────────────────
//  HARDWARE SERIALS
// ─────────────────────────────────────────────────────────────────────────────

HardwareSerial gpsSerial(2);  // UART2: RX=GPIO16, TX=GPIO17
HardwareSerial simSerial(1);  // UART1: RX=GPIO26, TX=GPIO27

// ─────────────────────────────────────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────────────────────────────────────

TinyGPSPlus gps;
WebServer    apServer(80);

struct GpsSnapshot {
  double   lat;
  double   lon;
  double   speed_kmh;
  double   altitude_m;
  uint32_t satellites;
  double   hdop;
  char     date[11];    // "YYYY-MM-DD\0"
  char     utcTime[9];  // "HH:MM:SS\0"
  bool     valid;
};

static GpsSnapshot snap         = {};
static bool        networkReady = false;
static bool        sosActive    = false;
static uint32_t    lastSendMs   = 0;
static uint32_t    lastSosMs    = 0;
static uint32_t    lastStatusMs = 0;
static uint8_t     errCount     = 0;
static uint32_t    sendCount    = 0;   // total successful POSTs this session

// ─────────────────────────────────────────────────────────────────────────────
//  LOGGING MACROS
// ─────────────────────────────────────────────────────────────────────────────

#define LOG(x)    Serial.print(x)
#define LOGLN(x)  Serial.println(x)
#define LOGF(...) Serial.printf(__VA_ARGS__)

// Print a divider line to make sections easy to spot in Serial Monitor
#define DIVIDER() Serial.println(F("--------------------------------------------"))

// ═════════════════════════════════════════════════════════════════════════════
//  STATUS BANNER — printed to Serial Monitor periodically in loop()
// ═════════════════════════════════════════════════════════════════════════════

void printLiveStatus() {
  DIVIDER();
  LOGF("[STATUS] Uptime: %lu s  |  Sends OK: %lu  |  Errors: %d\n",
       millis() / 1000, sendCount, errCount);

  // Network
  if (networkReady) {
    LOGLN(F("[NET]    4G: CONNECTED  -- A7670C online"));
  } else {
    LOGLN(F("[NET]    4G: OFFLINE    -- waiting for re-init"));
  }

  // GPS
  if (snap.valid) {
    LOGF("[GPS]    FIX OK  Lat=%.6f  Lon=%.6f\n", snap.lat, snap.lon);
    LOGF("[GPS]            Speed=%.1f km/h  Alt=%.1f m  Sats=%lu  HDOP=%.2f\n",
         snap.speed_kmh, snap.altitude_m, snap.satellites, snap.hdop);
    LOGF("[GPS]            Date=%s  Time=%s UTC\n", snap.date, snap.utcTime);
  } else {
    LOGF("[GPS]    NO FIX  |  chars=%lu  sentences=%lu  failed=%lu\n",
         gps.charsProcessed(), gps.sentencesWithFix(), gps.failedChecksum());
    if (gps.charsProcessed() == 0) {
      LOGLN(F("[GPS]    WARNING: 0 bytes from NEO-6M — check wiring on GPIO16"));
    }
  }

  // SOS
  if (sosActive) {
    LOGLN(F("[SOS]    *** SOS ACTIVE *** hold button to maintain"));
  }

  DIVIDER();
}

// ═════════════════════════════════════════════════════════════════════════════
//  GPS FUNCTIONS
// ═════════════════════════════════════════════════════════════════════════════

void gps_feed() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
}

// Block until valid GPS fix or timeout. Prints a progress line every 5 s.
bool gps_waitForFix(uint32_t timeoutMs) {
  DIVIDER();
  LOGLN(F("[GPS] Searching for satellites..."));
  LOGLN(F("[GPS] (open-sky view needed — indoors may take longer)"));
  DIVIDER();

  uint32_t start   = millis();
  uint32_t nextLog = millis();
  bool     warnedNoBytes = false;

  while (millis() - start < timeoutMs) {
    gps_feed();

    // Warn once if NEO-6M is sending nothing at all
    if (!warnedNoBytes && millis() - start > 5000 && gps.charsProcessed() == 0) {
      warnedNoBytes = true;
      LOGLN(F("[GPS] WARNING: No bytes from NEO-6M after 5 s"));
      LOGLN(F("[GPS]          Check: VCC=3.3V, GND, TX->GPIO16 wiring"));
    }

    if (gps.location.isValid() && gps.location.age() < 2000) {
      uint32_t sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
      double   hdop = gps.hdop.isValid()       ? gps.hdop.hdop()        : 99.9;
      DIVIDER();
      LOGF("[GPS] *** FIX ACQUIRED in %lu s ***\n", (millis() - start) / 1000);
      LOGF("[GPS]     Latitude  : %.6f\n", gps.location.lat());
      LOGF("[GPS]     Longitude : %.6f\n", gps.location.lng());
      LOGF("[GPS]     Satellites: %lu\n",  sats);
      LOGF("[GPS]     HDOP      : %.2f\n", hdop);
      DIVIDER();
      return true;
    }

    if (millis() - nextLog >= 5000) {
      nextLog = millis();
      uint32_t sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
      LOGF("[GPS] Searching... %lu s elapsed | sats=%lu | chars=%lu\n",
           (millis() - start) / 1000, sats, gps.charsProcessed());
    }

    delay(50);
  }

  DIVIDER();
  LOGLN(F("[GPS] *** NO FIX within timeout ***"));
  LOGLN(F("[GPS]     Will retry in main loop. Continuing without GPS..."));
  DIVIDER();
  return false;
}

// Snapshot latest fix into snap struct. Returns false if stale or invalid.
bool gps_snapshot() {
  gps_feed();

  if (!gps.location.isValid() || gps.location.age() > 5000) {
    if (snap.valid) {
      // Lost fix that we previously had — warn once
      LOGLN(F("[GPS] Fix lost — location data is stale (>5 s old)"));
    }
    snap.valid = false;
    return false;
  }

  snap.lat        = gps.location.lat();
  snap.lon        = gps.location.lng();
  snap.speed_kmh  = gps.speed.isValid()      ? gps.speed.kmph()       : 0.0;
  snap.altitude_m = gps.altitude.isValid()   ? gps.altitude.meters()  : 0.0;
  snap.satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
  snap.hdop       = gps.hdop.isValid()       ? gps.hdop.hdop()        : 99.9;

  if (gps.date.isValid()) {
    snprintf(snap.date, sizeof(snap.date), "%04d-%02d-%02d",
             gps.date.year(), gps.date.month(), gps.date.day());
  } else {
    strlcpy(snap.date, "0000-00-00", sizeof(snap.date));
  }

  if (gps.time.isValid()) {
    snprintf(snap.utcTime, sizeof(snap.utcTime), "%02d:%02d:%02d",
             gps.time.hour(), gps.time.minute(), gps.time.second());
  } else {
    strlcpy(snap.utcTime, "00:00:00", sizeof(snap.utcTime));
  }

  snap.valid = true;
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  AT COMMAND HELPERS
// ═════════════════════════════════════════════════════════════════════════════

void sim_clearRx() {
  delay(50);
  while (simSerial.available()) simSerial.read();
}

bool sim_sendExpect(const char* cmd, const char* expected,
                    uint32_t timeoutMs = AT_TIMEOUT_MS) {
  sim_clearRx();
  simSerial.print(cmd);
  simSerial.print(F("\r\n"));

  String   resp;
  resp.reserve(256);
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (simSerial.available()) resp += (char)simSerial.read();
    if (resp.indexOf(expected)    != -1) { return true; }
    if (resp.indexOf(F("ERROR"))  != -1) { return false; }
    delay(10);
  }
  return false;
}

String sim_sendCapture(const char* cmd, uint32_t timeoutMs = AT_TIMEOUT_MS) {
  sim_clearRx();
  simSerial.print(cmd);
  simSerial.print(F("\r\n"));

  String   resp;
  resp.reserve(512);
  uint32_t start    = millis();
  uint32_t lastChar = millis();

  while (millis() - start < timeoutMs) {
    while (simSerial.available()) {
      resp    += (char)simSerial.read();
      lastChar = millis();
    }
    bool complete = (resp.indexOf(F("OK"))    != -1 ||
                     resp.indexOf(F("ERROR")) != -1);
    bool silence  = (resp.length() > 0 && millis() - lastChar > 400);
    if (complete || silence) {
      delay(60);
      while (simSerial.available()) resp += (char)simSerial.read();
      break;
    }
    delay(10);
  }
  return resp;
}

void sim_hardReset() {
  // No reset pin connected — just wait for module to self-recover
  LOGLN(F("[SIM] Waiting 5 s for module to recover (no reset pin connected)..."));
  delay(5000);
  sim_clearRx();
}

bool sim_isAlive() {
  sim_clearRx();
  simSerial.print(F("AT\r\n"));
  delay(600);
  String r;
  while (simSerial.available()) r += (char)simSerial.read();
  return r.indexOf(F("OK")) != -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SIM / NETWORK INITIALIZATION
// ═════════════════════════════════════════════════════════════════════════════

bool sim_checkSim() {
  LOGLN(F("[SIM] Checking SIM card..."));
  String r = sim_sendCapture("AT+CPIN?", 10000);
  if (r.indexOf(F("READY"))   != -1) { LOGLN(F("[SIM] SIM card: READY"));                      return true; }
  if (r.indexOf(F("SIM PIN")) != -1) { LOGLN(F("[SIM] SIM card requires PIN — not supported")); return false; }
  LOGLN(F("[SIM] SIM card not detected — check if inserted correctly"));
  return false;
}

void sim_logSignal() {
  String r   = sim_sendCapture("AT+CSQ");
  int    idx = r.indexOf(F("+CSQ:"));
  if (idx != -1) {
    int rssi = r.substring(idx + 5).toInt();
    int dbm  = (rssi < 99) ? (-113 + 2 * rssi) : -999;
    const char* quality = (rssi >= 20) ? "EXCELLENT" :
                          (rssi >= 15) ? "GOOD"      :
                          (rssi >= 10) ? "FAIR"      :
                          (rssi >=  5) ? "POOR"      : "NO SIGNAL";
    LOGF("[SIM] Signal: CSQ=%d  %d dBm  [%s]\n", rssi, dbm, quality);
  }
}

bool sim_waitRegistered(uint32_t timeoutMs = 90000) {
  LOGLN(F("[SIM] Waiting for VI network registration (CS + PS)..."));
  uint32_t start = millis();

  bool csOk = false, psOk = false;

  while (millis() - start < timeoutMs) {
    // CS registration (voice/SMS) — AT+CREG
    if (!csOk) {
      String rc = sim_sendCapture("AT+CREG?");
      if (rc.indexOf(F(",1")) != -1 || rc.indexOf(F(",5")) != -1) {
        LOGLN(F("[SIM] CS registered (voice/SMS ready)"));
        csOk = true;
      }
    }

    // PS registration (LTE data) — try AT+CEREG first (LTE), fallback AT+CGREG (GPRS)
    if (!psOk) {
      String re = sim_sendCapture("AT+CEREG?");
      if (re.indexOf(F(",1")) != -1 || re.indexOf(F(",5")) != -1) {
        LOGLN(F("[SIM] LTE PS registered (data ready)"));
        psOk = true;
      } else {
        String rg = sim_sendCapture("AT+CGREG?");
        if (rg.indexOf(F(",1")) != -1 || rg.indexOf(F(",5")) != -1) {
          LOGLN(F("[SIM] GPRS PS registered (data ready)"));
          psOk = true;
        }
      }
    }

    if (csOk && psOk) return true;

    LOGF("[SIM] CS:%s PS:%s — waiting... %lu s\n",
         csOk ? "OK" : "wait", psOk ? "OK" : "wait",
         (millis() - start) / 1000);
    delay(4000);
  }

  LOGLN(F("[SIM] Registration TIMEOUT"));
  if (!csOk) LOGLN(F("[SIM] CS failed — check SIM inserted, VI coverage, antenna"));
  if (!psOk) LOGLN(F("[SIM] PS failed — data won't work even though calls/SMS may work"));
  // Allow proceed if at least CS registered — data may still come up
  return csOk;
}

bool sim_activateDataContext() {
  // Explicitly attach to packet service before activating PDP context.
  // Required on VI Vodafone — without this AT+CGACT silently fails.
  LOGLN(F("[SIM] Attaching to packet service (AT+CGATT=1)..."));
  if (!sim_sendExpect("AT+CGATT=1", "OK", 15000)) {
    LOGLN(F("[SIM] CGATT=1 failed — retrying once..."));
    delay(3000);
    sim_sendExpect("AT+CGATT=1", "OK", 15000);
  }
  // Verify attach status
  String attachSt = sim_sendCapture("AT+CGATT?");
  LOGF("[SIM] Packet attach status: %s\n", attachSt.c_str());

  const char* apns[] = { SIM_APN, "vodafone", "portalnmms", "internet", nullptr };

  for (int i = 0; apns[i] != nullptr; i++) {
    LOGF("[SIM] Trying APN: %s\n", apns[i]);
    sim_sendExpect("AT+CGACT=0,1", "OK", 10000);
    delay(1000);

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apns[i]);
    if (!sim_sendExpect(cmd, "OK")) continue;
    delay(500);

    if (sim_sendExpect("AT+CGACT=1,1", "OK", 30000)) {
      LOGF("[SIM] Data context ACTIVE — APN: %s\n", apns[i]);
      return true;
    }
    LOGF("[SIM] APN failed: %s\n", apns[i]);
    delay(2000);
  }

  LOGLN(F("[SIM] All APNs failed — no internet connection"));
  return false;
}

void sim_configureSsl() {
  sim_sendExpect("AT+CSSLCFG=\"ignorertctime\",0,1", "OK", 3000);
  sim_sendExpect("AT+CSSLCFG=\"sslversion\",0,4",    "OK", 3000);
  sim_sendExpect("AT+CSSLCFG=\"authmode\",0,0",      "OK", 3000);
}

bool sim_initialize() {
  DIVIDER();
  LOGLN(F("[SIM] Initializing A7670C 4G module..."));
  DIVIDER();

  if (!sim_isAlive()) {
    LOGLN(F("[SIM] Module not responding — attempting hard reset"));
    sim_hardReset();
    if (!sim_isAlive()) {
      LOGLN(F("[SIM] FATAL: Module unresponsive after reset"));
      LOGLN(F("[SIM] Check: 5V power supply, GND shared with ESP32, TX/RX wiring"));
      return false;
    }
  }
  LOGLN(F("[SIM] Module alive"));

  sim_sendExpect("ATE0",      "OK");
  sim_sendExpect("AT+CMEE=2", "OK");

  if (!sim_checkSim())           return false;
  sim_logSignal();
  if (!sim_waitRegistered())     return false;
  sim_logSignal();
  sim_configureSsl();
  if (!sim_activateDataContext()) return false;

  networkReady = true;
  errCount     = 0;
  DIVIDER();
  LOGLN(F("[SIM] A7670C READY -- 4G internet active"));
  LOGF("[SIM] Endpoint: https://%s%s\n", NGROK_HOST, SERVER_PATH);
  DIVIDER();
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  JSON PAYLOAD BUILDER
// ═════════════════════════════════════════════════════════════════════════════

int buildPayload(char* buf, int bufLen, bool sos) {
  StaticJsonDocument<384> doc;
  doc["dev_id"]     = BUS_ID;
  doc["lat"]        = snap.lat;
  doc["lon"]        = snap.lon;
  doc["speed_kmh"]  = (double)(round(snap.speed_kmh  * 10.0) / 10.0);
  doc["sos_active"] = sos ? 1 : 0;
  doc["altitude"]   = (double)(round(snap.altitude_m * 10.0) / 10.0);
  doc["satellites"] = (int)snap.satellites;
  doc["hdop"]       = (double)(round(snap.hdop       * 100.0) / 100.0);
  doc["gps_date"]   = snap.date;
  doc["gps_time"]   = snap.utcTime;

  return serializeJson(doc, buf, bufLen);
}

// ═════════════════════════════════════════════════════════════════════════════
//  HTTP POST
// ═════════════════════════════════════════════════════════════════════════════

bool http_post(const char* payload, int payloadLen) {
  LOGF("[HTTP] Sending %d bytes to https://%s%s\n",
       payloadLen, NGROK_HOST, SERVER_PATH);

  sim_sendExpect("AT+HTTPTERM", "OK", 4000);
  delay(300);

  if (!sim_sendExpect("AT+HTTPINIT", "OK", 6000)) {
    LOGLN(F("[HTTP] HTTPINIT failed")); return false;
  }
  if (!sim_sendExpect("AT+HTTPSSL=1", "OK", 3000)) {
    LOGLN(F("[HTTP] SSL enable warning"));
  }
  if (!sim_sendExpect("AT+HTTPPARA=\"CID\",1", "OK")) {
    sim_sendExpect("AT+HTTPTERM", "OK", 2000); return false;
  }

  char urlCmd[320];
  snprintf(urlCmd, sizeof(urlCmd),
           "AT+HTTPPARA=\"URL\",\"https://%s%s\"", NGROK_HOST, SERVER_PATH);
  if (!sim_sendExpect(urlCmd, "OK", 5000)) {
    LOGLN(F("[HTTP] Set URL failed"));
    sim_sendExpect("AT+HTTPTERM", "OK", 2000); return false;
  }

  if (!sim_sendExpect("AT+HTTPPARA=\"CONTENT\",\"application/json\"", "OK")) {
    sim_sendExpect("AT+HTTPTERM", "OK", 2000); return false;
  }

  // Link SSL profile 0 to HTTP (required for HTTPS on A7670C)
  sim_sendExpect("AT+HTTPPARA=\"SSLCFG\",0", "OK", 3000);

  // Send auth token + ngrok browser-warning bypass header
  char hdrCmd[200];
  snprintf(hdrCmd, sizeof(hdrCmd),
           "AT+HTTPPARA=\"USERDATA\",\"Token: %s\\r\\nngrok-skip-browser-warning: true\"",
           DEVICE_TOKEN);
  sim_sendExpect(hdrCmd, "OK");

  char dataCmd[48];
  snprintf(dataCmd, sizeof(dataCmd), "AT+HTTPDATA=%d,10000", payloadLen);
  sim_clearRx();
  simSerial.print(dataCmd);
  simSerial.print(F("\r\n"));

  String   prompt;
  uint32_t t0 = millis();
  while (millis() - t0 < 7000) {
    while (simSerial.available()) prompt += (char)simSerial.read();
    if (prompt.indexOf(F("DOWNLOAD")) != -1) break;
    delay(20);
  }
  if (prompt.indexOf(F("DOWNLOAD")) == -1) {
    LOGLN(F("[HTTP] No DOWNLOAD prompt from module"));
    sim_sendExpect("AT+HTTPTERM", "OK", 2000); return false;
  }

  simSerial.print(payload);
  delay(700);

  String ack;
  t0 = millis();
  while (millis() - t0 < 4000) {
    while (simSerial.available()) ack += (char)simSerial.read();
    if (ack.indexOf(F("OK")) != -1) break;
    delay(20);
  }

  sim_clearRx();
  simSerial.print(F("AT+HTTPACTION=1\r\n"));

  String   actionResp;
  t0 = millis();
  while (millis() - t0 < HTTP_TIMEOUT_MS) {
    while (simSerial.available()) actionResp += (char)simSerial.read();
    if (actionResp.indexOf(F("+HTTPACTION:")) != -1) break;
    delay(100);
  }

  int httpCode = -1;
  int idx      = actionResp.indexOf(F("+HTTPACTION:"));
  if (idx != -1) {
    String part = actionResp.substring(idx + 12);
    part.trim();
    int c1 = part.indexOf(',');
    if (c1 != -1) {
      int    c2      = part.indexOf(',', c1 + 1);
      String codeStr = (c2 != -1) ? part.substring(c1+1, c2) : part.substring(c1+1);
      codeStr.trim();
      httpCode = codeStr.toInt();
    }
  }

  String serverReply = sim_sendCapture("AT+HTTPREAD", 5000);
  // Strip AT echo lines, print just the body
  int bodyStart = serverReply.indexOf("\r\n\r\n");
  if (bodyStart == -1) bodyStart = serverReply.indexOf("+HTTPREAD:");
  if (bodyStart != -1)
    LOGF("[HTTP] Server reply: %s\n", serverReply.substring(bodyStart).c_str());

  sim_sendExpect("AT+HTTPTERM", "OK", 3000);

  bool success = (httpCode >= 200 && httpCode < 300);
  if (success) {
    sendCount++;
    LOGF("[HTTP] POST OK  HTTP %d  (total sent: %lu)\n", httpCode, sendCount);
  } else {
    LOGF("[HTTP] POST FAILED  HTTP %d\n", httpCode);
    if (httpCode == 401 || httpCode == 403) {
      LOGLN(F("[HTTP] Auth error — check DEVICE_TOKEN matches server .env"));
    } else if (httpCode == 307) {
      LOGLN(F("[HTTP] ngrok redirect — ngrok-skip-browser-warning header may be malformed"));
    } else if (httpCode == -1) {
      LOGLN(F("[HTTP] No response — check NGROK_HOST is correct and server is running"));
    }
  }
  return success;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SOS BUTTON
// ═════════════════════════════════════════════════════════════════════════════

void sos_poll() {
  // SOS button not connected — sosActive stays false
  // (can be set manually in code if needed later)
}

// ═════════════════════════════════════════════════════════════════════════════
//  MAIN SEND CYCLE
// ═════════════════════════════════════════════════════════════════════════════

void telemetry_send() {
  if (!gps_snapshot()) {
    LOGLN(F("[MAIN] No GPS fix — skipping send, waiting for satellites..."));
    gps_waitForFix(30000);
    return;
  }

  sos_poll();

  // Print current coordinates before sending
  LOGF("[MAIN] Sending: Lat=%.6f  Lon=%.6f  Speed=%.1f km/h  Sats=%lu\n",
       snap.lat, snap.lon, snap.speed_kmh, snap.satellites);

  char payloadBuf[512];
  int  payloadLen = buildPayload(payloadBuf, sizeof(payloadBuf), sosActive);
  if (payloadLen <= 0) {
    LOGLN(F("[MAIN] JSON build failed")); return;
  }
  LOGF("[MAIN] Payload (%d bytes): %s\n", payloadLen, payloadBuf);

  if (http_post(payloadBuf, payloadLen)) {
    errCount = 0;
  } else {
    errCount++;
    LOGF("[MAIN] Consecutive errors: %d / 5\n", errCount);
    if (errCount >= 5) {
      DIVIDER();
      LOGLN(F("[MAIN] 5 consecutive failures — network may be lost"));
      LOGLN(F("[MAIN] Reinitializing 4G connection..."));
      DIVIDER();
      networkReady = false;
      errCount     = 0;
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOCAL WI-FI AP TEST PANEL
//  Connect to Wi-Fi: BusTracker-Panel / bustrack8
//  Then open http://192.168.4.1 in any browser
//  Works independently — transmission to ngrok still runs normally
// ═════════════════════════════════════════════════════════════════════════════

static String wp_simSignal() {
  String r   = sim_sendCapture("AT+CSQ", 1000);
  int    idx = r.indexOf("+CSQ:");
  if (idx == -1) return "no response";
  int rssi   = r.substring(idx + 5).toInt();
  if (rssi == 99) return "no signal";
  int dbm    = -113 + 2 * rssi;
  const char* q = (rssi >= 20) ? "Excellent" :
                  (rssi >= 15) ? "Good"      :
                  (rssi >= 10) ? "Fair"      :
                  (rssi >=  5) ? "Poor"      : "Very poor";
  char buf[48];
  snprintf(buf, sizeof(buf), "CSQ %d  (~%d dBm)  %s", rssi, dbm, q);
  return String(buf);
}

static String wp_simOperator() {
  String r  = sim_sendCapture("AT+COPS?", 1500);
  int    q1 = r.indexOf('"');
  int    q2 = r.indexOf('"', q1 + 1);
  if (q1 >= 0 && q2 > q1) return r.substring(q1 + 1, q2);
  return "not registered";
}

static String wp_simPin() {
  String r = sim_sendCapture("AT+CPIN?", 1000);
  if (r.indexOf("READY")        != -1) return "READY";
  if (r.indexOf("SIM PIN")      != -1) return "PIN required";
  if (r.indexOf("NOT INSERTED") != -1) return "no SIM";
  return "unknown";
}

static String htmlEsc(String s) {
  s.replace("&","&amp;"); s.replace("<","&lt;"); s.replace(">","&gt;");
  return s;
}

void wp_handleRoot() {
  const char* page = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BusTracker Panel</title>
<style>
body{font-family:system-ui,Arial;margin:0;background:#0f172a;color:#e2e8f0}
.wrap{max-width:540px;margin:auto;padding:16px}
h1{font-size:20px;margin-bottom:4px}
.sub{color:#64748b;font-size:13px;margin-bottom:16px}
h2{font-size:14px;color:#94a3b8;margin:18px 0 6px}
.card{background:#1e293b;border-radius:12px;padding:14px;margin:10px 0}
input,button{font-size:15px;padding:10px;border-radius:8px;border:none;width:100%;box-sizing:border-box;margin:5px 0}
input{background:#334155;color:#fff}
button{background:#3b82f6;color:#fff;font-weight:600;cursor:pointer}
button.red{background:#ef4444}
.row{display:flex;gap:8px}.row button{margin:0}
pre{background:#0f172a;padding:10px;border-radius:8px;white-space:pre-wrap;font-size:12px;max-height:220px;overflow:auto;margin:6px 0}
.stat{font-size:14px;line-height:2}
.stat b{color:#38bdf8}
.gps-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:6px}
.gbox{background:#0f172a;border-radius:8px;padding:10px;text-align:center}
.gbox .val{font-size:22px;font-weight:700;color:#34d399}
.gbox .lbl{font-size:11px;color:#64748b;margin-top:2px}
.badge{display:inline-block;padding:2px 10px;border-radius:20px;font-size:12px;font-weight:600}
.on{background:#166534;color:#4ade80}.off{background:#7f1d1d;color:#fca5a5}
</style></head><body><div class="wrap">
<h1>BusTracker Panel</h1>
<div class="sub">SIMCom A7670C &middot; VI &middot; Local AP</div>

<h2>GPS Live</h2>
<div class="card" id="gps">Loading…</div>

<h2>SIM Status</h2>
<div class="card stat" id="sim">Loading…</div>

<h2>Send SMS</h2>
<div class="card">
  <input id="snum" placeholder="+9198XXXXXXXX">
  <input id="smsg" placeholder="Message text">
  <button onclick="doSms()">Send SMS</button>
</div>

<h2>Voice Call</h2>
<div class="card">
  <input id="cnum" placeholder="+9198XXXXXXXX">
  <div class="row">
    <button onclick="doCall()">Call</button>
    <button class="red" onclick="doHang()">Hang up</button>
  </div>
</div>

<h2>Inbox</h2>
<div class="card">
  <button onclick="doRead()">Read stored SMS</button>
  <pre id="out">—</pre>
</div>

<script>
function setOut(t){document.getElementById('out').textContent=t}
async function refreshGps(){
  try{let r=await fetch('/gps');document.getElementById('gps').innerHTML=await r.text();}catch(e){}
}
async function refreshSim(){
  try{let r=await fetch('/simstatus');document.getElementById('sim').innerHTML=await r.text();}catch(e){}
}
async function doSms(){
  let n=document.getElementById('snum').value,m=document.getElementById('smsg').value;
  setOut('Sending…');
  let r=await fetch('/sms?num='+encodeURIComponent(n)+'&msg='+encodeURIComponent(m));
  setOut(await r.text());
}
async function doCall(){
  let n=document.getElementById('cnum').value;setOut('Calling '+n+'…');
  let r=await fetch('/call?num='+encodeURIComponent(n));setOut(await r.text());
}
async function doHang(){let r=await fetch('/hangup');setOut(await r.text());}
async function doRead(){setOut('Reading…');let r=await fetch('/readsms');setOut(await r.text());}
refreshGps();refreshSim();
setInterval(refreshGps,3000);
setInterval(refreshSim,8000);
</script>
</div></body></html>)HTML";
  apServer.send(200, "text/html", page);
}

void wp_handleGps() {
  char buf[320];
  if (snap.valid) {
    snprintf(buf, sizeof(buf),
      "<div class='gps-grid'>"
      "<div class='gbox'><div class='val'>%.6f</div><div class='lbl'>Latitude</div></div>"
      "<div class='gbox'><div class='val'>%.6f</div><div class='lbl'>Longitude</div></div>"
      "<div class='gbox'><div class='val'>%.1f</div><div class='lbl'>Speed (km/h)</div></div>"
      "<div class='gbox'><div class='val'>%lu</div><div class='lbl'>Satellites</div></div>"
      "<div class='gbox'><div class='val'>%.1f m</div><div class='lbl'>Altitude</div></div>"
      "<div class='gbox'><div class='val'>%.2f</div><div class='lbl'>HDOP</div></div>"
      "</div><div style='margin-top:8px;font-size:12px;color:#64748b'>%s %s UTC</div>",
      snap.lat, snap.lon, snap.speed_kmh, snap.satellites,
      snap.altitude_m, snap.hdop, snap.date, snap.utcTime);
  } else {
    snprintf(buf, sizeof(buf),
      "<span class='badge off'>NO FIX</span>"
      " &nbsp; chars=%lu &nbsp; failed=%lu &nbsp; (searching…)",
      gps.charsProcessed(), gps.failedChecksum());
  }
  apServer.send(200, "text/html", buf);
}

void wp_handleSimStatus() {
  String sig  = wp_simSignal();
  String oper = wp_simOperator();
  String pin  = wp_simPin();
  String net  = networkReady
    ? "<span class='badge on'>ONLINE</span>"
    : "<span class='badge off'>OFFLINE</span>";
  String html =
    "SIM: <b>" + pin + "</b><br>"
    "Operator: <b>" + oper + "</b><br>"
    "Signal: <b>" + sig + "</b><br>"
    "4G Data: " + net;
  apServer.send(200, "text/html", html);
}

void wp_handleSms() {
  String num = apServer.arg("num");
  String msg = apServer.arg("msg");
  if (num.length() < 5) { apServer.send(200, "text/plain", "Bad number"); return; }
  sim_sendExpect("AT+CMGF=1", "OK", 600);
  sim_sendExpect("AT+CSCS=\"GSM\"", "OK", 600);
  sim_clearRx();
  simSerial.print("AT+CMGS=\"" + num + "\"\r");
  delay(600);
  simSerial.print(msg);
  simSerial.write(26);
  String resp; uint32_t t0 = millis();
  while (millis() - t0 < 9000) {
    while (simSerial.available()) resp += (char)simSerial.read();
    if (resp.indexOf("+CMGS") >= 0 || resp.indexOf("ERROR") >= 0) break;
  }
  bool ok = resp.indexOf("+CMGS") >= 0;
  apServer.send(200, "text/plain", ok ? "SMS sent OK" : "SMS failed:\n" + htmlEsc(resp));
}

void wp_handleCall() {
  String num = apServer.arg("num");
  if (num.length() < 5) { apServer.send(200, "text/plain", "Bad number"); return; }
  String r = sim_sendCapture(("ATD" + num + ";").c_str(), 1500);
  apServer.send(200, "text/plain", "Dialing " + num + "\n" + htmlEsc(r));
}

void wp_handleHangup() {
  String r = sim_sendCapture("AT+CHUP", 1000);
  apServer.send(200, "text/plain", "Hung up\n" + htmlEsc(r));
}

void wp_handleReadSms() {
  sim_sendExpect("AT+CMGF=1", "OK", 600);
  String r = sim_sendCapture("AT+CMGL=\"ALL\"", 5000);
  if (r.length() < 3) r = "(no messages / empty)";
  apServer.send(200, "text/plain", htmlEsc(r));
}

void webpanel_setup() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  apServer.on("/",          wp_handleRoot);
  apServer.on("/gps",       wp_handleGps);
  apServer.on("/simstatus", wp_handleSimStatus);
  apServer.on("/sms",       wp_handleSms);
  apServer.on("/call",      wp_handleCall);
  apServer.on("/hangup",    wp_handleHangup);
  apServer.on("/readsms",   wp_handleReadSms);
  apServer.begin();
  LOGLN(F("[AP] Wi-Fi panel started"));
  LOGF("[AP] Connect to Wi-Fi: %s  password: %s\n", AP_SSID, AP_PASS);
  LOGF("[AP] Then open: http://");
  LOGLN(WiFi.softAPIP());
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(MONITOR_BAUD);
  delay(600);

  LOGLN(F(""));
  LOGLN(F("============================================"));
  LOGLN(F("  BusTracker ESP32 -- Powering Up          "));
  LOGLN(F("  Module: SIMCom A7670C + VI SIM            "));
  LOGLN(F("============================================"));
  LOGF("  Bus ID   : %s\n",   BUS_ID);
  LOGF("  Server   : https://%s%s\n", NGROK_HOST, SERVER_PATH);
  LOGF("  APN      : %s\n",   SIM_APN);
  LOGLN(F("  SOS/RST  : not connected (disabled)       "));
  LOGLN(F("============================================"));
  LOGLN(F(""));

  // Start GPS UART first — GPS runs independently of SIM
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  LOGLN(F("[GPS] UART2 started (RX=GPIO16, TX=GPIO17, baud=9600)"));

  // Start SIM UART
  simSerial.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  LOGLN(F("[SIM] UART1 started (RX=GPIO26, TX=GPIO27, baud=115200)"));
  LOGLN(F("[SIM] Waiting 3 s for module to power up..."));
  delay(3000);

  // Try SIM init up to 3 times, then continue without it
  // GPS and Serial Monitor work even if SIM fails
  bool simOk = false;
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    LOGF("[SIM] Init attempt %d / 3...\n", attempt);
    if (sim_initialize()) { simOk = true; break; }
    if (attempt < 3) {
      LOGLN(F("[SIM] Retrying in 5 s..."));
      delay(5000);
    }
  }

  if (!simOk) {
    DIVIDER();
    LOGLN(F("[SIM] *** A7670C NOT RESPONDING ***"));
    LOGLN(F("[SIM] Check: 5V supply to A7670C, shared GND, TX->GPIO26, RX->GPIO27"));
    LOGLN(F("[SIM] Continuing in GPS-only mode — coordinates will show below"));
    DIVIDER();
  }

  // Acquire first GPS fix — runs regardless of SIM state
  LOGLN(F("[GPS] Starting satellite search..."));
  if (!gps_waitForFix(GPS_FIX_TIMEOUT_MS)) {
    LOGLN(F("[GPS] *** No fix within 2 min — check antenna/open-sky view ***"));
    LOGLN(F("[GPS] Continuing — will keep retrying in main loop"));
  }

  // Start local Wi-Fi AP test panel
  webpanel_setup();

  DIVIDER();
  LOGF("[MAIN] Setup complete | SIM: %s | GPS: searching\n",
       simOk ? "ONLINE" : "OFFLINE");
  DIVIDER();
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════

static uint32_t lastSimRetryMs = 0;
#define SIM_RETRY_INTERVAL_MS  60000UL   // retry SIM every 60 s if offline

void loop() {
  // Always feed GPS — must never be starved
  gps_feed();

  // Serve local AP web panel requests
  apServer.handleClient();

  // Periodic live-status print (GPS + SIM state every 5 s)
  if (millis() - lastStatusMs >= STATUS_PRINT_MS) {
    lastStatusMs = millis();
    gps_snapshot();
    printLiveStatus();
  }

  // If SIM offline, retry every 60 s — GPS still runs normally
  if (!networkReady) {
    if (millis() - lastSimRetryMs >= SIM_RETRY_INTERVAL_MS) {
      lastSimRetryMs = millis();
      LOGLN(F("[SIM] Attempting to reconnect A7670C..."));
      sim_initialize();
    }
    delay(10);
    return;   // skip send — but GPS feed and status print still run above
  }

  // Send telemetry on interval (only when SIM is online)
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    telemetry_send();
  }

  delay(10);
}
