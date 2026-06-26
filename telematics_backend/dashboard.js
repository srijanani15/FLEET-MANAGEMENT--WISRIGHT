/* ═══════════════════════════════════════════════════════════════
   FleetTrack Dashboard — 100% live backend data
   No simulation. All state comes from /telemetry/all-latest.
═══════════════════════════════════════════════════════════════ */

// Auto-detect backend: same host as the page (works for localhost AND LAN IP)
const BACKEND = window.location.port
  ? `${window.location.protocol}//${window.location.hostname}:${window.location.port}`
  : `${window.location.protocol}//${window.location.hostname}`;
let backendOnline = false;

/* ── Bus display metadata (config only — not data) ── */
const BMETA = {
  'VTUESP32-0091':{num:'Bus 91',route:'Avadi → Anna Nagar → Central',    color:'#58a6ff',trip:'8am'},
  'VTUESP32-0092':{num:'Bus 92',route:'Central → Perambur → Koyambedu', color:'#3fb950',trip:'8am'},
  'VTUESP32-0093':{num:'Bus 93',route:'Adyar → Guindy → Koyambedu',     color:'#d29922',trip:'3pm'},
  'VTUESP32-0094':{num:'Bus 94',route:'T Nagar → Egmore → Central',     color:'#f85149',trip:'3pm'},
  'VTUESP32-0095':{num:'Bus 95',route:'Avadi → Porur → Vadapalani',     color:'#f97316',trip:'8am'},
  'VTUESP32-0096':{num:'Bus 96',route:'Adyar → Guindy → Porur → Avadi',color:'#ec4899',trip:'3pm'},
};

/* ── Live state cache — filled only by syncFromAPI ── */
const sim = {};
Object.keys(BMETA).forEach(id => {
  sim[id] = {
    id, lat: null, lon: null, speed: 0, sos: 0,
    geo: null, stop: false, stopSince: null,
    ts: null, trail: [], lastUpdate: 0
  };
});

let selFilter = 'all', curList = 'all', curBus = null, tickId = null;

/* ═══════════════════════════════
   GEOFENCES — loaded from backend
═══════════════════════════════ */
let GEO = [
  {id:'chennai_central',   name:'Chennai Central',   lat:13.0827,lon:80.2707,r:300},
  {id:'egmore',            name:'Egmore',            lat:13.0784,lon:80.2617,r:300},
  {id:'royapettah',        name:'Royapettah',        lat:13.0524,lon:80.2623,r:300},
  {id:'t_nagar_bus_stand', name:'T Nagar Bus Stand', lat:13.0418,lon:80.2341,r:300},
  {id:'vadapalani',        name:'Vadapalani',        lat:13.0524,lon:80.2121,r:300},
  {id:'anna_nagar',        name:'Anna Nagar',        lat:13.0850,lon:80.2101,r:300},
  {id:'guindy',            name:'Guindy',            lat:13.0067,lon:80.2206,r:300},
  {id:'adyar',             name:'Adyar',             lat:13.0012,lon:80.2565,r:300},
  {id:'koyambedu',         name:'Koyambedu',         lat:13.0694,lon:80.1948,r:300},
  {id:'perambur',          name:'Perambur',          lat:13.1175,lon:80.2479,r:300},
  {id:'avadi',             name:'Avadi',             lat:13.1132,lon:80.1050,r:300},
  {id:'porur_junction',    name:'Porur Junction',    lat:13.0359,lon:80.1569,r:300},
];
let geoLayerGroup = null;

async function loadStopsConfig() {
  try {
    const r = await fetch(`${BACKEND}/telemetry/stops/config`, {signal: AbortSignal.timeout(3000)});
    if (!r.ok) return;
    const d = await r.json();
    const radius = d.radius_m || 300;
    GEO = (d.data || []).map(s => ({
      id: s.name.toLowerCase().replace(/\s+/g, '_'),
      name: s.name, lat: s.lat, lon: s.lon, r: radius
    }));
    if (mapInit) refreshMapGeofences();
  } catch {}
}

