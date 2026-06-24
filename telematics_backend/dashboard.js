const BACKEND  = "";
const POLL_MS  = 3000;
const MAX_TRAIL= 60;
const FALLBACK = "VTUESP32-0091";
let DEV_ID = FALLBACK;

const T0 = Date.now();
let maxSpd=0, totalDist=0, spdHist=[], historyData=[];
let prevLat=null, prevLon=null, hdg=0, sosCount=0, curSpd=0;
let paused=false, pollTimer=null;

// ── MAP ──────────────────────────────────────────────────────
const map=L.map("map",{zoomControl:true}).setView([13.0827,80.2707],15);
L.tileLayer("https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png",{subdomains:"abcd",maxZoom:19}).addTo(map);
loadStopConfig();

// ── CAR MARKER ───────────────────────────────────────────────
const carSVG=`<svg id="car-el" width="36" height="36" viewBox="0 0 36 36" fill="none">
  <rect x="8" y="13" width="20" height="12" rx="3" fill="#00d4ff" fill-opacity=".14" stroke="#00d4ff" stroke-width="1.5"/>
  <path d="M11 13 L14 8 H22 L25 13" fill="#00d4ff" fill-opacity=".18" stroke="#00d4ff" stroke-width="1.2"/>
  <circle cx="12" cy="25" r="3" fill="#04080f" stroke="#00d4ff" stroke-width="1.5"/>
  <circle cx="24" cy="25" r="3" fill="#04080f" stroke="#00d4ff" stroke-width="1.5"/>
  <line x1="14" y1="13" x2="22" y2="13" stroke="#00d4ff" stroke-width="1" opacity=".35"/>
  <rect x="15" y="9" width="6" height="4" rx="1" fill="#00d4ff" fill-opacity=".28"/>
</svg>`;
const vIcon=L.divIcon({className:"",html:`<div class="vmarker"><div class="ping"></div>${carSVG}</div>`,iconSize:[40,40],iconAnchor:[20,20]});
let marker=null, trailLayers=[], trailOn=true;

// ── APEXCHARTS RADIAL GAUGE ───────────────────────────────────
const gaugeEl = document.getElementById("apex-gauge");
const gaugeOpts = {
  series:[0],
  chart:{type:"radialBar",height:148,sparkline:{enabled:true},
    animations:{enabled:true,easing:"easeinout",speed:400,dynamicAnimation:{enabled:true,speed:300}}},
  plotOptions:{radialBar:{
    startAngle:-130,endAngle:130,
    hollow:{size:"55%",background:"transparent"},
    track:{background:"rgba(255,255,255,0.05)",strokeWidth:"96%"},
    dataLabels:{
      name:{show:true,offsetY:-4,fontSize:"9px",color:"#6a849e",fontFamily:"Inter,sans-serif",formatter:()=>"km/h"},
      value:{offsetY:7,fontSize:"20px",fontWeight:700,color:"#00d4ff",fontFamily:"'Orbitron',sans-serif",
        formatter:(v)=>Math.round(v*100)}
    }
  }},
  fill:{type:"gradient",gradient:{shade:"dark",type:"horizontal",gradientToColors:["#8b5cf6"],stops:[0,100]}},
  stroke:{lineCap:"round"},
  colors:["#00d4ff"],
  grid:{padding:{top:-10,bottom:-10}},
};
const gauge=new ApexCharts(gaugeEl,gaugeOpts);
gauge.render();

function updateGauge(spd){
  const pct=Math.min(spd/100,1);
  let col="#6a849e";
  if(spd>0&&spd<30)  col="#10b981";
  else if(spd<60)    col="#00d4ff";
  else if(spd<80)    col="#f59e0b";
  else if(spd>=80)   col="#ef4444";
  gauge.updateOptions({colors:[col],fill:{gradient:{gradientToColors:[spd===0?"#3a5268":"#8b5cf6"]}}},false,false);
  gauge.updateSeries([parseFloat(pct.toFixed(4))]);
}

