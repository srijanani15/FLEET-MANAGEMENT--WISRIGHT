# User Stories — IoT Smart Vehicle Telematics System

**Project:** IoT Smart Vehicle Telematics & Tracking System  
**Document Version:** 2.0  
**Date:** 2026-06-25  
**Source:** Telematics_PRD.md + mock.html implementation

---

## Actors

| Actor | Description |
|---|---|
| **Student / Passenger** | College student who needs to track their bus before and during the commute |
| **Parent / Guardian** | Family member who wants to confirm a student is safely on the bus |
| **Dispatcher / Operator** | Transport department staff monitoring the entire fleet in real time |
| **Transport Admin** | Department head who manages routes, buses, and system configuration |
| **Driver** | Bus driver who operates the vehicle and can trigger emergency SOS alerts |
| **System (ESP32 Device)** | On-board tracking unit that generates and transmits telemetry |
| **Backend / Cloud** | Ingestion API, data store, and event service that process device data |

---

## Epic 1 — Student Bus Tracking

### US-01 · View Available Trips
**As a** student,  
**I want to** see the available bus trips for the day (morning and afternoon),  
**so that** I know which buses are running and at what time.

**Acceptance Criteria:**
- Home page shows at minimum two trip slots: 8:00 AM and 3:00 PM
- Each trip card displays the departure time and a visual indicator (icon / colour)
- Cards are visible without logging in

---

### US-02 · Browse Bus List for a Trip
**As a** student,  
**I want to** tap a trip card and see a list of all buses assigned to that trip,  
**so that** I can find my specific bus number quickly.

**Acceptance Criteria:**
- Clicking a trip card navigates to a bus list page filtered by that trip (AM / PM)
- Each row shows: Bus Number, Route, Current Speed, Status (Moving / Stopped / SOS)
- List updates in real time (≤ 2 s refresh)

---

### US-03 · Filter Bus List by Status
**As a** student,  
**I want to** filter the bus list by "Moving", "Stopped", or "SOS",  
**so that** I can quickly identify buses in an emergency or find mine by status.

**Acceptance Criteria:**
- Filter chips (All / Moving / Stopped / SOS) appear above the table
- Clicking a chip instantly hides non-matching rows without a page reload
- Active chip is visually highlighted

---

### US-04 · Open Live Tracker for My Bus
**As a** student,  
**I want to** click my bus number and open a live map showing exactly where the bus is,  
**so that** I can plan when to leave home.

**Acceptance Criteria:**
- "Track" button on each bus row navigates to the Live Tracker view
- Map centres on the selected bus's current GPS coordinates
- Bus icon is visible on the map with its colour matching the bus identity

---

### US-05 · See Live Speed on the Map
**As a** student,  
**I want to** see the bus's current speed prominently on the tracker screen,  
**so that** I have confidence the bus is actually moving.

**Acceptance Criteria:**
- Speed displayed in km/h in a large, readable element (HUD / hero card)
- Colour coding: green ≤ 40 km/h, amber 41–70 km/h, red > 70 km/h
- Speed updates every ~1 second

---

### US-06 · View Exact GPS Coordinates
**As a** student,  
**I want to** see the bus's current latitude and longitude,  
**so that** I or my parents can verify its exact position.

**Acceptance Criteria:**
- Latitude and longitude shown as decimal values in the telemetry panel
- Values update in sync with the map marker movement

---

### US-07 · See Route Trail on Map
**As a** student,  
**I want to** see the path the bus has already travelled on the map,  
**so that** I can gauge how far along the route it is.

**Acceptance Criteria:**
- A coloured polyline trail follows the bus's recent track history
- Trail is capped to a reasonable recent history (≤ 80 points) to stay readable

---

### US-08 · Know Which Stop the Bus is At
**As a** student,  
**I want to** see if the bus is currently at or near a known stop/landmark,  
**so that** I know exactly where it is without interpreting raw coordinates.

