"""
IoT Smart Vehicle Telematics Backend
Module 4 - Sri Janani
Flask backend with MySQL storage for vehicle telemetry data.
"""

from flask import Flask, request, jsonify, send_from_directory
from flask_cors import CORS
import mysql.connector
import time
import math
import os

app = Flask(__name__)
CORS(app)

# ---------------------------------------------------------------------------
# MySQL configuration — loaded from .env file (never hardcode credentials)
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

DB_CONFIG = {
    "host":     os.environ.get("MYSQL_HOST",     "localhost"),
    "user":     os.environ.get("MYSQL_USER",     "root"),
    "password": os.environ.get("MYSQL_PASSWORD", ""),
    "database": os.environ.get("MYSQL_DATABASE", "telematics"),
}

# ---------------------------------------------------------------------------
# Device auth & rate limiting
# ---------------------------------------------------------------------------

DEVICE_TOKEN = os.environ.get("DEVICE_TOKEN", "")

# Per-device rate limit: max 2 requests per second (token bucket, 2 tokens/s)
_rate_buckets: dict = {}          # dev_id → {"tokens": float, "last": float}
_RATE_MAX     = 2.0               # burst capacity
_RATE_REFILL  = 2.0               # tokens added per second
_rate_lock    = __import__("threading").Lock()


def _check_rate(dev_id: str) -> bool:
    """Return True if request is allowed, False if rate-limited. Thread-safe."""
    now = time.time()
    with _rate_lock:
        if dev_id not in _rate_buckets:
            _rate_buckets[dev_id] = {"tokens": _RATE_MAX, "last": now}
        b = _rate_buckets[dev_id]
        elapsed = now - b["last"]
        b["tokens"] = min(_RATE_MAX, b["tokens"] + elapsed * _RATE_REFILL)
        b["last"] = now
        if b["tokens"] >= 1.0:
            b["tokens"] -= 1.0
            return True
        return False

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

GEOFENCE_RADIUS_M = 300

# In-memory geofence state per device
_active_at: dict = {}


# ---------------------------------------------------------------------------
# Database helpers
# ---------------------------------------------------------------------------

def get_db():
    """Return a new MySQL connection."""
    return mysql.connector.connect(**DB_CONFIG)


def query(sql, params=(), fetch="all"):
    """Run a SELECT and return rows as list-of-dicts."""
    conn = get_db()
    cur = None
    try:
        cur = conn.cursor(dictionary=True)
        cur.execute(sql, params)
        return cur.fetchall() if fetch == "all" else cur.fetchone()
    finally:
        if cur: cur.close()
        conn.close()


def execute(sql, params=(), lastrowid=False):
    """Run an INSERT / UPDATE and commit. Returns lastrowid if requested."""
    conn = get_db()
    cur = None
    try:
        cur = conn.cursor()
        cur.execute(sql, params)
        conn.commit()
        return cur.lastrowid if lastrowid else None
    finally:
        if cur: cur.close()
        conn.close()


def init_db():
    """Create the database and tables if they don't exist."""
    # Connect without specifying the database first so we can CREATE it
    cfg = {k: v for k, v in DB_CONFIG.items() if k != "database"}
    conn = mysql.connector.connect(**cfg)
    cur = None
    try:
        cur = conn.cursor()
        cur.execute(f"CREATE DATABASE IF NOT EXISTS `{DB_CONFIG['database']}`")
        cur.execute(f"USE `{DB_CONFIG['database']}`")
        cur.execute("""
            CREATE TABLE IF NOT EXISTS telemetry (
                id         INT AUTO_INCREMENT PRIMARY KEY,
                dev_id     VARCHAR(64)  NOT NULL,
                lat        DOUBLE       NOT NULL,
                lon        DOUBLE       NOT NULL,
                speed_kmh  DOUBLE       NOT NULL,
                sos_active TINYINT(1)   NOT NULL,
                timestamp  DOUBLE       NOT NULL,
                INDEX idx_dev_ts (dev_id, timestamp)
            )
        """)
        cur.execute("""
            CREATE TABLE IF NOT EXISTS stop_events (
                id            INT AUTO_INCREMENT PRIMARY KEY,
                dev_id        VARCHAR(64)  NOT NULL,
                location_name VARCHAR(128) NOT NULL,
                lat           DOUBLE       NOT NULL,
                lon           DOUBLE       NOT NULL,
                arrived_at    DOUBLE       NOT NULL,
                duration_sec  DOUBLE,
                INDEX idx_dev (dev_id)
            )
        """)
        conn.commit()
    finally:
        if cur: cur.close()
        conn.close()