// ── CHART.JS SPARKLINE ────────────────────────────────────────
const sparkCtx=document.getElementById("sparkCanvas").getContext("2d");
const grad=sparkCtx.createLinearGradient(0,0,0,52);
grad.addColorStop(0,"rgba(0,212,255,0.28)");
grad.addColorStop(1,"rgba(0,212,255,0)");
const spark=new Chart(sparkCtx,{
  type:"line",
  data:{labels:[],datasets:[{data:[],borderColor:"#00d4ff",borderWidth:1.5,
    backgroundColor:grad,fill:true,tension:.4,pointRadius:0}]},
  options:{
    responsive:true,maintainAspectRatio:false,
    animation:{duration:350},
    scales:{x:{display:false},y:{display:false,min:0,max:110}},
    plugins:{legend:{display:false},tooltip:{enabled:false}}
  }
});

function updateSpark(spd){
  spdHist.push(spd);
  if(spdHist.length>30) spdHist.shift();
  spark.data.labels=spdHist.map((_,i)=>i);
  spark.data.datasets[0].data=[...spdHist];
  spark.update("none");
}

// ── SPEED HELPERS ─────────────────────────────────────────────
function spdZone(s){
  if(s===0) return{lbl:"STOPPED",c:"#6a849e",bg:"rgba(107,132,158,.08)",b:"rgba(107,132,158,.18)"};
  if(s<30)  return{lbl:"SLOW",   c:"#10b981",bg:"rgba(16,185,129,.08)", b:"rgba(16,185,129,.22)"};
  if(s<60)  return{lbl:"NORMAL", c:"#00d4ff",bg:"rgba(0,212,255,.08)",  b:"rgba(0,212,255,.22)"};
  if(s<80)  return{lbl:"FAST",   c:"#f59e0b",bg:"rgba(245,158,11,.08)", b:"rgba(245,158,11,.22)"};
  return    {lbl:"OVERSPEED",     c:"#ef4444",bg:"rgba(239,68,68,.08)",  b:"rgba(239,68,68,.22)"};
}
function spdColor(s){
  if(s===0) return"#6a849e";if(s<30) return"#10b981";if(s<60) return"#00d4ff";if(s<80) return"#f59e0b";return"#ef4444";
}

// ── TOAST ────────────────────────────────────────────────────
const _ti={
  info:'<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>',
  warn:'<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>',
  err: '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>',
  ok:  '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><polyline points="20 6 9 17 4 12"/></svg>'
};
function toast(msg,type="info",dur=3200){
  const el=document.createElement("div");
  el.className=`toast-item ${type}`;
  el.innerHTML=`${_ti[type]||_ti.info}<span>${msg}</span>`;
  document.getElementById("toast").appendChild(el);
  setTimeout(()=>{
    el.style.cssText+="transition:opacity .28s,transform .28s;opacity:0;transform:translateX(8px)";
    setTimeout(()=>el.remove(),290);
  },dur);
}

// ── OVERLAY ──────────────────────────────────────────────────
function showW(){document.getElementById("wait-overlay").classList.remove("hidden")}
function hideW(){document.getElementById("wait-overlay").classList.add("hidden")}

// ── GEO ──────────────────────────────────────────────────────
function haversine(a,b,c,d){const R=6371000,r=Math.PI/180,dA=(c-a)*r,dB=(d-b)*r,x=Math.sin(dA/2)**2+Math.cos(a*r)*Math.cos(c*r)*Math.sin(dB/2)**2;return R*2*Math.atan2(Math.sqrt(x),Math.sqrt(1-x))}
function bearing(a,b,c,d){const r=Math.PI/180,dL=(d-b)*r,y=Math.sin(dL)*Math.cos(c*r),x=Math.cos(a*r)*Math.sin(c*r)-Math.sin(a*r)*Math.cos(c*r)*Math.cos(dL);return(Math.atan2(y,x)*180/Math.PI+360)%360}