**Acceptance Criteria:**
- Active geofence name is shown in the telemetry panel when inside a zone
- Landmark entry/exit events appear in the Landmark Log with timestamps

---

### US-20 · Search for My Bus by Number
**As a** student,  
**I want to** type my bus number in a search box on the home screen,  
**so that** I can jump directly to my bus's tracker without scrolling through the full list.

**Acceptance Criteria:**
- Search box is prominently placed on the home page hero section
- Typing a bus number filters or highlights the matching bus across all trips
- Pressing "Track" / Enter navigates directly to the Live Tracker for that bus

---

### US-21 · See Bus ETA at My Stop
**As a** student,  
**I want to** see an estimated time of arrival for my bus at my nearest stop,  
**so that** I know exactly when to start walking to the stop.

**Acceptance Criteria:**
- ETA is calculated from current bus position and average speed along remaining route
- ETA is displayed in minutes (e.g., "~12 min away")
- ETA updates every time the bus position updates

---

### US-22 · Use the App on My Phone
**As a** student,  
**I want to** access the tracking portal on my mobile phone without any loss of functionality,  
**so that** I can check the bus on the go without needing a laptop.

**Acceptance Criteria:**
- All three views (Home, Bus List, Live Tracker) are fully usable on screens as small as 360 px wide
- Map, speed HUD, and telemetry panel reflow correctly on small screens
- Touch targets (buttons, filter chips) are at least 44×44 px

---

### US-23 · Go Back to the Bus List Without Losing My Filter
**As a** student,  
**I want to** press a back button in the live tracker to return to the bus list,  
**so that** I can switch to tracking a different bus without starting over from the home page.

**Acceptance Criteria:**
- A back/close button is visible in the Live Tracker header
- Pressing it returns to the Bus List view with the previous trip filter (AM/PM) still applied
- The bus list scroll position is restored

---

### US-24 · See a Live Clock and Today's Date
**As a** student,  
**I want to** see the current time and date on the home page,  
**so that** I can quickly confirm I'm looking at today's trips.

**Acceptance Criteria:**
- A live clock (HH:MM:SS) is displayed and ticks every second
- Today's date is shown alongside the trip cards
- Clock and date are visible on all screen sizes

---

## Epic 2 — Parent / Guardian

### US-25 · Share Bus Location with Parent
**As a** student,  
**I want to** share a link to my bus's live tracker with my parent,  
**so that** they can independently verify where the bus is without calling me.

**Acceptance Criteria:**
- A share button or copyable URL is available on the Live Tracker view
- The link opens directly to the correct bus's tracker without requiring login
- Shared link reflects the same live data as the original session

---

### US-26 · Parent Confirms Bus is Moving
**As a** parent,  
**I want to** see the bus speed and movement status at a glance,  
**so that** I can confirm the bus is not delayed or broken down.

**Acceptance Criteria:**
- Speed and "Moving / Stopped" status are visible immediately on opening the tracker
- If the bus speed is 0 for more than 2 minutes, a "Bus appears stopped" notice is shown
- No account or login required for a parent to view the tracker

---

### US-27 · Parent Sees SOS Alert Immediately
**As a** parent,  
**I want to** be clearly informed if my child's bus has triggered an SOS,  
**so that** I can take immediate action to ensure their safety.

**Acceptance Criteria:**
- If `sos_active = 1`, the tracker view shows a full-colour SOS banner/animation
- The SOS banner is impossible to miss — it overrides the normal UI emphasis
- SOS state is visible to parents accessing via shared link

---

## Epic 3 — Dispatcher / Fleet Monitoring

### US-09 · View All Buses Simultaneously
**As a** dispatcher,  
**I want to** see a complete list of all active buses with their live status at a glance,  
**so that** I can monitor the whole fleet without opening each bus individually.

**Acceptance Criteria:**
- Bus list table shows all buses: Bus #, Route, Speed, Status columns
- Status badges (Moving / Stopped / SOS) update every ≤ 2 s
- Total counts (Active, Moving, SOS) shown in a stats strip

