"""
Mock Data Generator — Module 4
Simulates a vehicle moving around Chennai and POSTs telemetry
to the backend every 3 seconds.
"""

import requests
import random
import time
import math

BACKEND_URL = "http://localhost:5000/telemetry"
DEV_ID      = "VTUESP32-0091"
INTERVAL    = 3  # seconds between each POST

# Starting position — Chennai Central
lat = 13.0827
lon = 80.2707
speed = 30.0

# Small heading angle (degrees) — vehicle "drives" in this direction
heading = random.uniform(0, 360)

def move(lat, lon, heading, speed_kmh):
    """Move lat/lon slightly in the heading direction based on speed."""
    distance_km = speed_kmh * (INTERVAL / 3600)  # km travelled in interval
    delta = distance_km / 111.0                   # 1 degree lat ≈ 111 km
    rad = math.radians(heading)
    new_lat = lat + delta * math.cos(rad)
    new_lon = lon + delta * math.sin(rad)
    # Keep within Chennai area bounds
    new_lat = max(12.95, min(13.20, new_lat))
    new_lon = max(80.15, min(80.35, new_lon))
    return round(new_lat, 6), round(new_lon, 6)

def next_speed(speed):
    """Randomly accelerate or decelerate, occasionally stop."""
    change = random.uniform(-8, 8)
    speed = max(0, min(100, speed + change))
    if random.random() < 0.05:   # 5% chance to fully stop
        speed = 0
    return round(speed, 1)

def next_heading(heading):
    """Slightly turn the vehicle each step."""
    return (heading + random.uniform(-15, 15)) % 360

def should_trigger_sos():
    """1 in 40 chance of SOS per packet."""
    return 1 if random.random() < 0.025 else 0

print(f"Mock generator started — POSTing to {BACKEND_URL} every {INTERVAL}s")
print("Press Ctrl+C to stop.\n")

sos_cooldown = 0  # keep SOS on for a few cycles when triggered

while True:
    heading = next_heading(heading)
    speed   = next_speed(speed)
    lat, lon = move(lat, lon, heading, speed)

    if sos_cooldown > 0:
        sos_active = 1
        sos_cooldown -= 1
    elif should_trigger_sos():
        sos_active   = 1
        sos_cooldown = 4  # stay active for 4 more cycles (~12 seconds)
        print("  ⚠  SOS TRIGGERED!")
    else:
        sos_active = 0

    payload = {
        "dev_id":     DEV_ID,
        "lat":        lat,
        "lon":        lon,
        "speed_kmh":  speed,
        "sos_active": sos_active,
    }

    try:
        r = requests.post(BACKEND_URL, json=payload, timeout=5)
        status = r.json().get("status", "?")
        print(f"  lat={lat}  lon={lon}  speed={speed} km/h  sos={sos_active}  → {status}")
    except Exception as e:
        print(f"  ERROR: {e}")

    time.sleep(INTERVAL)