// ── APPLY TELEMETRY ───────────────────────────────────────────
function applyTel(t){
  hideW();
  curSpd=parseFloat(t.speed_kmh)||0;
  const la=parseFloat(t.lat), lo=parseFloat(t.lon);

  document.getElementById("v-id-lbl").textContent=t.dev_id;
  document.getElementById("v-coords").textContent=`${la.toFixed(5)}, ${lo.toFixed(5)}`;

  const vb=document.getElementById("v-badge");
  vb.className="v-badge online"; vb.textContent="ONLINE";

  const z=spdZone(curSpd);
  const sb=document.getElementById("spd-badge");
  sb.textContent=z.lbl; sb.style.cssText=`color:${z.c};border-color:${z.b};background:${z.bg}`;

  updateGauge(curSpd);
  updateSpark(curSpd);

  if(curSpd>maxSpd) maxSpd=curSpd;
  const elapsed=Math.round((Date.now()-T0)/1000), em=Math.floor(elapsed/60), es=elapsed%60;
  const tStr=em>0?`${em}m ${es}s`:`${es}s`;

  document.getElementById("st-time").textContent=tStr;
  document.getElementById("st-max").textContent=maxSpd.toFixed(1)+" km/h";

  const sos=!!t.sos_active;
  document.getElementById("sos-ring").style.display=sos?"block":"none";
  document.getElementById("sos-bar").style.display=sos?"block":"none";
  document.getElementById("sos-card").className=sos?"sos-card active":"sos-card";
  if(sos){sosCount++;document.getElementById("st-sos").textContent=sosCount;}

  let dk="0.00";
  if(prevLat!==null){
    const dist=haversine(prevLat,prevLon,la,lo);
    totalDist+=dist;
    dk=(totalDist/1000).toFixed(2);
    document.getElementById("st-dist").textContent=dk+" km";
    hdg=bearing(prevLat,prevLon,la,lo);
    if(trailOn){
      const sc=spdColor(curSpd);
      const seg=L.polyline([[prevLat,prevLon],[la,lo]],{color:sc,weight:4,opacity:.88}).addTo(map);
      trailLayers.push(seg);
      if(trailLayers.length>MAX_TRAIL){trailLayers.shift().remove();}
    }
  }
  prevLat=la; prevLon=lo;
  const ce=document.getElementById("car-el");
  if(ce) ce.style.transform=`rotate(${hdg}deg)`;
  if(!marker){marker=L.marker([la,lo],{icon:vIcon}).addTo(map);map.setView([la,lo],16);}
  else marker.setLatLng([la,lo]);

  const sps=spdHist.slice(-20);
  const avg=sps.length?Math.round(sps.reduce((a,b)=>a+b,0)/sps.length):0;
  document.getElementById("st-avg").textContent=avg+" km/h";

  // React stat cards update
  if(window._setStats) window._setStats({spd:curSpd.toFixed(1), dist:dk, max:maxSpd.toFixed(1), time:tStr});
}

// ── STOP MARKERS ─────────────────────────────────────────────
let stopMarkersVisible=false, stopLayerGroup=null, stopConfigData=[];

async function loadStopConfig(){
  try{
    const r=await fetch(`${BACKEND}/telemetry/stops/config`);
    if(!r.ok) return;
    const j=await r.json();
    stopConfigData=j.data;
    document.getElementById("stops-cfg-count").textContent=j.data.length;
    document.getElementById("stops-radius").textContent=j.radius_m+"m";
    stopLayerGroup=L.layerGroup();
    j.data.forEach(s=>{
      const icon=L.divIcon({
        className:"",
        html:`<div style="width:24px;height:24px;border-radius:50%;background:rgba(139,92,246,.13);border:1.5px solid rgba(139,92,246,.55);display:flex;align-items:center;justify-content:center;box-shadow:0 0 10px rgba(139,92,246,.28)"><svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="#8b5cf6" stroke-width="2.5"><path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z"/><circle cx="12" cy="10" r="3"/></svg></div>`,
        iconSize:[24,24],iconAnchor:[12,12]
      });
      const circle=L.circle([s.lat,s.lon],{radius:j.radius_m,color:"#8b5cf6",fillColor:"#8b5cf6",fillOpacity:.04,weight:1,dashArray:"4 5",opacity:.35});
      const mk=L.marker([s.lat,s.lon],{icon});
      mk.bindTooltip(`<b>${s.name}</b><br><small style="color:#6a849e">${s.lat.toFixed(4)}, ${s.lon.toFixed(4)}</small>`,{direction:"top",offset:[0,-7],className:"stop-tip"});
      stopLayerGroup.addLayer(circle);stopLayerGroup.addLayer(mk);
    });
  }catch(e){console.warn("stop config:",e);}
}

