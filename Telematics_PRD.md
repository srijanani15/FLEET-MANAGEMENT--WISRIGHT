# Product Requirements Document
## IoT Smart Vehicle Telematics & Tracking System

*GNSS Positioning · Geofenced Stop Detection · Axle-Based Speed Sensing · Cellular SOS Override*

| Field | Value |
|---|---|
| Document Type | Product Requirements Document (PRD) |
| Project | IoT Smart Vehicle Telematics Ecosystem |
| Version | 1.0 (Fuel Monitoring Module Removed) |
| Device ID Reference | VTUESP32-0091 |
| Prepared For | Internship / Capstone Engineering Project |

---

## Table of Contents
1. [Introduction & Document Purpose](#1-introduction--document-purpose)
2. [Project Overview](#2-project-overview)
3. [Objectives](#3-objectives)
4. [Scope](#4-scope)
5. [System Architecture Overview](#5-system-architecture-overview)
6. [Hardware Components](#6-hardware-components)
7. [Functional Requirements](#7-functional-requirements)
8. [Non-Functional Requirements](#8-non-functional-requirements)
9. [Software Architecture (Detailed)](#9-software-architecture-detailed)
10. [Dashboard Reference Layout](#10-dashboard-reference-layout)
11. [Success Metrics / KPIs](#11-success-metrics--kpis)
12. [Assumptions & Risks](#12-assumptions--risks)
13. [Future Scope](#13-future-scope-not-in-current-build)

---

## 1. Introduction & Document Purpose

This Product Requirements Document (PRD) defines the functional, technical, and software-level requirements for the IoT Smart Vehicle Telematics Ecosystem — an embedded tracking platform built around an ESP32 microcontroller. The system reports live vehicle location, detects arrival at predefined geofenced landmarks, calculates real-time mechanical speed from axle rotation, and provides a manual emergency SOS alert channel over cellular SMS.

This revision **removes the fuel level monitoring module** (resistive float sensor, fuel slosh stabilization, and the `fuel_pct` telemetry field) from the original concept blueprint. All references to fuel sensing have been excluded from scope, architecture, data schema, and dashboard layout. The document instead expands the software-side requirements — firmware logic, data structuring, communication protocol, backend processing, and dashboard behavior — to a level suitable for direct implementation by an engineering or internship team.

### 1.1 Intended Audience
- Embedded firmware engineers implementing ESP32 logic
- Backend / cloud engineers building the ingestion and storage layer
- Frontend engineers building the fleet dashboard
- Internship mentors and reviewers evaluating project scope and deliverables

### 1.2 Reference Document
This PRD is derived from and supersedes the original concept manual "Vehicle Telematics & Tracking Blueprint," with the fuel monitoring subsystem removed per project requirements.

---

## 2. Project Overview

The system is an edge-to-cloud telematics pipeline. An ESP32 acts as the on-vehicle compute core, reading two physical sensor inputs (GNSS/cellular module and an optical speed encoder) plus one digital input (SOS push button). It computes derived metrics locally, packages them as JSON, and transmits them over a cellular uplink to a cloud gateway, which stores the data and renders it on a live dispatcher dashboard.

### 2.1 Core Capabilities
- Continuous GNSS-based latitude/longitude acquisition
- Geofence bounding-box evaluation for landmark/stop detection and dwell-time logging
- Real-time vehicle speed calculation from optical axle pulse counting
- Manual SOS interrupt with priority SMS dispatch, independent of the main telemetry loop
- Structured JSON telemetry transmission with bandwidth-aware deduplication
- Cloud-side ingestion, storage, and live dashboard visualization

---

## 3. Objectives

1. Provide dispatchers with real-time vehicle position and motion status.
2. Automatically detect and log arrival/departure at predefined operational landmarks (depots, fuel stations, logistics hubs) without manual driver input.
3. Calculate vehicle speed independently of satellite signal quality, using a hardware-based axle encoder.
4. Guarantee that emergency alerts reach dispatch within a bounded time, even if the main telemetry loop is busy or blocked.
5. Minimize cellular data cost through intelligent transmission-frequency throttling.
6. Present all of the above through a single, real-time operational dashboard.

---

## 4. Scope

### 4.1 In Scope
- GNSS location acquisition and parsing
- Geofencing logic and stop/landmark event logging
- Optical encoder-based speed measurement
- SOS hardware interrupt and SMS alerting
- JSON telemetry packet structuring and transmission
- Edge-side data deduplication / adaptive transmission rate
- Cloud ingestion API, data store, and live dashboard

### 4.2 Out of Scope
- **Fuel level sensing, fuel slosh stabilization, and any fuel-related telemetry field or dashboard widget (explicitly removed in this revision)**
- Driver identification / biometric authentication
- Predictive maintenance or engine diagnostics (OBD-II) integration
- Route optimization or turn-by-turn navigation
- Native mobile applications (web dashboard only, in this phase)

---

## 5. System Architecture Overview

The architecture follows a linear edge-to-cloud pipeline: physical sensors feed the ESP32 core, which aggregates and structures data, hands it to the cellular modem, and the modem uplinks to a cloud gateway for storage and visualization.

### 5.1 Hardware Data Path

```
Sensors / Digital Pins  →  ESP32 Core (Data Aggregator)
         (pulses, NMEA strings, button interrupt)
ESP32 Core  →  Cellular/GNSS Module (Modem Transceiver)
Cellular/GNSS Module  →  Cellular Tower (Wireless Uplink)
Cellular Tower  →  Cloud Gateway  →  Database  →  Dashboard
```

### 5.2 Software Execution Sequence

1. Boot hardware, initialize UART/I-O pins and interrupt vectors.
2. Parse incoming GNSS/NMEA coordinate strings.
3. Calculate instantaneous speed from encoder pulse count.
4. Evaluate current coordinates against stored geofence bounding boxes.
5. Construct and stream the JSON telemetry packet (or keep-alive pulse if deduplicated).

The SOS button operates outside this sequential loop via a hardware interrupt, described in Section 7.4.

---

## 6. Hardware Components

The fuel level sensor and associated wiring have been removed from the bill of materials. The active component set is:

| Component | Recommended Model | Purpose |
|---|---|---|
| Main Processing Core | ESP32 WROOM-32D | Central compute hub: reads pins, runs calculations, manages timing, packages telemetry for transmission. |
| Cellular & GNSS Unit | SIM808 (2G) or A7670G (4G LTE) | Combined GPS receiver and GSM modem; supplies coordinate vectors and transmits data / SOS SMS alerts. |
| Speed Sensor Module | LM393 Optical Encoder Pickup | Tracks axle rotation directly to compute linear vehicle speed, independent of satellite signal. |
| Emergency Interface | Momentary Push Button | Manual emergency trigger; forces an immediate interrupt-driven priority alert. |

---

## 7. Functional Requirements

### 7.1 GNSS Location Tracking

**FR-1:** The system shall continuously acquire latitude/longitude from the GNSS receiver by parsing NMEA sentence rows (e.g., GPRMC/GPGGA) delivered over UART from the cellular/GNSS module.

- Firmware shall validate the NMEA checksum before accepting a coordinate fix.
- Firmware shall discard fixes with an invalid or "no-fix" status flag and retain the last known good coordinate for continuity.
- Coordinate parsing shall convert NMEA degree-minute format into standard decimal latitude/longitude before use elsewhere in the firmware.

### 7.2 Geofenced Landmark / Stop Detection

**FR-2:** The system shall maintain a local table of named geofence bounding boxes (e.g., Warehouse Alpha, Fuel Station #09, Logistics Highway Hub), each defined by a center coordinate and radius.

- On each location update, firmware shall check the current coordinate against every stored geofence using a distance calculation (haversine or equirectangular approximation suitable for short ranges).
- A "Stop State" shall be logged when the vehicle is inside a geofence **and** calculated speed remains at zero for a configurable continuous duration (default: 60 seconds).
- Each Stop State event shall record: landmark name, entry timestamp, exit timestamp, and total dwell duration.
- Stop State entry and exit shall each generate one telemetry event sent to the cloud, rather than continuous repeated logging while stationary.

### 7.3 Speed Sensing & Calculation

**FR-3:** The system shall calculate real-time vehicle speed using pulses generated by the optical encoder as the vehicle's axle rotates, independent of GNSS signal availability.

- Firmware shall count pulses using a hardware timer/interrupt over a fixed sampling window (e.g., 200–500 ms).
- Speed (km/h) shall be derived as: `pulses_per_window × wheel_circumference × (3600 / window_seconds)`, converted to consistent units.
- A configurable smoothing factor (simple moving average over the last N samples) may be applied to reduce jitter from uneven pulse timing, without introducing lag that would mask a genuine stop event.
- If the encoder reports zero pulses for the geofence dwell-check window described in 7.2, speed is treated as zero for Stop State logic.

### 7.4 Emergency SOS Override

**FR-4:** The system shall provide a hardware-interrupt-driven emergency channel that bypasses the standard sequential execution loop.

- Pressing the SOS push button shall trigger an immediate hardware interrupt service routine (ISR), independent of where the main loop currently is in its execution sequence.
- The ISR shall capture the most recently known coordinates and immediately command the cellular modem to send a priority SMS alert to a pre-configured dispatcher number, without waiting for the next scheduled telemetry transmission.
- The ISR shall set `sos_active = 1` in subsequent telemetry packets until the condition is manually cleared (e.g., a second press, or dispatcher acknowledgment via the dashboard).
- SOS transmission shall take priority over any pending standard data packet in the modem's send queue.

### 7.5 Telemetry Packaging (Software Data Contract)

**FR-5:** All telemetry from the device shall conform to a fixed JSON schema so the cloud ingestion layer can reliably parse every field. The `fuel_pct` field present in the original concept blueprint is removed; no fuel-related field is transmitted.

```json
{
  "dev_id": "VTUESP32-0091",
  "ts": "2026-06-25T17:12:00Z",
  "lat": 13.0827,
  "lon": 80.2707,
  "speed_kmh": 64.0,
  "geofence_id": "warehouse_alpha",
  "stop_state": false,
  "sos_active": 0
}
```

**Field Definitions**

| Field | Type | Description |
|---|---|---|
| dev_id | string | Unique identifier of the physical tracking unit. |
| ts | ISO-8601 string | UTC timestamp at which the reading was generated on-device. |
| lat / lon | float | Decimal-degree coordinates parsed from NMEA GNSS data. |
| speed_kmh | float | Instantaneous speed computed from axle encoder pulses. |
| geofence_id | string \| null | Identifier of the geofence the vehicle currently occupies, if any. |
| stop_state | boolean | True while the vehicle satisfies the Stop State condition (Section 7.2). |
| sos_active | 0 \| 1 | Binary flag set by the SOS interrupt; cleared on manual reset. |

### 7.6 Data Deduplication & Adaptive Transmission

**FR-6:** To reduce cellular data cost, firmware shall avoid transmitting redundant, unchanged telemetry on every cycle.

- Firmware shall compare the current reading against the last transmitted reading using configurable change thresholds (e.g., coordinate delta below ~5 m, speed delta below ~1 km/h).
- If all monitored values are within threshold of the last transmission and no Stop/SOS state transition has occurred, firmware shall send a lightweight keep-alive packet (`dev_id` + `ts` only) instead of a full payload.
- Any state transition (geofence entry/exit, SOS trigger) shall always force an immediate full-payload transmission regardless of the deduplication threshold.

### 7.7 Cloud Ingestion & Dashboard

**FR-7:** The cloud layer shall receive, validate, store, and visualize incoming telemetry in near real time.

- An HTTPS/MQTT ingestion endpoint shall accept JSON packets matching the schema in Section 7.5 and reject malformed payloads with a logged error.
- Valid packets shall be persisted to a time-series-capable data store, keyed by `dev_id` and timestamp.
- The dashboard shall subscribe to live updates (WebSocket or short-poll) and render the latest position, speed, geofence/stop status, and SOS state per device.
- Historical Stop State events shall be queryable as a per-device landmark log (as shown in Section 10).

---

## 8. Non-Functional Requirements

| Category | Requirement |
|---|---|
| Latency | SOS alert dispatch within 5 seconds of button press, end-to-end to SMS gateway. |
| Reliability | Firmware shall retain last-known-good GNSS fix and continue speed reporting through brief signal loss (tunnels, urban canyons). |
| Bandwidth Efficiency | Deduplication logic (FR-6) shall reduce idle-state data volume by a target of 70%+ versus naive per-second streaming. |
| Data Integrity | All ingested packets shall be schema-validated; malformed packets are logged, not silently dropped or partially stored. |
| Scalability | Backend ingestion and dashboard shall support multiple concurrent `dev_id` streams without per-device hardcoding. |
| Security | Telemetry transport shall use TLS; device authentication via per-device token/`dev_id` credential pairing. |

---

## 9. Software Architecture (Detailed)

### 9.1 Firmware Module Breakdown
- **GNSS Parser Module** — reads UART stream from cellular/GNSS unit, validates NMEA checksums, converts to decimal coordinates.
- **Speed Calculation Module** — timer-interrupt pulse counter, converts pulses to km/h, applies moving-average smoothing.
- **Geofence Engine** — holds the local bounding-box table, runs distance checks each cycle, manages Stop State timers and transitions.
- **SOS Interrupt Handler** — ISR bound to the push button GPIO pin; pre-empts the main loop and triggers priority SMS dispatch.
- **Telemetry Packager** — assembles validated sensor outputs into the JSON schema (Section 7.5); applies deduplication logic before handing off to the modem driver.
- **Modem Driver / Transport Layer** — manages AT-command sessions with the SIM808/A7670G module for both data uplink and SMS sending.

### 9.2 Backend / Cloud Components
- **Ingestion API** — stateless endpoint (HTTPS or MQTT broker) that authenticates the device, validates payload schema, and writes to the data store.
- **Data Store** — time-series-oriented database (e.g., a relational store with a timestamp-indexed table, or a dedicated time-series DB) holding raw telemetry and derived Stop State events.
- **Event/Alert Service** — listens for `sos_active` transitions and Stop State entries/exits, and pushes notifications to the dashboard and/or dispatcher channels.
- **Dashboard Backend** — serves aggregated/last-known-state queries and streams live updates to connected dashboard clients.

### 9.3 Dashboard Frontend Responsibilities
- Render live device position on a map view, updated as new telemetry arrives.
- Display current speed and geofence/stop status per device.
- Surface SOS alarm state prominently, with clear armed/triggered visual distinction.
- List recent geofenced landmark events with arrival time and dwell duration, per Section 7.2.
- Maintain a system flag stack showing active emergencies, if any.

---

## 10. Dashboard Reference Layout

The table below reflects the cloud dashboard fields after fuel monitoring removal — no fuel/tank widget is present.

| Panel | Content |
|---|---|
| Device Header | Device ID (e.g., VTUESP32-0091), connection/sync status. |
| GNSS Coordinates | Live latitude / longitude readout. |
| Odometry / Speed | Current speed in km/h from the axle encoder. |
| SOS Alarm Circuit | Armed / Safe / Triggered status indicator. |
| Live Position Map | Real-time route map with current geofence label overlay. |
| Geofenced Landmarks Log | Timestamped list of landmark arrivals with dwell duration. |
| System Flag Stack | Active emergency / alert summary. |

---

## 11. Success Metrics / KPIs

- SOS-to-dispatch latency consistently under 5 seconds in field testing.
- Speed reading accuracy within ±3% of a reference measurement across test runs.
- Geofence entry/exit detection with zero missed Stop State events across test routes.
- Measured reduction in transmitted data volume versus a naive 1-second streaming baseline, per FR-6.
- Dashboard reflects new telemetry within 2 seconds of cloud ingestion.

---

## 12. Assumptions & Risks

### 12.1 Assumptions
- A cellular signal is available for the majority of the operating route; brief dead zones are tolerated via last-known-state retention.
- The optical encoder is correctly calibrated to the vehicle's actual wheel circumference at deployment time.
- Dispatch SMS numbers are pre-provisioned in firmware/configuration before field deployment.

### 12.2 Risks
- Extended cellular outage could delay both routine telemetry and SOS alerts; a local buffering/retry strategy should be considered in a future revision.
- Incorrect geofence radius sizing could cause false Stop State triggers near closely spaced landmarks.
- Encoder mounting drift over vehicle lifetime could degrade speed accuracy without recalibration.

---

## 13. Future Scope (Not in Current Build)

- Re-introducing fuel-level monitoring as an optional add-on module, if reinstated in a later phase.
- OBD-II integration for engine diagnostics and predictive maintenance.
- Native mobile dispatcher app with push notifications.
- Offline buffering of telemetry during extended connectivity loss, with bulk upload on reconnect.
