"""
WSGI entrypoint for production (gunicorn).

Purpose
    app.py calls init_db() only inside its `if __name__ == "__main__"` block,
    which gunicorn never executes. Without this file the MySQL schema would
    never be created in a containerised deployment. This module:
      1. imports the Flask `app` object for gunicorn (`wsgi:app`)
      2. creates the database + tables (init_db) before serving
      3. retries the DB connection at boot, so the app can start alongside a
         freshly-provisioned MySQL container that is still coming up.

    ngrok is intentionally NOT started here — on a real VPS the public domain
    (served by Coolify/Traefik) replaces the ngrok tunnel. Leave NGROK_AUTHTOKEN
    empty in the environment.

Run with a SINGLE worker (see gunicorn_conf.py) — the app holds live trip /
geofence / rate-limit state in memory.

Input:  environment variables (MYSQL_HOST/USER/PASSWORD/DATABASE, DEVICE_TOKEN)
Output: a WSGI `app` callable ready for gunicorn, with the schema guaranteed.
"""

import time
import mysql.connector

from app import app, init_db  # noqa: F401  (app is imported for gunicorn)


def _init_with_retry(attempts: int = 30, delay: float = 2.0) -> None:
    """Create schema, retrying while MySQL finishes starting up."""
    last_err = None
    for i in range(1, attempts + 1):
        try:
            init_db()
            print(f"[wsgi] Database schema ready (attempt {i}).", flush=True)
            return
        except mysql.connector.Error as e:
            last_err = e
            print(f"[wsgi] MySQL not ready (attempt {i}/{attempts}): {e}", flush=True)
            time.sleep(delay)
    raise RuntimeError(
        f"Database unreachable after {attempts} attempts — check MYSQL_* env vars. "
        f"Last error: {last_err}"
    )


_init_with_retry()
