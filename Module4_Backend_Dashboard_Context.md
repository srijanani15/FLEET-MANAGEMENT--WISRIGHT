# Module 4 — Backend & Dashboard
## Project Context File for Claude Code / VS Code

**Owner:** Sri Janani
**Project:** IoT Smart Vehicle Telematics Ecosystem (Fleet Management)
**Module Scope:** Backend API, Database, Live Dashboard

---

## 1. Project Context (Read This First)

This is a vehicle telematics prototype. A vehicle has an ESP32 with a GPS+cellular module (SIM808/A7670G), a speed sensor (LM393 optical encoder), and an SOS push button. The vehicle continuously sends telemetry data over the cellular network to a backend server, which stores it and displays it on a live dashboard for a dispatcher.

**Note:** Fuel sensor has been removed from this project's scope. Do not include `fuel_pct` or any fuel-related logic anywhere.

### How the Full System Works (for context only — not your job to build)
- **Module 1 (Vishali):** Reads speed sensor + SOS button on the ESP32.
- **Module 2 (Sudharshan):** Reads GPS coordinates, sends data/SMS over cellular network.
- **Module 3 (Yuvanesh):** Combines speed, GPS, and SOS data, checks geofence landmarks, builds the final JSON packet, sends it to my backend.
- **Module 4 (Me — Sri Janani):** Everything below. Receives the JSON, stores it, displays it.

---

## 2. My Module's Responsibility

Build a backend server that receives telemetry JSON, stores it in a database, and a dashboard webpage that displays it live — map, speed gauge, SOS alert, and a log of stop events.

I am building this **completely independently**, without waiting for hardware. I will use a mock data generator to simulate the vehicle until the other modules are ready, then swap in real data later.

---

## 3. Locked Data Contract (DO NOT CHANGE FIELD NAMES)

This is the exact JSON format the vehicle will eventually send to my `/telemetry` endpoint:

```json
{
  "dev_id": "VTUESP32-0091",
  "lat": 13.0827,
  "lon": 80.2707,
  "speed_kmh": 64,
  "sos_active": 0
}
```

| Field | Type | Notes |
|---|---|---|
| dev_id | string | Unique device identifier |
| lat | float | Latitude |
| lon | float | Longitude |
| speed_kmh | float/int | Vehicle speed |
| sos_active | int (0 or 1) | Emergency flag |

---

## 4. Required Features

### Backend
1. `POST /telemetry` — receives and validates JSON matching the contract above
2. Reject malformed/missing-field requests with a clear 400 error
3. SQLite database — store every valid record with an auto timestamp
4. `GET /telemetry/latest?dev_id=...` — returns most recent record for a device
5. `GET /telemetry/history?dev_id=...` — returns last 50 records, newest first (used for geofence log + history view)
6. `GET /telemetry/stops?dev_id=...` — returns logged "Stop State" events (location name, timestamp, duration) — these will come from Module 3, stored as part of the telemetry record or a separate table
7. CORS enabled so the dashboard frontend can call these endpoints

### Mock Data Generator
8. A standalone script that POSTs fake JSON packets to my own backend every few seconds, with randomized but realistic lat/lon/speed values, and occasionally sets `sos_active: 1` to test the alert flow

### Dashboard (single page)
9. Live GPS coordinates display (text)
10. Live map showing a pin at the vehicle's current position (Leaflet.js or similar)
11. Speed gauge (visual, updates live)
12. SOS status indicator — flashing red banner when `sos_active = 1`, normal/green otherwise
13. Scrolling log table of geofence "Stop State" events
14. Auto-refresh via polling (every few seconds) — no manual reload needed

---

## 5. Build Order (work through these one at a time, test after each)

1. Backend server setup (Flask + `/telemetry` POST endpoint + validation)
2. SQLite database integration
3. `GET /telemetry/latest` and `GET /telemetry/history` endpoints
4. Mock data generator script
5. Dashboard UI — static layout matching the feature list above
6. Live map integration
7. Speed gauge (dynamic, pulling from latest record)
8. SOS alert banner (dynamic)
9. Geofence "Stop State" log table
10. Polling/auto-refresh logic
11. Full test using only mock data (confirm everything updates correctly, including SOS trigger and stop logging)
12. — STOP HERE until Module 3 is ready —
13. Swap mock data generator for real transmission from Module 3
14. Fix any field-name/type mismatches
15. Final polish/styling pass for demo

---

## 6. Important Constraints

- No fuel sensor / no `fuel_pct` field anywhere in code, database schema, or UI.
- Keep code clean and commented — this needs to be explained in a project report/demo.
- Don't build all features in one shot — build and test incrementally, one feature at a time.
- Use SQLite (not a heavier DB) — this is a student/internship prototype, not production.
- Validation must reject bad data — don't assume every incoming packet is well-formed.

---

## 7. First Task To Ask Claude For

Start with: *"Build me a Flask backend with a POST /telemetry endpoint matching the data contract in section 3 above, with validation and SQLite storage."*

Then proceed through the build order in section 5, one step at a time.
