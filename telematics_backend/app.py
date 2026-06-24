"""
IoT Smart Vehicle Telematics Backend
Module 4 - Sri Janani
Flask backend with SQLite storage for vehicle telemetry data.
"""

from flask import Flask, request, jsonify, send_from_directory
from flask_cors import CORS
import sqlite3
import time
import math
import os

app = Flask(__name__)
CORS(app)

DB_PATH = os.path.join(os.path.dirname(__file__), "telemetry.db")

# ---------------------------------------------------------------------------
# Known bus stops / depots — geofence targets
# ---------------------------------------------------------------------------

STOPS = [
    {"name": "Chennai Central",   "lat": 13.0827, "lon": 80.2707},
    {"name": "Egmore",            "lat": 13.0784, "lon": 80.2617},
    {"name": "Royapettah",        "lat": 13.0524, "lon": 80.2623},
    {"name": "T Nagar Bus Stand", "lat": 13.0418, "lon": 80.2341},
    {"name": "Vadapalani",        "lat": 13.0524, "lon": 80.2121},
    {"name": "Anna Nagar",        "lat": 13.0850, "lon": 80.2101},
    {"name": "Guindy",            "lat": 13.0067, "lon": 80.2206},
    {"name": "Adyar",             "lat": 13.0012, "lon": 80.2565},
    {"name": "Koyambedu",         "lat": 13.0694, "lon": 80.1948},
    {"name": "Perambur",          "lat": 13.1175, "lon": 80.2479},
    {"name": "Avadi",             "lat": 13.1132, "lon": 80.1050},
    {"name": "Porur Junction",    "lat": 13.0359, "lon": 80.1569},
]

GEOFENCE_RADIUS_M = 300   # metres — vehicle must be within this to count as "at stop"

# In-memory state: tracks which stop (if any) each device is currently inside
# { dev_id: {"stop_name": str, "arrived_at": float, "event_id": int} }
_active_at: dict = {}


# ---------------------------------------------------------------------------
# Geofence helpers
# ---------------------------------------------------------------------------

def haversine_m(lat1, lon1, lat2, lon2) -> float:
    """Return distance in metres between two GPS coordinates."""
    R = 6_371_000
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2) ** 2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def run_geofence(dev_id: str, lat: float, lon: float, ts: float):
    """
    Called after every telemetry insert.
    Detects entry into / exit from known stops and writes to stop_events.
    """
    # Find nearest stop and its distance
    nearest, nearest_dist = None, float("inf")
    for stop in STOPS:
        d = haversine_m(lat, lon, stop["lat"], stop["lon"])
        if d < nearest_dist:
            nearest_dist, nearest = d, stop

    in_zone  = nearest_dist <= GEOFENCE_RADIUS_M
    was      = _active_at.get(dev_id)          # previous state for this device

    if in_zone:
        if was is None:
            # Fresh arrival at a stop
            with get_db() as conn:
                cur = conn.execute(
                    "INSERT INTO stop_events (dev_id, location_name, lat, lon, arrived_at, duration_sec) VALUES (?,?,?,?,?,?)",
                    (dev_id, nearest["name"], nearest["lat"], nearest["lon"], ts, None),
                )
                event_id = cur.lastrowid
                conn.commit()
            _active_at[dev_id] = {"stop_name": nearest["name"], "arrived_at": ts, "event_id": event_id}

        elif was["stop_name"] != nearest["name"]:
            # Moved directly from one stop zone into another — close old, open new
            with get_db() as conn:
                conn.execute(
                    "UPDATE stop_events SET duration_sec=? WHERE id=?",
                    (round(ts - was["arrived_at"], 1), was["event_id"]),
                )
                cur = conn.execute(
                    "INSERT INTO stop_events (dev_id, location_name, lat, lon, arrived_at, duration_sec) VALUES (?,?,?,?,?,?)",
                    (dev_id, nearest["name"], nearest["lat"], nearest["lon"], ts, None),
                )
                event_id = cur.lastrowid
                conn.commit()
            _active_at[dev_id] = {"stop_name": nearest["name"], "arrived_at": ts, "event_id": event_id}
        # else: still inside same zone — nothing to do

    else:
        if was is not None:
            # Vehicle left the stop — stamp the departure duration
            with get_db() as conn:
                conn.execute(
                    "UPDATE stop_events SET duration_sec=? WHERE id=?",
                    (round(ts - was["arrived_at"], 1), was["event_id"]),
                )
                conn.commit()
            del _active_at[dev_id]


