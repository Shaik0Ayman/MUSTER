// SPDX-License-Identifier: GPL-3.0-only
// dashboard.h - the page the lead serves at http://192.168.4.1/ . One
// self-contained file: no CDN, no fonts, no libraries, because the lead's access
// point has no internet. It polls /api/state twice a second and keeps the
// history of each board in the browser, so the lead holds none.
// tools/trio_mock.py serves this exact page with simulated data for previewing.
#pragma once
#include <pgmspace.h>

const char TRIO_DASH_HTML[] PROGMEM = R"TRIOHTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Muster trio</title>
<style>
:root{
  color-scheme: light;
  --bg:#f4f3f0; --surface:#fcfcfb; --line:#e3e1db; --track:#ece9e2;
  --text:#0b0b0b; --text2:#52514e; --muted:#7a7873;
  --b0:#2a78d6; --b1:#eb6834; --b2:#1baf7a; --b3:#eda100;
  --good:#0ca30c; --warn:#fab219; --bad:#d03b3b;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    color-scheme: dark;
    --bg:#111110; --surface:#1a1a19; --line:#2c2c2a; --track:#2a2a28;
    --text:#f0efec; --text2:#c3c2b7; --muted:#8d8c85;
    --b0:#3987e5; --b1:#d95926; --b2:#199e70; --b3:#c98500;
  }
}
:root[data-theme="dark"]{
  color-scheme: dark;
  --bg:#111110; --surface:#1a1a19; --line:#2c2c2a; --track:#2a2a28;
  --text:#f0efec; --text2:#c3c2b7; --muted:#8d8c85;
  --b0:#3987e5; --b1:#d95926; --b2:#199e70; --b3:#c98500;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:1180px;margin:0 auto;padding:20px 16px 40px}
header{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:baseline;justify-content:space-between;margin-bottom:16px}
h1{font-size:20px;margin:0;font-weight:650}
.sub{color:var(--text2);font-size:13px}
.pill{display:inline-flex;align-items:center;gap:6px;font-size:13px;color:var(--text);border:1px solid var(--line);background:var(--surface);border-radius:999px;padding:3px 10px}
.pill i{font-style:normal;font-weight:700}
.ok i{color:var(--good)} .off i{color:var(--bad)} .wait i{color:var(--warn)}
.banner{display:none;margin:0 0 12px;padding:8px 12px;border:1px solid var(--bad);border-radius:8px;background:var(--surface);font-size:14px}
.banner.on{display:block}
.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(130px,100%),1fr));gap:10px;margin-bottom:14px}
.kpi{background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:10px 14px}
.kpi b{display:block;font-size:26px;line-height:1.15;font-weight:650;font-variant-numeric:tabular-nums}
.kpi span{font-size:12px;color:var(--text2)}
section.card,.board{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:14px 16px}
h2{font-size:13px;font-weight:650;margin:0 0 10px;color:var(--text2);letter-spacing:.02em;text-transform:uppercase}
.share{display:flex;gap:2px;height:28px;border-radius:6px;overflow:hidden;background:var(--track)}
.share div{display:flex;align-items:center;justify-content:center;font-size:12px;font-weight:600;color:#fff;min-width:0;white-space:nowrap;overflow:hidden;text-shadow:0 0 2px rgba(0,0,0,.45)}
.legend{display:flex;flex-wrap:wrap;gap:4px 16px;margin-top:8px;font-size:13px;color:var(--text2)}
.sw{display:inline-block;width:10px;height:10px;border-radius:3px;margin-right:6px;vertical-align:-1px}
.boards{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(300px,100%),1fr));gap:12px;margin:14px 0}
.board.lost{opacity:.62}
.bh{display:flex;align-items:center;gap:8px;margin-bottom:2px}
.bh .nm{font-weight:650;font-size:16px}
.bh .st{margin-left:auto}
.chip{color:var(--text2);font-size:13px;margin-bottom:10px}
.duty{display:flex;align-items:baseline;gap:8px}
.duty b{font-size:34px;font-weight:650;font-variant-numeric:tabular-nums;line-height:1}
.duty span{color:var(--text2);font-size:13px}
.meter{height:8px;background:var(--track);border-radius:4px;margin:8px 0 6px;overflow:hidden}
.meter div{height:100%;border-radius:4px;width:0}
.cap{font-size:12px;color:var(--muted);margin:10px 0 2px}
.spark{position:relative}
.spark svg{display:block;width:100%;height:56px;touch-action:none}
.tip{position:absolute;top:-4px;pointer-events:none;background:var(--surface);color:var(--text);border:1px solid var(--line);border-radius:6px;padding:2px 7px;font-size:12px;white-space:nowrap;display:none;box-shadow:0 2px 8px rgba(0,0,0,.18);transform:translateX(-50%)}
dl{display:grid;grid-template-columns:auto 1fr;gap:3px 14px;margin:12px 0 0;font-size:13px}
dt{color:var(--text2)} dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}
table{width:100%;border-collapse:collapse;font-size:13px}
td{padding:4px 0;border-top:1px solid var(--line);vertical-align:top}
td:first-child{width:70px;color:var(--muted);font-variant-numeric:tabular-nums}
footer{color:var(--muted);font-size:12px;margin-top:14px}
</style>
</head>
<body>
<main>
  <header>
    <div>
      <h1>Muster &middot; trio demo</h1>
      <div class="sub">Three ESP32s share one math task over ESP-NOW. Each answer sets a PWM duty on the board that computed it.</div>
    </div>
    <span id="conn" class="pill wait"><i>&#9680;</i><span>connecting</span></span>
  </header>
  <div id="banner" class="banner">Cannot reach the lead. Check you are still on the <b>muster-trio</b> WiFi. Retrying&hellip;</div>

  <div class="kpis">
    <div class="kpi"><b id="k-alive">-</b><span>boards alive</span></div>
    <div class="kpi"><b id="k-ok">-</b><span>calls completed</span></div>
    <div class="kpi"><b id="k-cps">-</b><span>calls per second</span></div>
    <div class="kpi"><b id="k-fail">-</b><span>calls failed</span></div>
    <div class="kpi"><b id="k-radio">-</b><span>radio send failures</span></div>
  </div>

  <section class="card">
    <h2>Who did the work</h2>
    <div id="share" class="share"></div>
    <div id="legend" class="legend"></div>
  </section>

  <div id="boards" class="boards"></div>

  <section class="card">
    <h2>Cluster events</h2>
    <table><tbody id="log"><tr><td></td><td>nothing yet</td></tr></tbody></table>
  </section>
  <footer id="foot">Updates every 0.5 s &middot; served by the lead</footer>
