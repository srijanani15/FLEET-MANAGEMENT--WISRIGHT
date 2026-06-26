"""
Multi-Bus Simulator — FleetTrack Module 4
Drives all 6 VTUESP32 devices along real Chennai routes.
Each bus runs in its own thread, posting to the Flask backend every INTERVAL seconds.

Usage:
    python simulate.py                        # run all 6 buses
    python simulate.py --buses 91 92          # run only Bus 91 and 92
    python simulate.py --backend http://192.168.1.10:5000
"""

import time
import math
import random
import urllib.request
import urllib.error
import json
import argparse
import threading
import os

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

def _load_env():
    env_path = os.path.join(os.path.dirname(__file__), ".env")
    if os.path.exists(env_path):
        with open(env_path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    os.environ.setdefault(k.strip(), v.strip())

_load_env()

BACKEND       = os.environ.get("SIMULATOR_BACKEND", "http://localhost:5000")
DEVICE_TOKEN  = os.environ.get("DEVICE_TOKEN", "")
INTERVAL      = 2    # seconds between packets
STEPS_BETWEEN = 20   # interpolation steps between stops
DWELL_MIN     = 20   # minimum dwell seconds at each stop
DWELL_MAX     = 90   # maximum dwell seconds at each stop

# ---------------------------------------------------------------------------
# Real Chennai stop positions (matches backend STOPS exactly)
# ---------------------------------------------------------------------------

STOPS = {
    "chennai_central":   (13.0827, 80.2707),
    "egmore":            (13.0784, 80.2617),
    "royapettah":        (13.0524, 80.2623),
    "t_nagar":           (13.0418, 80.2341),
    "vadapalani":        (13.0524, 80.2121),
    "anna_nagar":        (13.0850, 80.2101),
    "guindy":            (13.0067, 80.2206),
    "adyar":             (13.0012, 80.2565),
    "koyambedu":         (13.0694, 80.1948),
    "perambur":          (13.1175, 80.2479),
    "avadi":             (13.1132, 80.1050),
    "porur_junction":    (13.0359, 80.1569),
}

# Each bus has its own route through a subset of stops
ROUTES = {
    "VTUESP32-0091": ["avadi","anna_nagar","koyambedu","t_nagar","guindy","adyar","egmore","chennai_central","avadi"],
    "VTUESP32-0092": ["chennai_central","perambur","anna_nagar","koyambedu","porur_junction","vadapalani","t_nagar","chennai_central"],
    "VTUESP32-0093": ["adyar","guindy","t_nagar","vadapalani","koyambedu","anna_nagar","perambur","adyar"],
    "VTUESP32-0094": ["t_nagar","royapettah","egmore","chennai_central","perambur","t_nagar"],
    "VTUESP32-0095": ["avadi","porur_junction","vadapalani","t_nagar","avadi"],
    "VTUESP32-0096": ["adyar","guindy","porur_junction","avadi","adyar"],
}

# Starting offsets so buses are spread across their routes at launch
START_OFFSET = {
    "VTUESP32-0091": 0,
    "VTUESP32-0092": 2,
    "VTUESP32-0093": 1,
    "VTUESP32-0094": 0,
    "VTUESP32-0095": 1,
    "VTUESP32-0096": 2,
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def interp(a, b, t):
    return a + (b - a) * t

def post(dev_id, lat, lon, speed, sos):
    payload = json.dumps({
        "dev_id": dev_id,
        "lat": round(lat, 6),
        "lon": round(lon, 6),
        "speed_kmh": round(speed, 1),
        "sos_active": sos,
    }).encode()
    headers = {"Content-Type": "application/json"}
    if DEVICE_TOKEN:
        headers["X-Device-Token"] = DEVICE_TOKEN
    req = urllib.request.Request(
        f"{BACKEND}/telemetry",
        data=payload,
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status
    except urllib.error.URLError as e:
        return f"ERR:{e.reason}"

# ---------------------------------------------------------------------------
# Per-bus runner
# ---------------------------------------------------------------------------

_lock = threading.Lock()

def run_bus(dev_id):
    route_keys = ROUTES[dev_id]
    n = len(route_keys)
    start_i = START_OFFSET.get(dev_id, 0) % (n - 1)

    pkt = 0
    sos_active = 0
    sos_clear_at = 0

    with _lock:
        print(f"  [{dev_id}] starting at stop #{start_i} ({route_keys[start_i]})")

    i = start_i
    while True:
        src_key = route_keys[i]
        dst_key = route_keys[(i + 1) % n]
        src_lat, src_lon = STOPS[src_key]
        dst_lat, dst_lon = STOPS[dst_key]

        # --- DWELL at current stop ---
        dwell_secs = random.randint(DWELL_MIN, DWELL_MAX)
        dwell_packets = max(1, dwell_secs // INTERVAL)
        for _ in range(dwell_packets):
            pkt += 1
            now = time.time()
            if sos_active and now > sos_clear_at:
                sos_active = 0
            # random SOS: 0.5% chance per tick while not already active
            if not sos_active and random.random() < 0.005:
                sos_active = 1
                sos_clear_at = now + 10
            lat = src_lat + random.gauss(0, 0.00006)
            lon = src_lon + random.gauss(0, 0.00006)
            spd = round(random.uniform(0, 2), 1)
            status = post(dev_id, lat, lon, spd, sos_active)
            tag = "SOS!" if sos_active else f"DWELL @ {src_key}"
            with _lock:
                print(f"  [{dev_id}] #{pkt:04d}  {lat:.5f},{lon:.5f}  {spd:4.1f}km/h  [{tag}]  → {status}")
            time.sleep(INTERVAL)

        # --- TRAVEL to next stop ---
        for s in range(1, STEPS_BETWEEN + 1):
            pkt += 1
            now = time.time()
            if sos_active and now > sos_clear_at:
                sos_active = 0
            if not sos_active and random.random() < 0.005:
                sos_active = 1
                sos_clear_at = now + 10
            t = s / STEPS_BETWEEN
            ease = t * t * (3 - 2 * t)   # smooth-step
            lat = interp(src_lat, dst_lat, ease)
            lon = interp(src_lon, dst_lon, ease)
            bell = math.sin(math.pi * t)
            spd = round(20 + bell * random.uniform(25, 45), 1)
            status = post(dev_id, lat, lon, spd, sos_active)
            with _lock:
                prog = f"{s}/{STEPS_BETWEEN}"
                print(f"  [{dev_id}] #{pkt:04d}  {lat:.5f},{lon:.5f}  {spd:5.1f}km/h  [→ {dst_key} {prog}]  → {status}")
            time.sleep(INTERVAL)

        i = (i + 1) % (n - 1)

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    global BACKEND
    parser = argparse.ArgumentParser(description="FleetTrack multi-bus simulator")
    parser.add_argument("--backend", default=BACKEND, help="Backend URL (default: %(default)s)")
    parser.add_argument("--buses", nargs="+", metavar="ID",
                        help="Bus numbers to run, e.g. --buses 91 92 (default: all 6)")
    args = parser.parse_args()

    BACKEND = args.backend.rstrip("/")

    all_ids = list(ROUTES.keys())
    if args.buses:
        ids = [f"VTUESP32-{b.zfill(4)}" if len(b) <= 4 else b for b in args.buses]
        ids = [i for i in ids if i in ROUTES]
        if not ids:
            print("No matching bus IDs. Use 91–96 or full IDs like VTUESP32-0091.")
            return
    else:
        ids = all_ids

    print("=" * 60)
    print("  FleetTrack Multi-Bus Simulator — Module 4")
    print(f"  Backend  : {BACKEND}")
    print(f"  Token    : {'set (' + str(len(DEVICE_TOKEN)) + ' chars)' if DEVICE_TOKEN else 'not set'}")
    print(f"  Buses    : {', '.join(ids)}")
    print(f"  Interval : {INTERVAL}s  |  Stops/route: real Chennai GPS")
    print("  Press Ctrl+C to stop all buses")
    print("=" * 60)

    threads = []
    for dev_id in ids:
        t = threading.Thread(target=run_bus, args=(dev_id,), daemon=True, name=dev_id)
        t.start()
        threads.append(t)
        time.sleep(0.4)  # stagger startup so threads don't all POST at exactly t=0

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n  Simulator stopped.")

if __name__ == "__main__":
    main()
