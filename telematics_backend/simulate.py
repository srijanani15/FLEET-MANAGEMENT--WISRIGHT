"""
Vehicle Simulator — FleetTrack Module 4
Sends live telemetry to the backend every 3 seconds,
simulating a bus moving through Chennai stops.
"""

import time
import math
import random
import urllib.request
import urllib.error
import json

BACKEND  = "http://localhost:5000"
DEV_ID   = "VTUESP32-0091"
INTERVAL = 3   # seconds between packets

# Route: cycle through these Chennai stops in order
ROUTE = [
    {"name": "Chennai Central",   "lat": 13.0827, "lon": 80.2707},
    {"name": "Egmore",            "lat": 13.0784, "lon": 80.2617},
    {"name": "Royapettah",        "lat": 13.0524, "lon": 80.2623},
    {"name": "T Nagar Bus Stand", "lat": 13.0418, "lon": 80.2341},
    {"name": "Vadapalani",        "lat": 13.0524, "lon": 80.2121},
    {"name": "Anna Nagar",        "lat": 13.0850, "lon": 80.2101},
    {"name": "Koyambedu",         "lat": 13.0694, "lon": 80.1948},
    {"name": "Guindy",            "lat": 13.0067, "lon": 80.2206},
    {"name": "Adyar",             "lat": 13.0012, "lon": 80.2565},
    {"name": "Perambur",          "lat": 13.1175, "lon": 80.2479},
]

STEPS_BETWEEN = 18   # interpolation steps between stops
DWELL_STEPS   = 5    # steps to linger at each stop (speed ~0)


def interp(a, b, t):
    return a + (b - a) * t


def post(payload):
    data = json.dumps(payload).encode()
    req  = urllib.request.Request(
        f"{BACKEND}/telemetry",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status
    except urllib.error.URLError as e:
        print(f"  [ERROR] {e.reason}")
        return None


def run():
    print("=" * 52)
    print("  FleetTrack Vehicle Simulator")
    print(f"  Device : {DEV_ID}")
    print(f"  Backend: {BACKEND}")
    print(f"  Route  : {len(ROUTE)} stops, looping forever")
    print("  Press Ctrl+C to stop")
    print("=" * 52)

    n      = len(ROUTE)
    pkt    = 0

    while True:
        for i in range(n):
            src = ROUTE[i]
            dst = ROUTE[(i + 1) % n]

            # ── DWELL at current stop ─────────────────────────
            for d in range(DWELL_STEPS):
                pkt += 1
                spd = round(random.uniform(0, 2), 1)
                lat = src["lat"] + random.gauss(0, 0.00008)
                lon = src["lon"] + random.gauss(0, 0.00008)
                sos = 1 if (pkt == 7) else 0   # one SOS demo on packet 7
                status = post({"dev_id": DEV_ID, "lat": lat, "lon": lon,
                               "speed_kmh": spd, "sos_active": sos})
                tag = "SOS!" if sos else f"AT {src['name']}"
                print(f"  #{pkt:04d}  {lat:.5f},{lon:.5f}  {spd:5.1f}km/h  [{tag}]  HTTP {status}")
                time.sleep(INTERVAL)

            # ── TRAVEL to next stop ───────────────────────────
            for s in range(1, STEPS_BETWEEN + 1):
                pkt += 1
                t   = s / STEPS_BETWEEN
                # ease-in-out so it accelerates then decelerates
                ease = t * t * (3 - 2 * t)
                lat  = interp(src["lat"], dst["lat"], ease)
                lon  = interp(src["lon"], dst["lon"], ease)
                # speed bell: fast in middle, slow at ends
                bell = math.sin(math.pi * t)
                spd  = round(20 + bell * random.uniform(25, 45), 1)
                status = post({"dev_id": DEV_ID, "lat": lat, "lon": lon,
                               "speed_kmh": spd, "sos_active": 0})
                prog = f"{s}/{STEPS_BETWEEN}"
                print(f"  #{pkt:04d}  {lat:.5f},{lon:.5f}  {spd:5.1f}km/h  [→ {dst['name']} {prog}]  HTTP {status}")
                time.sleep(INTERVAL)


if __name__ == "__main__":
    try:
        run()
    except KeyboardInterrupt:
        print("\n  Simulator stopped.")