</main>
<script>
"use strict";
var CHIPS={0:"ESP32",1:"ESP32",2:"ESP32-C6",3:"ESP32-S3"}, N=90, POLL=500;
var hist={}, cards={}, fails=0;
function $(id){return document.getElementById(id);}
function col(slot){return "var(--b"+(slot%4)+")";}
function dur(ms){var s=Math.floor(ms/1000),h=Math.floor(s/3600),m=Math.floor(s%3600/60);s%=60;return (h?h+":":"")+(h&&m<10?"0":"")+m+":"+(s<10?"0":"")+s;}
function us(v){return v>=1000?(v/1000).toFixed(1)+" ms":Math.round(v)+" µs";}
function pct(d){return Math.round(d/255*100);}
function el(tag,cls,html){var e=document.createElement(tag);if(cls)e.className=cls;if(html!==undefined)e.innerHTML=html;return e;}

var W=240,H=56,P=4;
function sparkInit(color){
  return '<svg viewBox="0 0 '+W+' '+H+'" preserveAspectRatio="none" role="img">'+
    '<line x1="0" x2="'+W+'" y1="'+(H-P)+'" y2="'+(H-P)+'" stroke="var(--line)" stroke-width="1"/>'+
    '<polyline class="ln" points="" fill="none" stroke="'+color+'" stroke-width="2" stroke-linejoin="round" stroke-linecap="round" vector-effect="non-scaling-stroke"/>'+
    '<g class="hv" style="display:none"><line y1="0" y2="'+H+'" stroke="var(--muted)" stroke-width="1" vector-effect="non-scaling-stroke"/>'+
    '<circle r="4" fill="'+color+'" stroke="var(--surface)" stroke-width="2" vector-effect="non-scaling-stroke"/></g></svg>';
}
function sparkSet(box,data,ymax){
  var pts=[],n=data.length,i,x,y;
  for(i=0;i<n;i++){x=W-P-(n-1-i)*(W-2*P)/(N-1); y=H-P-Math.min(data[i],ymax)/ymax*(H-2*P); pts.push(x.toFixed(1)+","+y.toFixed(1));}
  box.querySelector(".ln").setAttribute("points",n>1?pts.join(" "):"");
}
function hover(box,getData,fmt,ymax){
  var tip=box.querySelector(".tip");
  box.addEventListener("pointermove",function(ev){
    var d=getData(), svg=box.querySelector("svg"), r=svg.getBoundingClientRect(), n=d.length;
    if(n<2)return;
    var fx=(ev.clientX-r.left)/r.width*W, i=n-1-Math.round((W-P-fx)*(N-1)/(W-2*P));
    i=Math.max(0,Math.min(n-1,i));
    var x=W-P-(n-1-i)*(W-2*P)/(N-1), y=H-P-Math.min(d[i],ymax())/ymax()*(H-2*P), g=svg.querySelector(".hv");
    g.style.display="";g.querySelector("line").setAttribute("x1",x);g.querySelector("line").setAttribute("x2",x);
    var c=g.querySelector("circle");c.setAttribute("cx",x);c.setAttribute("cy",y);
    tip.textContent=fmt(d[i])+"  ·  "+((n-1-i)*POLL/1000).toFixed(1)+" s ago";
    tip.style.display="block";tip.style.left=Math.max(60,Math.min(r.width-60,x/W*r.width))+"px";
  });
  box.addEventListener("pointerleave",function(){tip.style.display="none";box.querySelector(".hv").style.display="none";});
}

