// Page web du banc de test (aucune ressource externe : fonctionne sans Internet)
#pragma once
#include <Arduino.h>

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Banc pédale PSA</title>
<style>
:root{--bg:#111;--card:#1c1c1e;--txt:#eee;--dim:#8a8a8e;--ok:#34c759;--warn:#ff9f0a;--bad:#ff453a}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--txt)}
header{display:flex;align-items:center;gap:8px;padding:8px 12px;background:#222;position:sticky;top:0;z-index:2;flex-wrap:wrap}
h1{font-size:16px;margin:0}
.pill{padding:3px 9px;border-radius:10px;font-weight:700;font-size:12px;color:#000;background:#666}
.sp{flex:1}
button{background:#333;color:var(--txt);border:0;border-radius:6px;padding:8px 12px;font-size:14px;cursor:pointer}
button.rec{background:var(--bad);color:#000;font-weight:700}
main{padding:10px;max-width:1100px;margin:auto}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px}
.card{background:var(--card);border-radius:8px;padding:8px 10px}
.l{font-size:12px;color:var(--dim)}
.v{font-size:21px;font-variant-numeric:tabular-nums;white-space:nowrap}
.big .v{font-size:34px;font-weight:700}
h2{font-size:13px;color:var(--dim);margin:16px 0 6px;text-transform:uppercase;letter-spacing:.05em}
canvas{display:block;width:100%;background:var(--card);border-radius:8px}
.note{color:var(--dim);font-size:13px}
</style></head><body>
<header>
  <h1>Banc pédale PSA</h1><span id="st" class="pill">...</span><span id="adc" class="note"></span>
  <span class="sp"></span>
  <button id="csv">Enregistrer CSV</button><button id="rst">Remise à zéro</button>
</header>
<main>
  <div class="grid">
    <div class="card big"><div class="l">écart S1 − K·S2 (% du 5 V)</div><div class="v" id="vE">-</div></div>
    <div class="card big"><div class="l">position</div><div class="v" id="vP">-</div></div>
  </div>
  <div class="grid" id="g" style="margin-top:8px"></div>
  <h2>Temps réel — 20 s</h2>
  <canvas id="ct" style="height:200px"></canvas>
  <canvas id="ce" style="height:110px;margin-top:6px"></canvas>
  <h2>Balayage — écart max par position (enfoncer lentement la pédale)</h2>
  <canvas id="cs" style="height:170px"></canvas>
  <p class="note">Abscisse : S1 en % du 5 V. Une barre orange ou rouge localise une zone de la course où les deux pistes divergent (poussière d'aimant, usure, défaut du capteur).</p>
  <h2>Statistiques depuis la remise à zéro</h2>
  <div class="grid" id="gs"></div>
</main>
<script>
const $=id=>document.getElementById(id), WIN=20, H=[];
const COL={ok:'#34c759',warn:'#ff9f0a',bad:'#ff453a'};
let th={k:2,warn:3,fault:6}, rec=null;
function card(g,id,l){$(g).insertAdjacentHTML('beforeend',`<div class="card"><div class="l">${l}</div><div class="v" id="${id}">-</div></div>`)}
[['vS1','S1 (broche 1)'],['vS2K','K × S2'],['vS2','S2 (broche 3)'],['vV','Vcc (broche 4)'],['vR','S1 / S2'],
 ['vM','écart max'],['vF','incidents'],['vO','hors plage'],['vHz','cadence']].forEach(([i,l])=>card('g',i,l));
[['vA','pente S1/S2'],['vB','décalage'],['vR2','R²'],['vS1r','S1 min..max'],['vS2r','S2 min..max'],['vVr','Vcc min..max'],['vN','échantillons']].forEach(([i,l])=>card('gs',i,l));
function set(id,t,c){const e=$(id);e.textContent=t;e.style.color=c||''}
const ec=v=>{v=Math.abs(v);return v>th.fault?COL.bad:(v>th.warn?COL.warn:COL.ok)};
const stc={'OK':COL.ok,'ECART':COL.warn};

function canvas(cv){const r=devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;cv.width=w*r;cv.height=h*r;const x=cv.getContext('2d');x.scale(r,r);x.font='10px sans-serif';return [x,w,h]}
function drawTime(){
  let [x,w,h]=canvas($('ct'));if(!H.length)return;
  const now=H[H.length-1].t,X=t=>w-(now-t)/1000/WIN*w,Y=v=>h-4-v/5*(h-8);
  for(let v=0;v<=5;v++){x.strokeStyle='#2c2c2e';x.beginPath();x.moveTo(0,Y(v));x.lineTo(w,Y(v));x.stroke();x.fillStyle='#666';x.fillText(v+' V',3,Y(v)-2)}
  [['S1','#ffd60a',p=>[p.s1,p.w.s1min,p.w.s1max]],['K × S2','#64d2ff',p=>[p.s2*th.k,p.w.s2kmin,p.w.s2kmax]]].forEach(([n,c,f],k)=>{
    x.fillStyle=c+'55';H.forEach(p=>{const b=f(p);x.fillRect(X(p.t)-1.5,Y(b[2]),3,Math.max(1,Y(b[1])-Y(b[2])))});
    x.strokeStyle=c;x.lineWidth=1.5;x.beginPath();H.forEach((p,i)=>{const b=f(p);i?x.lineTo(X(p.t),Y(b[0])):x.moveTo(X(p.t),Y(b[0]))});x.stroke();
    x.fillStyle=c;x.fillText(n,w-60,13+13*k)});
  [x,w,h]=canvas($('ce'));
  const E=10,Ye=v=>h/2-Math.max(-E,Math.min(E,v))/E*(h/2-4);
  x.strokeStyle='#555';x.beginPath();x.moveTo(0,h/2);x.lineTo(w,h/2);x.stroke();
  [[th.warn,COL.warn],[th.fault,COL.bad]].forEach(([v,c])=>{x.strokeStyle=c+'88';x.setLineDash([4,4]);[v,-v].forEach(u=>{x.beginPath();x.moveTo(0,Ye(u));x.lineTo(w,Ye(u));x.stroke()});x.setLineDash([])});
  H.forEach(p=>{const a=Math.max(Math.abs(p.w.emin),Math.abs(p.w.emax));x.fillStyle=ec(a);x.fillRect(X(p.t)-1.5,Ye(p.w.emax),3,Math.max(1,Ye(p.w.emin)-Ye(p.w.emax)))});
  x.fillStyle='#888';x.fillText('écart ±10 %',3,11);
}
async function drawSweep(){
  try{
    const b=await (await fetch('/api/sweep')).json();
    const [x,w,h]=canvas($('cs')),Y=v=>h-14-Math.min(v,10)/10*(h-20);
    [[th.warn,COL.warn],[th.fault,COL.bad]].forEach(([v,c])=>{x.strokeStyle=c+'88';x.setLineDash([4,4]);x.beginPath();x.moveTo(0,Y(v));x.lineTo(w,Y(v));x.stroke();x.setLineDash([])});
    let seen=0;b.forEach((e,i)=>{if(e===null)return;seen++;x.fillStyle=ec(e);const x0=i*w/b.length;x.fillRect(x0,Y(e),Math.max(1,w/b.length-1),Math.max(1,Y(0)-Y(e)))});
    if(H.length){const p=H[H.length-1],cx=p.s1/(p.vcc>3?p.vcc:5)*w;x.strokeStyle='#fff';x.beginPath();x.moveTo(cx,0);x.lineTo(cx,h-14);x.stroke()}
    x.fillStyle='#888';x.fillText('0 %',2,h-2);x.fillText('100 % Vcc',w-55,h-2);x.fillText(`couvert ${seen}/100 — échelle 0..10 %`,3,11);
  }catch(e){}
  setTimeout(drawSweep,1000);
}
function update(d){
  th=d.th;$('adc').textContent='ADC '+d.adc;
  const p=$('st');p.textContent=d.st;p.style.background=stc[d.st]||COL.bad;
  set('vE',(d.err>=0?'+':'')+d.err.toFixed(2)+' %',ec(d.err));set('vP',d.pos.toFixed(0)+' %');
  set('vS1',d.s1.toFixed(3)+' V');set('vS2K',(d.s2*th.k).toFixed(3)+' V');set('vS2',d.s2.toFixed(3)+' V');set('vV',d.vcc.toFixed(3)+' V');
  set('vR',d.s2>0.05?(d.s1/d.s2).toFixed(3):'-');
  set('vM',(d.errMax>=0?'+':'')+d.errMax.toFixed(2)+' % à '+d.errMaxPos.toFixed(0)+' %',ec(d.errMax));
  set('vF',d.faults,d.faults?COL.bad:'');set('vO',d.oor,d.oor?COL.bad:'');set('vHz',d.hz+' Hz');
  set('vA',d.reg?d.a.toFixed(4):'—',d.reg?(Math.abs(d.a-th.k)>0.1?COL.warn:COL.ok):'');
  set('vB',d.reg?(d.b>=0?'+':'')+d.b.toFixed(3)+' V':'—');set('vR2',d.reg?d.r2.toFixed(5):'—');
  set('vS1r',d.s1r[0].toFixed(3)+' .. '+d.s1r[1].toFixed(3));set('vS2r',d.s2r[0].toFixed(3)+' .. '+d.s2r[1].toFixed(3));
  set('vVr',d.vccr[0].toFixed(3)+' .. '+d.vccr[1].toFixed(3));set('vN',d.n);
}
async function poll(){
  try{
    const d=await (await fetch('/api/live',{cache:'no-store'})).json();
    H.push(d);while(H.length&&d.t-H[0].t>WIN*1000)H.shift();
    if(rec)rec.push([d.t,d.s1,d.s2,d.vcc,d.err,d.w.emin,d.w.emax,d.w.s1min,d.w.s1max].join(','));
    update(d);drawTime();
  }catch(e){const p=$('st');p.textContent='HORS LIGNE';p.style.background='#666'}
  setTimeout(poll,100);
}
$('rst').onclick=()=>{fetch('/api/reset',{method:'POST'});H.length=0};
$('csv').onclick=()=>{
  const b=$('csv');
  if(!rec){rec=['t_ms,S1_V,S2_V,Vcc_V,ecart_pct,ecart_min,ecart_max,S1_min,S1_max'];b.textContent='Arrêter et télécharger';b.classList.add('rec');return}
  const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([rec.join('\n')+'\n'],{type:'text/csv'}));
  a.download='banc_pedale_'+new Date().toISOString().slice(0,19).replace(/[:T]/g,'-')+'.csv';a.click();
  rec=null;b.textContent='Enregistrer CSV';b.classList.remove('rec');
};
addEventListener('resize',drawTime);
poll();drawSweep();
</script></body></html>
)HTML";