function toggleStopMarkers(){
  if(!stopLayerGroup){toast("Stop config not loaded yet","warn");return;}
  stopMarkersVisible=!stopMarkersVisible;
  const lbl=document.getElementById("stops-btn-lbl"), btn=document.getElementById("btn-stops");
  if(stopMarkersVisible){
    stopLayerGroup.addTo(map); lbl.textContent="Hide Stops"; btn.classList.add("active");
    toast(`Showing ${stopConfigData.length} geofence stops`,"info");
  }else{
    stopLayerGroup.remove(); lbl.textContent="Show Stops"; btn.classList.remove("active");
    toast("Stop markers hidden","info",1800);
  }
}

// ── APPLY STOPS ───────────────────────────────────────────────
function applyStops(stops){
  document.getElementById("stops-cnt").textContent=stops.length;
  const tb=document.getElementById("stops-body"), ab=document.getElementById("stops-active-badge");
  if(!stops.length){
    tb.innerHTML='<tr><td colspan="5" class="nodata">Geofence active — waiting for vehicle to enter a stop zone.</td></tr>';
    ab.style.display="none"; return;
  }
  const isAt=stops[0].duration_sec==null;
  ab.style.display=isAt?"inline-block":"none";
  tb.innerHTML=stops.map((s,i)=>{
    const arr=new Date(s.arrived_at*1000).toLocaleTimeString([],{hour:"2-digit",minute:"2-digit",second:"2-digit"});
    const day=new Date(s.arrived_at*1000).toLocaleDateString([],{month:"short",day:"numeric"});
    let dur,badge;
    if(s.duration_sec!=null){
      const m=Math.floor(s.duration_sec/60),sec=Math.round(s.duration_sec%60);
      dur=m>0?`${m}m ${sec}s`:`${sec}s`;
      badge=`<span style="font-size:.64rem;font-weight:600;padding:2px 8px;border-radius:6px;background:rgba(58,82,104,.08);border:1px solid rgba(58,82,104,.2);color:var(--txt3)">DEPARTED</span>`;
    }else{
      dur=`<span class="still">● Now</span>`;
      badge=`<span style="font-size:.64rem;font-weight:600;padding:2px 8px;border-radius:6px;background:rgba(16,185,129,.1);border:1px solid rgba(16,185,129,.28);color:var(--green)">AT STOP</span>`;
    }
    return`<tr>
      <td style="color:var(--txt3);font-size:.67rem">${i+1}</td>
      <td><span class="loc-badge">${s.location_name}</span><br><span style="font-size:.62rem;color:var(--txt3)">${day}</span></td>
      <td style="font-size:.69rem;font-variant-numeric:tabular-nums">${arr}</td>
      <td>${dur}</td><td>${badge}</td>
    </tr>`;
  }).join("");
}

// ── HEALTH ────────────────────────────────────────────────────
function setHealth(id,state,text){
  const dot=document.getElementById("h-dot-"+id);
  dot.className="h-dot "+state;
  document.getElementById("h-val-"+id).textContent=text;
}
async function checkHealth(){
  try{
    const r=await fetch(`${BACKEND}/health`);
    if(r.ok){const j=await r.json();setHealth("backend","ok","Online");setHealth("db","ok",`${j.records} recs`);}
    else{setHealth("backend","err","Error "+r.status);setHealth("db","warn","Unknown");}
  }catch{setHealth("backend","err","Unreachable");setHealth("db","err","No conn");}
}

// ── DB STATS ──────────────────────────────────────────────────
async function fetchDbStats(){
  try{
    const r=await fetch(`${BACKEND}/telemetry/stats?dev_id=${DEV_ID}`);
    if(r.ok){
      const d=(await r.json()).data;
      document.getElementById("db-total").textContent=d.total;
      document.getElementById("db-avg").textContent=d.avg_speed+" km/h";
      document.getElementById("db-max").textContent=d.max_speed+" km/h";
      document.getElementById("db-first").textContent=new Date(d.first_seen*1000).toLocaleTimeString();
      document.getElementById("st-total").textContent=d.total;
      document.getElementById("st-dbmax").textContent=d.max_speed+" km/h";
      document.getElementById("st-dbavg").textContent=d.avg_speed+" km/h";
    }
  }catch{}
}