# ---------------------------------------------------------------------------
# Geofence helpers
# ---------------------------------------------------------------------------

def haversine_m(lat1, lon1, lat2, lon2) -> float:
    R = 6_371_000
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2) ** 2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def run_geofence(dev_id: str, lat: float, lon: float, ts: float):
    nearest, nearest_dist = None, float("inf")
    for stop in STOPS:
        d = haversine_m(lat, lon, stop["lat"], stop["lon"])
        if d < nearest_dist:
            nearest_dist, nearest = d, stop

    in_zone = nearest_dist <= GEOFENCE_RADIUS_M
    was = _active_at.get(dev_id)

    if in_zone:
        if was is None:
            event_id = execute(
                "INSERT INTO stop_events (dev_id, location_name, lat, lon, arrived_at, duration_sec) VALUES (%s,%s,%s,%s,%s,%s)",
                (dev_id, nearest["name"], nearest["lat"], nearest["lon"], ts, None),
                lastrowid=True,
            )
            _active_at[dev_id] = {"stop_name": nearest["name"], "arrived_at": ts, "event_id": event_id}

        elif was["stop_name"] != nearest["name"]:
            execute(
                "UPDATE stop_events SET duration_sec=%s WHERE id=%s",
                (round(ts - was["arrived_at"], 1), was["event_id"]),
            )
            event_id = execute(
                "INSERT INTO stop_events (dev_id, location_name, lat, lon, arrived_at, duration_sec) VALUES (%s,%s,%s,%s,%s,%s)",
                (dev_id, nearest["name"], nearest["lat"], nearest["lon"], ts, None),
                lastrowid=True,
            )
            _active_at[dev_id] = {"stop_name": nearest["name"], "arrived_at": ts, "event_id": event_id}
    else:
        if was is not None:
            execute(
                "UPDATE stop_events SET duration_sec=%s WHERE id=%s",
                (round(ts - was["arrived_at"], 1), was["event_id"]),
            )
            del _active_at[dev_id]


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
def dashboard_css():
    return send_from_directory(os.path.dirname(__file__), "dashboard.css")

@app.route("/dashboard.js")
def dashboard_js():
    return send_from_directory(os.path.dirname(__file__), "dashboard.js")


# ---------------------------------------------------------------------------
# Health check
# ---------------------------------------------------------------------------

@app.route("/health", methods=["GET"])
def health():
    row = query("SELECT COUNT(*) AS cnt FROM telemetry", fetch="one")
    return jsonify({"status": "ok", "records": row["cnt"], "time": time.time()}), 200


# ---------------------------------------------------------------------------
# Telemetry endpoints
# ---------------------------------------------------------------------------

@app.route("/telemetry", methods=["POST"])
def post_telemetry():
    # Auth: if DEVICE_TOKEN is configured, every POST must carry the matching header
    if DEVICE_TOKEN:
        if request.headers.get("X-Device-Token") != DEVICE_TOKEN:
            return jsonify({"status": "error", "message": "Unauthorized."}), 401

    data = request.get_json(silent=True)
    valid, error = validate(data, REQUIRED_TELEMETRY)
    if not valid:
        return jsonify({"status": "error", "message": error}), 400

    # Rate limit per device (checked after we have dev_id from body)
    if not _check_rate(data.get("dev_id", "")):
        return jsonify({"status": "error", "message": "Rate limit exceeded."}), 429
    if data["sos_active"] not in (0, 1):
        return jsonify({"status": "error", "message": "Field 'sos_active' must be 0 or 1."}), 400
    if not (-90 <= data["lat"] <= 90):
        return jsonify({"status": "error", "message": "Field 'lat' must be between -90 and 90."}), 400
    if not (-180 <= data["lon"] <= 180):
        return jsonify({"status": "error", "message": "Field 'lon' must be between -180 and 180."}), 400
    if data["speed_kmh"] < 0:
        return jsonify({"status": "error", "message": "Field 'speed_kmh' must be >= 0."}), 400

    ts = time.time()
    execute(
        "INSERT INTO telemetry (dev_id, lat, lon, speed_kmh, sos_active, timestamp) VALUES (%s,%s,%s,%s,%s,%s)",
        (data["dev_id"], data["lat"], data["lon"], data["speed_kmh"], data["sos_active"], ts),
    )
    run_geofence(data["dev_id"], data["lat"], data["lon"], ts)
    return jsonify({"status": "ok", "timestamp": ts}), 201


