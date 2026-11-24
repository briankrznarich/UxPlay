#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real coordinator + hosted page + headless browser, with synthetic HID hardware.
No real phone, helper, or ControlFloor connection is used.
"""
import asyncio,base64,json,pathlib,socket,sqlite3,sys,tempfile,time
from aiohttp import ClientSession,web

async def main(binary):
    host='calibration-fixture';base='';token='';pairing=None;code='';opened=None
    active=None;sequence=0;pending=None;calls=[];browser=None;profile=None
    cdp=None;tab=None;capture=False;test_error=False
    async def until(predicate,seconds=15):
        deadline=time.monotonic()+seconds
        while True:
            value=predicate()
            if asyncio.iscoroutine(value):value=await value
            if value:return value
            assert time.monotonic()<deadline,('Timed out',calls[-5:])
            await asyncio.sleep(.05)
    async def fetch_shortcut():
        # Deliberately bogus Host proves URL choice uses the accepted socket.
        async with http.get(base+'/shortcuts/next',headers={'Host':'wrong-interface.local:9876'}) as r:
            data=await r.json();assert r.status==200,data;return data
    async def report(req,**extra):
        async with http.post(base+'/shortcuts/report',json=dict(protocolVersion=2,shortcutVersion='0.4.19',status='ok',**req,**extra)) as r:
            data=await r.json();assert r.status==200,data
    async def devices(_):
        return web.json_response({'bluetoothHosts':[dict(bluetoothHost=host,keyboard=True,consumerControls=True,liveMouse=True,
            mouse=dict(logicalWidth=375,logicalHeight=667,down=False,calibrating=bool(active),capture={'state':'idle'}))]})
    async def mouse(kind,x,y):
        if not cdp:return
        await cdp('Input.dispatchMouseEvent',dict(type=kind,x=x,y=y-70,button='left' if kind!='mouseMoved' else 'none',
            buttons=0 if kind=='mouseReleased' else 1,clickCount=1))
    async def post(r):
        nonlocal pairing,code,opened,active,sequence,pending,profile
        body=await r.json();action=r.match_info['action'];calls.append((action,body))
        assert body.get('device')==host,body
        if action=='mouseUp' and not active:
            req=await fetch_shortcut()
            if req['command']=='initiate-pairing':pairing=req
            if req['command']=='open-url':
                opened=req['url'];await report(req)
        elif action=='type':code=body['text']
        elif action=='typeSpecialKey' and body['key']=='Enter':
            await report(pairing,pairingId=code,model='iPhone SE',iosVersion='26.6')
        if action=='mouseCalibration':
            op=body['operation']
            if op=='begin':
                assert not active;active=body['session'];sequence=0
                return web.json_response({'testVersion':1})
            assert active==body['session'],body
            if op=='cancel':active=None;return web.json_response({'ok':True})
            if op=='profiles':return web.json_response({'profiles':[]})
            if op=='trial':
                assert body['sequence']==sequence+1;sequence+=1
                x,y=body['x']*375,body['y']*667
                await mouse('mousePressed',x,y)
                if body.get('hover'):await mouse('mouseReleased',x,y)
                for dx,dy in body['reports']:
                    radius=(dx*dx+dy*dy)**.5
                    distance=0 if max(abs(dx),abs(dy))<4 else (radius-3)*2.5
                    if radius:x+=dx*distance/radius;y+=dy*distance/radius
                    if not body.get('hover'):await mouse('mouseMoved',x,y)
                if body.get('hover'):await mouse('mousePressed',x,y)
                await mouse('mouseReleased',x,y)
                # Simulated hardware remains "calibrating" across inventory polls.
                await asyncio.sleep(.02)
                return web.json_response({'ok':True,'sequence':sequence})
            if op=='prepare':
                profile=body['profile'];pending={'name':'web-fixture','fingerprint':'a'*64,'profile':profile}
                if test_error:return web.json_response({'error':'Fixture preparation rejected'},status=400)
                return web.json_response(pending)
            if op=='commit':
                assert pending and body['fingerprint']==pending['fingerprint'];active=None
                return web.json_response({'name':pending['name'],'fingerprint':pending['fingerprint']})
            raise AssertionError(body)
        return web.json_response({'submitted':True,'mouse':{'down':action=='mouseDown'}})
    app=web.Application();app.router.add_get('/devices',devices);app.router.add_post('/{action}',post)
    runner=web.AppRunner(app);await runner.setup();site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
    helper=f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}'
    async def snapshot():
        nonlocal token
        async with http.get(base+'/api/keyboards') as r:
            data=await r.json();token=data['mutationToken'];return data['keyboards'][0]
    async def matches(**expected):
        row=await snapshot();return row if all(row.get(k)==v for k,v in expected.items()) else None
    async def control(action):
        async with http.post(base+'/api/keyboards/control',headers={'X-UxPlay-Token':token},json={'id':host,'action':action}) as r:
            data=await r.json();assert r.status==202,data
    with tempfile.TemporaryDirectory(prefix='cf-calibration-') as directory:
        db=pathlib.Path(directory)/'keyboards.sqlite3'
        with sqlite3.connect(db) as conn:
            conn.execute('CREATE TABLE keyboards(source TEXT NOT NULL,host TEXT NOT NULL,record TEXT NOT NULL,PRIMARY KEY(source,host))')
            conn.execute('INSERT INTO keyboards VALUES(?,?,?)',('iphone-use:'+helper,host,json.dumps(dict(host=host,autoConnect=False,label='Calibration fixture'))))
        with socket.socket() as listener:listener.bind(('127.0.0.1',0));port=listener.getsockname()[1]
        log=open(pathlib.Path(directory)/'stderr','w+')
        process=await asyncio.create_subprocess_exec(binary,helper,directory,str(port),stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.PIPE,stderr=log)
        first=json.loads(await process.stdout.readline());base=f'http://127.0.0.1:{first["port"]}'
        async def drain():
            while await process.stdout.readline():pass
        reader=asyncio.create_task(drain())
        browser_reader=None
        try:
            async with ClientSession() as http:
                await until(lambda:matches(inputReady=True));await control('pair');await until(lambda:matches(phase='paired'))
                await control('calibrate-new');await until(lambda:opened)
                assert opened.startswith(base+'/mouse-calibration#'),opened
                assert 'wrong-interface' not in opened
                sid=opened.split('#')[1]
                async with http.post(base+'/api/calibration',json={'session':'stale','operation':'begin'}) as r:assert r.status==409
                async with http.post(base+'/api/calibration',headers={'Origin':'http://evil.example'},json={'session':sid,'operation':'begin'}) as r:assert r.status==403
                print('PASS paired-only action, open-url callback without AirPlay, accepted local IP (ignores Host), stale/cross-origin rejection')
                browser=await asyncio.create_subprocess_exec('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome','--headless=new','--remote-debugging-port=0','--user-data-dir='+directory+'/chrome','--no-first-run','--no-default-browser-check','about:blank',stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.DEVNULL)
                marker=pathlib.Path(directory)/'chrome/DevToolsActivePort';await until(marker.exists)
                browser_port=marker.read_text().splitlines()[0]
                async with http.get('http://127.0.0.1:'+browser_port+'/json/list') as r:target=(await r.json())[0]
                async with http.ws_connect(target['webSocketDebuggerUrl']) as ws:
                    seq=0;waiting={};errors=[]
                    async def receive():
                        async for message in ws:
                            value=json.loads(message.data)
                            if 'id' in value:waiting.pop(value['id']).set_result(value)
                            elif value.get('method')=='Runtime.exceptionThrown':errors.append(value)
                    browser_reader=asyncio.create_task(receive())
                    async def call(method,params={}):
                        nonlocal seq
                        seq+=1;f=asyncio.get_running_loop().create_future();waiting[seq]=f
                        await ws.send_json(dict(id=seq,method=method,params=params));data=await asyncio.wait_for(f,10)
                        assert 'error' not in data,data;return data.get('result',{})
                    cdp=call
                    async def js(expression):
                        result=await call('Runtime.evaluate',dict(expression=expression,returnByValue=True,awaitPromise=True));assert 'exceptionDetails' not in result,result
                        return result['result'].get('value')
                    await call('Runtime.enable');await call('Page.enable')
                    # Simulate Safari's content being shorter and starting 70 points below the screen top.
                    await call('Emulation.setDeviceMetricsOverride',dict(width=375,height=570,screenWidth=375,screenHeight=667,deviceScaleFactor=1,mobile=True))
                    await call('Page.navigate',dict(url=opened))
                    async def done():return await js("document.body && (document.body.classList.contains('done')||document.body.classList.contains('failed'))")
                    await until(done,90)
                    state=await js("({status:document.getElementById('status').textContent,done:document.body.classList.contains('done')})")
                    assert state['done'],state
                    row=await snapshot();assert row['phase']=='paired' and not row['calibration']['active'],row
                    assert row['calibration']['result']['fingerprint']=='a'*64
                    assert profile['width']==375 and profile['height']==667,profile
                    assert profile['measuredCounts']==list(range(1,len(profile['axisPoints'])))
                    assert len(profile['axisPoints'])>20
                    assert not errors,errors
                    print('PASS hosted page full dense sweep + advanced validation + committed profile, with simulated browser chrome offset')
                    shot=await call('Page.captureScreenshot',{'format':'png'})
                    pathlib.Path('/tmp/uxplay-calibration-complete.png').write_bytes(base64.b64decode(shot['data']))
                    # Second run exercises cancellation and stale page after a fresh session.
                    opened=None;await control('calibrate-new');await until(lambda:opened)
                    async with http.post(base+'/api/calibration',json={'session':sid,'operation':'trial'}) as r:assert r.status==409
                    sid=opened.split('#')[1]
                    await call('Page.navigate',dict(url=opened));await until(lambda:active)
                    await control('cancel-calibration');await until(lambda:not active)
                    assert (await snapshot())['phase']=='paired'
                    print('PASS cancellation releases helper session; fresh run rejects previous page token')
                    # Driver errors are shown without resetting keyboard/IP or committing.
                    test_error=True;opened=None;await control('calibrate-new');await until(lambda:opened)
                    await call('Page.navigate',dict(url=opened));await until(lambda:active);await until(done,90)
                    state=await js("document.getElementById('status').textContent");assert 'Fixture preparation rejected' in state,state
                    await until(lambda:not active);row=await snapshot();assert row['phase']=='paired' and row['ip']=='127.0.0.1'
                    assert len([b for a,b in calls if a=='mouseCalibration' and b['operation']=='commit'])==1
                    print('PASS driver failure cancels raw session, preserves paired IP, and never commits partial calibration')
                    # Verify action is rendered in dashboard and can be used for a matched/complete keyboard.
                    await call('Page.navigate',dict(url=base))
                    await until(lambda:js("!!document.querySelector('option[value=calibrate-new]')"))
                    assert await js("document.querySelector('option[value=calibrate]').textContent")=='Detect / calibrate mouse'
                    print('PASS manual calibration dropdown is present in the keyboard dashboard')
                browser_reader.cancel();await asyncio.gather(browser_reader,return_exceptions=True);browser_reader=None
        except Exception:
            log.flush();log.seek(0);print(log.read()[-6000:],file=sys.stderr);raise
        finally:
            cdp=None
            if browser_reader:browser_reader.cancel();await asyncio.gather(browser_reader,return_exceptions=True)
            if browser and browser.returncode is None:browser.terminate();await browser.wait()
            if process.returncode is None:process.stdin.write(b'{"type":"quit"}\n');await process.stdin.drain();await asyncio.wait_for(process.wait(),30)
            await reader;log.close();await runner.cleanup()
    print('All web calibration integration checks passed')

asyncio.run(main(sys.argv[1]))