---

### US-10 · Detect SOS Emergencies Immediately
**As a** dispatcher,  
**I want to** be alerted immediately when a bus triggers an SOS,  
**so that** I can take emergency action within seconds.

**Acceptance Criteria:**
- SOS-active buses are visually highlighted (red border / animation) in the bus list
- Live Tracker view shows a prominent SOS flare/badge when `sos_active = 1`
- SOS status persists until manually cleared

---

### US-11 · Review Landmark Event Log
**As a** dispatcher,  
**I want to** view a timestamped log of each bus's arrivals at and departures from known stops,  
**so that** I can verify that the route is running on schedule.

**Acceptance Criteria:**
- Landmark Log panel lists every geofence entry/exit event in chronological order
- Each entry shows: landmark name, event type (ENTERED / EXITED), timestamp
- Log persists for the duration of the session

---

### US-12 · See Geofence Zones on the Map
**As a** dispatcher,  
**I want to** see the defined geofence zones drawn on the map,  
**so that** I can visually confirm whether a bus is within an expected area.

**Acceptance Criteria:**
- Geofence circles are rendered as translucent overlays on the Leaflet map
- Each circle is labelled with the landmark name

---

### US-13 · Read the Raw Telemetry Packet
**As a** dispatcher or developer,  
**I want to** see the raw JSON telemetry packet the device last transmitted,  
**so that** I can debug issues or verify data integrity.

**Acceptance Criteria:**
- A "Last JSON Packet" panel displays the most recent packet in formatted JSON
- Packet matches the schema: `dev_id`, `ts`, `lat`, `lon`, `speed_kmh`, `geofence_id`, `stop_state`, `sos_active`
- Panel refreshes on every tick

---

### US-28 · Identify Overspeeding Buses
**As a** dispatcher,  
**I want to** immediately spot any bus travelling above the safe speed limit (70 km/h),  
**so that** I can contact the driver and prevent an accident.

**Acceptance Criteria:**
- Speed values above 70 km/h are shown in red in the bus list table
- An "Overspeed" badge appears next to the speed value when threshold is exceeded
- The Live Tracker HUD turns red and shows a warning label when the tracked bus is overspeeding

---

### US-29 · See Stop Dwell Duration
**As a** dispatcher,  
**I want to** know how long a bus has been stopped at a particular landmark,  
**so that** I can identify unusual delays and take corrective action.

**Acceptance Criteria:**
- When a bus is in Stop State inside a geofence, a live dwell timer (MM:SS) is shown in the landmark log
- If dwell time exceeds a configurable threshold (e.g., 10 minutes), the row is highlighted as a delay
- Dwell time is also included in the telemetry Stop State event sent to the cloud

---

### US-30 · Monitor System Connection Status
**As a** dispatcher,  
**I want to** know if a device has gone offline or stopped sending data,  
**so that** I can distinguish a real problem from a connectivity issue.

**Acceptance Criteria:**
- If no telemetry packet is received from a device for > 30 seconds, it is marked "Offline" in the bus list
- Offline buses are visually distinguished (greyed out / strikethrough)
- The last known position and timestamp are retained and shown until the device reconnects

---

### US-31 · Acknowledge and Clear an SOS
**As a** dispatcher,  
**I want to** mark an SOS alert as acknowledged after I have responded,  
**so that** the system reflects the current handled state and does not keep alarming.

**Acceptance Criteria:**
- An "Acknowledge SOS" button is visible on the Live Tracker when `sos_active = 1`
- Pressing it sets `sos_active = 0` (or sends a clear command to the device)
- The SOS visual indicators are removed after acknowledgement

---

## Epic 4 — Transport Admin

### US-32 · Add or Update a Geofence
**As a** transport admin,  
**I want to** add a new geofence landmark or update an existing one's radius,  
**so that** the system accurately reflects the current set of operational stops.

