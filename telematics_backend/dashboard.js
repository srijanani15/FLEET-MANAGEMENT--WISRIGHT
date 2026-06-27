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

/* ── Live state cache — entries created ONLY when real data arrives from backend ── */
const sim = {};

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

// Always returns the nearest stop with distance in metres
function nearestStop(la, lo) {
  if (!GEO.length) return null;
  let best = null, bestDist = Infinity;
  GEO.forEach(g => { const d = hav(la, lo, g.lat, g.lon); if (d < bestDist) { bestDist = d; best = g; } });
  return { stop: best, dist: Math.round(bestDist) };
}

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

      // Accumulate GPS trail — only push when position actually changes
      const tr = sim[id].trail;
      const last = tr[tr.length - 1];
      if (!last || last[0] !== t.lat || last[1] !== t.lon) {
        tr.push([t.lat, t.lon]);
        if (tr.length > 120) tr.shift();
      }
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
  const totalEl = document.getElementById('sBusTotal');
  if (totalEl) totalEl.textContent = all.length;
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
  const dow = date.getDay(); // 0=Sun
  const label = `${dn[dow]}, ${date.getDate()} ${mn[date.getMonth()]} ${date.getFullYear()}`;

  const liveBuses  = Object.keys(sim).filter(id => sim[id].lastUpdate > 0);
  const sosId      = liveBuses.find(id => sim[id].sos);

  const sosBanner = sosId ? `<div class="sos-home-banner">
    <div class="sos-home-banner-icon">🚨</div>
    <div class="sos-home-banner-text">
      <div class="sos-home-banner-title">${BMETA[sosId]?.num || sosId} — SOS alert active</div>
      <div class="sos-home-banner-sub">${BMETA[sosId]?.route || 'Live GPS Device'} · Dispatcher notified via SMS</div>
    </div>
    <button class="sos-home-banner-btn" onclick="openTracker('${sosId}')">View Live →</button>
  </div>` : '';

  const offlineBanner = !backendOnline ? `<div class="sos-home-banner" style="border-color:#6b7280;background:#1a1f2e">
    <div class="sos-home-banner-icon">📡</div>
    <div class="sos-home-banner-text">
      <div class="sos-home-banner-title" style="color:#9ca3af">Backend offline — showing schedule</div>
      <div class="sos-home-banner-sub" style="color:#6b7280">Live GPS will appear once backend connects to ${BACKEND}</div>
    </div>
  </div>` : '';

  // Sunday = holiday leave
  if (dow === 0) {
    sec.innerHTML = `${sosBanner}${offlineBanner}
      <div class="trips-title" style="margin-top:8px">${label}</div>
      <div class="weekend-box">
        <div class="wb-icon">🌴</div>
        <div style="font-size:1.05rem;font-weight:700;color:#f97316;margin-bottom:4px">Sunday Leave</div>
        <p>No bus service today. All routes resume Monday 8:00 AM.</p>
      </div>`;
    return;
  }

  // Mon–Sat: always show full schedule from BMETA, enrich with live data if available
  const allAmIds = Object.keys(BMETA).filter(id => BMETA[id].trip === '8am');
  const allPmIds = Object.keys(BMETA).filter(id => BMETA[id].trip === '3pm');

  function scheduleStrip(id) {
    const m   = BMETA[id];
    const b   = sim[id];
    const clr = STRIP_COLORS[id] || 'red';
    const hasData = b && b.lastUpdate > 0;
    const spdPct  = hasData ? spct(b.speed) : 0;
    const spdTxt  = b && b.sos ? '🚨 SOS' : hasData ? (b.speed + ' km/h') : 'Scheduled';
    const spdClass = (b && b.speed > 70) ? 'fast' : (b && b.speed > 40) ? 'mid' : '';
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

  const liveChip = backendOnline ? '<div class="tc-chip">📍 Live GPS</div>' : '<div class="tc-chip" style="color:#9ca3af">📅 Scheduled</div>';

  const amSection = `
    <div class="trip-card morning">
      <div class="trip-card-bar"></div>
      <div class="trip-card-body">
        <div class="tc-top"><div class="tc-icon">🌅</div><span class="tc-badge badge-am">8:00 AM</span></div>
        <div class="tc-title">Morning to College</div>
        <div class="tc-time">${allAmIds.length} buses · ${backendOnline ? 'Live GPS' : 'Scheduled'}</div>
        <div class="tc-meta">
          <div class="tc-chip">🚌 <b>${allAmIds.length}</b> Buses</div>
          ${liveChip}
        </div>
        <div class="tc-strips">${allAmIds.map(scheduleStrip).join('')}</div>
        <button class="tc-btn am-btn" onclick="showBusList('8am','Morning to College — 8:00 AM')">View All &amp; Track →</button>
      </div>
    </div>`;

  const pmSection = `
    <div class="trip-card return">
      <div class="trip-card-bar"></div>
      <div class="trip-card-body">
        <div class="tc-top"><div class="tc-icon">🌆</div><span class="tc-badge badge-pm">3:00 PM</span></div>
        <div class="tc-title">Evening Return</div>
        <div class="tc-time">${allPmIds.length} buses · ${backendOnline ? 'Live GPS' : 'Scheduled'}</div>
        <div class="tc-meta">
          <div class="tc-chip">🚌 <b>${allPmIds.length}</b> Buses</div>
          ${liveChip}
        </div>
        <div class="tc-strips">${allPmIds.map(scheduleStrip).join('')}</div>
        <button class="tc-btn pm-btn" onclick="showBusList('3pm','Evening Return — 3:00 PM')">View All &amp; Track →</button>
      </div>
    </div>`;

  // Extra hardware devices not in BMETA
  const otherBuses = liveBuses.filter(id => !BMETA[id]);
  const otherSection = otherBuses.length ? `
    <div class="trip-card morning" style="grid-column:1/-1">
      <div class="trip-card-bar"></div>
      <div class="trip-card-body">
        <div class="tc-top"><div class="tc-icon">📡</div><span class="tc-badge badge-am">Live</span></div>
        <div class="tc-title">Live Devices</div>
        <div class="tc-time">${otherBuses.length} device${otherBuses.length>1?'s':''} · Real GPS</div>
        <div class="tc-strips">${otherBuses.map(id => stripHtml(id)).join('')}</div>
        <button class="tc-btn am-btn" onclick="showBusList('all','All Live Devices')">View All &amp; Track →</button>
      </div>
    </div>` : '';

  sec.innerHTML = `
    ${sosBanner}${offlineBanner}
    <div class="trips-title" style="margin-top:8px">${label} — ${backendOnline ? 'Live Fleet' : 'Bus Schedule'}</div>
    <div class="trips-grid">${amSection}${pmSection}${otherSection}</div>`;
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
    const spd = hasData ? b.speed : 0;
    const spdClass = spd > 70 ? 'fast' : spd > 40 ? 'mid' : '';
    spdEl.textContent = (b && b.sos) ? '🚨 SOS' : hasData ? (spd + ' km/h') : 'Scheduled';
    spdEl.className = 'tc-strip-spd ' + spdClass;
  });
}
setInterval(updateHomeStrips, 3000);

