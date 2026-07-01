/*
  ╔═══════════════════════════════════════════════════════════════╗
  ║      BusTracker — WiFi-only GPS Test Firmware (ESP32)         ║
  ║      NEO-6M GPS ONLY — no 4G/A7670C module needed             ║
  ╠═══════════════════════════════════════════════════════════════╣
  ║  WIRING                                                        ║
  ║  NEO-6M  TX  →  ESP32 GPIO 16  (GPS_RX_PIN)                   ║
  ║  NEO-6M  RX  →  ESP32 GPIO 17  (GPS_TX_PIN, optional)         ║
  ║  NEO-6M  VCC →  3.3 V                                         ║
  ║  NEO-6M  GND →  GND                                           ║
  ╠═══════════════════════════════════════════════════════════════╣
  ║  Libraries (Arduino IDE → Library Manager):                    ║
  ║    TinyGPSPlus  by Mikal Hart       (>= 1.0.3)                ║
  ║    ArduinoJson  by Benoit Blanchon  (v6 or v7)                 ║
  ║    (WiFi, WiFiClientSecure, HTTPClient are bundled with the    ║
  ║     ESP32 board package — no separate install needed)          ║
  ╠═══════════════════════════════════════════════════════════════╣
  ║  Board: ESP32 Dev Module  |  Upload Speed: 921600              ║
  ╚═══════════════════════════════════════════════════════════════╝

  PURPOSE
  ──────────────────────────────────────────────────────────────
  This is a stripped-down GPS-only test build. It connects the ESP32
  to your normal WiFi network (same network as your laptop) and POSTs
  real GPS fixes (lat, lon, speed, bus number) straight to the hosted
  backend at https://fms.wisright.com/telemetry — no ngrok needed.

  Data flow:
    NEO-6M GPS --> ESP32 UART2 --> build JSON --> WiFi (HTTPS)
    --> fms.wisright.com/telemetry --> MySQL --> Bus Live Test panel

  Open the dashboard's "🚌 Bus Test" view, enter the BUS_ID below
  in the Bus ID box, and you should see live GPS fixes appear.
*/

#include <TinyGPSPlus.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ─────────────────────────────────────────────────────────────────────────────
//  USER CONFIGURATION  <-- Edit these before uploading
// ─────────────────────────────────────────────────────────────────────────────

// WiFi network — MUST be the same network your laptop is on
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASS     "YOUR_WIFI_PASSWORD"

// Hosted backend — no ngrok needed anymore
#define SERVER_HOST   "fms.wisright.com"
#define SERVER_PATH   "/telemetry"
#define USE_HTTPS     true      // fms.wisright.com serves over HTTPS

// Auth token — must match DEVICE_TOKEN in server .env (see telematics_backend/.env)
#define DEVICE_TOKEN  "YOUR_DEVICE_TOKEN"

// Unique ID shown on the dashboard's Bus Live Test panel — type this
// exact value into the "Bus ID" box on https://fms.wisright.com
#define BUS_ID        "BUS01"

// ─────────────────────────────────────────────────────────────────────────────
//  HARDWARE PINS
// ─────────────────────────────────────────────────────────────────────────────

#define GPS_RX_PIN    16   // ESP32 <- NEO-6M TX
#define GPS_TX_PIN    17   // ESP32 -> NEO-6M RX (optional)

#define GPS_BAUD      9600
#define MONITOR_BAUD  115200

// ─────────────────────────────────────────────────────────────────────────────
//  TIMING
// ─────────────────────────────────────────────────────────────────────────────

#define SEND_INTERVAL_MS      3000UL   // POST every 3 seconds
#define GPS_FIX_TIMEOUT_MS   120000UL  // Wait up to 2 min for first fix
#define WIFI_TIMEOUT_MS       20000UL  // WiFi connect timeout
#define STATUS_PRINT_MS        5000UL

HardwareSerial gpsSerial(2);  // UART2: RX=GPIO16, TX=GPIO17
TinyGPSPlus    gps;

struct GpsSnapshot {
  double   lat;
  double   lon;
  double   speed_kmh;
  double   altitude_m;
  uint32_t satellites;
  double   hdop;
  char     date[11];
  char     utcTime[9];
  bool     valid;
};