**Acceptance Criteria:**
- Admin interface allows creating a geofence by entering: name, latitude, longitude, radius (metres)
- New geofences take effect on all device firmware on the next update cycle
- Existing geofences can be edited or deleted without redeploying firmware

---

### US-33 · Assign Buses to Routes and Trips
**As a** transport admin,  
**I want to** assign specific buses to specific routes and trips (AM / PM),  
**so that** the student-facing portal shows accurate bus lists for each trip.

**Acceptance Criteria:**
- Admin panel lists all registered device IDs and allows mapping them to a route and trip slot
- Changes appear in the portal's bus list within one refresh cycle
- Deactivated buses are hidden from the student view

---

### US-34 · View Historical Route Playback
**As a** transport admin,  
**I want to** replay a bus's GPS track from a past date,  
**so that** I can investigate incidents or verify that drivers followed the correct route.

**Acceptance Criteria:**
- Historical track is retrieved from the time-series data store by selecting device + date range
- Playback shows the bus icon moving along its historical path at adjustable speed (1×, 5×, 10×)
- Landmark events from the same period are shown alongside the replay

---

### US-35 · Export Trip Summary Report
**As a** transport admin,  
**I want to** download a summary report for a trip (all buses, stops reached, delays, SOS events),  
**so that** I can share it with management or use it for compliance records.

**Acceptance Criteria:**
- Export is available in CSV or PDF format
- Report includes per-bus data: route, total distance, average speed, stops reached, dwell times, SOS events
- Report can be filtered by date range and trip (AM / PM)

---

### US-36 · Register a New Device
**As a** transport admin,  
**I want to** register a new ESP32 tracking device with a unique device ID,  
**so that** its telemetry is accepted and displayed by the system without code changes.

**Acceptance Criteria:**
- Registration requires: device ID (e.g., VTUESP32-XXXX), bus number, assigned route
- After registration, the device's first valid packet is ingested and shown on the dashboard
- Duplicate device IDs are rejected with a clear error message

---

## Epic 5 — Driver / Device

### US-14 · Trigger Emergency SOS Alert
**As a** driver,  
**I want to** press a physical button to send an emergency alert,  
**so that** dispatch is notified immediately regardless of normal telemetry status.

**Acceptance Criteria:**
- SOS button press triggers a hardware interrupt (ISR) on the ESP32
- The ISR sends a priority SMS to the pre-configured dispatcher number within 5 seconds
- `sos_active = 1` is set in all subsequent telemetry packets until cleared
- SOS alert bypasses any pending standard data in the modem queue

---

### US-15 · Maintain Last Known Location During Signal Loss
**As a** driver (and indirectly as a student/dispatcher),  
**I want** the device to retain and display the last known GPS coordinates during brief signal loss,  
**so that** the tracker does not go blank in tunnels or low-signal areas.

**Acceptance Criteria:**
- Firmware retains last valid GNSS fix when incoming NMEA sentences have "no-fix" status
- Dashboard continues to display last known position with a visual indication of staleness if applicable

---

### US-16 · Automatic Geofence Stop Detection
**As a** driver,  
**I want** the system to automatically detect when I'm stopped at a known landmark,  
**so that** I don't need to manually log arrivals.

**Acceptance Criteria:**
- System detects vehicle inside a geofence bounding box + speed = 0 for ≥ 60 seconds
- Stop State event is logged and transmitted to the cloud automatically
- No driver input required

---

### US-37 · Device Boots and Connects Automatically
**As a** driver,  
**I want** the tracking device to start transmitting automatically when I turn on the bus,  
**so that** I don't need to manually activate it before each trip.

**Acceptance Criteria:**
- ESP32 boots, connects to cellular network, and begins sending telemetry within 60 seconds of power-on
- No driver action is required to initiate tracking
- A status LED or indicator confirms active connection to the driver

---

### US-38 · Device Recovers After Temporary Power Loss
**As a** driver,  
**I want** the device to resume normal operation if the bus power momentarily drops,  
**so that** a brief engine off event doesn't permanently disable tracking.

