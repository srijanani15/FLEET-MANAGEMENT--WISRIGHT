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
  ║    TinyGPSPlus  by Mikal Hart       (≥ 1.0.3)               ║
  ║    ArduinoJson  by Benoit Blanchon  (v6 or v7)               ║
  ╠═══════════════════════════════════════════════════════════════╣
  ║  Board: ESP32 Dev Module  |  Upload Speed: 921600            ║
  ║  CPU: 240 MHz  |  Flash: 4 MB  |  Partition: Default         ║
  ╚═══════════════════════════════════════════════════════════════╝

  Data flow:
    NEO-6M GPS → ESP32 UART2 → build JSON → UART1 → A7670C 4G
    → Airtel LTE → Internet → ngrok HTTPS → Flask /telemetry
    → MySQL → Dashboard Leaflet map
*/

#include <TinyGPSPlus.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────────────────────────────────────────
//  USER CONFIGURATION  ←  Edit these before uploading
// ─────────────────────────────────────────────────────────────────────────────

// Your ngrok host ONLY — no "https://", no "/" at end.
// Find it in the terminal when you run: python app.py
// Example: "a1b2c3d4.ngrok-free.app"
#define NGROK_HOST      "YOUR-NGROK-ID.ngrok-free.app"

// Backend route — must match Flask endpoint
#define SERVER_PATH     "/telemetry"

// Auth token — must match DEVICE_TOKEN in server .env
#define DEVICE_TOKEN    "fleet-secret-2024"

// Unique ID shown on the dashboard for this physical bus
#define BUS_ID          "BUS01"

// Airtel India APN (fallbacks are tried automatically)
#define AIRTEL_APN      "airtelgprs.com"

// ─────────────────────────────────────────────────────────────────────────────
//  HARDWARE PINS
// ─────────────────────────────────────────────────────────────────────────────

#define GPS_RX_PIN      16   // ESP32 ← NEO-6M TX
#define GPS_TX_PIN      17   // ESP32 → NEO-6M RX  (connect even if unused)
#define SIM_RX_PIN      26   // ESP32 ← A7670C TX
#define SIM_TX_PIN      27   // ESP32 → A7670C RX
#define SIM_RST_PIN      4   // A7670C RESET (active LOW, optional)
#define SOS_BTN_PIN      0   // Boot button — hold to send SOS (active LOW)

// ─────────────────────────────────────────────────────────────────────────────
//  BAUD RATES
// ─────────────────────────────────────────────────────────────────────────────

#define GPS_BAUD        9600
#define SIM_BAUD        115200
#define MONITOR_BAUD    115200

// ─────────────────────────────────────────────────────────────────────────────
//  TIMING  (milliseconds)
// ─────────────────────────────────────────────────────────────────────────────

#define SEND_INTERVAL_MS      5000UL   // POST every 5 seconds
#define GPS_FIX_TIMEOUT_MS   120000UL  // Wait up to 2 min for first fix
#define AT_TIMEOUT_MS          5000UL  // Generic AT command response timeout
#define HTTP_TIMEOUT_MS       35000UL  // AT+HTTPACTION wait timeout
#define SIM_REINIT_DELAY_MS   15000UL  // Pause before re-init on failure
#define SOS_AUTO_CLEAR_MS     30000UL  // Auto-clear SOS after 30 s

// ─────────────────────────────────────────────────────────────────────────────
//  HARDWARE SERIALS
// ─────────────────────────────────────────────────────────────────────────────

HardwareSerial gpsSerial(2);    // UART2: GPIO 16 RX, 17 TX
HardwareSerial simSerial(1);    // UART1: GPIO 26 RX, 27 TX

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
  char     date[11];     // "YYYY-MM-DD\0"
  char     utcTime[9];   // "HH:MM:SS\0"
  bool     valid;
};

