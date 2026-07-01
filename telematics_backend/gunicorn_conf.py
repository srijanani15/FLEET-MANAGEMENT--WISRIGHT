"""
Gunicorn configuration — Telematics Backend (Module 4).

WHY workers = 1
    app.py keeps live state in module-level dicts:
      - _active_trips / _active_at / _in_mandatory_stop  (trip + geofence state)
      - _rate_buckets                                     (per-device rate limiting)
      - _stops_cache / _routes_cache                      (60 s config caches)
    Each gunicorn worker is a separate process with its OWN copy of these dicts.
    Running >1 worker would desync trip tracking and geofence events depending on
    which worker a request lands on. We therefore run a SINGLE worker and use
    threads for concurrency. Each DB query opens/closes its own connection
    (see get_db/query/execute), and the caches/rate-limiter use locks, so the
    threaded model is safe.

    This is fine for a prototype / small fleet. To scale horizontally later,
    move that in-memory state into MySQL or Redis, then raise the worker count.
"""

bind = "0.0.0.0:5000"
workers = 1
threads = 8
timeout = 120
graceful_timeout = 30
keepalive = 5

# Log to stdout/stderr so Coolify captures them
accesslog = "-"
errorlog = "-"
loglevel = "info"