function makeCard(b){
  var c={slot:b.slot}, root=el("div","board");
  root.innerHTML=
   '<div class="bh"><span class="sw" style="background:'+col(b.slot)+';margin:0"></span><span class="nm"></span><span class="pill st"></span></div>'+
   '<div class="chip"></div>'+
   '<div class="duty"><b class="d">0%</b><span>PWM duty on GPIO output</span></div>'+
   '<div class="meter"><div style="background:'+col(b.slot)+'"></div></div>'+
   '<div class="cap">PWM duty, last 45 s</div><div class="spark s1"><div class="tip"></div></div>'+
   '<div class="cap">Time per task, last 45 s</div><div class="spark s2"><div class="tip"></div></div>'+
   '<dl></dl>';
  c.root=root;
  c.s1=root.querySelector(".s1"); c.s2=root.querySelector(".s2");
  c.s1.insertAdjacentHTML("afterbegin",sparkInit(col(b.slot))); c.s2.insertAdjacentHTML("afterbegin",sparkInit(col(b.slot)));
  hover(c.s1,function(){return hist[b.slot].duty;},function(v){return pct(v)+"% duty";},function(){return 255;});
  hover(c.s2,function(){return hist[b.slot].busy;},function(v){return us(v);},function(){return c.ymax2||1;});
  $("boards").appendChild(root);
  return c;
}

