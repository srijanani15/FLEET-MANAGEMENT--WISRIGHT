/*
  ╔═══════════════════════════════════════════════════════════════╗
  ║          BusTracker — Production ESP32 Firmware              ║
  ║   NEO-6M GPS + SIMCom A7670C 4G LTE + Airtel SIM            ║
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
    --> Airtel LTE --> Internet --> ngrok HTTPS --> Flask /telemetry
    --> MySQL --> Dashboard Leaflet map
*/

#include <TinyGPSPlus.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────────────────────────────────────────
//  USER CONFIGURATION  <-- Edit these before uploading
// ─────────────────────────────────────────────────────────────────────────────

// Paste the ngrok hostname here AFTER starting the Flask server.
// Example: "a1b2c3d4.ngrok-free.app"
// HOW TO FIND IT: run  python app.py  and look for the line:
//   ESP32 endpoint: https://<THIS_PART>.ngrok-free.app/telemetry
#define NGROK_HOST    "YOUR-NGROK-ID.ngrok-free.app"

// Backend route — must match Flask endpoint
#define SERVER_PATH   "/telemetry"

// Auth token — must match DEVICE_TOKEN in server .env
#define DEVICE_TOKEN  "fleet-secret-2024"

// Unique ID shown on the dashboard for this physical bus
#define BUS_ID        "BUS01"

// Airtel India APN (fallbacks tried automatically)
#define AIRTEL_APN    "airtelgprs.com"

// ─────────────────────────────────────────────────────────────────────────────
//  HARDWARE PINS
// ─────────────────────────────────────────────────────────────────────────────

#define GPS_RX_PIN    16   // ESP32 <- NEO-6M TX
#define GPS_TX_PIN    17   // ESP32 -> NEO-6M RX
#define SIM_RX_PIN    26   // ESP32 <- A7670C TX
#define SIM_TX_PIN    27   // ESP32 -> A7670C RX
#define SIM_RST_PIN    4   // A7670C RESET (active LOW)
#define SOS_BTN_PIN    0   // Boot button — hold to send SOS (active LOW)

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
  LOGLN(F("[SIM] Hard-resetting A7670C via GPIO4..."));
  pinMode(SIM_RST_PIN, OUTPUT);
  digitalWrite(SIM_RST_PIN, LOW);
  delay(300);
  digitalWrite(SIM_RST_PIN, HIGH);
  LOGLN(F("[SIM] Waiting 7 s for module boot..."));
  delay(7000);
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
  LOGLN(F("[SIM] Waiting for Airtel network registration..."));
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    String r = sim_sendCapture("AT+CREG?");
    if (r.indexOf(F(",1")) != -1) { LOGLN(F("[SIM] Registered (home network)"));  return true; }
    if (r.indexOf(F(",5")) != -1) { LOGLN(F("[SIM] Registered (roaming)"));       return true; }
    LOGF("[SIM] Not registered yet... %lu s\n", (millis() - start) / 1000);
    delay(4000);
  }

  LOGLN(F("[SIM] Network registration TIMEOUT"));
  LOGLN(F("[SIM] Check: SIM inserted? Airtel coverage? Antenna connected?"));
  return false;
}