// US-20: search by bus number / route
function doSearch() {
  const v = document.getElementById('srchIn').value.trim().toLowerCase();
  if (v.length < 2) { alert('Please enter at least 2 characters.'); return; }
  // Search only buses that have real data
  const exact = Object.keys(sim).filter(id => sim[id].lastUpdate > 0).find(id =>
    (BMETA[id]?.num  || '').toLowerCase().includes(v) ||
    (BMETA[id]?.route|| '').toLowerCase().includes(v) ||
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
  // Only show buses that have sent real data
  let rows = Object.values(sim).filter(b => {
    if (b.lastUpdate === 0) return false;
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
    const ns     = (hasData && b.lat != null) ? nearestStop(b.lat, b.lon) : null;
    const geo    = b.geo
      ? `📍 ${b.geo.name}`
      : ns ? `Near ${ns.stop.name} <span style="font-size:.65rem;color:#6b7280">(${ns.dist}m)</span>` : '—';
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
let lmap = null, lmarker = null, ltrail = null;

function destroyMap() {
  if (lmap) { lmap.remove(); lmap = null; }
  lmarker = null; ltrail = null; geoLayerGroup = null;
}

function initMap(lat, lon, zoom) {
  destroyMap();
  lmap = L.map('liveMap', { zoomControl: true, preferCanvas: true });
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
    { attribution: '© OpenStreetMap', maxZoom: 19 }).addTo(lmap);
  geoLayerGroup = L.layerGroup().addTo(lmap);
  refreshMapGeofences();
  ltrail = L.polyline([], { color: '#58a6ff', weight: 3, opacity: .6, dashArray: '5 5' }).addTo(lmap);
  lmap.setView([lat, lon], zoom);
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
  // Clear any running interval synchronously — prevents double-click race
  if (tickId) { clearInterval(tickId); tickId = null; }
  curBus = id;
  const m = BMETA[id] || {num: id, route: 'Live GPS Device', color: '#a5b4fc'};
  document.getElementById('trkName').textContent = m.num;
  document.getElementById('trkSub').textContent  = m.route;
  location.hash = id;
  showV('trackerView');

  requestAnimationFrame(() => requestAnimationFrame(() => {
    const b = sim[id];
    const hasData = b && b.lastUpdate > 0 && b.lat !== null;
    const lat  = hasData ? b.lat  : 13.0694;
    const lon  = hasData ? b.lon  : 80.1948;
    const zoom = hasData ? 16 : 13;

    initMap(lat, lon, zoom);
    lmarker = L.marker([lat, lon], { icon: busIcon(m.color, !!(b && b.sos)) }).addTo(lmap);

    // Immediately fetch fresh GPS so map opens on the correct position, not stale data
    syncFromAPI().then(() => {
      const fresh = sim[id];
      if (fresh && fresh.lat !== null) {
        lmarker.setLatLng([fresh.lat, fresh.lon]);
        lmap.setView([fresh.lat, fresh.lon], 16);
      }
      updateTele(id);
    });
    updateTele(id);
    currentTripId = null;
    _updateTripPanelEmpty();
    fetchActiveTrip(id).then(trip => {
      const btn = document.getElementById('tripActionBtn');
      if (trip && trip.status === 'active') {
        currentTripId = trip.id;
        if (btn) { btn.textContent = '⏹ End Trip'; btn.classList.add('trip-active'); }
      } else {
        if (btn) { btn.textContent = '▶ Start Trip'; btn.classList.remove('trip-active'); }
      }
      updateTripPanel(id);
    });
    tickId = setInterval(() => { updateTele(id); updateTripPanel(id); }, 3000);
  }));
}

function leaveTracker() {
  if (tickId) { clearInterval(tickId); tickId = null; }
  destroyMap();
  // Reset trip UI — trip stays active on backend, just hidden
  currentTripId = null;
  const btn = document.getElementById('tripActionBtn');
  if (btn) { btn.textContent = '▶ Start Trip'; btn.classList.remove('trip-active'); }
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
  if (!sim[id]) return; // backend hasn't returned data for this device yet
  const b = sim[id], m = BMETA[id] || {num: id, route: 'Live GPS Device', color: '#a5b4fc'};
  const hasData     = b.lastUpdate > 0 && b.lat !== null;
  const isSos       = !!b.sos;
  const sc          = sclr(b.speed);
  const p           = spct(b.speed);
  const isOverspeed = b.speed > 70;
  const isOffline   = !hasData || (Date.now() - b.lastUpdate) > 30000;

  if (hasData && lmap && lmarker) {
    lmarker.setLatLng([b.lat, b.lon]);
    lmarker.setIcon(busIcon(m.color, isSos));
    lmarker.bindPopup(`<b>${m.num}</b><br>Speed: <b>${b.speed} km/h</b><br>${b.geo ? 'At: ' + b.geo.name : 'En route'}`);
    if (ltrail) { ltrail.setLatLngs(b.trail); ltrail.setStyle({color: m.color}); }
    // Always pan to keep the bus centred — GPS updates every 3s so movement is small
    lmap.panTo([b.lat, b.lon], {animate: true, duration: 0.8});
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

  // Geofence — show nearest stop when outside all geofences
  const geoNs = (hasData && b.lat != null) ? nearestStop(b.lat, b.lon) : null;
  document.getElementById('tGeoIco').textContent = b.geo ? '📌' : (geoNs ? '🛣️' : '⏳');
  document.getElementById('tGeoNm').textContent  = b.geo
    ? b.geo.name
    : geoNs ? `Near ${geoNs.stop.name}` : 'Awaiting GPS signal';
  document.getElementById('tGeoSb').textContent  = b.geo
    ? 'Inside geofence · ' + b.geo.id
    : geoNs ? `${geoNs.dist} m away · En route` : '—';

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
  if (hash) openTracker(hash);
});

/* ═══════════════════════════════════════════════════════════
   TRIP MANAGEMENT
   Start / End trips, accumulate km, detect off-route, record
   mandatory stop arrivals/departures and dwell times.
═══════════════════════════════════════════════════════════ */

let currentTripId   = null;
let _routeStopsCache = null;

async function tripAction() {
  if (!curBus) return;
  const btn = document.getElementById('tripActionBtn');
  if (currentTripId) {
    if (!confirm(`End trip for ${BMETA[curBus]?.num || curBus}?`)) return;
    try {
      const r = await fetch(`${BACKEND}/trip/end`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({dev_id: curBus}),
      });
      const d = await r.json();
      if (d.status === 'ok') {
        const finishedId = currentTripId;
        currentTripId = null;
        if (btn) { btn.textContent = '▶ Start Trip'; btn.classList.remove('trip-active'); }
        _updateTripPanelEmpty();
        if (confirm('Trip ended! View full summary?')) showTripSummary(finishedId);
      }
    } catch { alert('Failed to end trip — check backend connection.'); }
  } else {
    try {
      const r = await fetch(`${BACKEND}/trip/start`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({dev_id: curBus, route_key: 'office_to_mogappair'}),
      });
      const d = await r.json();
      if (d.status === 'ok') {
        currentTripId = d.trip_id;
        if (btn) { btn.textContent = '⏹ End Trip'; btn.classList.add('trip-active'); }
        updateTripPanel(curBus);
      }
    } catch { alert('Failed to start trip — check backend connection.'); }
  }
}

async function fetchActiveTrip(devId) {
  try {
    const r = await fetch(`${BACKEND}/trip/active/${devId}`, {signal: AbortSignal.timeout(2500)});
    if (!r.ok) return null;
    const d = await r.json();
    return d.data ? {...d.data, stops_visited: d.stops_visited || []} : null;
  } catch { return null; }
}

async function fetchRouteStops() {
  if (_routeStopsCache) return _routeStopsCache;
  try {
    const r = await fetch(`${BACKEND}/routes`, {signal: AbortSignal.timeout(2000)});
    if (!r.ok) return [];
    const d = await r.json();
    const route = (d.data || []).find(x => x.key === 'office_to_mogappair');
    _routeStopsCache = route ? route.stops : [];
    return _routeStopsCache;
  } catch { return []; }
}

function fmtDuration(sec) {
  sec = Math.round(+sec || 0);
  if (sec < 60) return `${sec}s`;
  const m = Math.floor(sec / 60), s = sec % 60;
  return m >= 60 ? `${Math.floor(m/60)}h ${m%60}m` : `${m}m ${s}s`;
}

function fmtTime(ts) {
  if (!ts) return '—';
  return new Date((+ts) * 1000).toLocaleTimeString('en-IN',
    {hour:'2-digit', minute:'2-digit', second:'2-digit'});
}

function _updateTripPanelEmpty() {
  const panel = document.getElementById('tripPanel');
  if (panel) panel.style.display = 'none';
  const presEl = document.getElementById('presencePanelContent');
  if (presEl) presEl.innerHTML =
    '<div style="font-size:.73rem;color:#8b949e">Start a trip to enable passenger tracking.</div>';
}

async function updateTripPanel(id) {
  if (!id) return;
  const trip = await fetchActiveTrip(id);
  const panel = document.getElementById('tripPanel');
  const content = document.getElementById('tripPanelContent');
  const presEl  = document.getElementById('presencePanelContent');
  if (!panel || !content) return;

  if (!trip || trip.status !== 'active') {
    panel.style.display = 'none';
    _updateTripPanelEmpty();
    return;
  }

  // Sync button state (e.g. after opening tracker for a device with existing trip)
  currentTripId = trip.id;
  const btn = document.getElementById('tripActionBtn');
  if (btn && !btn.classList.contains('trip-active')) {
    btn.textContent = '⏹ End Trip'; btn.classList.add('trip-active');
  }

  panel.style.display = '';

  const elapsed  = Math.floor(Date.now() / 1000 - trip.start_time);
  const elStr    = fmtDuration(elapsed);
  const allStops = await fetchRouteStops();
  const visited  = new Set((trip.stops_visited || []).map(s => s.stop_name));

  const stopsHtml = allStops.map(s => {
    const done   = visited.has(s.name);
    const detail = (trip.stops_visited || []).find(v => v.stop_name === s.name);
    const extra  = detail?.dwell_sec != null
      ? `<span class="sp-dwell">${fmtDuration(detail.dwell_sec)} dwell</span>`
      : (detail ? '<span class="sp-here">● Here now</span>' : '');
    return `<div class="sp-row${done ? ' sp-done' : ''}">
      <span class="sp-dot${done ? ' done' : ''}"></span>
      <span class="sp-nm">${s.name}</span>${extra}</div>`;
  }).join('');

  content.innerHTML = `
    <div class="trip-metrics">
      <div class="tm"><div class="tm-v">${(+trip.total_km||0).toFixed(2)}</div><div class="tm-l">km</div></div>
      <div class="tm"><div class="tm-v">${elStr}</div><div class="tm-l">elapsed</div></div>
      <div class="tm"><div class="tm-v">${trip.passengers_onboard||0}</div><div class="tm-l">onboard</div></div>
      <div class="tm"><div class="tm-v${trip.off_route_count>0?' warn':''}"> ${trip.off_route_count}</div><div class="tm-l">off-route</div></div>
    </div>
    <div class="stop-progress">${stopsHtml}</div>
    <div class="trip-start-note">Started ${fmtTime(trip.start_time)} · Route: ${trip.route_name}</div>`;

  // Presence controls
  if (presEl) {
    const curStop = sim[id]?.geo?.name || '';
    presEl.innerHTML = `
      <div class="pax-row">
        <span class="pax-icon">🧍</span>
        <span class="pax-count"><b id="paxCount">${trip.passengers_onboard||0}</b> onboard</span>
      </div>
      <div class="pres-ctrls">
        <input id="presCount" type="number" min="1" max="50" value="1" class="pres-num"/>
        <button class="pres-btn board" onclick="logPresence('board')">🟢 Board</button>
        <button class="pres-btn alight" onclick="logPresence('alight')">🔴 Alight</button>
      </div>
      ${curStop ? `<div class="pres-stop">At: ${curStop}</div>` : ''}`;
  }
}

async function logPresence(type) {
  if (!curBus || !currentTripId) return;
  const count     = Math.max(1, parseInt(document.getElementById('presCount')?.value || '1', 10));
  const stopName  = sim[curBus]?.geo?.name || '';
  try {
    const r = await fetch(`${BACKEND}/presence`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({dev_id: curBus, event_type: type, count, stop_name: stopName}),
    });
    const d = await r.json();
    if (d.status === 'ok') {
      const el = document.getElementById('paxCount');
      if (el) el.textContent = d.passengers_onboard;
    }
  } catch {}
}