static GpsSnapshot snap        = {};
static bool        networkReady = false;
static bool        sosActive    = false;
static uint32_t    lastSendMs   = 0;
static uint32_t    lastSosMs    = 0;
static uint8_t     errCount     = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  SERIAL MONITOR LOGGING
// ─────────────────────────────────────────────────────────────────────────────

#define LOG(x)    Serial.print(x)
#define LOGLN(x)  Serial.println(x)
#define LOGF(...) Serial.printf(__VA_ARGS__)

// ═════════════════════════════════════════════════════════════════════════════
//  GPS FUNCTIONS
// ═════════════════════════════════════════════════════════════════════════════

// Feed all bytes currently in the GPS UART buffer into TinyGPSPlus.
// Call this as often as possible — starving it causes sentence drops.
void gps_feed() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
}

// Block until a valid GPS fix is obtained or timeoutMs elapses.
// Prints progress every 5 seconds. Returns true on success.
bool gps_waitForFix(uint32_t timeoutMs) {
  LOGLN(F("[GPS] Waiting for satellite fix..."));
  uint32_t start   = millis();
  uint32_t nextLog = millis();

  while (millis() - start < timeoutMs) {
    gps_feed();

    if (gps.location.isValid() && gps.location.age() < 2000) {
      uint32_t sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
      double   hdop = gps.hdop.isValid()       ? gps.hdop.hdop()        : 99.9;
      LOGF("[GPS] Fix in %lu s — Sats=%lu HDOP=%.2f\n",
           (millis() - start) / 1000, sats, hdop);
      return true;
    }

    if (millis() - nextLog >= 5000) {
      nextLog = millis();
      LOGF("[GPS] Searching... %lu s | chars=%lu sents=%lu failed=%lu\n",
           (millis() - start) / 1000,
           gps.charsProcessed(),
           gps.sentencesWithFix(),
           gps.failedChecksum());
    }

    delay(50);
  }

  LOGLN(F("[GPS] Timeout — no fix yet"));
  return false;
}