function refreshMapGeofences() {
  if (!lmap) return;
  if (geoLayerGroup) geoLayerGroup.clearLayers();
  else { geoLayerGroup = L.layerGroup().addTo(lmap); }
  GEO.forEach(g => {
    L.circle([g.lat, g.lon], {radius: g.r, color:'#7b2d8b', fillColor:'#7b2d8b', fillOpacity:.07, weight:1.5, dashArray:'6 4'})
      .addTo(geoLayerGroup).bindPopup(`<b>${g.name}</b>`);
    L.circleMarker([g.lat, g.lon], {radius:4, color:'#7b2d8b', fillColor:'#7b2d8b', fillOpacity:.7, weight:2})
      .addTo(geoLayerGroup).bindTooltip(g.name, {direction:'top'});
  });
}

/* ═══════════════════════════════
   MATH HELPERS
═══════════════════════════════ */
function hav(a, b, c, d) {
  const R=6371000, ra=Math.PI/180, dA=(c-a)*ra, dB=(d-b)*ra;
  const x = Math.sin(dA/2)**2 + Math.cos(a*ra)*Math.cos(c*ra)*Math.sin(dB/2)**2;
  return R * 2 * Math.atan2(Math.sqrt(x), Math.sqrt(1-x));
}
function chkGeo(la, lo) { return GEO.find(g => hav(la, lo, g.lat, g.lon) <= g.r) || null; }

// US-05 / US-28: speed colour thresholds
function sclr(s) { return s > 70 ? '#f85149' : s > 40 ? '#d29922' : '#3fb950'; }
function spct(s) { return Math.min(100, Math.round(s / 80 * 100)); }

/* ═══════════════════════════════
   REAL BACKEND SYNC (only data source)
═══════════════════════════════ */
async function syncFromAPI() {
  try {
    const r = await fetch(`${BACKEND}/telemetry/all-latest`, {signal: AbortSignal.timeout(2500)});
    if (!r.ok) throw new Error('bad status');
    const rows = (await r.json()).data || [];

    if (!backendOnline) {
      backendOnline = true;
      const badge = document.getElementById('backendBadge');
      if (badge) { badge.textContent = '🟢 Live'; badge.style.color = '#16a34a'; }
    }

    rows.forEach(t => {
      const id = t.dev_id;
      // Auto-register any new device arriving from hardware
      if (!sim[id]) {
        sim[id] = {id, lat: null, lon: null, speed: 0, sos: 0,
                   geo: null, stop: false, stopSince: null,
                   ts: null, trail: [], lastUpdate: 0};
        if (!BMETA[id]) BMETA[id] = {
          num: id.replace('VTUESP32-0', 'Bus '),
          route: 'Live GPS Device', color: '#a5b4fc', trip: '8am'
        };
      }

      sim[id].lat   = t.lat;
      sim[id].lon   = t.lon;
      sim[id].speed = parseFloat(t.speed_kmh) || 0;
      sim[id].sos   = (t.sos_active && !sosAcknowledged.has(id)) ? 1 : 0;
      sim[id].ts    = new Date(t.timestamp * 1000).toISOString();
      sim[id].stop  = sim[id].speed < 6;
      sim[id].lastUpdate = Date.now();

      const gf = chkGeo(t.lat, t.lon);
      sim[id].geo = gf || null;

      if (sim[id].speed < 6) { if (!sim[id].stopSince) sim[id].stopSince = Date.now(); }
      else sim[id].stopSince = null;

      // Accumulate GPS trail (cap at 120 points)
      sim[id].trail.push([t.lat, t.lon]);
      if (sim[id].trail.length > 120) sim[id].trail.shift();
    });
  } catch {
    if (backendOnline) {
      backendOnline = false;
      const badge = document.getElementById('backendBadge');
      if (badge) { badge.textContent = '🔴 Offline'; badge.style.color = '#B91C1C'; }
    }
  }
}
setInterval(syncFromAPI, 3000);
syncFromAPI();
loadStopsConfig();

let _stopFetchController = null;

async function fetchStopEvents(id) {
  if (_stopFetchController) _stopFetchController.abort();
  _stopFetchController = new AbortController();
  const signal = AbortSignal.any
    ? AbortSignal.any([_stopFetchController.signal, AbortSignal.timeout(2000)])
    : _stopFetchController.signal;
  try {
    const r = await fetch(`${BACKEND}/telemetry/stops?dev_id=${id}`, {signal});
    if (!r.ok) return null;
    return (await r.json()).data || [];
  } catch { return null; }
}