// ── HISTORY LOG ───────────────────────────────────────────────
async function fetchHistory(){
  try{
    const r=await fetch(`${BACKEND}/telemetry/history?dev_id=${DEV_ID}`);
    if(!r.ok) return;
    historyData=(await r.json()).data;
    document.getElementById("hist-cnt").textContent=historyData.length;
    const tb=document.getElementById("hist-body");
    if(!historyData.length){tb.innerHTML='<tr><td colspan="6" class="nodata">No history yet.</td></tr>';return;}
    tb.innerHTML=historyData.map((r,i)=>{
      const spd=parseFloat(r.speed_kmh), sc=spdColor(spd);
      const sos=r.sos_active?`<span style="color:var(--red);font-weight:700;font-size:.66rem">● SOS</span>`:`<span style="color:var(--green);font-size:.66rem">✓ OK</span>`;
      return`<tr>
        <td style="color:var(--txt3);font-size:.66rem">${i+1}</td>
        <td style="font-size:.69rem">${new Date(r.timestamp*1000).toLocaleTimeString()}</td>
        <td style="font-size:.69rem;font-variant-numeric:tabular-nums">${r.lat.toFixed(5)}</td>
        <td style="font-size:.69rem;font-variant-numeric:tabular-nums">${r.lon.toFixed(5)}</td>
        <td><span class="spd-chip" style="background:${sc}16;border:1px solid ${sc}40;color:${sc}">${spd.toFixed(1)} km/h</span></td>
        <td>${sos}</td>
      </tr>`;
    }).join("");
  }catch{}
}

// ── EXPORT CSV ────────────────────────────────────────────────
function exportCSV(){
  if(!historyData.length){toast("No history data to export","warn");return;}
  const hdr="id,dev_id,timestamp,lat,lon,speed_kmh,sos_active\n";
  const rows=historyData.map(r=>`${r.id},${r.dev_id},${new Date(r.timestamp*1000).toISOString()},${r.lat},${r.lon},${r.speed_kmh},${r.sos_active}`).join("\n");
  const a=Object.assign(document.createElement("a"),{
    href:URL.createObjectURL(new Blob([hdr+rows],{type:"text/csv"})),
    download:`fleettrack_${DEV_ID}_${new Date().toISOString().slice(0,10)}.csv`
  });
  a.click();
  toast(`Exported ${historyData.length} records as CSV`,"ok");
}

// ── TABS ──────────────────────────────────────────────────────
function switchTab(name){
  ["stops","history","stats"].forEach(t=>{
    document.getElementById("tab-"+t).classList.toggle("active",t===name);
    document.getElementById("pane-"+t).classList.toggle("active",t===name);
  });
}

// ── SIDEBAR ───────────────────────────────────────────────────
let sidebarOpen=true;
function toggleSidebar(){
  sidebarOpen=!sidebarOpen;
  document.getElementById("sidebar").classList.toggle("collapsed",!sidebarOpen);
  setTimeout(()=>map.invalidateSize(),290);
  toast(sidebarOpen?"Sidebar opened":"Full map view","info",1800);
}

// ── MAP CONTROLS ─────────────────────────────────────────────
function centerMap(){
  if(marker){map.setView(marker.getLatLng(),16,{animate:true,duration:1});toast("Centered on vehicle","info",2000);}
  else toast("No vehicle data yet","warn");
}
function toggleTrail(){
  trailOn=!trailOn;
  trailLayers.forEach(l=>l.setStyle({opacity:trailOn?.88:0}));
  toast(trailOn?"Trail visible":"Trail hidden","info",2000);
}
function clearTrail(){
  trailLayers.forEach(l=>l.remove()); trailLayers=[];
  totalDist=0;
  document.getElementById("st-dist").textContent="0.00 km";
  if(window._setStats) window._setStats(s=>({...s, dist:"0.00"}));
  toast("Trail cleared","ok",2000);
}
function togglePause(){
  paused=!paused;
  const pi=document.getElementById("pause-icon"), pl=document.getElementById("pause-lbl");
  const hd=document.getElementById("h-dot-poll"), hv=document.getElementById("h-val-poll");
  if(paused){
    pl.textContent="Resume";
    pi.innerHTML='<polygon points="5 3 19 12 5 21 5 3"/>';
    hd.className="h-dot warn"; hv.textContent="Paused";
    if(pollTimer){clearInterval(pollTimer);pollTimer=null;}
    toast("Auto-refresh paused","warn",2500);
  }else{
    pl.textContent="Pause";
    pi.innerHTML='<rect x="6" y="4" width="4" height="16"/><rect x="14" y="4" width="4" height="16"/>';
    hd.className="h-dot ok"; hv.textContent="Every 3s";
    pollTimer=setInterval(poll,POLL_MS);
    toast("Auto-refresh resumed","ok",2500);
  }
}

