# ---------------------------------------------------------------------------
# Telematics Backend — production image for Coolify / VPS
# Flask app served by gunicorn. Schema init + DB-wait handled in wsgi.py.
# ---------------------------------------------------------------------------
FROM python:3.12-slim

# Faster, quieter, no .pyc clutter
ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PIP_NO_CACHE_DIR=1

WORKDIR /app

# Install deps first for better layer caching
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# App code (dashboard.html/js/css, app.py, wsgi.py, gunicorn_conf.py)
COPY . .

EXPOSE 5000

# IMPORTANT: single worker (see gunicorn_conf.py — in-memory trip/geofence state)
CMD ["gunicorn", "--config", "gunicorn_conf.py", "wsgi:app"]