/* ═══════════════════════════════
   VIEW SWITCH
═══════════════════════════════ */
function showV(id) {
  document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));
  document.getElementById(id).classList.add('active');
}

/* ═══════════════════════════════
   HOME
═══════════════════════════════ */
function tick() {
  const n = new Date();
  document.getElementById('homeClock').textContent =
    n.toLocaleTimeString('en-IN', {hour:'2-digit', minute:'2-digit', second:'2-digit', hour12:true});
  const dateEl = document.getElementById('homeDate');
  if (dateEl) {
    const days = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
    const months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    dateEl.textContent = `${days[n.getDay()]}, ${n.getDate()} ${months[n.getMonth()]} ${n.getFullYear()}`;
  }
  const all = Object.values(sim).filter(b => b.lastUpdate > 0);
  document.getElementById('sMoving').textContent  = all.filter(b => !b.stop && !b.sos).length;
  document.getElementById('sStopped').textContent = all.filter(b =>  b.stop && !b.sos).length;
  document.getElementById('sSos').textContent     = all.filter(b =>  b.sos).length;
}
setInterval(tick, 1000); tick();

function buildDates() {
  const row = document.getElementById('dateRow');
  const today = new Date();
  const ord = n => { const s=['th','st','nd','rd'], v=n%100; return n+(s[(v-20)%10]||s[v]||s[0]); };
  const days = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
  const dow = today.getDay();
  const mon = new Date(today); mon.setDate(today.getDate() - (dow === 0 ? 6 : dow - 1));

  const hbtn = document.createElement('button');
  hbtn.textContent = 'TODAY'; hbtn.className = 'dr-home dr-sel';
  hbtn.onclick = () => {
    row.querySelectorAll('button').forEach(b => b.classList.remove('dr-sel'));
    hbtn.classList.add('dr-sel'); buildTrips(today);
  };
  row.appendChild(hbtn);

  for (let i = 0; i < 8; i++) {
    const d = new Date(mon); d.setDate(mon.getDate() + i);
    const isToday = d.toDateString() === today.toDateString();
    const btn = document.createElement('button');
    if (isToday) btn.className = 'dr-today';
    btn.innerHTML = `${ord(d.getDate())} ${days[d.getDay()]}`;
    btn.onclick = (function(dd, b) { return () => {
      row.querySelectorAll('button').forEach(x => x.classList.remove('dr-sel'));
      b.classList.add('dr-sel'); buildTrips(dd);
    };})(d, btn);
    row.appendChild(btn);
  }
  buildTrips(today);
}

const STRIP_COLORS = {
  'VTUESP32-0091':'red','VTUESP32-0092':'grn',
  'VTUESP32-0093':'blue','VTUESP32-0094':'red',
  'VTUESP32-0095':'amb','VTUESP32-0096':'pnk'
};

/* Strip progress bar = speed percentage (0 → 80 km/h scale).
   Shows real speed from backend; no fake route progress. */
function stripHtml(id) {
  const b = sim[id], m = BMETA[id];
  const hasData = b.lastUpdate > 0;
  const clr = STRIP_COLORS[id] || 'red';
  const spdPct = hasData ? spct(b.speed) : 0;
  const spdTxt = b.sos ? '🚨 SOS' : hasData ? (b.speed + ' km/h') : 'No signal';
  const spdClass = b.speed > 70 ? 'fast' : b.speed > 40 ? 'mid' : '';
  return `<div class="tc-strip" id="hs-${id}">
    <div class="tc-strip-label">${m.num}</div>
    <div class="tc-strip-wrap">
      <div class="tc-strip-track"></div>
      <div class="tc-strip-fill ${clr}" id="hsfill-${id}" style="width:${spdPct}%"></div>
      <div class="tc-strip-bus" id="hsbus-${id}" style="left:${Math.min(93,spdPct)}%">🚌</div>
    </div>
    <div class="tc-strip-spd ${spdClass}" id="hsspd-${id}">${spdTxt}</div>
  </div>`;
}