bool sim_activateDataContext() {
  const char* apns[] = { AIRTEL_APN, "www", "airtel.net.in", "internet", nullptr };

  for (int i = 0; apns[i] != nullptr; i++) {
    LOGF("[SIM] Trying APN: %s\n", apns[i]);
    sim_sendExpect("AT+CGACT=0,1", "OK", 10000);
    delay(800);

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apns[i]);
    if (!sim_sendExpect(cmd, "OK")) continue;

    if (sim_sendExpect("AT+CGACT=1,1", "OK", 25000)) {
      LOGF("[SIM] Data context ACTIVE — APN: %s\n", apns[i]);
      return true;
    }
    LOGF("[SIM] APN failed: %s\n", apns[i]);
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

  char hdrCmd[160];
  snprintf(hdrCmd, sizeof(hdrCmd),
           "AT+HTTPPARA=\"USERDATA\",\"Token: %s\"", DEVICE_TOKEN);
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

  sim_sendCapture("AT+HTTPREAD", 5000);
  sim_sendExpect("AT+HTTPTERM", "OK", 3000);

  bool success = (httpCode >= 200 && httpCode < 300);
  if (success) {
    sendCount++;
    LOGF("[HTTP] POST OK  HTTP %d  (total sent: %lu)\n", httpCode, sendCount);
  } else {
    LOGF("[HTTP] POST FAILED  HTTP %d\n", httpCode);
    if (httpCode == 401 || httpCode == 403) {
      LOGLN(F("[HTTP] Auth error — check DEVICE_TOKEN matches server .env"));
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
  if (digitalRead(SOS_BTN_PIN) == LOW) {
    delay(60);
    if (digitalRead(SOS_BTN_PIN) == LOW && !sosActive) {
      sosActive = true;
      lastSosMs = millis();
      DIVIDER();
      LOGLN(F("[SOS] *** SOS ACTIVATED — emergency flag set ***"));
      LOGLN(F("[SOS]     Will auto-clear in 30 s"));
      DIVIDER();
    }
  }
  if (sosActive && millis() - lastSosMs > SOS_AUTO_CLEAR_MS) {
    sosActive = false;
    LOGLN(F("[SOS] SOS auto-cleared — ARMED / SAFE"));
  }
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
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(MONITOR_BAUD);
  delay(600);

  LOGLN(F(""));
  LOGLN(F("============================================"));
  LOGLN(F("  BusTracker ESP32 -- Powering Up          "));
  LOGLN(F("============================================"));
  LOGF("  Bus ID   : %s\n",   BUS_ID);
  LOGF("  Server   : https://%s%s\n", NGROK_HOST, SERVER_PATH);
  LOGF("  APN      : %s\n",   AIRTEL_APN);
  LOGLN(F("============================================"));
  LOGLN(F(""));

  pinMode(SOS_BTN_PIN, INPUT_PULLUP);

  // Start GPS UART
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  LOGLN(F("[GPS] UART2 started (RX=GPIO16, TX=GPIO17, baud=9600)"));

  // Start SIM UART
  simSerial.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  LOGLN(F("[SIM] UART1 started (RX=GPIO26, TX=GPIO27, baud=115200)"));
  LOGLN(F("[SIM] Waiting 2 s for modules to power up..."));
  delay(2000);

  // Initialize A7670C — retry with hard reset every 3 fails
  uint8_t attempt = 0;
  while (!sim_initialize()) {
    attempt++;
    LOGF("[MAIN] Init attempt %d failed\n", attempt);
    if (attempt % 3 == 0) sim_hardReset();
    LOGF("[MAIN] Retrying in %lu s...\n", SIM_REINIT_DELAY_MS / 1000);
    delay(SIM_REINIT_DELAY_MS);
  }

  // Acquire first GPS fix
  if (!gps_waitForFix(GPS_FIX_TIMEOUT_MS)) {
    LOGLN(F("[GPS] Continuing without fix — will retry every send cycle"));
  }

  DIVIDER();
  LOGLN(F("[MAIN] Setup complete -- entering send loop"));
  LOGLN(F("       Hold GPIO0 (boot button) to trigger SOS"));
  DIVIDER();
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
  // Always feed GPS — must not be starved
  gps_feed();

  // Re-init 4G if network was lost
  if (!networkReady) {
    LOGLN(F("[MAIN] 4G connection lost -- reinitializing..."));
    delay(SIM_REINIT_DELAY_MS);
    sim_initialize();
    return;
  }

  // Poll SOS button
  sos_poll();

  // Periodic live-status print (every STATUS_PRINT_MS)
  if (millis() - lastStatusMs >= STATUS_PRINT_MS) {
    lastStatusMs = millis();
    gps_snapshot();   // refresh snap before printing
    printLiveStatus();
  }

  // Send telemetry on interval
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    telemetry_send();
  }

  delay(10);
}