**Acceptance Criteria:**
- On reboot, the device re-initialises all modules and resumes telemetry transmission
- Any Stop State timer is reset on reboot; geofence detection restarts cleanly
- Dispatcher is not shown a false "Offline → Online" alert if the gap is under 10 seconds

---

## Epic 6 — System / Device Behaviour

### US-17 · Transmit Structured JSON Telemetry
**As a** backend system,  
**I want** each device to transmit a fixed-schema JSON payload,  
**so that** the ingestion layer can reliably parse every field without custom per-device logic.

**Acceptance Criteria:**
- Every packet contains: `dev_id`, `ts`, `lat`, `lon`, `speed_kmh`, `geofence_id`, `stop_state`, `sos_active`
- No `fuel_pct` or other removed fields are present
- Malformed packets are rejected with a logged error at the ingestion layer

---

### US-18 · Adaptive Transmission (Deduplication)
**As a** system operator,  
**I want** the device to send a lightweight keep-alive instead of a full packet when nothing has changed,  
**so that** cellular data costs are reduced by at least 70% during idle periods.

**Acceptance Criteria:**
- If coordinate delta < ~5 m and speed delta < ~1 km/h and no state transition occurred, only `dev_id` + `ts` are sent
- Any geofence entry/exit or SOS event always forces a full-payload transmission
- Measured data volume reduction ≥ 70% vs. naive 1-second full streaming baseline

---

### US-19 · Calculate Speed Independently of GPS
**As a** dispatcher,  
**I want** the bus speed to come from the axle encoder rather than GPS-derived speed,  
**so that** speed readings remain accurate even when satellite signal is weak.

**Acceptance Criteria:**
- Speed in km/h is computed from optical encoder pulse count over a fixed 200–500 ms window
- Formula: `pulses × wheel_circumference × (3600 / window_seconds)`
- A moving-average smoothing factor is applied to reduce jitter

---

### US-39 · Validate NMEA Checksum Before Using Coordinates
**As a** backend system,  
**I want** the firmware to validate the NMEA checksum of each GPS sentence before accepting it,  
**so that** corrupted coordinates are never transmitted as real data.

**Acceptance Criteria:**
- Firmware computes and compares the NMEA XOR checksum for every received sentence
- Sentences with invalid checksums are discarded; the last known good coordinate is retained
- Checksum failures are counted and can be surfaced as a diagnostic field in telemetry if needed

---

### US-40 · Scale to Multiple Concurrent Devices
**As a** backend system,  
**I want** the ingestion and dashboard layer to handle multiple device streams simultaneously,  
**so that** adding more buses does not require any per-device code changes.

**Acceptance Criteria:**
- Ingestion API routes incoming packets by `dev_id` dynamically — no hardcoded device list
- Dashboard renders a row per active `dev_id` without configuration changes
- All device streams can be active simultaneously without performance degradation

---

### US-41 · Secure Telemetry Transport with TLS
**As a** system operator,  
**I want** all telemetry transmitted over HTTPS/TLS,  
**so that** device location and emergency data cannot be intercepted in transit.

**Acceptance Criteria:**
- All device-to-cloud communication uses TLS 1.2 or higher
- Devices authenticate using a per-device token / `dev_id` credential pair
- Packets received over unencrypted channels are rejected

---

### US-42 · Log and Reject Malformed Packets
**As a** backend system,  
**I want** malformed or incomplete telemetry packets to be logged and rejected rather than silently dropped or partially stored,  
**so that** data integrity is maintained and issues are traceable.

**Acceptance Criteria:**
- Schema validation runs on every incoming packet at the ingestion API
- Packets missing required fields or with incorrect types are rejected with a 400-level response
- Rejected packets are logged with the reason, device ID, and timestamp for later review

---

### US-43 · Dashboard Reflects New Telemetry Within 2 Seconds
**As a** dispatcher,  
**I want** the dashboard to update within 2 seconds of new data arriving at the cloud,  
**so that** the displayed position is always current and trustworthy.