function buildTrips(date) {
  const mn = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
  const dn = ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];
  const sec = document.getElementById('tripsSection');
  const wknd = date.getDay() === 0 || date.getDay() === 6;
  const label = `${dn[date.getDay()]}, ${date.getDate()} ${mn[date.getMonth()]} ${date.getFullYear()}`;
  const amBuses = Object.keys(BMETA).filter(id => BMETA[id].trip === '8am');
  const pmBuses = Object.keys(BMETA).filter(id => BMETA[id].trip === '3pm');
  const sosId = Object.keys(BMETA).find(id => sim[id]?.sos);

  if (wknd) {
    sec.innerHTML = `<div class="trips-title">${label}</div>
    <div class="weekend-box"><div class="wb-icon">🏖️</div><p>No bus service on weekends.</p></div>`;
    return;
  }

  const sosBanner = sosId ? `<div class="sos-home-banner">
    <div class="sos-home-banner-icon">🚨</div>
    <div class="sos-home-banner-text">
      <div class="sos-home-banner-title">${BMETA[sosId].num} — SOS alert active</div>
      <div class="sos-home-banner-sub">${BMETA[sosId].route} · Dispatcher notified via SMS</div>
    </div>
    <button class="sos-home-banner-btn" onclick="openTracker('${sosId}')">View Live →</button>
  </div>` : '';

  const offlineBanner = !backendOnline ? `<div class="sos-home-banner" style="border-color:#6b7280;background:#1a1f2e">
    <div class="sos-home-banner-icon">📡</div>
    <div class="sos-home-banner-text">
      <div class="sos-home-banner-title" style="color:#9ca3af">Backend offline</div>
      <div class="sos-home-banner-sub" style="color:#6b7280">Waiting for connection to ${BACKEND}</div>
    </div>
  </div>` : '';

  sec.innerHTML = `
    ${offlineBanner}${sosBanner}
    <div class="trips-title" style="margin-top:${(sosId||!backendOnline)?'16px':'0'}">${label} — Scheduled Trips</div>
    <div class="trips-grid">
      <div class="trip-card morning">
        <div class="trip-card-bar"></div>
        <div class="trip-card-body">
          <div class="tc-top"><div class="tc-icon">🌅</div><span class="tc-badge badge-am">8:00 AM</span></div>
          <div class="tc-title">Morning to College</div>
          <div class="tc-time">${amBuses.length} buses · Live GPS</div>
          <div class="tc-meta">
            <div class="tc-chip">🚌 <b>${amBuses.length}</b> Buses</div>
            <div class="tc-chip">📍 Live GPS</div>
          </div>
          <div class="tc-strips">${amBuses.map(stripHtml).join('')}</div>
          <button class="tc-btn am-btn" onclick="showBusList('8am','Morning to College — 8:00 AM')">View All Buses &amp; Track →</button>
        </div>
      </div>
      <div class="trip-card return">
        <div class="trip-card-bar"></div>
        <div class="trip-card-body">
          <div class="tc-top"><div class="tc-icon">🌆</div><span class="tc-badge badge-pm">3:00 PM</span></div>
          <div class="tc-title">Return from College</div>
          <div class="tc-time">${pmBuses.length} buses · Live GPS</div>
          <div class="tc-meta">
            <div class="tc-chip">🚌 <b>${pmBuses.length}</b> Buses</div>
            <div class="tc-chip">📍 Live GPS</div>
          </div>
          <div class="tc-strips">${pmBuses.map(stripHtml).join('')}</div>
          <button class="tc-btn pm-btn" onclick="showBusList('3pm','Return from College — 3:00 PM')">View All Buses &amp; Track →</button>
        </div>
      </div>
    </div>`;
}

function updateHomeStrips() {
  if (!document.getElementById('homeView').classList.contains('active')) return;
  Object.keys(BMETA).forEach(id => {
    const b = sim[id];
    const hasData = b && b.lastUpdate > 0;
    const spdPct = hasData ? spct(b.speed) : 0;
    const fillEl = document.getElementById('hsfill-' + id);
    const busEl  = document.getElementById('hsbus-'  + id);
    const spdEl  = document.getElementById('hsspd-'  + id);
    if (!fillEl || !busEl || !spdEl) return;
    fillEl.style.width = spdPct + '%';
    busEl.style.left   = Math.min(93, spdPct) + '%';
    const spdClass = b.speed > 70 ? 'fast' : b.speed > 40 ? 'mid' : '';
    spdEl.textContent = b.sos ? '🚨 SOS' : hasData ? (b.speed + ' km/h') : 'No signal';
    spdEl.className = 'tc-strip-spd ' + spdClass;
  });
}
setInterval(updateHomeStrips, 3000);