static GpsSnapshot snap       = {};
static uint32_t    lastSendMs = 0;
static uint32_t    lastStatusMs = 0;
static uint32_t    sendCount  = 0;
static uint32_t    errCount   = 0;

#define LOG(x)    Serial.print(x)
#define LOGLN(x)  Serial.println(x)
#define LOGF(...) Serial.printf(__VA_ARGS__)
#define DIVIDER() Serial.println(F("--------------------------------------------"))

// ═════════════════════════════════════════════════════════════════════════════
//  GPS FUNCTIONS
// ═════════════════════════════════════════════════════════════════════════════

void gps_feed() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
}

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

    if (!warnedNoBytes && millis() - start > 5000 && gps.charsProcessed() == 0) {
      warnedNoBytes = true;
      LOGLN(F("[GPS] WARNING: No bytes from NEO-6M after 5 s"));
      LOGLN(F("[GPS]          Check: VCC=3.3V, GND, TX->GPIO16 wiring"));
    }

    if (gps.location.isValid() && gps.location.age() < 2000) {
      DIVIDER();
      LOGF("[GPS] *** FIX ACQUIRED in %lu s ***\n", (millis() - start) / 1000);
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

  LOGLN(F("[GPS] *** NO FIX within timeout — continuing, will keep retrying ***"));
  return false;
}

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
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  WIFI
// ═════════════════════════════════════════════════════════════════════════════