// ── POLL LOOP ─────────────────────────────────────────────────
async function poll(){
  const t0=performance.now();
  let currentRecords='—';
  try{
    const dr=await fetch(`${BACKEND}/telemetry/devices`);
    if(dr.ok){
      const dj=await dr.json();
      if(dj.data.length>0){
        DEV_ID=dj.data[0].dev_id;
        currentRecords=String(dj.data[0].total);
      } else {
        showW();
        if(window._setHeader) window._setHeader({ms:'— ms',records:'—',live:false,label:'No Data',time:'—',fast:null});
        return;
      }
    }
    const[tr,sr]=await Promise.all([
      fetch(`${BACKEND}/telemetry/latest?dev_id=${DEV_ID}`),
      fetch(`${BACKEND}/telemetry/stops?dev_id=${DEV_ID}`)
    ]);
    if(tr.ok){
      const tel=(await tr.json()).data;
      applyTel(tel);
      const age=Date.now()/1000-tel.timestamp;
      if(age<10)       setHealth("device","ok","Active · "+DEV_ID);
      else if(age<30)  setHealth("device","warn","Delayed · "+Math.round(age)+"s ago");
      else             setHealth("device","err","Stale · "+Math.round(age)+"s ago");
    }
    if(sr.ok) applyStops((await sr.json()).data);
    await Promise.all([fetchDbStats(),fetchHistory(),checkHealth()]);

    const ms=Math.round(performance.now()-t0);
    if(window._setHeader) window._setHeader({
      ms:`${ms} ms`,
      records:currentRecords,
      live:true,
      label:'Live · '+DEV_ID,
      time:new Date().toLocaleTimeString(),
      fast:ms<200
    });
  }catch(e){
    if(window._setHeader) window._setHeader(s=>({...s,live:false,label:'Error'}));
    console.error(e);
  }
}

// ── REACT COMPONENTS ─────────────────────────────────────────
// HeaderPills and StatCards are React-managed; vanilla JS calls
// window._setHeader / window._setStats to push new state in.
(function mountReact(){
  const { useState } = React;
  const h = React.createElement;

  function HeaderPills(){
    const [s, set] = useState({ms:'— ms',records:'—',live:false,label:'Connecting…',time:'—',fast:null});
    window._setHeader = set;
    return h('div', {className:'hdr-right'},
      h('div', {className:'pill'+(s.fast===null?'':s.fast?' fast':' slow')}, s.ms),
      h('div', {className:'pill'},
        'Records ',
        h('strong', {style:{color:'var(--txt)'}}, s.records)
      ),
      h('div', {className:s.live?'pill live':'pill offline'},
        s.live ? h('span',{className:'dot'}) : null,
        ' ',
        h('span', null, s.label)
      ),
      h('div', {className:'pill', style:{fontSize:'.65rem'}}, s.time)
    );
  }

  function StatCards(){
    const [s, set] = useState({spd:'—',dist:'0.00',max:'—',time:'—'});
    window._setStats = set;
    const card = (cls, label, val, unit) =>
      h('div', {className:'stat-card '+cls},
        h('div', {className:'slbl'}, label),
        h('div', {className:'sval '+cls}, val),
        h('div', {className:'ssub'}, unit)
      );
    return h('div', {className:'stat-grid'},
      card('c','Current Speed', s.spd, 'km/h'),
      card('g','Distance',      s.dist,'km'),
      card('a','Peak Speed',    s.max, 'km/h'),
      card('p','Session Time',  s.time,'elapsed')
    );
  }

  const hdrEl   = document.getElementById('react-header-pills');
  const statsEl = document.getElementById('react-stat-cards');
  if(hdrEl)   ReactDOM.createRoot(hdrEl).render(h(HeaderPills));
  if(statsEl) ReactDOM.createRoot(statsEl).render(h(StatCards));
})();

// ── START ────────────────────────────────────────────────────
poll();
pollTimer=setInterval(poll,POLL_MS);
