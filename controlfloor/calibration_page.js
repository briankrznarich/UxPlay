/* SPDX-License-Identifier: GPL-3.0-or-later */
'use strict';
// Shortcuts may reopen an existing Safari tab with only a fresh fragment.
window.addEventListener('hashchange',()=>location.reload());
(async function() {
const $=id=>document.getElementById(id),sleep=ms=>new Promise(r=>setTimeout(r,ms));
const session=location.hash.slice(1),surface=$('surface'),canvas=$('trace'),ctx=canvas.getContext('2d');
let aborted=false,finished=false,trace=null,sequence=0,baseline='',map=null,geometry;
const status=text=>{$('status').textContent=text;};
function shape(){const v=visualViewport;return JSON.stringify([innerWidth,innerHeight,screen.width,screen.height,v?.width,v?.height,v?.scale,v?.offsetLeft,v?.offsetTop]);}
function healthy(){
  if(aborted)throw Error('Calibration cancelled');
  if(document.hidden)throw Error('Calibration page left the foreground');
  if(baseline&&shape()!==baseline)throw Error('Viewport changed during calibration; reopen the page and retry');
}
async function post(body){
  const response=await fetch('/api/calibration',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({session,...body}),signal:AbortSignal.timeout(30000)});
  const data=await response.json();if(!response.ok)throw Error(data.error||`HTTP ${response.status}`);return data;
}
async function call(operation,args={}) {
  healthy();
  if(args.label)status(args.label);
  let result=await post({operation,...args});const job=result.job;
  while(result.busy){await sleep(80);healthy();result=await post({operation:'status'});if(result.job!==job)throw Error('Calibration operation changed');}
  if(result.error)throw Error(result.error);
  return result.result;
}
function point(event){return {x:event.clientX,y:event.clientY,t:event.timeStamp};}
function draw(){
  ctx.clearRect(0,0,canvas.width,canvas.height);
  if(!trace?.points.length)return;
  const r=surface.getBoundingClientRect();ctx.strokeStyle='#167dba';ctx.lineWidth=2;ctx.beginPath();
  trace.points.forEach((p,i)=>{if(i)ctx.lineTo(p.x-r.left,p.y-r.top);else ctx.moveTo(p.x-r.left,p.y-r.top);});ctx.stroke();
  const p=trace.points.at(-1);ctx.fillStyle='#167dba';ctx.beginPath();ctx.arc(p.x-r.left,p.y-r.top,4,0,Math.PI*2);ctx.fill();
}
function record(event){
  if(!trace)return;
  if(trace.points.length>=4096){trace.cancelled=true;return;}
  for(const e of (event.getCoalescedEvents?.()||[]).concat([event])) {
    const p=point(e);if(trace.points.length<4096)trace.points.push(p);
    const r=surface.getBoundingClientRect();
    if(p.x<r.left+10||p.x>r.right-10||p.y<r.top+10||p.y>r.bottom-10)trace.clipped=true;
    if(map){const q=logical(p);if(q.x<geometry.width*.2||q.x>geometry.width*.8||q.y<geometry.height*.2||q.y>geometry.height*.8)trace.clipped=true;}
  }
  draw();
}
surface.addEventListener('pointerdown',event=>{event.preventDefault();if(!trace)return;if(trace.pointer!==null&&trace.pointer!==event.pointerId)trace.cancelled=true;trace.pointer=event.pointerId;trace.downs++;surface.setPointerCapture(event.pointerId);record(event);});
surface.addEventListener('pointermove',event=>{if(trace&&trace.pointer===event.pointerId&&trace.downs>trace.ups){event.preventDefault();record(event);}});
surface.addEventListener('pointerup',event=>{event.preventDefault();if(trace&&trace.pointer===event.pointerId){record(event);trace.ups++;trace.pointer=null;}});
surface.addEventListener('pointercancel',event=>{if(trace){record(event);trace.cancelled=true;}});
surface.addEventListener('contextmenu',event=>event.preventDefault());
function logical(p){return {x:p.x*map.sx+map.ox,y:p.y*map.sy+map.oy};}
async function raw(reports,{x,y,label,interval=.03,hover=false}) {
  healthy();status(label);trace={points:[],downs:0,ups:0,clipped:false,cancelled:false,pointer:null};
  await call('trial',{sequence:++sequence,reports,x,y,orientation:'portrait',interval,hover,label});
  const until=performance.now()+2500,expected=hover?2:1;
  while(trace.ups<expected&&!trace.cancelled&&performance.now()<until){await sleep(40);healthy();}
  if(trace.cancelled||trace.downs!==expected||trace.ups!==expected||trace.points.length<2)
    throw Error('Incomplete mouse gesture: Safari did not receive the expected down/move/up');
  const result=trace;trace=null;return result;
}
async function anchors(){
  const w=Math.round(screen.width),h=Math.round(screen.height);
  if(w>=h)throw Error('Please rotate the phone to portrait and run calibration again');
  geometry={width:w,height:h};
  const a=await raw([[0,0]],{x:.45,y:.45,label:'Measuring page position (1/3)'});
  const b=await raw([[0,0]],{x:.55,y:.55,label:'Measuring page position (2/3)'});
  const p=a.points[0],q=b.points[0];
  const sx=w*.1/(q.x-p.x),sy=h*.1/(q.y-p.y);
  if(!Number.isFinite(sx)||!Number.isFinite(sy)||sx<.5||sx>2||sy<.5||sy>2||Math.abs(sx/sy-1)>.08)
    throw Error('Screen-to-page mapping could not be established');
  map={sx,sy,ox:w*.45-p.x*sx,oy:h*.45-p.y*sy};
  const c=await raw([[0,0]],{x:.5,y:.5,label:'Checking page position (3/3)'}),center=logical(c.points[0]);
  if(Math.hypot(center.x-w*.5,center.y-h*.5)>4)throw Error('Absolute positioning is inconsistent');
  const r=surface.getBoundingClientRect(),lo=logical({x:r.left+16,y:r.top+16}),hi=logical({x:r.right-16,y:r.bottom-16});
  geometry.left=Math.max(w*.2,lo.x);geometry.right=Math.min(w*.8,hi.x);
  geometry.top=Math.max(h*.2,lo.y);geometry.bottom=Math.min(h*.8,hi.y);
  geometry.horizontal=(geometry.right-geometry.left)*.8;geometry.vertical=(geometry.bottom-geometry.top)*.8;
  if(geometry.horizontal<50||geometry.vertical<80)throw Error('Not enough unobstructed canvas space');
  $('detail').textContent=`${w} × ${h} · usable travel ${Math.round(geometry.vertical)} pt`;
}
async function trial(reports,options={}) {
  const g=geometry,first=reports.find(r=>r[0]||r[1])||[0,0],dx=first[0],dy=first[1];
  const shift=sequence%2?.025:-.025;
  const x=g.left+(g.right-g.left)*(dx===0?.5+shift:dx<0?.9:.1);
  const y=g.top+(g.bottom-g.top)*(dy===0?.5+shift:dy<0?.9:.1);
  const t=await raw(reports,{...options,x:x/g.width,y:y/g.height,label:options.label||'Measuring pointer movement'});
  const start=logical(t.points[0]),end=logical(t.points.at(-1));
  if(Math.hypot(start.x-x,start.y-y)>12)throw Error('Absolute pointer missed the trial start');
  return {dx:end.x-start.x,dy:end.y-start.y,clipped:t.clipped};
}
async function abort(reason){
  if(finished||aborted)return;aborted=true;document.body.classList.add('failed');status(reason);
  try{await post({operation:'abort',error:reason});}catch(_){}
}
$('cancel').onclick=()=>abort('Calibration cancelled');
window.addEventListener('pagehide',()=>{if(!finished&&!aborted)navigator.sendBeacon('/api/calibration',new Blob([JSON.stringify({session,operation:'abort',error:'Calibration page closed'})],{type:'application/json'}));});
try {
  if(!/^[0-9a-f-]{36}$/.test(session))throw Error('Start calibration from the UxPlay keyboard dashboard');
  let previous=shape(),stable=performance.now(),deadline=performance.now()+20000;
  while(performance.now()-stable<1000){await sleep(100);healthy();const current=shape();if(current!==previous){previous=current;stable=performance.now();}if(performance.now()>deadline)throw Error('Page did not settle');}
  baseline=shape();canvas.width=surface.clientWidth;canvas.height=surface.clientHeight;
  let state=await post({operation:'status'});
  while(!state.inputIdle){await sleep(100);healthy();if(performance.now()>deadline)throw Error('Mouse 8 has not finished');state=await post({operation:'status'});}
  await call('begin',{label:'Starting calibration'});
  await anchors();
  const result=await MouseCalibration.run({call,trial},geometry,{force:state.force});
  finished=true;document.body.classList.add('done');$('cancel').hidden=true;
  status(`${result.matched?'Verified existing':'Generated'} profile: ${result.name}`);
  $('hint').textContent=`${result.validation.checks} checks passed. Maximum error ${result.validation.maxError.toFixed(2)} points. You can leave this page.`;
}catch(error){await abort(error.message);}
})();