// US-20: search by bus number / route
function doSearch() {
  const v = document.getElementById('srchIn').value.trim().toLowerCase();
  if (v.length < 2) { alert('Please enter at least 2 characters.'); return; }
  const exact = Object.keys(BMETA).find(id =>
    BMETA[id].num.toLowerCase().includes(v) ||
    BMETA[id].route.toLowerCase().includes(v) ||
    id.toLowerCase().includes(v)
  );
  if (exact) { openTracker(exact); return; }
  showBusList('all', 'Search: "' + v + '"');
}

document.addEventListener('DOMContentLoaded', () => {
  const inp = document.getElementById('srchIn');
  if (inp) inp.addEventListener('keydown', e => { if (e.key === 'Enter') doSearch(); });
});

function goHome() {
  if (tickId) { clearInterval(tickId); tickId = null; }
  location.hash = '';
  showV('homeView');
}

buildDates();

/* ═══════════════════════════════
   BUS LIST
═══════════════════════════════ */
function showBusList(trip, title) {
  curList = trip; selFilter = 'all';
  document.querySelectorAll('.ftag').forEach((b, i) => b.classList.toggle('on', i === 0));
  document.getElementById('listTitle').textContent = title;
  renderTable(trip, 'all');
  showV('busListView');
}

function applyFilter(f, btn) {
  selFilter = f;
  document.querySelectorAll('.ftag').forEach(b => b.classList.remove('on'));
  btn.classList.add('on');
  renderTable(curList, f);
}

// US-28: overspeed badge  US-30: offline detection
function renderTable(trip, f) {
  let rows = Object.values(sim).filter(b => {
    if (trip === '8am') return BMETA[b.id]?.trip === '8am';
    if (trip === '3pm') return BMETA[b.id]?.trip === '3pm';
    return true;
  });
  if (f === 'moving')  rows = rows.filter(b => !b.stop && !b.sos && b.lastUpdate > 0);
  if (f === 'stopped') rows = rows.filter(b =>  b.stop && !b.sos && b.lastUpdate > 0);
  if (f === 'sos')     rows = rows.filter(b => !!b.sos);

  const body = document.getElementById('busTbody');
  if (!rows.length) {
    body.innerHTML = '<tr><td colspan="6" style="text-align:center;padding:18px;color:#7b2d8b">No buses match this filter.</td></tr>';
    return;
  }

  const now = Date.now();
  body.innerHTML = rows.map(b => {
    const m = BMETA[b.id];
    const hasData     = b.lastUpdate > 0;
    const isSos       = !!b.sos;
    const isOffline   = !hasData || (now - b.lastUpdate) > 30000;
    const isOverspeed = b.speed > 70;
    const sc  = sclr(b.speed);
    const dotCls = isSos ? 'sdot-sos' : isOffline ? 'sdot-off' : b.stop ? 'sdot-st' : 'sdot-mv';
    const stTxt  = isSos ? '⚠ SOS'   : isOffline ? 'No signal' : b.stop ? 'Stopped' : 'Moving';
    const geo    = b.geo ? b.geo.name : hasData ? 'En route' : '—';
    const overspeedBadge = isOverspeed ? `<span class="overspeed-badge">OVERSPEED</span>` : '';
    return `<tr class="${isOffline ? 'offline-row' : ''}">
      <td><b>${m.num}</b></td>
      <td style="max-width:150px;font-size:.76rem;color:#a8a29e">${m.route}</td>
      <td><span class="sdot ${dotCls}"></span>${stTxt}</td>
      <td><b style="color:${sc}">${hasData ? b.speed : '—'}</b>${hasData ? ' km/h' : ''} ${overspeedBadge}</td>
      <td style="font-size:.74rem;color:#a8a29e">${
        isOffline
          ? `<span style="color:#6b7280;font-size:.7rem">Awaiting GPS signal…</span>`
          : geo + (() => { const e = etaToNextStop(b); return e ? `<br><span style="color:#58a6ff;font-size:.68rem">~${e} min to next stop</span>` : ''; })()
      }</td>
      <td><button class="trk-btn${isSos ? ' sos' : ''}" onclick="openTracker('${b.id}')">📍 Track</button></td>
    </tr>`;
  }).join('');
}

