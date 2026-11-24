# SPDX-License-Identifier: GPL-3.0-or-later
"""Simulated calibration page for coordinator tests; recipe/browser tests are separate."""
import asyncio,time

async def complete_calibration(http,base,url,release=None):
    session=url.split('#')[1]
    async def request(operation,**args):
        async with http.post(base+'/api/calibration',json=dict(session=session,operation=operation,**args)) as r:
            value=await r.json()
            if r.status==409:return None # Cancelled/replaced page must not continue.
            assert r.status in (200,202),value
            return value
    deadline=time.monotonic()+15
    while True:
        state=await request('status')
        if not state or not state['active']:return
        if state['inputIdle']:break
        assert time.monotonic()<deadline
        await asyncio.sleep(.03)
    if release:await release.wait()
    for operation,args in [('begin',{}),('prepare',{'profile':{'testVersion':1,'width':375,'height':667,'model':'radial-linear-component-gate','deadZone':1,'axisPoints':[0,1]}}),('commit',{'fingerprint':'a'*64})]:
        state=await request(operation,**args)
        if state is None:return
        while state['busy']:
            await asyncio.sleep(.03);state=await request('status')
            if state is None:return
            assert time.monotonic()<deadline
        assert not state['error'],state