/* ═══════════════════════════════════════════════════════════
   TRIP LOG VIEW (View 4)
═══════════════════════════════════════════════════════════ */

async function showTripLog() {
  showV('tripLogView');
  const sub     = document.getElementById('tripLogSub');
  const content = document.getElementById('tripLogContent');
  if (sub) sub.textContent = 'Route history & passenger data';
  content.innerHTML = '<div style="color:#8b949e;padding:24px;font-size:.82rem">Loading trips…</div>';

  try {
    const devId = curBus;
    const url   = devId
      ? `${BACKEND}/trips?dev_id=${devId}&limit=30`
      : `${BACKEND}/trips?limit=30`;
    const r = await fetch(url, {signal: AbortSignal.timeout(4000)});
    const d = await r.json();
    const trips = d.data || [];

    if (!trips.length) {
      content.innerHTML = `<div class="tlog-empty">
        <div style="font-size:2rem;margin-bottom:10px">🗺</div>
        <div>No trips recorded yet.</div>
        <div style="color:#6b7280;font-size:.75rem;margin-top:6px">Open the tracker and tap "▶ Start Trip" to begin.</div>
      </div>`;
      return;
    }

    content.innerHTML = trips.map(t => {
      const dur    = t.end_time ? fmtDuration(t.end_time - t.start_time) : 'Active';
      const isActive = t.status === 'active';
      const badge  = isActive
        ? '<span class="tlog-badge active">● Active</span>'
        : '<span class="tlog-badge done">✓ Done</span>';
      const offBadge = t.off_route_count > 0
        ? `<span class="tlog-badge offroute">⚠ ${t.off_route_count} off-route</span>`
        : '';
      return `<div class="tlog-row" onclick="showTripSummary(${t.id})">
        <div class="tlog-left">
          <div class="tlog-route">${t.route_name}</div>
          <div class="tlog-meta">
            ${new Date(t.start_time*1000).toLocaleString('en-IN')}
            &nbsp;·&nbsp; ${dur}
            &nbsp;·&nbsp; ${(+t.total_km||0).toFixed(2)} km
          </div>
          <div class="tlog-badges">${badge}${offBadge}</div>
        </div>
        <div class="tlog-chev">›</div>
      </div>`;
    }).join('');
  } catch {
    content.innerHTML = '<div style="color:#f85149;padding:20px;font-size:.82rem">Failed to load trips. Is the backend running?</div>';
  }
}

