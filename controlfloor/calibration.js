/* SPDX-License-Identifier: GPL-3.0-or-later
 * Transport/DOM-independent calibration recipe. Units are portrait screen points.
 * io.call(operation, args), io.trial(reports, options) are the only boundaries.
 */
(function(root) {
'use strict';
const MODEL='radial-linear-component-gate';
const tolerance=d=>Math.max(.75,Math.abs(d)*.01);
function valid(p) {
  return p && p.testVersion===1 && p.model===MODEL && Number.isInteger(p.width) && p.width>0 &&
    Number.isInteger(p.height) && p.height>=p.width && Array.isArray(p.axisPoints) &&
    p.axisPoints.length>=2 && p.axisPoints.length<=128 && p.axisPoints[0]===0 &&
    Number.isInteger(p.deadZone) && p.deadZone>=1 && p.deadZone<p.axisPoints.length &&
    p.axisPoints.every((v,i,a)=>Number.isFinite(v)&&v>=0&&v<=100000&&(!i||v>=a[i-1])&&(i>=p.deadZone||v===0)) && p.axisPoints.at(-1)>0;
}
function movement(p,[x,y]) {
  if(Math.max(Math.abs(x),Math.abs(y))<p.deadZone)return [0,0];
  const r=Math.hypot(x,y),a=p.axisPoints;
  if(r>a.length-1)throw Error('Trial exceeds measured profile domain');
  const lo=Math.floor(r), hi=Math.ceil(r), d=a[lo]+(a[hi]-a[lo])*(r-lo);
  return [x*d/r,y*d/r];
}
function equivalent(a,b) {
  return a.deadZone===b.deadZone && a.axisPoints.length===b.axisPoints.length &&
    a.axisPoints.every((v,i)=>Math.abs(v-b.axisPoints[i])<=.75);
}
function safeCount(p,limit) {
  let n=p.axisPoints.length-1;
  while(n>0 && p.axisPoints[n]>limit)n--;
  return n;
}
function nextCount(candidates,used,limit) {
  const max=Math.min(...candidates.map(c=>safeCount(c.profile,limit)));
  if(!max||!candidates.length)return 0;
  if(!used.length)return Math.min(max,...candidates.map(c=>c.profile.deadZone));
  const target=max*([0,.35,.7,1][used.length]);
  let best=0,score=-Infinity;
  for(let n=1;n<=max;n++) {
    if(used.includes(n))continue;
    const values=candidates.map(c=>c.profile.axisPoints[n]),hi=Math.max(...values),lo=Math.min(...values);
    const value=(hi-lo)/tolerance(hi)-Math.abs(n-target)/Math.max(1,max);
    if(value>score){score=value;best=n;}
  }
  return best;
}
async function validate(io,p,g) {
  if(!valid(p)||p.width!==g.width||p.height!==g.height)throw Error('Selected profile has invalid curve or screen dimensions');
  const max=safeCount(p,g.vertical*.85), gate=p.deadZone;
  if(max<gate)throw Error('Canvas has insufficient room to validate selected profile');
  const checks=[],add=(reports,label,interval=.03,hover=false)=>checks.push({reports,label,interval,hover});
  for(const n of new Set([gate,Math.round((gate+max)/2),max,...[19,28,56,88,120].filter(n=>n>=gate&&n<=max)]))
    add([[0,n]],'Endpoint validation');
  let n=Math.min(32,max);
  while(n>gate && (p.axisPoints[n]*4>Math.min(g.horizontal,g.vertical)*.8 || Math.hypot(n,n)>p.axisPoints.length-1))n--;
  if(p.axisPoints[n]*4>Math.min(g.horizontal,g.vertical)*.8 || Math.hypot(n,n)>p.axisPoints.length-1)
    throw Error('Insufficient canvas space for advanced validation');
  for(let i=0;i<3;i++)add([[n,0]],'Repeatability');
  for(const r of [[-n,0],[0,n],[0,-n],[n,n],[-n,n]])add([r],'Axis / diagonal');
  for(const r of [[gate-1,gate-1],[gate-1,gate]])
    if(Math.hypot(...r)<=p.axisPoints.length-1)add([r],'Dead zone');
  const four=Array.from({length:4},()=>[n,0]);
  for(const interval of [.01,.03,.1])add(four,'Report timing',interval);
  if(n*4<=max)add([[0,n*4]],'Packet size');
  add([[n,0],[-n,0]],'Return to start');
  add([[n,0]],'Hover versus drag',.03,true);
  let maxError=0;
  for(const check of checks) {
    const expected=check.reports.map(r=>movement(p,r)).reduce((a,b)=>[a[0]+b[0],a[1]+b[1]],[0,0]);
    const result=await io.trial(check.reports,check);
    const error=Math.hypot(result.dx-expected[0],result.dy-expected[1]);
    if(result.clipped||!Number.isFinite(error)||error>Math.max(2,Math.hypot(...expected)*.03))
      throw Error(`${check.label} failed (${error.toFixed(2)} points error${result.clipped?', clipped':''})`);
    maxError=Math.max(maxError,error);
  }
  return {checks:checks.length,maxError};
}
async function run(io,g,{force=false}={}) {
  let saved,matched=false;
  if(!force) {
    let candidates=(await io.call('profiles',{})).profiles.filter(c=>valid(c.profile)&&c.profile.width===g.width&&c.profile.height===g.height);
    const used=[];
    while(candidates.length&&used.length<4) {
      const n=nextCount(candidates,used,g.vertical*.85);if(!n)break;
      const r=await io.trial([[0,n]],{label:'Detecting existing profile'});used.push(n);
      candidates=r.clipped?[]:candidates.filter(c=>Math.hypot(r.dx,r.dy-c.profile.axisPoints[n])<=tolerance(c.profile.axisPoints[n]));
    }
    const first=candidates[0];
    if(first && used.filter(n=>first.profile.axisPoints[n]>.75).length>=2 && candidates.every(c=>equivalent(first.profile,c.profile))) {
      saved=await io.call('select',{fingerprint:first.fingerprint});matched=true;
    }
  }
  if(!saved) {
    const points=[0],measured=[];
    for(let n=1;n<=127;n++) {
      const r=await io.trial([[0,n]],{label:`Measuring HID count ${n} / 127`});
      if(r.clipped)break;
      if(!Number.isFinite(r.dx)||!Number.isFinite(r.dy)||r.dy<-.5||Math.abs(r.dx)>2)throw Error('Vertical probe moved on the wrong axis');
      if(r.dy+.75<points.at(-1))throw Error('Non-monotonic movement; calibration interrupted or mouse model unsupported');
      points.push(Math.max(0,points.at(-1),r.dy));measured.push(n);
      if(r.dy>=g.vertical*.9)break;
      // Stop rather than intentionally sweep through Safari chrome. Keep only measured counts.
      if(n>2 && r.dy+Math.max(1,points.at(-1)-points.at(-2))>=g.vertical)break;
    }
    const gate=points.findIndex(v=>v>=.01);
    if(gate<1||points.at(-1)<.75)throw Error('No usable movement measured');
    for(let n=0;n<gate;n++)points[n]=0;
    const profile={testVersion:1,width:g.width,height:g.height,model:MODEL,deadZone:gate,axisPoints:points,
      measuredCounts:measured,evidence:{measuredPoints:measured.length,advanced:true,webCanvas:true}};
    saved=await io.call('prepare',{profile});
  }
  const validation=await validate(io,saved.profile,g);
  const result=await io.call('commit',{fingerprint:saved.fingerprint});
  return {...result,matched,validation,profile:saved.profile};
}
const api={run,validate,valid,movement,nextCount};
if(typeof module!=='undefined'&&module.exports)module.exports=api;
else root.MouseCalibration=api;
})(globalThis);