// Snapshot the latest GPS fix into the global 'snap' struct.
// Returns false if data is not valid or stale (>5 s old).
bool gps_snapshot() {
  gps_feed();

  if (!gps.location.isValid() || gps.location.age() > 5000) {
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

  LOGF("[GPS] Lat=%.6f Lon=%.6f Spd=%.1f Alt=%.1f Sats=%lu HDOP=%.2f %s %s\n",
       snap.lat, snap.lon, snap.speed_kmh, snap.altitude_m,
       snap.satellites, snap.hdop, snap.date, snap.utcTime);

  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  AT COMMAND HELPERS
// ═════════════════════════════════════════════════════════════════════════════

// Drain pending bytes from the A7670C UART buffer.
void sim_clearRx() {
  delay(50);
  while (simSerial.available()) simSerial.read();
}

// Send an AT command and wait until 'expected' appears in the response,
// or until timeoutMs elapses. Returns true on match.
bool sim_sendExpect(const char* cmd, const char* expected,
                    uint32_t timeoutMs = AT_TIMEOUT_MS) {
  sim_clearRx();
  simSerial.print(cmd);
  simSerial.print(F("\r\n"));
  LOG(F("[AT>>] ")); LOGLN(cmd);

  String   resp;
  resp.reserve(256);
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (simSerial.available()) resp += (char)simSerial.read();

    if (resp.indexOf(expected) != -1) {
      LOG(F("[AT<<] ")); LOGLN(resp);
      return true;
    }
    if (resp.indexOf(F("ERROR")) != -1) {
      LOG(F("[AT<<] ERROR: ")); LOGLN(resp);
      return false;
    }
    delay(10);
  }

  LOG(F("[AT<<] TIMEOUT, got: ")); LOGLN(resp);
  return false;
}

// Send an AT command and return the complete response string.
// Considers response complete when "OK"/"ERROR" is seen, or 400 ms of silence.
String sim_sendCapture(const char* cmd, uint32_t timeoutMs = AT_TIMEOUT_MS) {
  sim_clearRx();
  simSerial.print(cmd);
  simSerial.print(F("\r\n"));
  LOG(F("[AT>>] ")); LOGLN(cmd);

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

  LOG(F("[AT<<] ")); LOGLN(resp);
  return resp;
}

// Assert RST pin LOW for 300 ms then release — hard-resets the A7670C.
void sim_hardReset() {
  LOGLN(F("[SIM] Hard reset via GPIO 4..."));
  pinMode(SIM_RST_PIN, OUTPUT);
  digitalWrite(SIM_RST_PIN, LOW);
  delay(300);
  digitalWrite(SIM_RST_PIN, HIGH);
  delay(7000);   // A7670C needs ~5–7 s to boot
  sim_clearRx();
}

// Probe with bare AT — returns true if the module answers "OK".
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

// Verify SIM card is inserted and the PIN is not required.
bool sim_checkSim() {
  LOGLN(F("[SIM] Checking SIM card..."));
  String r = sim_sendCapture("AT+CPIN?", 10000);

  if (r.indexOf(F("READY")) != -1) {
    LOGLN(F("[SIM] SIM READY")); return true;
  }
  if (r.indexOf(F("SIM PIN")) != -1) {
    LOGLN(F("[SIM] SIM PIN required! Send: AT+CPIN=<YOUR_PIN>")); return false;
  }
  LOGLN(F("[SIM] SIM not detected or unknown state")); return false;
}

// Log current RSSI/signal quality to Serial.
void sim_logSignal() {
  String r   = sim_sendCapture("AT+CSQ");
  int    idx = r.indexOf(F("+CSQ:"));
  if (idx != -1) {
    int rssi = r.substring(idx + 5).toInt();
    LOGF("[SIM] Signal: CSQ=%d  (~%d dBm)\n",
         rssi, (rssi < 99) ? (-113 + 2 * rssi) : -999);
  }
}

// Wait until the module reports network registration (home=1 or roaming=5).
bool sim_waitRegistered(uint32_t timeoutMs = 90000) {
  LOGLN(F("[SIM] Waiting for network registration..."));
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    String r = sim_sendCapture("AT+CREG?");
    // Response: +CREG: <n>,<stat> — stat 1=home, 5=roaming
    if (r.indexOf(F(",1")) != -1 || r.indexOf(F(",5")) != -1) {
      LOGLN(F("[SIM] Registered")); return true;
    }
    LOGF("[SIM] Not registered (%lu s)\n", (millis()-start)/1000);
    delay(4000);
  }

  LOGLN(F("[SIM] Registration timeout")); return false;
}

// Configure Airtel APN and activate PDP data context.
// Tries fallback APNs automatically if the primary fails.
bool sim_activateDataContext() {
  const char* apns[] = { AIRTEL_APN, "www", "airtel.net.in", "internet", nullptr };

  for (int i = 0; apns[i] != nullptr; i++) {
    LOGF("[SIM] Trying APN: %s\n", apns[i]);

    // Deactivate existing context before reconfiguring
    sim_sendExpect("AT+CGACT=0,1", "OK", 10000);
    delay(800);

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apns[i]);
    if (!sim_sendExpect(cmd, "OK")) continue;

    if (sim_sendExpect("AT+CGACT=1,1", "OK", 25000)) {
      LOGF("[SIM] Data context active — APN: %s\n", apns[i]);
      return true;
    }
  }

  LOGLN(F("[SIM] All APNs failed")); return false;
}

// Configure TLS/SSL for the HTTPS connection to ngrok.
void sim_configureSsl() {
  // Ignore certificate time validation (ESP32 has no RTC so time may be wrong)
  sim_sendExpect("AT+CSSLCFG=\"ignorertctime\",0,1", "OK", 3000);
  // Accept TLS 1.1 and TLS 1.2
  sim_sendExpect("AT+CSSLCFG=\"sslversion\",0,4",    "OK", 3000);
  // Do NOT verify server certificate (ngrok certs are valid, but skip for robustness)
  sim_sendExpect("AT+CSSLCFG=\"authmode\",0,0",      "OK", 3000);
}