async function showTripSummary(tripId) {
  showV('tripLogView');
  const sub     = document.getElementById('tripLogSub');
  const content = document.getElementById('tripLogContent');
  if (sub) sub.textContent = `Trip #${tripId} — Full Summary`;
  content.innerHTML = '<div style="color:#8b949e;padding:24px;font-size:.82rem">Loading summary…</div>';

  try {
    const r = await fetch(`${BACKEND}/trip/summary/${tripId}`, {signal: AbortSignal.timeout(4000)});
    const d = await r.json();
    if (d.status !== 'ok') { content.innerHTML = '<div style="color:#f85149;padding:20px">Trip not found.</div>'; return; }
    _renderTripSummary(d.data, content);
  } catch {
    content.innerHTML = '<div style="color:#f85149;padding:20px;font-size:.82rem">Failed to load summary.</div>';
  }
}

function _renderTripSummary(data, el) {
  const {trip, stops, presence, off_route} = data;
  const dur      = trip.end_time ? fmtDuration(trip.end_time - trip.start_time) : 'Ongoing';
  const totalPax = presence.filter(p => p.event_type === 'board').reduce((s, p) => s + (+p.count||0), 0);

  // Build per-stop passenger aggregates
  const paxByStop = {};
  presence.forEach(p => {
    if (!p.stop_name) return;
    if (!paxByStop[p.stop_name]) paxByStop[p.stop_name] = {board:0, alight:0};
    if (p.event_type === 'board') paxByStop[p.stop_name].board  += +p.count||0;
    else                          paxByStop[p.stop_name].alight += +p.count||0;
  });

  const stopRows = stops.length
    ? stops.map((s, i) => {
        const pax = paxByStop[s.stop_name] || {board:0, alight:0};
        return `<tr>
          <td>${i+1}</td>
          <td><b>${s.stop_name}</b></td>
          <td>${fmtTime(s.arrived_at)}</td>
          <td>${s.departed_at ? fmtTime(s.departed_at) : '<span style="color:#3fb950">Here now</span>'}</td>
          <td>${s.dwell_sec != null ? fmtDuration(s.dwell_sec) : '—'}</td>
          <td>${s.distance_from_prev > 0 ? (+s.distance_from_prev).toFixed(2)+' km' : '—'}</td>
          <td>${s.time_from_prev > 0 ? fmtDuration(s.time_from_prev) : '—'}</td>
          <td style="color:#3fb950;font-weight:700">+${pax.board}</td>
          <td style="color:#f85149;font-weight:700">−${pax.alight}</td>
          <td><b>${+s.passengers_onboard||0}</b></td>
        </tr>`;
      }).join('')
    : '<tr><td colspan="10" style="color:#8b949e;text-align:center;padding:16px">No mandatory stops recorded.</td></tr>';

  const offRows = off_route.slice(0, 20).map(e =>
    `<tr>
      <td>${fmtTime(e.timestamp)}</td>
      <td>${(+e.lat).toFixed(5)}</td>
      <td>${(+e.lon).toFixed(5)}</td>
      <td style="color:#f85149;font-weight:700">${(+e.distance_from_route||0).toFixed(0)} m</td>
    </tr>`).join('');

  const presRows = presence.map(p =>
    `<tr>
      <td>${fmtTime(p.timestamp)}</td>
      <td style="color:${p.event_type==='board'?'#3fb950':'#f85149'};font-weight:700">
        ${p.event_type==='board'?'🟢 Board':'🔴 Alight'}</td>
      <td>${p.count}</td>
      <td>${p.stop_name||'—'}</td>
    </tr>`).join('');

  el.innerHTML = `
    <button class="back-to-list" onclick="showTripLog()">← All Trips</button>

    <div class="sum-hero">
      <div class="sh"><div class="sh-v">${(+trip.total_km||0).toFixed(2)}</div><div class="sh-l">Total km</div></div>
      <div class="sh"><div class="sh-v">${dur}</div><div class="sh-l">Duration</div></div>
      <div class="sh"><div class="sh-v">${totalPax}</div><div class="sh-l">Total Pax</div></div>
      <div class="sh"><div class="sh-v${trip.off_route_count>0?' warn':''}">${trip.off_route_count}</div><div class="sh-l">Off-Route</div></div>
    </div>

    <div class="sum-meta">
      <div><b>Route:</b> ${trip.route_name}</div>
      <div><b>Start:</b> ${new Date(trip.start_time*1000).toLocaleString('en-IN')}</div>
      <div><b>End:</b> ${trip.end_time ? new Date(trip.end_time*1000).toLocaleString('en-IN') : '—'}</div>
      <div><b>Device:</b> ${trip.dev_id}</div>
      <div><b>Status:</b> <span style="color:${trip.status==='active'?'#3fb950':'#8b949e'}">${trip.status}</span></div>
    </div>

    <div class="sum-title">📍 Mandatory Stop Log</div>
    <div class="tbl-wrap" style="margin:0 0 16px">
      <table>
        <thead><tr>
          <th>#</th><th>Stop</th><th>Arrived</th><th>Departed</th>
          <th>Dwell</th><th>Distance</th><th>Travel Time</th>
          <th>Boarded</th><th>Alighted</th><th>Onboard</th>
        </tr></thead>
        <tbody>${stopRows}</tbody>
      </table>
    </div>

    ${off_route.length ? `
    <div class="sum-title" style="color:#f85149">⚠ Off-Route Events (${off_route.length})</div>
    <div class="tbl-wrap" style="margin:0 0 16px">
      <table>
        <thead><tr><th>Time</th><th>Lat</th><th>Lon</th><th>Distance from Route</th></tr></thead>
        <tbody>${offRows}</tbody>
      </table>
    </div>` : ''}

    ${presence.length ? `
    <div class="sum-title">👥 Passenger Event Log</div>
    <div class="tbl-wrap" style="margin:0 0 16px">
      <table>
        <thead><tr><th>Time</th><th>Type</th><th>Count</th><th>Stop</th></tr></thead>
        <tbody>${presRows}</tbody>
      </table>
    </div>` : ''}`;
}

function leaveTripLog() {
  const sub = document.getElementById('tripLogSub');
  if (sub) sub.textContent = 'Route history & passenger data';
  if (curBus) showV('trackerView');
  else goHome();
}