bool wifi_connect() {
  DIVIDER();
  LOGF("[WIFI] Connecting to \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(300);
    LOG(F("."));
  }
  LOGLN(F(""));

  if (WiFi.status() != WL_CONNECTED) {
    LOGLN(F("[WIFI] FAILED to connect — check SSID/password"));
    return false;
  }

  LOGF("[WIFI] Connected — IP: %s  RSSI: %d dBm\n",
       WiFi.localIP().toString().c_str(), WiFi.RSSI());
  DIVIDER();
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  JSON PAYLOAD — same shape as the 4G firmware, so it matches the
//  backend's REQUIRED_TELEMETRY fields and the Bus Live Test panel
// ═════════════════════════════════════════════════════════════════════════════

int buildPayload(char* buf, int bufLen) {
  StaticJsonDocument<384> doc;
  doc["dev_id"]     = BUS_ID;
  doc["lat"]        = snap.lat;
  doc["lon"]        = snap.lon;
  doc["speed_kmh"]  = (double)(round(snap.speed_kmh  * 10.0) / 10.0);
  doc["sos_active"] = 0;
  doc["altitude"]   = (double)(round(snap.altitude_m * 10.0) / 10.0);
  doc["satellites"] = (int)snap.satellites;
  doc["hdop"]       = (double)(round(snap.hdop       * 100.0) / 100.0);
  doc["gps_date"]   = snap.date;
  doc["gps_time"]   = snap.utcTime;

  return serializeJson(doc, buf, bufLen);
}

// ═════════════════════════════════════════════════════════════════════════════
//  HTTP POST over WiFi
// ═════════════════════════════════════════════════════════════════════════════

bool http_post(const char* payload) {
  bool ok = false;

  if (USE_HTTPS) {
    WiFiClientSecure client;
    client.setInsecure();   // skip cert validation — fine for a test build

    HTTPClient https;
    String url = String("https://") + SERVER_HOST + SERVER_PATH;
    if (https.begin(client, url)) {
      https.addHeader("Content-Type", "application/json");
      https.addHeader("Token", DEVICE_TOKEN);
      int code = https.POST((uint8_t*)payload, strlen(payload));
      LOGF("[HTTP] POST %s -> HTTP %d\n", url.c_str(), code);
      if (code > 0) {
        String resp = https.getString();
        LOGF("[HTTP] Server reply: %s\n", resp.c_str());
      }
      ok = (code >= 200 && code < 300);
      https.end();
    } else {
      LOGLN(F("[HTTP] Unable to begin HTTPS connection"));
    }
  } else {
    HTTPClient http;
    String url = String("http://") + SERVER_HOST + SERVER_PATH;
    if (http.begin(url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Token", DEVICE_TOKEN);
      int code = http.POST((uint8_t*)payload, strlen(payload));
      LOGF("[HTTP] POST %s -> HTTP %d\n", url.c_str(), code);
      if (code > 0) {
        String resp = http.getString();
        LOGF("[HTTP] Server reply: %s\n", resp.c_str());
      }
      ok = (code >= 200 && code < 300);
      http.end();
    } else {
      LOGLN(F("[HTTP] Unable to begin HTTP connection"));
    }
  }

  return ok;
}

// ═════════════════════════════════════════════════════════════════════════════
//  MAIN SEND CYCLE
// ═════════════════════════════════════════════════════════════════════════════

void telemetry_send() {
  if (!gps_snapshot()) {
    LOGLN(F("[MAIN] No GPS fix — skipping send, waiting for satellites..."));
    return;
  }

  char payloadBuf[384];
  int  payloadLen = buildPayload(payloadBuf, sizeof(payloadBuf));
  if (payloadLen <= 0) {
    LOGLN(F("[MAIN] JSON build failed"));
    return;
  }

  // Print the exact JSON being sent to the Serial Monitor
  DIVIDER();
  LOGF("[JSON] %s\n", payloadBuf);
  DIVIDER();

  if (WiFi.status() != WL_CONNECTED) {
    LOGLN(F("[MAIN] WiFi disconnected — reconnecting..."));
    wifi_connect();
    return;
  }

  if (http_post(payloadBuf)) {
    sendCount++;
    errCount = 0;
    LOGF("[MAIN] Sent OK  (total sent: %lu)\n", sendCount);
  } else {
    errCount++;
    LOGF("[MAIN] Send FAILED  (errors: %lu)\n", errCount);
  }
}

void printLiveStatus() {
  DIVIDER();
  LOGF("[STATUS] Uptime: %lu s | Sends OK: %lu | Errors: %lu | WiFi: %s\n",
       millis() / 1000, sendCount, errCount,
       WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
  if (snap.valid) {
    LOGF("[GPS]    FIX OK  Lat=%.6f  Lon=%.6f  Speed=%.1f km/h  Sats=%lu\n",
         snap.lat, snap.lon, snap.speed_kmh, snap.satellites);
  } else {
    LOGF("[GPS]    NO FIX  chars=%lu sentences=%lu failed=%lu\n",
         gps.charsProcessed(), gps.sentencesWithFix(), gps.failedChecksum());
  }
  DIVIDER();
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP / LOOP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(MONITOR_BAUD);
  delay(600);

  LOGLN(F(""));
  LOGLN(F("============================================"));
  LOGLN(F("  BusTracker WiFi Test -- Powering Up       "));
  LOGLN(F("  NEO-6M GPS only, no 4G module              "));
  LOGLN(F("============================================"));
  LOGF("  Bus ID   : %s\n", BUS_ID);
  LOGF("  Server   : %s://%s%s\n", USE_HTTPS ? "https" : "http", SERVER_HOST, SERVER_PATH);
  LOGLN(F("============================================"));
  LOGLN(F(""));

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  LOGLN(F("[GPS] UART2 started (RX=GPIO16, TX=GPIO17, baud=9600)"));

  if (!wifi_connect()) {
    LOGLN(F("[WIFI] Will keep retrying in the main loop..."));
  }

  LOGLN(F("[GPS] Starting satellite search..."));
  if (!gps_waitForFix(GPS_FIX_TIMEOUT_MS)) {
    LOGLN(F("[GPS] *** No fix yet — check antenna/open-sky view ***"));
    LOGLN(F("[GPS] Continuing — will keep retrying in main loop"));
  }

  DIVIDER();
  LOGLN(F("[MAIN] Setup complete"));
  DIVIDER();
}

void loop() {
  gps_feed();

  if (millis() - lastStatusMs >= STATUS_PRINT_MS) {
    lastStatusMs = millis();
    gps_snapshot();
    printLiveStatus();
  }

  if (WiFi.status() != WL_CONNECTED) {
    wifi_connect();
    delay(500);
    return;
  }

  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    telemetry_send();
  }

  delay(10);
}