@app.route("/telemetry/latest", methods=["GET"])
def get_latest():
    dev_id = request.args.get("dev_id")
    if not dev_id:
        return jsonify({"status": "error", "message": "Query parameter 'dev_id' is required."}), 400
    row = query(
        "SELECT * FROM telemetry WHERE dev_id=%s ORDER BY timestamp DESC LIMIT 1",
        (dev_id,), fetch="one"
    )
    if not row:
        return jsonify({"status": "error", "message": f"No records found for dev_id '{dev_id}'."}), 404
    return jsonify({"status": "ok", "data": row}), 200


@app.route("/telemetry/history", methods=["GET"])
def get_history():
    dev_id = request.args.get("dev_id")
    if not dev_id:
        return jsonify({"status": "error", "message": "Query parameter 'dev_id' is required."}), 400
    rows = query(
        "SELECT * FROM telemetry WHERE dev_id=%s ORDER BY timestamp DESC LIMIT 50",
        (dev_id,)
    )
    return jsonify({"status": "ok", "data": rows}), 200


@app.route("/telemetry/all-latest", methods=["GET"])
def get_all_latest():
    """Return the most recent telemetry record for every known device."""
    rows = query(
        """SELECT t.* FROM telemetry t
           INNER JOIN (
             SELECT dev_id, MAX(timestamp) AS ts FROM telemetry GROUP BY dev_id
           ) m ON t.dev_id = m.dev_id AND t.timestamp = m.ts
           ORDER BY t.timestamp DESC"""
    )
    return jsonify({"status": "ok", "data": rows}), 200


@app.route("/telemetry/devices", methods=["GET"])
def get_devices():
    rows = query(
        "SELECT dev_id, MAX(timestamp) AS last_seen, COUNT(*) AS total FROM telemetry GROUP BY dev_id ORDER BY last_seen DESC"
    )
    return jsonify({"status": "ok", "data": rows}), 200


# ---------------------------------------------------------------------------
# Stop event endpoints
# ---------------------------------------------------------------------------

@app.route("/telemetry/stops/config", methods=["GET"])
def get_stops_config():
    return jsonify({"status": "ok", "data": STOPS, "radius_m": GEOFENCE_RADIUS_M}), 200


@app.route("/telemetry/stops", methods=["GET"])
def get_stops():
    dev_id = request.args.get("dev_id")
    if not dev_id:
        return jsonify({"status": "error", "message": "Query parameter 'dev_id' is required."}), 400
    rows = query(
        "SELECT * FROM stop_events WHERE dev_id=%s ORDER BY arrived_at DESC",
        (dev_id,)
    )
    return jsonify({"status": "ok", "data": rows}), 200


@app.route("/telemetry/stats", methods=["GET"])
def get_stats():
    dev_id = request.args.get("dev_id")
    if not dev_id:
        return jsonify({"status": "error", "message": "Query parameter 'dev_id' is required."}), 400
    row = query(
        """SELECT COUNT(*) AS total,
                  ROUND(AVG(speed_kmh), 1) AS avg_speed,
                  ROUND(MAX(speed_kmh), 1) AS max_speed,
                  MIN(timestamp) AS first_seen,
                  MAX(timestamp) AS last_seen
           FROM telemetry WHERE dev_id=%s""",
        (dev_id,), fetch="one"
    )
    if not row or not row["total"]:
        return jsonify({"status": "error", "message": f"No records for dev_id '{dev_id}'."}), 404
    return jsonify({"status": "ok", "data": row}), 200


@app.route("/telemetry/stops", methods=["POST"])
def post_stop():
    data = request.get_json(silent=True)
    valid, error = validate(data, REQUIRED_STOP)
    if not valid:
        return jsonify({"status": "error", "message": error}), 400
    execute(
        "INSERT INTO stop_events (dev_id, location_name, lat, lon, arrived_at, duration_sec) VALUES (%s,%s,%s,%s,%s,%s)",
        (data["dev_id"], data["location_name"], data["lat"], data["lon"],
         data["arrived_at"], data.get("duration_sec")),
    )
    return jsonify({"status": "ok"}), 201


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    init_db()
    print("=" * 50)
    print("  Telematics Backend — Module 4 (Sri Janani)")
    print("  Database : MySQL —", DB_CONFIG["host"], "/", DB_CONFIG["database"])
    print("=" * 50)
    print(f"  Dashboard: http://0.0.0.0:5000/")
    print(f"  Health   : http://0.0.0.0:5000/health")
    print("=" * 50)
    app.run(host="0.0.0.0", port=5000, debug=True)
