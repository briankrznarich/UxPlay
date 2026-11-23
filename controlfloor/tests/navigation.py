#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CF navigation using an isolated mock helper; never sends real HID reports."""
import asyncio, json, pathlib, signal, sys, tempfile, time
from aiohttp import web
from integration import Session

async def main(binary):
    sessions = asyncio.Queue()
    calls = []
    host = 'sole-bluetooth-phone'
    keyboard = consumer = mouse_ready = True
    held = False
    fail = ''
    entered = asyncio.Event()
    catalog = {'revision':'navigation-v1', 'keys':[
        {'name':k,'kind':'consumer' if k=='Fn' else 'key'}
        for k in ('Fn','Tab','KeyC','KeyH','KeyN','KeyS','KeyL','KeyX','ArrowUp')],
        'shortcuts':[{'id':id,'keys':keys,'executable':True} for id,keys in [
            ('home',['Fn','KeyH']),('controlCenter',['Fn','KeyC']),('appSwitcher',['Fn','ArrowUp']),
            ('notificationCenter',['Fn','KeyN']),('siri',['Fn','KeyS']),
            ('lockScreen',['Tab','KeyL']),('accessibilityShortcut',['Tab','KeyX'])]]}
    async def devices(r):
        return web.json_response({'bluetoothHosts':[{'bluetoothHost':host,'liveMouse':mouse_ready,
            'keyboard':keyboard,'consumerControls':consumer,
            'mouse':{'logicalWidth':375,'logicalHeight':667,'down':held}}]})
    async def discovery(r): return web.json_response(catalog)
    async def post(r):
        nonlocal held
        body=await r.json(); action=r.match_info['action'];calls.append((action,body))
        assert body['device']==host and 'modelName' not in body and 'gesture' not in body
        if action=='executeKeyboardShortcut':
            assert set(body)=={'id','revision','device'} and body['revision']==catalog['revision']
            assert keyboard
            entry=next(e for e in catalog['shortcuts'] if e['id']==body['id'])
            assert consumer or 'Fn' not in entry['keys']
        elif action=='typeSpecialKey':
            assert set(body)=={'key','device'} and consumer
        else:
            assert action in ('mouseDown','mouseMove','mouseUp') and mouse_ready
            if action=='mouseDown':held=True
            elif action=='mouseUp':held=False
            if action!='mouseUp':
                assert body['width']==375 and body['height']==667 and body['orientation']=='portrait'
                assert 0<=body['x']<375 and 0<=body['y']<667
            entered.set()
        if action==fail:return web.json_response({'error':'injected failure'},status=503)
        return web.json_response({'submitted':True,'mouse':{'down':held}})
    async def login(r):
        response=web.Response(text='ok');response.set_cookie('session','test');return response
    async def provider(r):
        ws=web.WebSocketResponse();await ws.prepare(r)
        s=Session(ws);await sessions.put(s);await s.read();return ws
    app=web.Application();app.router.add_post('/login',login);app.router.add_get('/provider/ws',provider)
    app.router.add_get('/input/devices',devices);app.router.add_get('/input/getKeyboardShortcuts',discovery)
    app.router.add_post('/input/{action}',post)
    runner=web.AppRunner(app);await runner.setup();site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
    base=f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}'
    with tempfile.TemporaryDirectory() as directory:
        root=pathlib.Path(directory);config=root/'provider.json'
        config.write_text(json.dumps(dict(server=base,username='mock',password='mock')))
        with (root/'process.log').open('w+') as output:
            p=await asyncio.create_subprocess_exec(binary,'--controlfloor-test-source','--controlfloor-config',str(config),
                '--controlfloor-log-dir',str(root/'logs'),'--controlfloor-mouse-url',base+'/input',stdout=output,stderr=output)
            try:
                s=await asyncio.wait_for(sessions.get(),15)
                async def readiness():
                    for _ in range(80):
                        status=(await s.command('getDeviceStatus'))['value']
                        if status['keyboardAvailable']==keyboard and status['consumerControlsAvailable']==consumer and status['liveMouse']==mouse_ready:return
                        await asyncio.sleep(.05)
                    raise AssertionError(status)
                async def ok(action,args=None):
                    response=await s.command(action,args);assert response['status']=='ok',response;return response.get('value')
                await readiness()
                for action,key in [('simulateHomeButton','GoHome'),('simulateVolumeUpButton','AudioVolumeUp'),('simulateVolumeDownButton','AudioVolumeDown')]:
                    value=await ok(action);assert value['inputMethod']=='bluetoothKeyboard'
                    assert calls[-1]==('typeSpecialKey',{'key':key,'device':host})
                for action,id in [('showControlCenter','controlCenter'),('showAppSwitcher','appSwitcher'),
                                  ('showNotificationCenter','notificationCenter'),('simulateLockButton','lockScreen')]:
                    await ok(action);assert calls[-1][1]['id']==id
                for gesture,id in [('ShowControlCenter','controlCenter'),('ShowAppSwitcher','appSwitcher'),
                                   ('ShowNotificationCenter','notificationCenter'),('AskSiri','siri'),('AccessibilityShortcut','accessibilityShortcut')]:
                    await ok('performGesture',{'gesture':gesture,'device':'evil','orientation':'landscapeLeft'})
                    assert calls[-1]==('executeKeyboardShortcut',{'id':id,'revision':catalog['revision'],'device':host})
                assert all(not action.startswith('mouse') for action,_ in calls)
                print('PASS CF hardware/navigation aliases, HID home/volume, catalog shortcuts preferred over mouse')
                # Resolved overrides can remove the Fn/consumer-report requirement.
                cc=catalog['shortcuts'][1];cc['keys']=['Tab','KeyC'];catalog['revision']='overridden'
                consumer=False;await readiness()
                await ok('showControlCenter');assert calls[-1][1]['revision']=='overridden'
                before=len(calls);assert (await s.command('simulateVolumeUpButton'))['status']=='error';assert len(calls)==before
                cc['disabled']=True
                before=len(calls);assert (await s.command('showControlCenter'))['status']=='error';assert len(calls)==before
                cc['disabled']=False
                fail='executeKeyboardShortcut';before=len(calls)
                response=await s.command('showNotificationCenter') # Fn unavailable; no submission, mouse fallback.
                assert response['status']=='ok' and response['value']['inputMethod']=='bluetoothMouse',response
                before=len(calls);response=await s.command('showControlCenter')
                assert response['status']=='error' and 'not retried' in response['error']
                assert len(calls)==before+1 and calls[-1][0]=='executeKeyboardShortcut'
                fail=''
                print('PASS catalog overrides/revisions, subscription checks, disabled binding and no fallback/replay after POST failure')
                keyboard=False;await readiness()
                # Synthetic model is unknown: spoofing a viewer model must not enable a system edge swipe.
                before=len(calls)
                response=await s.command('showControlCenter',{'modelName':'iPhone SE'})
                assert response['status']=='error' and len(calls)==before
                for gesture in ('CenterSwipeUp','CenterSwipeDown','CenterSwipeLeft','CenterSwipeRight',
                                'EdgeSwipeUp','EdgeSwipeDown','EdgeSwipeLeft','EdgeSwipeRight','PageUp','PageDown','JiggleScreenContents'):
                    before=len(calls);value=await ok('performGesture',{'gesture':gesture,'x':123,'y':321})
                    sequence=calls[before:];assert sequence[0][0]=='mouseDown' and sequence[-1][0]=='mouseUp' and not held
                    assert value['inputMethod']=='bluetoothMouse'
                    if gesture.startswith('Page'):assert sequence[0][1]['x']==123 and sequence[0][1]['y']==321
                assert not (await s.command('getDeviceStatus'))['value']['mouseEnabled']
                for action in ('centerSwipeUp','centerSwipeDown','centerSwipeLeft','centerSwipeRight',
                               'edgeSwipeUp','edgeSwipeDown','edgeSwipeLeft','edgeSwipeRight'):
                    await ok(action)
                for action,args in [('performGesture',{'gesture':'TwoFingerTouch'}),('performGesture',{'gesture':'SwitchToNextOpenApp'}),
                                    ('performGesture',{'gesture':42}),('simulateHomeButton',{'duration':1}),
                                    ('performGesture',{'gesture':'PageDown','x':5})]:
                    before=len(calls);assert (await s.command(action,args))['status']=='error';assert len(calls)==before
                fail='mouseMove';before=len(calls)
                assert (await s.command('centerSwipeUp'))['status']=='error'
                assert [c[0] for c in calls[before:]]==['mouseDown','mouseMove','mouseUp'] and not held
                fail=''
                print('PASS mouse gestures/aliases without live-mouse enable, invalid requests, trusted model, release after failed movement')
                await ok('startVideo',{'maxDimension':320,'maxFramesPerSecond':20});await s.collect(.2)
                entered.clear();before=len(calls)
                task=asyncio.create_task(s.command('performGesture',{'gesture':'JiggleScreenContents'}))
                await asyncio.wait_for(entered.wait(),2)
                start=time.monotonic();await ok('ping');assert time.monotonic()-start<.5
                assert len(await s.collect(.5))>=4
                assert (await task)['status']=='ok' and not held
                # Disconnect interrupts a long drag and releases the button before its final segment.
                entered.clear();before=len(calls)
                pending=asyncio.create_task(s.command('performGesture',{'gesture':'JiggleScreenContents'}))
                await asyncio.wait_for(entered.wait(),2);await s.ws.close()
                for _ in range(50):
                    if not held:break
                    await asyncio.sleep(.05)
                assert not held and calls[-1][0]=='mouseUp'
                assert len(calls[before:])<10,calls[before:]
                pending.cancel();await asyncio.gather(pending,return_exceptions=True)
                print('PASS navigation does not block video/ping; disconnect cancels gesture and releases held mouse')
            except BaseException:
                output.flush();output.seek(0);print(output.read(),file=sys.stderr);raise
            finally:
                if p.returncode is None:p.send_signal(signal.SIGTERM)
                try:await asyncio.wait_for(p.wait(),15)
                except asyncio.TimeoutError:p.kill();await p.wait()
                await runner.cleanup();assert p.returncode==0,p.returncode
if __name__=='__main__':asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