**Acceptance Criteria:**
- Dashboard uses WebSocket or short-poll (≤ 2 s interval) to receive live updates
- Map marker, speed, and status badge all update within the 2-second window
- No manual refresh is required by the user

---

## Priority Matrix

| Story | Actor | Priority | Complexity |
|---|---|---|---|
| US-01 View Trips | Student | High | Low |
| US-02 Browse Bus List | Student | High | Low |
| US-04 Open Live Tracker | Student | High | Medium |
| US-05 Live Speed | Student | High | Low |
| US-10 SOS Alert Display | Dispatcher | Critical | High |
| US-14 Trigger SOS | Driver | Critical | High |
| US-09 Fleet Overview | Dispatcher | High | Medium |
| US-17 JSON Telemetry Schema | System | High | Medium |
| US-22 Mobile Responsive UI | Student | High | Medium |
| US-40 Multi-Device Scaling | System | High | Medium |
| US-41 TLS Security | System | High | Medium |
| US-28 Overspeed Detection | Dispatcher | High | Low |
| US-43 2s Dashboard Refresh | Dispatcher | High | Medium |
| US-03 Filter Bus List | Student | Medium | Low |
| US-06 GPS Coordinates | Student | Medium | Low |
| US-07 Route Trail | Student | Medium | Medium |
| US-08 Stop Detection Display | Student | Medium | Medium |
| US-11 Landmark Event Log | Dispatcher | Medium | Medium |
| US-12 Geofence on Map | Dispatcher | Medium | Medium |
| US-18 Deduplication | System | Medium | Medium |
| US-19 Encoder Speed | System | Medium | High |
| US-20 Search by Bus Number | Student | Medium | Low |
| US-21 ETA at Stop | Student | Medium | High |
| US-23 Back Navigation | Student | Medium | Low |
| US-24 Live Clock | Student | Medium | Low |
| US-25 Share Bus Link | Student | Medium | Medium |
| US-26 Parent Speed View | Parent | Medium | Low |
| US-27 Parent SOS View | Parent | Medium | Low |
| US-29 Stop Dwell Duration | Dispatcher | Medium | Medium |
| US-30 Offline Detection | Dispatcher | Medium | Medium |
| US-31 Acknowledge SOS | Dispatcher | Medium | Medium |
| US-37 Auto Boot Tracking | Driver | Medium | Medium |
| US-39 NMEA Checksum | System | Medium | Medium |
| US-42 Reject Malformed Packets | System | Medium | Low |
| US-15 Signal Loss Retention | System | Medium | Medium |
| US-16 Auto Stop Detection | System | Medium | High |
| US-38 Power Loss Recovery | Driver | Low | Medium |
| US-13 Raw JSON Packet Viewer | Dispatcher/Dev | Low | Low |
| US-32 Add Geofence | Admin | Low | High |
| US-33 Assign Routes | Admin | Low | Medium |
| US-34 Route Playback | Admin | Low | High |
| US-35 Export Report | Admin | Low | Medium |
| US-36 Register Device | Admin | Low | Low |

---

## Summary

| Epic | Stories | New in v2.0 |
|---|---|---|
| Epic 1 — Student Bus Tracking | US-01 to US-08, US-20 to US-24 | US-20, 21, 22, 23, 24 |
| Epic 2 — Parent / Guardian | US-25, US-26, US-27 | All new |
| Epic 3 — Dispatcher / Fleet | US-09 to US-13, US-28 to US-31 | US-28, 29, 30, 31 |
| Epic 4 — Transport Admin | US-32 to US-36 | All new |
| Epic 5 — Driver / Device | US-14 to US-16, US-37, US-38 | US-37, 38 |
| Epic 6 — System Behaviour | US-17 to US-19, US-39 to US-43 | US-39, 40, 41, 42, 43 |
| **Total** | **43 user stories** | **+24 added in v2.0** |

---

*User stories derived from `Telematics_PRD.md` and the implemented mock at `mock.html`.*
