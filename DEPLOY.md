# Deployment Guide — Fleet Telematics on Coolify

Deploys the Flask backend + dashboard to your VPS via Coolify, backed by a
Coolify-managed MySQL, on a subdomain (e.g. `fleet.yourdomain.com`), and points
the ESP32 firmware at it. The dashboard and the device API share the one domain.

```
ESP32 (4G) ──HTTPS──> fleet.yourdomain.com ──> Coolify / Traefik (Let's Encrypt TLS)
                                                  │
                                                  ├── App: Flask + gunicorn (:5000)
                                                  │      /            → dashboard
                                                  │      /telemetry   → device ingest
                                                  └── MySQL 8 (managed, persistent volume)
```

## What was added for deployment

| File | Purpose |
|---|---|
| `telematics_backend/Dockerfile` | Builds the app image, runs gunicorn |
| `telematics_backend/wsgi.py` | Gunicorn entrypoint; creates the DB schema on boot with retry (app.py only did this under `python app.py`) |
| `telematics_backend/gunicorn_conf.py` | **1 worker + threads** — required because trip/geofence/rate-limit state is in-memory |
| `telematics_backend/.env.example` | Template for the env vars to set in Coolify |
| `telematics_backend/.dockerignore` | Keeps `.env` and cruft out of the image |
| `requirements.txt` | Added `gunicorn` |

`app.py` itself was **not modified**.

---

## Step 1 — Push the code to Git

Coolify deploys from a Git repo. Push this repository (including the new files
above) to your Git remote and note the branch (e.g. `main`).

## Step 2 — DNS

Point the subdomain at your VPS:

```
Type: A     Name: fleet     Value: <your VPS public IP>
```

Wait for it to resolve before requesting the certificate in Coolify.

## Step 3 — Create the MySQL database in Coolify

1. In your Coolify project → **+ New** → **Database** → **MySQL** (8.x).
2. Deploy it. Coolify generates the root password and an internal hostname.
3. From the database's **Environment / Connection** details, note:
   - internal **hostname** (use this as `MYSQL_HOST`, e.g. `telematics-mysql`)
   - **root password**
4. Ensure the app and the database are in the **same Coolify project** so they
   share the internal network.

> Using the **root** user for `MYSQL_USER` on first boot is simplest: `wsgi.py`
> runs `CREATE DATABASE IF NOT EXISTS` + `CREATE TABLE`, which needs schema
> privileges. You can switch to a scoped user later once the schema exists.

## Step 4 — Create the App (backend) in Coolify

1. **+ New** → **Application** → **Public/Private Repository** → select this repo.
2. **Build Pack: Dockerfile.**
3. **Base Directory:** `/telematics_backend` (the Dockerfile lives here).
4. **Port / Ports Exposes:** `5000`.
5. **Domain:** `https://fleet.yourdomain.com` → Coolify auto-provisions Let's
   Encrypt TLS via Traefik.=
6. **Health check path:** `/health` (optional but recommended).

## Step 5 — Environment variables (App → Environment Variables)

From `telematics_backend/.env.example`:

```
MYSQL_HOST=<coolify mysql internal hostname>
MYSQL_USER=root
MYSQL_PASSWORD=<coolify mysql root password>
MYSQL_DATABASE=telematics
DEVICE_TOKEN=<long random token>      # remember this for the firmware
NGROK_AUTHTOKEN=                      # leave EMPTY
```

## Step 6 — Deploy & verify

1. Click **Deploy**. Watch logs for `[wsgi] Database schema ready`.
2. Open `https://fleet.yourdomain.com/health` → expect `{"status":"ok", ...}`.
3. Open `https://fleet.yourdomain.com/` → dashboard loads (shows 🔴 Offline
   until a device posts — that's expected).
4. Smoke-test ingestion from your machine (replace token):

   ```bash
   curl -X POST https://fleet.yourdomain.com/telemetry \
     -H "Content-Type: application/json" \
     -H "Token: <DEVICE_TOKEN>" \
     -d '{"dev_id":"BUS01","lat":13.0827,"lon":80.2707,"speed_kmh":42,"sos_active":0}'
   ```
   Expect `201 {"status":"ok", ...}`. The dashboard should now show `BUS01`.

## Step 7 — (One-time) configure stops & routes

Geofencing/trip tracking need at least one stop and route (DB-driven, no
hardcoding):

```bash
# a geofence stop
curl -X POST https://fleet.yourdomain.com/stops/config \
  -H "Content-Type: application/json" \
  -d '{"name":"Warehouse Alpha","lat":13.0827,"lon":80.2707,"radius_m":300}'

# a route (>= 2 ordered stops)
curl -X POST https://fleet.yourdomain.com/routes/config \
  -H "Content-Type: application/json" \
  -d '{"route_key":"r1","name":"Route 1","stops":[
        {"name":"Start","lat":13.0827,"lon":80.2707},
        {"name":"End","lat":13.0600,"lon":80.2500}]}'
```

---

## Step 8 — Point the ESP32 at your domain

Edit `BusTracking/ESP32/bus_final/bus_final.ino`:

```cpp
// line 61 — swap the ngrok host for your subdomain (NO https://, NO trailing slash)
#define NGROK_HOST    "fleet.yourdomain.com"

// line 67 — must equal DEVICE_TOKEN set in Coolify
#define DEVICE_TOKEN  "<the same long random token>"
```

Then reflash from the Arduino IDE (Board: *ESP32 Dev Module*, 115200 baud
serial). No other firmware changes are needed:

- The firmware already POSTs over **HTTPS to port 443** (`https://<host>/telemetry`).
- Its TLS is set to accept the server cert without pinning
  (`AT+CSSLCFG "authmode",0,0`), so Coolify's Let's Encrypt cert works as-is.
- The leftover `ngrok-skip-browser-warning` header it sends is harmless against
  a real domain (optional cleanup: remove it in `http_post()`).

On the Serial Monitor you should see `[HTTP] POST OK  HTTP 201`, and the bus
appears live on the dashboard.

---

## Notes & gotchas

- **Do not raise gunicorn workers above 1** without moving in-memory state
  (trips, geofence, rate-limit, caches) into MySQL/Redis — see
  `gunicorn_conf.py` for the reasoning.
- **Rate limit** is 2 requests/sec per `dev_id`; the firmware posts every 5 s, so
  it's well within budget.
- **MySQL persistence:** confirm the Coolify MySQL resource has a persistent
  volume so telemetry survives redeploys.
- **Backups:** enable scheduled backups on the Coolify MySQL resource if this is
  more than a throwaway demo.
- To later lock down the DB user: create a scoped user with full privileges on
  the `telematics` schema and swap `MYSQL_USER`/`MYSQL_PASSWORD` once the tables
  exist.