setInterval(() => {
  if (document.getElementById('busListView').classList.contains('active')) renderTable(curList, selFilter);
}, 3000);

/* ═══════════════════════════════
   TRACKER
═══════════════════════════════ */
let lmap = null, lmarker = null, ltrail = null, mapInit = false;

function ensureMap() {
  if (mapInit) return;
  lmap = L.map('liveMap', {zoomControl: true}).setView([13.05, 80.22], 13);
  L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',
    {subdomains:'abcd', attribution:'© OpenStreetMap', maxZoom:19}).addTo(lmap);
  geoLayerGroup = L.layerGroup().addTo(lmap);
  refreshMapGeofences();
  ltrail = L.polyline([], {color:'#58a6ff', weight:3, opacity:.5, dashArray:'5 5'}).addTo(lmap);
  mapInit = true;
}

function busIcon(color, sos) {
  const c = sos ? '#f85149' : color;
  return L.divIcon({className:'', html:`<div style="position:relative;width:44px;height:44px">
    <svg viewBox="0 0 44 44" width="44" height="44">
      <rect x="4" y="10" width="36" height="24" rx="5" fill="${c}" opacity=".95"/>
      <rect x="7"  y="14" width="10" height="8" rx="1.5" fill="white" opacity=".9"/>
      <rect x="20" y="14" width="10" height="8" rx="1.5" fill="white" opacity=".9"/>
      <circle cx="12" cy="36" r="4" fill="#111"/>
      <circle cx="32" cy="36" r="4" fill="#111"/>
      ${sos ? '<rect x="15" y="3" width="14" height="8" rx="2" fill="#f85149"/><text x="22" y="9.5" text-anchor="middle" font-size="5.5" font-weight="bold" fill="white" font-family="sans-serif">SOS</text>' : ''}
    </svg>
    ${sos ? '<div style="position:absolute;top:0;right:0;width:10px;height:10px;background:#f85149;border-radius:50%;border:2px solid #0d1117;animation:pa .8s infinite"></div>' : ''}
  </div>`, iconSize:[44,44], iconAnchor:[22,40], popupAnchor:[0,-40]});
}

// US-21: ETA — nearest stop ahead, using real GPS speed and stop positions
function etaToNextStop(b) {
  if (b.lat === null || b.speed < 2) return null;
  let best = null, bestDist = Infinity;
  GEO.forEach(g => {
    if (b.geo && g.name === b.geo.name) return; // skip stop we're currently at
    const d = hav(b.lat, b.lon, g.lat, g.lon);
    if (d < bestDist) { bestDist = d; best = g; }
  });
  if (!best) return null;
  return Math.max(1, Math.round(bestDist / (b.speed * 1000 / 60)));
}

// US-25: copy tracker link to clipboard
function shareTracker() {
  const url = window.location.href;
  if (navigator.clipboard) {
    navigator.clipboard.writeText(url).then(() => {
      const btn = document.querySelector('.share-btn');
      if (btn) { const orig = btn.textContent; btn.textContent = '✓ Copied!'; setTimeout(() => { btn.textContent = orig; }, 2000); }
    }).catch(() => prompt('Copy this link:', url));
  } else {
    prompt('Copy this link:', url);
  }
}

// US-25: deep link via URL hash
function openTracker(id) {
  curBus = id;
  const m = BMETA[id];
  document.getElementById('trkName').textContent = m.num;
  document.getElementById('trkSub').textContent  = m.route;
  location.hash = id;
  showV('trackerView');

  setTimeout(() => {
    ensureMap(); lmap.invalidateSize();
    const b = sim[id];
    const hasData = b.lastUpdate > 0 && b.lat !== null;
    const center  = hasData ? [b.lat, b.lon] : [13.05, 80.22];
    if (!lmarker) lmarker = L.marker(center, {icon: busIcon(m.color, !!b.sos)}).addTo(lmap);
    lmap.flyTo(center, 15, {animate: true, duration: 1.2});
    updateTele(id);
    if (tickId) clearInterval(tickId);
    tickId = setInterval(() => updateTele(id), 3000);
  }, 80);
}