function update(s){
  var boards=s.boards, tot=0, i;
  $("k-alive").textContent=s.alive+" / "+boards.length;
  $("k-ok").textContent=s.ok;
  $("k-cps").textContent=s.cps.toFixed(1);
  $("k-fail").textContent=s.failed;
  $("k-radio").textContent=s.radio.txfail;
  var conn=$("conn");
  if(s.alive>=boards.length&&boards.length>1){conn.className="pill ok";conn.innerHTML="<i>✓</i><span>all boards up</span>";}
  else if(boards.length<=1){conn.className="pill wait";conn.innerHTML="<i>◐</i><span>waiting for workers</span>";}
  else{conn.className="pill off";conn.innerHTML="<i>✕</i><span>"+(boards.length-s.alive)+" board(s) lost</span>";}

  boards.forEach(function(b){
    var h=hist[b.slot]||(hist[b.slot]={duty:[],busy:[]});
    h.duty.push(b.duty); h.busy.push(b.busy);
    if(h.duty.length>N){h.duty.shift();h.busy.shift();}
    var c=cards[b.slot]||(cards[b.slot]=makeCard(b)), r=c.root;
    tot+=b.calls;
    r.classList.toggle("lost",!b.alive);
    r.querySelector(".nm").textContent=b.name;
    var st=r.querySelector(".st");
    if(!b.alive){st.className="pill st off";st.innerHTML="<i>✕</i> lost";}
    else if(!b.lead&&b.age<0){st.className="pill st wait";st.innerHTML="<i>◐</i> joined, no result yet";}
    else{st.className="pill st ok";st.innerHTML="<i>✓</i> alive";}
    r.querySelector(".chip").textContent=(CHIPS[b.chip]||"ESP32")+" · node "+b.node+" · "+b.cpu+" MHz";
    r.querySelector(".d").textContent=pct(b.duty)+"%";
    r.querySelector(".meter div").style.width=pct(b.duty)+"%";
    c.ymax2=Math.max.apply(null,h.busy.concat([1]))*1.15;
    sparkSet(c.s1,h.duty,255); sparkSet(c.s2,h.busy,c.ymax2);
    var rows=[["Tasks run on this board",b.tasks],["Results seen by lead",b.calls],
      ["Last task",b.calls?us(b.busy)+" (avg "+us(b.avg)+")":"-"],["Series value",b.calls?b.val.toFixed(3):"-"],
      ["Free heap",(b.heap/1024).toFixed(1)+" KB"],["Uptime",dur(b.up)],
      ["Calls in flight / queued",b.inflight+" / "+b.queued],["Last result",b.age<0?"-":(b.age/1000).toFixed(1)+" s ago"],
      ["Chip ID",b.uid]];
    r.querySelector("dl").innerHTML=rows.map(function(x){return "<dt>"+x[0]+"</dt><dd>"+x[1]+"</dd>";}).join("");
  });

  var share=$("share"), legend=$("legend");
  share.innerHTML="";legend.innerHTML="";
  boards.forEach(function(b){
    var p=tot?b.calls/tot*100:0;
    if(p>0){var d=el("div","",p>=9?Math.round(p)+"%":"");d.style.flex=String(p);d.style.background=col(b.slot);d.title=b.name+": "+b.calls+" calls";share.appendChild(d);}
    legend.appendChild(el("span","",'<span class="sw" style="background:'+col(b.slot)+'"></span>'+b.name+" · "+b.calls+" calls ("+Math.round(p)+"%)"));
  });
  if(!tot)share.appendChild(el("div","","no results yet"));

  var lg=$("log");
  lg.innerHTML=s.log.length?s.log.slice().reverse().map(function(l){return "<tr><td>"+dur(l.t)+"</td><td>"+l.m+"</td></tr>";}).join(""):"<tr><td></td><td>nothing yet</td></tr>";
  $("foot").textContent="Updates every 0.5 s · served by the lead · WiFi \""+s.ssid+"\", channel "+s.ch+" · lead up "+dur(s.t);
}

function tick(){
  var ctl=new AbortController(), to=setTimeout(function(){ctl.abort();},1500);
  fetch("/api/state",{signal:ctl.signal,cache:"no-store"}).then(function(r){return r.json();}).then(function(s){
    clearTimeout(to);fails=0;$("banner").classList.remove("on");update(s);
  }).catch(function(){
    clearTimeout(to);fails++;
    if(fails>=2){$("banner").classList.add("on");$("conn").className="pill off";$("conn").innerHTML="<i>✕</i><span>lead unreachable</span>";}
  }).then(function(){setTimeout(tick,POLL);});
}
tick();
</script>
</body>
</html>
)TRIOHTML";
