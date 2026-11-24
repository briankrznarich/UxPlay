// SPDX-License-Identifier: GPL-3.0-or-later
'use strict';
const assert=require('node:assert/strict');
const engine=require('../calibration.js');
const geometry={width:375,height:667,horizontal:160,vertical:240};
const curve=Array.from({length:101},(_,i)=>i<4?0:(i-3)*2.5);
const profile={testVersion:1,width:375,height:667,model:'radial-linear-component-gate',deadZone:4,axisPoints:curve};
function fixture({catalog=[],alter=null,prepare=null}={}) {
  const calls=[],trials=[];let selected=null;
  return {calls,trials,
    async call(op,args) {
      calls.push({op,args});
      if(op==='profiles')return {profiles:catalog};
      if(op==='select'){selected=catalog.find(c=>c.fingerprint===args.fingerprint);return selected;}
      if(op==='prepare'){selected={name:'new-curve',fingerprint:'a'.repeat(64),profile:prepare?prepare(args.profile):args.profile};return selected;}
      if(op==='commit'){assert.equal(args.fingerprint,selected.fingerprint);return selected;}
      throw Error('Unexpected operation '+op);
    },
    async trial(reports,options) {
      trials.push({reports,options});
      const values=reports.map(r=>engine.movement(profile,r));
      const [dx,dy]=values.reduce((a,b)=>[a[0]+b[0],a[1]+b[1]],[0,0]);
      const result={dx,dy,clipped:Math.abs(dy)>geometry.vertical||Math.abs(dx)>geometry.horizontal};
      return alter?alter(result,options,reports):result;
    }
  };
}
(async()=>{
  let io=fixture(),result=await engine.run(io,geometry,{force:true});
  assert.equal(result.matched,false);assert(result.validation.checks>20);
  assert.equal(io.calls.filter(c=>c.op==='profiles').length,0);
  const generated=io.calls.find(c=>c.op==='prepare').args.profile;
  assert.equal(generated.deadZone,4);assert.deepEqual(generated.axisPoints,curve.slice(0,generated.axisPoints.length));
  assert.deepEqual(generated.measuredCounts,Array.from({length:generated.axisPoints.length-1},(_,i)=>i+1));
  assert(generated.axisPoints.at(-1)<=geometry.vertical);
  assert(io.trials.some(t=>t.options.hover));assert.deepEqual([...new Set(io.trials.filter(t=>t.options.label==='Report timing').map(t=>t.options.interval))],[.01,.03,.1]);
  console.log('PASS dense full profile, bounded measurements, axes, diagonals, hover, timings, validation before commit');
  const saved={profile,name:'known',fingerprint:'b'.repeat(64)};
  io=fixture({catalog:[saved]});result=await engine.run(io,geometry);
  assert(result.matched);assert(!io.calls.some(c=>c.op==='prepare'));assert(io.trials.filter(t=>t.options.label==='Detecting existing profile').length<=4);
  console.log('PASS adaptive existing-profile match with independent validation');
  const shorter={name:'web-shorter',fingerprint:'c'.repeat(64),profile:{...profile,axisPoints:curve.slice(0,90)}};
  // Native/full-screen measurements differ slightly from Safari's mapping and
  // can cover more counts; either catalog order must reuse the bounded curve.
  const longer={...saved,profile:{...profile,axisPoints:curve.map(v=>v*1.0045)}};
  for(const catalog of [[longer,shorter],[shorter,longer]]) {
    io=fixture({catalog});result=await engine.run(io,geometry);
    assert(result.matched);assert.equal(result.fingerprint,shorter.fingerprint);
    assert(!io.calls.some(c=>c.op==='prepare'));
    assert(result.validation.checks>20);
  }
  // Agreement only at sampled counts is insufficient. Reject different gates,
  // divergence inside the chosen domain, and cases with only dead-zone probes.
  const diverging={...longer,profile:{...longer.profile,axisPoints:longer.profile.axisPoints.map((v,i)=>i===50?v+1.5:v)}};
  assert.equal(engine.detectionWinner([shorter,diverging],[4,25,60,80]),null);
  const otherGate={...longer,profile:{...longer.profile,deadZone:3}};
  assert.equal(engine.detectionWinner([shorter,otherGate],[4,25,60,80]),null);
  assert.equal(engine.detectionWinner([shorter],[1,2,3]),null);
  console.log('PASS shorter/longer noisy duplicates reuse shortest fingerprint; divergent curves remain ambiguous');

  io=fixture({catalog:[{...saved,profile:{...profile,axisPoints:curve.map(n=>n*1.5)}}]});result=await engine.run(io,geometry);
  assert(!result.matched);assert(io.calls.some(c=>c.op==='prepare'));
  console.log('PASS unmatched catalog continues into complete generation');
  io=fixture({alter:(r,o)=>o.label==='Report timing'?{...r,dx:r.dx+10}:r});
  await assert.rejects(()=>engine.run(io,geometry,{force:true}),/Report timing failed/);
  assert(!io.calls.some(c=>c.op==='commit'));
  io=fixture({alter:(r,o)=>o.label.startsWith('Measuring')?{...r,dx:5}:r});
  await assert.rejects(()=>engine.run(io,geometry,{force:true}),/wrong axis/);
  assert(!io.calls.some(c=>c.op==='prepare'));
  io=fixture({alter:(r,o,reports)=>o.label.startsWith('Measuring')&&reports[0][1]===10?{...r,dy:0}:r});
  await assert.rejects(()=>engine.run(io,geometry,{force:true}),/Non-monotonic/);
  assert(!io.calls.some(c=>c.op==='commit'));
  console.log('PASS wrong-axis, nonlinear/timing failures never activate a bad profile');
  io=fixture({alter:(r,o,reports)=>o.label.startsWith('Measuring')&&reports[0][1]>=60?{...r,clipped:true}:r});
  result=await engine.run(io,geometry,{force:true});assert.equal(result.profile.axisPoints.length,60);
  console.log('PASS clipped trial discarded; profile never extrapolates beyond measured domain');
})().catch(e=>{console.error(e);process.exitCode=1;});