// Complete initialization: AT check → SIM → network → data context.
bool sim_initialize() {
  LOGLN(F("[SIM] ═══ Initializing A7670C ═══"));

  // Ensure module is alive
  if (!sim_isAlive()) {
    LOGLN(F("[SIM] Not responding, hard-resetting..."));
    sim_hardReset();
    if (!sim_isAlive()) {
      LOGLN(F("[SIM] Module unresponsive after reset"));
      return false;
    }
  }

  sim_sendExpect("ATE0",      "OK");   // Disable echo
  sim_sendExpect("AT+CMEE=2", "OK");   // Verbose error reports

  if (!sim_checkSim())          return false;
  sim_logSignal();
  if (!sim_waitRegistered())    return false;
  sim_logSignal();
  sim_configureSsl();
  if (!sim_activateDataContext()) return false;

  networkReady = true;
  errCount     = 0;
  LOGLN(F("[SIM] ═══ A7670C ready ═══"));
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  JSON PAYLOAD BUILDER
// ═════════════════════════════════════════════════════════════════════════════

// Build the JSON string from the latest GPS snapshot.
// Fields match the Flask /telemetry endpoint exactly.
// Returns byte-count written into buf, or 0 on failure.
int buildPayload(char* buf, int bufLen, bool sos) {
  // ArduinoJson v6 — if using v7 replace with: JsonDocument doc;
  StaticJsonDocument<384> doc;

  // Core fields — required by Flask /telemetry
  doc["dev_id"]     = BUS_ID;
  doc["lat"]        = snap.lat;
  doc["lon"]        = snap.lon;
  doc["speed_kmh"]  = (double)(round(snap.speed_kmh * 10.0) / 10.0);
  doc["sos_active"] = sos ? 1 : 0;

  // Extended fields — stored by the updated backend
  doc["altitude"]   = (double)(round(snap.altitude_m * 10.0) / 10.0);
  doc["satellites"] = (int)snap.satellites;
  doc["hdop"]       = (double)(round(snap.hdop * 100.0) / 100.0);
  doc["gps_date"]   = snap.date;
  doc["gps_time"]   = snap.utcTime;

  int len = serializeJson(doc, buf, bufLen);
  LOGF("[JSON] %d bytes: %s\n", len, buf);
  return len;
}

// ═════════════════════════════════════════════════════════════════════════════
//  HTTP POST
// ═════════════════════════════════════════════════════════════════════════════

// Execute one HTTPS POST to the Flask backend via A7670C AT commands.
// Returns true when the server responds with HTTP 2xx.
bool http_post(const char* payload, int payloadLen) {
  LOGLN(F("[HTTP] Starting HTTPS POST..."));

  // Clean up any previous HTTP session
  sim_sendExpect("AT+HTTPTERM", "OK", 4000);
  delay(300);

  // ── 1. Initialize HTTP stack ──────────────────────────────────────────────
  if (!sim_sendExpect("AT+HTTPINIT", "OK", 6000)) {
    LOGLN(F("[HTTP] HTTPINIT failed")); return false;
  }

  // ── 2. Enable SSL for https:// ────────────────────────────────────────────
  if (!sim_sendExpect("AT+HTTPSSL=1", "OK", 3000)) {
    LOGLN(F("[HTTP] SSL enable warning — continuing"));
  }

  // ── 3. Set PDP context ID ─────────────────────────────────────────────────
  if (!sim_sendExpect("AT+HTTPPARA=\"CID\",1", "OK")) {
    sim_sendExpect("AT+HTTPTERM", "OK", 2000);
    return false;
  }

  // ── 4. Set URL (https://ngrok-host/path) ─────────────────────────────────
  char urlCmd[320];
  snprintf(urlCmd, sizeof(urlCmd),
           "AT+HTTPPARA=\"URL\",\"https://%s%s\"", NGROK_HOST, SERVER_PATH);
  if (!sim_sendExpect(urlCmd, "OK", 5000)) {
    LOGLN(F("[HTTP] Set URL failed"));
    sim_sendExpect("AT+HTTPTERM", "OK", 2000);
    return false;
  }

  // ── 5. Content-Type header ────────────────────────────────────────────────
  if (!sim_sendExpect("AT+HTTPPARA=\"CONTENT\",\"application/json\"", "OK")) {
    sim_sendExpect("AT+HTTPTERM", "OK", 2000);
    return false;
  }

  // ── 6. Auth token header (matches Flask DEVICE_TOKEN check) ──────────────
  char hdrCmd[160];
  snprintf(hdrCmd, sizeof(hdrCmd),
           "AT+HTTPPARA=\"USERDATA\",\"Token: %s\"", DEVICE_TOKEN);
  sim_sendExpect(hdrCmd, "OK");   // Non-fatal if unsupported by firmware

  // ── 7. Upload payload ─────────────────────────────────────────────────────
  char dataCmd[48];
  snprintf(dataCmd, sizeof(dataCmd), "AT+HTTPDATA=%d,10000", payloadLen);

  sim_clearRx();
  simSerial.print(dataCmd);
  simSerial.print(F("\r\n"));
  LOG(F("[AT>>] ")); LOGLN(dataCmd);

  // Wait for module to prompt "DOWNLOAD"
  String   prompt;
  uint32_t t0 = millis();
  while (millis() - t0 < 7000) {
    while (simSerial.available()) prompt += (char)simSerial.read();
    if (prompt.indexOf(F("DOWNLOAD")) != -1) break;
    delay(20);
  }
  if (prompt.indexOf(F("DOWNLOAD")) == -1) {
    LOGLN(F("[HTTP] No DOWNLOAD prompt")); sim_sendExpect("AT+HTTPTERM", "OK", 2000); return false;
  }

  // Write JSON bytes
  simSerial.print(payload);
  LOGF("[AT>>] (payload %d bytes sent)\n", payloadLen);
  delay(700);

  // Drain confirmation ("OK" after data accepted)
  String ackResp;
  t0 = millis();
  while (millis() - t0 < 4000) {
    while (simSerial.available()) ackResp += (char)simSerial.read();
    if (ackResp.indexOf(F("OK")) != -1) break;
    delay(20);
  }

  // ── 8. Execute POST ───────────────────────────────────────────────────────
  sim_clearRx();
  simSerial.print(F("AT+HTTPACTION=1\r\n"));
  LOGLN(F("[AT>>] AT+HTTPACTION=1"));

  // Wait for +HTTPACTION: 1,<code>,<body_len>
  String   actionResp;
  t0 = millis();
  while (millis() - t0 < HTTP_TIMEOUT_MS) {
    while (simSerial.available()) actionResp += (char)simSerial.read();
    if (actionResp.indexOf(F("+HTTPACTION:")) != -1) break;
    delay(100);
  }
  LOG(F("[AT<<] ")); LOGLN(actionResp);

  // ── 9. Parse HTTP status code ─────────────────────────────────────────────
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
  LOGF("[HTTP] Status code: %d\n", httpCode);

  // Read response body (for debugging)
  String body = sim_sendCapture("AT+HTTPREAD", 5000);
  LOGF("[HTTP] Response body: %s\n", body.c_str());

  // Always terminate the session
  sim_sendExpect("AT+HTTPTERM", "OK", 3000);

  bool success = (httpCode >= 200 && httpCode < 300);
  LOGLN(success ? F("[HTTP] POST success") : F("[HTTP] POST failed"));
  return success;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SOS BUTTON
// ═════════════════════════════════════════════════════════════════════════════

// Poll GPIO 0 (Boot button, active LOW).
// Sets sosActive = true on press; auto-clears after SOS_AUTO_CLEAR_MS.
void sos_poll() {
  if (digitalRead(SOS_BTN_PIN) == LOW) {
    delay(60);   // debounce
    if (digitalRead(SOS_BTN_PIN) == LOW && !sosActive) {
      sosActive = true;
      lastSosMs = millis();
      LOGLN(F("[SOS] *** SOS ACTIVATED — will clear in 30 s ***"));
    }
  }
  if (sosActive && millis() - lastSosMs > SOS_AUTO_CLEAR_MS) {
    sosActive = false;
    LOGLN(F("[SOS] SOS auto-cleared"));
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  MAIN SEND CYCLE
// ═════════════════════════════════════════════════════════════════════════════

// One complete telemetry cycle: snapshot GPS → build JSON → HTTP POST.
// On 5 consecutive failures, marks network as not-ready to trigger re-init.
void telemetry_send() {
  if (!gps_snapshot()) {
    LOGLN(F("[MAIN] GPS not valid — waiting 30 s for fix..."));
    gps_waitForFix(30000);
    return;
  }

  sos_poll();

  char payloadBuf[512];
  int  payloadLen = buildPayload(payloadBuf, sizeof(payloadBuf), sosActive);
  if (payloadLen <= 0) {
    LOGLN(F("[MAIN] JSON serialization failed")); return;
  }

  if (http_post(payloadBuf, payloadLen)) {
    errCount = 0;
  } else {
    errCount++;
    LOGF("[MAIN] Consecutive errors: %d/5\n", errCount);
    if (errCount >= 5) {
      LOGLN(F("[MAIN] 5 errors — scheduling SIM re-init"));
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
  LOGLN(F("╔══════════════════════════════════════════╗"));
  LOGLN(F("║  BusTracker ESP32 — Booting             ║"));
  LOGLN(F("╚══════════════════════════════════════════╝"));
  LOGF("  Bus ID  : %s\n",   BUS_ID);
  LOGF("  Host    : %s\n",   NGROK_HOST);
  LOGF("  Path    : %s\n",   SERVER_PATH);
  LOGF("  APN     : %s\n\n", AIRTEL_APN);

  // SOS button setup (GPIO0 is pulled up on ESP32 dev boards)
  pinMode(SOS_BTN_PIN, INPUT_PULLUP);

  // NEO-6M on UART2
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  LOGLN(F("[GPS] UART2 started  (RX=GPIO16, TX=GPIO17)"));

  // A7670C on UART1
  simSerial.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  LOGLN(F("[SIM] UART1 started  (RX=GPIO26, TX=GPIO27)"));

  delay(2000);   // Allow both modules to complete power-on sequence

  // Initialize A7670C — retry with hard reset on repeated failure
  uint8_t attempt = 0;
  while (!sim_initialize()) {
    attempt++;
    LOGF("[MAIN] Init attempt %d failed — retry in %lu s\n",
         attempt, SIM_REINIT_DELAY_MS / 1000);
    if (attempt % 3 == 0) {
      LOGLN(F("[MAIN] Forcing hard reset..."));
      sim_hardReset();
    }
    delay(SIM_REINIT_DELAY_MS);
  }

  // Acquire first GPS fix
  if (!gps_waitForFix(GPS_FIX_TIMEOUT_MS)) {
    LOGLN(F("[GPS] No fix during setup — continuing, will retry in loop"));
  }

  LOGLN(F("[MAIN] Setup complete — entering main loop"));
  LOGLN(F("       Press GPIO0 boot button to trigger SOS"));
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
  // Feed GPS bytes continuously — this is the highest-priority task
  gps_feed();

  // Re-initialize SIM if network was lost (set by telemetry_send on 5 errors)
  if (!networkReady) {
    LOGLN(F("[MAIN] Network lost — reinitializing..."));
    delay(SIM_REINIT_DELAY_MS);
    sim_initialize();
    return;
  }

  // Poll SOS button
  sos_poll();

  // Send telemetry on interval
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    telemetry_send();
  }

  delay(10);
}