# ---------------------------------------------------------------------------
# Database helpers
# ---------------------------------------------------------------------------

def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with get_db() as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS telemetry (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                dev_id     TEXT    NOT NULL,
                lat        REAL    NOT NULL,
                lon        REAL    NOT NULL,
                speed_kmh  REAL    NOT NULL,
                sos_active INTEGER NOT NULL CHECK(sos_active IN (0, 1)),
                timestamp  REAL    NOT NULL
            )
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS stop_events (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                dev_id        TEXT  NOT NULL,
                location_name TEXT  NOT NULL,
                lat           REAL  NOT NULL,
                lon           REAL  NOT NULL,
                arrived_at    REAL  NOT NULL,
                duration_sec  REAL
            )
        """)
        conn.commit()


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

REQUIRED_TELEMETRY = {
    "dev_id":     str,
    "lat":        (int, float),
    "lon":        (int, float),
    "speed_kmh":  (int, float),
    "sos_active": int,
}

REQUIRED_STOP = {
    "dev_id":        str,
    "location_name": str,
    "lat":           (int, float),
    "lon":           (int, float),
    "arrived_at":    (int, float),
}


def validate(data, schema):
    if not isinstance(data, dict):
        return False, "Request body must be a JSON object."
    for field, expected in schema.items():
        if field not in data:
            return False, f"Missing required field: '{field}'."
        if not isinstance(data[field], expected):
            t = expected.__name__ if isinstance(expected, type) else " or ".join(x.__name__ for x in expected)
            return False, f"Field '{field}' must be of type {t}."
    return True, None


# ---------------------------------------------------------------------------
# Serve dashboard
# ---------------------------------------------------------------------------

@app.route("/")
def dashboard():
    return send_from_directory(os.path.dirname(__file__), "dashboard.html")

@app.route("/dashboard.css")
def serve_css():
    return send_from_directory(os.path.dirname(__file__), "dashboard.css")

@app.route("/dashboard.js")
def serve_js():
    return send_from_directory(os.path.dirname(__file__), "dashboard.js")


# ---------------------------------------------------------------------------
# Health check — Module 3 can ping this to confirm backend is up
# ---------------------------------------------------------------------------

@app.route("/health", methods=["GET"])
def health():
    with get_db() as conn:
        count = conn.execute("SELECT COUNT(*) FROM telemetry").fetchone()[0]
    return jsonify({
        "status": "ok",
        "records": count,
        "time": time.time()
    }), 200


# ---------------------------------------------------------------------------
# Telemetry endpoints
# ---------------------------------------------------------------------------

@app.route("/telemetry", methods=["POST"])
def post_telemetry():
    """Receive, validate, and store a telemetry packet from the vehicle (Module 3)."""
    data = request.get_json(silent=True)

    valid, error = validate(data, REQUIRED_TELEMETRY)
    if not valid:
        return jsonify({"status": "error", "message": error}), 400

    if data["sos_active"] not in (0, 1):
        return jsonify({"status": "error", "message": "Field 'sos_active' must be 0 or 1."}), 400
    if not (-90 <= data["lat"] <= 90):
        return jsonify({"status": "error", "message": "Field 'lat' must be between -90 and 90."}), 400
    if not (-180 <= data["lon"] <= 180):
        return jsonify({"status": "error", "message": "Field 'lon' must be between -180 and 180."}), 400
    if data["speed_kmh"] < 0:
        return jsonify({"status": "error", "message": "Field 'speed_kmh' must be >= 0."}), 400

    ts = time.time()
    with get_db() as conn:
        conn.execute(
            "INSERT INTO telemetry (dev_id, lat, lon, speed_kmh, sos_active, timestamp) VALUES (?,?,?,?,?,?)",
            (data["dev_id"], data["lat"], data["lon"], data["speed_kmh"], data["sos_active"], ts),
        )
        conn.commit()

    run_geofence(data["dev_id"], data["lat"], data["lon"], ts)

    return jsonify({"status": "ok", "timestamp": ts}), 201


@app.route("/telemetry/latest", methods=["GET"])
def get_latest():
    """Return the most recent telemetry record for a given device."""
    dev_id = request.args.get("dev_id")
    if not dev_id:
        return jsonify({"status": "error", "message": "Query parameter 'dev_id' is required."}), 400

    with get_db() as conn:
        row = conn.execute(
            "SELECT * FROM telemetry WHERE dev_id=? ORDER BY timestamp DESC LIMIT 1",
            (dev_id,),
        ).fetchone()

    if row is None:
        return jsonify({"status": "error", "message": f"No records found for dev_id '{dev_id}'."}), 404

    return jsonify({"status": "ok", "data": dict(row)}), 200


@app.route("/telemetry/history", methods=["GET"])
def get_history():
    """Return last 50 records for a device, newest first."""
    dev_id = request.args.get("dev_id")
    if not dev_id:
        return jsonify({"status": "error", "message": "Query parameter 'dev_id' is required."}), 400

    with get_db() as conn:
        rows = conn.execute(
            "SELECT * FROM telemetry WHERE dev_id=? ORDER BY timestamp DESC LIMIT 50",
            (dev_id,),
        ).fetchall()

    return jsonify({"status": "ok", "data": [dict(r) for r in rows]}), 200


@app.route("/telemetry/devices", methods=["GET"])
def get_devices():
    """Return all known device IDs and their latest timestamp — used by dashboard to auto-detect."""
    with get_db() as conn:
        rows = conn.execute(
            "SELECT dev_id, MAX(timestamp) as last_seen, COUNT(*) as total FROM telemetry GROUP BY dev_id ORDER BY last_seen DESC"
        ).fetchall()
    return jsonify({"status": "ok", "data": [dict(r) for r in rows]}), 200


# ---------------------------------------------------------------------------
# Stop events endpoints
# ---------------------------------------------------------------------------

@app.route("/telemetry/stops/config", methods=["GET"])
def get_stops_config():
    """Return the full list of defined geofence stops (for map markers)."""
    return jsonify({"status": "ok", "data": STOPS, "radius_m": GEOFENCE_RADIUS_M}), 200


@app.route("/telemetry/stops", methods=["GET"])
def get_stops():
    """Return geofence stop events for a device."""
    dev_id = request.args.get("dev_id")
    if not dev_id:
        return jsonify({"status": "error", "message": "Query parameter 'dev_id' is required."}), 400

    with get_db() as conn:
        rows = conn.execute(
            "SELECT * FROM stop_events WHERE dev_id=? ORDER BY arrived_at DESC",
            (dev_id,),
        ).fetchall()

    return jsonify({"status": "ok", "data": [dict(r) for r in rows]}), 200


@app.route("/telemetry/stats", methods=["GET"])
def get_stats():
    """Return aggregate stats for a device: avg/max speed, total distance estimate."""
    dev_id = request.args.get("dev_id")
    if not dev_id:
        return jsonify({"status": "error", "message": "Query parameter 'dev_id' is required."}), 400

    with get_db() as conn:
        row = conn.execute(
            """SELECT COUNT(*) as total,
                      ROUND(AVG(speed_kmh), 1) as avg_speed,
                      ROUND(MAX(speed_kmh), 1) as max_speed,
                      MIN(timestamp) as first_seen,
                      MAX(timestamp) as last_seen
               FROM telemetry WHERE dev_id=?""",
            (dev_id,)
        ).fetchone()

    if row is None or row["total"] == 0:
        return jsonify({"status": "error", "message": f"No records for dev_id '{dev_id}'."}), 404

    return jsonify({"status": "ok", "data": dict(row)}), 200


@app.route("/telemetry/stops", methods=["POST"])
def post_stop():
    """
    Log a geofence stop event sent by Module 3.
    Required fields: dev_id, location_name, lat, lon, arrived_at
    Optional: duration_sec (send when vehicle leaves the stop)
    """
    data = request.get_json(silent=True)

    valid, error = validate(data, REQUIRED_STOP)
    if not valid:
        return jsonify({"status": "error", "message": error}), 400

    with get_db() as conn:
        conn.execute(
            """INSERT INTO stop_events (dev_id, location_name, lat, lon, arrived_at, duration_sec)
               VALUES (?,?,?,?,?,?)""",
            (data["dev_id"], data["location_name"], data["lat"], data["lon"],
             data["arrived_at"], data.get("duration_sec")),
        )
        conn.commit()

    return jsonify({"status": "ok"}), 201


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    init_db()
    print("=" * 50)
    print("  Telematics Backend — Module 4 (Sri Janani)")
    print("=" * 50)
    print(f"  Database : {DB_PATH}")
    print(f"  Dashboard: http://0.0.0.0:5000/")
    print(f"  Health   : http://0.0.0.0:5000/health")
    print("=" * 50)
    app.run(host="0.0.0.0", port=5000, debug=True)
