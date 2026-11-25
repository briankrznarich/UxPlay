#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CF keyboard adapter tests using mocked iphone-use, never real HID input."""
import asyncio, json, pathlib, signal, sys, tempfile, time
from aiohttp import web
from integration import Session

async def main(binary):
    sessions = asyncio.Queue()
    calls = []
    host = 'sole-bluetooth-phone'
    keyboard = True
    consumer = True
    delay = 0
    fail = False
    entered = asyncio.Event()
    keys = ['Backspace', 'Tab', 'Enter', 'Escape', 'PageUp', 'PageDown', 'End', 'Home',
            'ArrowLeft', 'ArrowUp', 'ArrowRight', 'ArrowDown', 'Delete', 'Space', 'CapsLock',
            'Control', 'Shift', 'Alt', 'Meta', 'ControlRight', 'ShiftRight', 'AltRight', 'MetaRight',
            'Fn', 'GoHome', 'Eject', 'AudioVolumeUp', 'AudioVolumeDown', 'AudioVolumeMute', 'MediaPlayPause',
            'Insert', 'PrintScreen', 'ScrollLock', 'Pause', 'NumLock', 'ContextMenu',
            'Minus', 'Equal', 'BracketLeft', 'BracketRight', 'Backslash', 'Semicolon', 'Quote', 'Backquote', 'Comma', 'Period', 'Slash',
            'NumpadDivide', 'NumpadMultiply', 'NumpadSubtract', 'NumpadAdd', 'NumpadEnter', 'NumpadDecimal',
            'startCapture', 'endCapture', 'replayCapture']
    keys += [f'F{i}' for i in range(1,13)] + [f'Key{c}' for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ']
    keys += [f'Digit{i}' for i in range(10)] + [f'Numpad{i}' for i in range(10)]
    aliases = {'return':'Enter', 'ForwardDelete':'Delete', 'left':'ArrowLeft', 'cmd':'Meta', 'option':'Alt', 'globe':'Fn', ' ':'Space'}
    consumer_keys = {'Fn','GoHome','Eject','AudioVolumeUp','AudioVolumeDown','AudioVolumeMute','MediaPlayPause'}
    catalog = {'schemaVersion':1,'revision':'test-revision','shortcuts':[{'id':'selectAll','keys':['Meta','KeyA'],'executable':True}],
               'keys':[{'name':key,'label':key,'kind':'consumer' if key in consumer_keys else 'key','aliases':[]} for key in keys]}
    async def devices(r):
        return web.json_response({'bluetoothHosts':[{'bluetoothHost':host,'liveMouse':False,
            'keyboard':keyboard,'consumerControls':consumer,'mouse':{}}]})
    async def discovery(r):
        assert not r.query
        return web.json_response(catalog)
    async def post(r):
        body = await r.json(); action = r.match_info['action']; calls.append((action,body))
        assert body['device'] == host and 'mac' not in body and 'orientation' not in body
        if action == 'typeSpecialKey':
            assert set(body) == {'key','device'}
            requested = [aliases.get(body['key'],body['key'])]
        elif action == 'pressKeyChord':
            assert set(body) == {'keys','device'}; requested = body['keys']
        else:
            assert action == 'executeKeyboardShortcut' and set(body) <= {'id','revision','device'}
            if body.get('revision') != 'test-revision': return web.json_response({'error':'stale catalog revision'},status=400)
            requested = ['Meta','KeyA']
        if any(key not in keys for key in requested): return web.json_response({'error':'unknown key'},status=400)
        if len(set(requested)) != len(requested): return web.json_response({'error':'duplicate keys in chord'},status=400)
        if not keyboard and any(key not in consumer_keys for key in requested): return web.json_response({'error':'keyboard report not subscribed'},status=503)
        if not consumer and any(key in consumer_keys for key in requested): return web.json_response({'error':'consumer report not subscribed'},status=503)
        entered.set(); await asyncio.sleep(delay)
        if fail: return web.json_response({'error':'simulated transport failure'},status=503)
        return web.json_response({'ok':True,'submitted':True,'keys':requested,'bluetoothHost':host,'keyboardRequestId':'fixture'})
    async def login(r):
        response=web.Response(text='ok');response.set_cookie('session','test');return response
    async def provider(r):
        ws=web.WebSocketResponse();await ws.prepare(r)
        s=Session(ws);await sessions.put(s);await s.read();return ws
    app=web.Application();app.router.add_post('/login',login);app.router.add_get('/provider/ws',provider)
    app.router.add_get('/input/devices',devices)
    for action in ('getKeyboardKeys','getKeyboardShortcuts'): app.router.add_get('/input/'+action,discovery)
    app.router.add_post('/input/{action}',post)
    runner=web.AppRunner(app);await runner.setup()
    site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
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
                        if status['keyboardAvailable']==keyboard and status['consumerControlsAvailable']==consumer:return
                        await asyncio.sleep(.05)
                    raise AssertionError(status)
                await readiness()
                assert (await s.command('getDeviceStatus'))['value']['liveMouse']
                assert not (await s.command('getDeviceStatus'))['value']['mouseAvailable']
                for action in ('getKeyboardKeys','getKeyboardShortcuts'):
                    response=await s.command(action)
                    assert response['status']=='ok' and response['value']==dict(catalog,agent='Bluetooth')
                # Existing frontend can send either CFAgent or Bluetooth agent hints.
                for i,key in enumerate(keys+list(aliases)):
                    response=await s.command('typeSpecialKey',{'key':key,'device':'evil','mac':'evil','text':'must not be typed'},agent=['CFAgent','Bluetooth',None][i%3])
                    assert response['status']=='ok' and response['value']['submitted'],response
                    assert response['value']['keys']==[aliases.get(key,key)]
                print('PASS every CFA special key, helper raw/consumer/modifier/virtual key families, aliases, frontend agent hints and target ownership')
                for action,args in [('typeSpecialKey',{}),('typeSpecialKey',{'key':None}),('typeSpecialKey',{'key':42}),
                                    ('typeSpecialKey',{'key':''}),('typeSpecialKey',{'key':'a'*129}),
                                    ('pressKeyChord',{'keys':[]}),('pressKeyChord',{'keys':['Meta',42]}),('pressKeyChord',{'keys':['KeyA']*16}),
                                    ('executeKeyboardShortcut',{'id':'selectAll','revision':42})]:
                    before=len(calls);assert (await s.command(action,args))['status']=='error';assert len(calls)==before
                for action,args,error in [('typeSpecialKey',{'key':'NotAKey'},'unknown key'),
                                          ('pressKeyChord',{'keys':['Meta','NotAKey']},'unknown key'),
                                          ('pressKeyChord',{'keys':['Meta','Meta']},'duplicate'),
                                          ('executeKeyboardShortcut',{'id':'selectAll','revision':'old'},'stale')]:
                    before=len(calls);response=await s.command(action,args)
                    assert response['status']=='error' and error in response['error']
                    assert len(calls)==before+1
                assert (await s.command('pressKeyChord',{'keys':['Meta','KeyA']}))['value']['keys']==['Meta','KeyA']
                assert (await s.command('executeKeyboardShortcut',{'id':'selectAll','revision':'test-revision'}))['value']['keys']==['Meta','KeyA']
                print('PASS malformed input rejected before dispatch; helper validation errors returned without retries; chords and named shortcuts')
                keyboard=False;await readiness()
                assert (await s.command('typeSpecialKey',{'key':'Fn'}))['status']=='ok'
                assert (await s.command('typeSpecialKey',{'key':'Backspace'}))['status']=='error'
                consumer=False;await readiness()
                before=len(calls);assert (await s.command('typeSpecialKey',{'key':'Fn'}))['status']=='error';assert len(calls)==before
                assert (await s.command('getKeyboardKeys'))['status']=='ok'
                keyboard=True;await readiness()
                assert (await s.command('typeSpecialKey',{'key':'Backspace'}))['status']=='ok'
                assert (await s.command('typeSpecialKey',{'key':'Fn'}))['status']=='error'
                print('PASS keyboard-only and consumer-only subscriptions, discovery without input, no live-mouse requirement')
                await s.command('startVideo',{'maxDimension':320,'maxFramesPerSecond':20})
                await s.collect(.2);delay=3;entered.clear();before=len(calls)
                task=asyncio.create_task(s.command('typeSpecialKey',{'key':'Backspace'}))
                await asyncio.wait_for(entered.wait(),2)
                start=time.monotonic();assert (await s.command('ping'))['value']=='pong';assert time.monotonic()-start<.5
                assert len(await s.collect(.5))>=4
                response=await task;assert response['status']=='error' and 'not retried' in response['error']
                await asyncio.sleep(1.2);assert len(calls)==before+1
                print('PASS slow/failed keyboard submission does not block ping/video and is never replayed')
            except BaseException:
                output.flush();output.seek(0);print(output.read(),file=sys.stderr);raise
            finally:
                if p.returncode is None:p.send_signal(signal.SIGTERM)
                try:await asyncio.wait_for(p.wait(),15)
                except asyncio.TimeoutError:p.kill();await p.wait()
                await runner.cleanup()
                assert p.returncode==0,p.returncode
if __name__=='__main__':asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