function leaveTracker() {
  if (tickId) { clearInterval(tickId); tickId = null; }
  if (lmarker) { lmarker.remove(); lmarker = null; }
  if (ltrail)  { ltrail.setLatLngs([]); }
  location.hash = '';
  showV('busListView');
}

// Tracks buses whose SOS the operator has acknowledged — suppresses backend restore
const sosAcknowledged = new Set();

// US-31: acknowledge SOS — latches suppression so syncFromAPI can't restore it
function acknowledgeSOSFor(id) {
  sosAcknowledged.add(id);
  if (sim[id]) sim[id].sos = 0;
  updateTele(id);
}

// US-26 / US-28 / US-31: updateTele
function updateTele(id) {
  const b = sim[id], m = BMETA[id];
  const hasData     = b.lastUpdate > 0 && b.lat !== null;
  const isSos       = !!b.sos;
  const sc          = sclr(b.speed);
  const p           = spct(b.speed);
  const isOverspeed = b.speed > 70;
  const isOffline   = !hasData || (Date.now() - b.lastUpdate) > 30000;

  if (hasData) {
    if (!lmarker) {
      lmarker = L.marker([b.lat, b.lon], {icon: busIcon(m.color, isSos)}).addTo(lmap);
    } else {
      lmarker.setLatLng([b.lat, b.lon]);
      lmarker.setIcon(busIcon(m.color, isSos));
    }
    lmarker.bindPopup(`<b>${m.num}</b><br>Speed: <b>${b.speed} km/h</b><br>${b.geo ? 'At: ' + b.geo.name : 'En route'}`);
    if (ltrail) { ltrail.setLatLngs(b.trail); ltrail.setStyle({color: m.color}); }
    lmap.panTo([b.lat, b.lon], {animate: true, duration: .5});
  }

  // HUD — US-28: red + OVERSPEED label when >70
  const hudNum = document.getElementById('hudNum');
  hudNum.textContent = hasData ? b.speed : '—';
  hudNum.style.color = sc;
  const hudLbl = document.querySelector('.hud-lbl');
  if (hudLbl) hudLbl.textContent = isOverspeed ? '⚠ OVERSPEED' : 'Axle Speed';
  const hb = document.getElementById('hudBar');
  hb.style.width = p + '%'; hb.style.background = sc;
  document.getElementById('hudSos').className = 'hud-sos' + (isSos ? ' on' : '');

  document.getElementById('tId').textContent = id;
  document.getElementById('tTs').textContent = isOffline
    ? 'Awaiting GPS signal…'
    : (b.ts || '—');

  document.getElementById('tSpd').textContent = hasData ? b.speed : '—';
  document.getElementById('tSpd').style.color = sc;
  const sb = document.getElementById('tSpdBar');
  sb.style.width = p + '%'; sb.style.background = sc;
  const mp = document.getElementById('tMpill');
  mp.textContent = isOffline ? '📡 No signal' : b.stop ? '⏸ Stopped' : '▶ Moving';
  mp.className = 'mpill ' + (isOffline ? 'mpill-st' : b.stop ? 'mpill-st' : 'mpill-mv');

  // US-21: ETA to next stop
  let etaEl = document.getElementById('tEta');
  const eta = etaToNextStop(b);
  if (eta && hasData) {
    if (!etaEl) {
      etaEl = document.createElement('div');
      etaEl.id = 'tEta';
      etaEl.style.cssText = 'margin-top:6px;font-size:.73rem;color:#58a6ff;font-family:"IBM Plex Mono",monospace';
      document.getElementById('tSpdBar').parentNode.appendChild(etaEl);
    }
    etaEl.textContent = `⏱ ~${eta} min to next stop`;
  } else if (etaEl) { etaEl.remove(); }

  // US-26: "bus appears stopped" notice
  const stoppedMs = b.stopSince ? (Date.now() - b.stopSince) : 0;
  let stoppedNotice = document.getElementById('stopped-notice');
  if (stoppedMs > 120000 && hasData) {
    const mm = Math.floor(stoppedMs / 60000), ss = Math.floor((stoppedMs % 60000) / 1000);
    if (!stoppedNotice) {
      stoppedNotice = document.createElement('div');
      stoppedNotice.id = 'stopped-notice';
      stoppedNotice.className = 'stopped-notice';
      document.getElementById('tSpdBar').parentNode.appendChild(stoppedNotice);
    }
    stoppedNotice.textContent = `⚠ Bus appears stopped for ${mm}m ${ss}s`;
  } else if (stoppedNotice) { stoppedNotice.remove(); }

  document.getElementById('tLat').textContent = hasData ? b.lat.toFixed(6) + '°' : '—';
  document.getElementById('tLon').textContent = hasData ? b.lon.toFixed(6) + '°' : '—';

  // SOS card — US-31: ack button
  const sc2 = document.getElementById('tSosCard');
  document.getElementById('tSosIco').textContent = isSos ? '🔴' : '🟢';
  document.getElementById('tSosSt').textContent  = isSos ? 'TRIGGERED — EMERGENCY' : 'ARMED / SAFE';
  document.getElementById('tSosSt').style.color  = isSos ? '#f85149' : '#3fb950';
  document.getElementById('tSosSub').textContent = isSos
    ? 'Priority SMS dispatched · ISR active'
    : 'No emergency detected';
  sc2.className = 'sos-card ' + (isSos ? 'sos-trig' : 'sos-safe');
  let ackBtn = document.getElementById('sos-ack-btn');
  if (isSos) {
    if (!ackBtn) {
      ackBtn = document.createElement('button');
      ackBtn.id = 'sos-ack-btn'; ackBtn.className = 'sos-ack-btn';
      ackBtn.textContent = 'Acknowledge SOS';
      ackBtn.onclick = () => acknowledgeSOSFor(id);
      sc2.appendChild(ackBtn);
    }
  } else if (ackBtn) { ackBtn.remove(); }

  // Geofence
  document.getElementById('tGeoIco').textContent = b.geo ? '📌' : '🛣️';
  document.getElementById('tGeoNm').textContent  = b.geo ? b.geo.name : (hasData ? 'Outside all geofences' : 'Awaiting GPS signal');
  document.getElementById('tGeoSb').textContent  = b.geo ? 'ID: ' + b.geo.id : (hasData ? 'En route between stops' : '—');

  // Live JSON packet
  document.getElementById('tJson').textContent = JSON.stringify(hasData ? {
    dev_id: id, ts: b.ts,
    lat: +b.lat.toFixed(6), lon: +b.lon.toFixed(6),
    speed_kmh: b.speed,
    geofence: b.geo ? b.geo.name : null,
    stop_state: b.stop, sos_active: b.sos
  } : {dev_id: id, status: 'awaiting_signal'}, null, 2);

  // Landmark stop log — always from real backend
  fetchStopEvents(id).then(stops => {
    if (curBus !== id) return; // discard stale response from a previous bus
    const logEl = document.getElementById('tLog');
    if (!logEl) return;
    if (!stops || !stops.length) {
      logEl.innerHTML = '<div style="font-size:.73rem;color:#8b949e">No stop events recorded yet.</div>';
      return;
    }
    logEl.innerHTML = stops.slice(0, 5).map(s => {
      const arr  = new Date(s.arrived_at * 1000).toLocaleTimeString();
      const dwell = s.duration_sec != null
        ? (Math.floor(s.duration_sec / 60) > 0 ? Math.floor(s.duration_sec / 60) + 'm ' : '') + (Math.round(s.duration_sec % 60)) + 's'
        : null;
      const longDwell = s.duration_sec != null && s.duration_sec > 600;
      return `<div class="log-row${longDwell ? ' log-row-delay' : ''}">
        <div class="log-nm">🏁 ${s.location_name}${longDwell ? ' <span class="log-delay-tag">DELAY</span>' : ''}</div>
        <div class="log-mt">Arrived ${arr}${dwell
          ? ` · <span class="log-dw${longDwell ? ' log-dw-warn' : ''}">Dwell: ${dwell}</span>`
          : ' · <span class="log-in">Currently inside</span>'}</div>
      </div>`;
    }).join('');
  });
}

/* ═══════════════════════════════
   US-25: Deep link on page load
═══════════════════════════════ */
window.addEventListener('load', () => {
  const hash = location.hash.slice(1);
  if (hash && BMETA[hash]) openTracker(hash);
});
