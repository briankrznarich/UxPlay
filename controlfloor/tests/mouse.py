#!/usr/bin/env python3
"""Real UxPlay/CF websocket integration against a fake iphone-use HTTP API.
No phone input is sent. Requires aiohttp and Pillow, like integration.py.
"""
import asyncio, json, os, pathlib, signal, sys, tempfile, time
from aiohttp import web
from integration import Session

async def main(binary):
    sessions = asyncio.Queue()
    calls = []
    host = 'test-bluetooth-host'
    peers = [host]
    held = False
    delay = 0
    fail = False
    keyboard_ready = True
    pointer_ready = True
    text_delay = 0
    typed = []
    mouse = {'profile':'existing-375x667', 'logicalWidth':375, 'logicalHeight':667,
             'usingDefaultProfile':False, 'down':False}
    entered = asyncio.Event()
    async def devices(r):
        return web.json_response({'bluetoothHosts':[
            {'bluetoothHost':h,'liveMouse':pointer_ready,'keyboard':keyboard_ready,'mouse':dict(mouse,down=held)} for h in peers]})
    async def mouse_call(r):
        nonlocal held
        body=await r.json();action=r.match_info['action'];calls.append((action,body))
        assert action in ('mouseDown','mouseMove','mouseUp','type'),action
        assert body['device']==host,body
        assert 'mac' not in body
        if action=='type':
            assert set(body)=={'device','text'},body
            text=body['text']
            assert all(32<=ord(c)<=126 or c in '\t\n' for c in text),text
            if text=='helper rejects this':return web.json_response({'error':'helper rejected whole string'},status=400)
            entered.set();await asyncio.sleep(text_delay)
            typed.append(text)
        if action=='mouseMove':
            entered.set()
            await asyncio.sleep(delay)
            if fail:return web.json_response({'error':'simulated helper failure'},status=503)
        if action=='mouseDown':held=True
        if action=='mouseUp':held=False
        return web.json_response({'ok':True,'mouse':dict(mouse,down=held),'bluetoothHost':host})
    async def login(r):
        response=web.Response(text='ok');response.set_cookie('session','mock');return response
    async def provider(r):
        assert r.cookies.get('session')=='mock'
        ws=web.WebSocketResponse();await ws.prepare(r)
        session=Session(ws);await sessions.put(session);await session.read();return ws
    app=web.Application()
    app.router.add_post('/login',login);app.router.add_get('/provider/ws',provider)
    app.router.add_get('/input/devices',devices);app.router.add_post('/input/{action}',mouse_call)
    runner=web.AppRunner(app);await runner.setup()
    site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
    base=f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}'
    async def until(predicate, timeout=4):
        deadline=time.monotonic()+timeout
        while not predicate():
            assert time.monotonic()<deadline,'condition timed out'
            await asyncio.sleep(.025)
    with tempfile.TemporaryDirectory() as directory:
        config=pathlib.Path(directory)/'provider.json'
        config.write_text(json.dumps({'server':base,'username':'mock-provider','password':'mock'}))
        config.chmod(0o600)
        with open(pathlib.Path(directory)/'provider.log','w+') as output:
            process=await asyncio.create_subprocess_exec(binary,'--controlfloor-test-source',
                '--controlfloor-log-dir',str(pathlib.Path(directory)/'logs'),'--controlfloor-config',str(config),'--controlfloor-mouse-url',base+'/input',stdout=output,stderr=output)
            stopped=False
            try:
                s=await asyncio.wait_for(sessions.get(),10)
                async def availability(expected):
                    for _ in range(60):
                        response=(await s.command('getDeviceStatus'))['value']
                        if response['liveMouse']==expected:return response
                        await asyncio.sleep(.05)
                    raise AssertionError(response)
                await availability(True)
                info=(await s.command('getDeviceInformation'))['value']
                assert (info['logicalWidthPortrait'],info['logicalHeightPortrait'])==(375,667),info
                caps=(await s.command('getCFAgentCapabilities'))['value']['capabilities']
                assert caps['input'] and not caps['nativeIOS'] and caps['tapTextCapable']==caps['tapTextInitialized']==1,caps
                assert (await s.command('multiTouchGesture',{'paths':[]}))['status']=='error'
                assert (await s.command('mouseDown',{'x':40,'y':50}))['status']=='error'
                assert not calls
                enabled=await s.command('enableLiveMouse')
                assert enabled['status']=='ok' and enabled['value']['mouse']['profile']==mouse['profile'],enabled
                assert not calls,'Enable must not calibrate, toggle AssistiveTouch, or send native-iOS commands'
                await s.command('startVideo',{'maxFramesPerSecond':20,'maxDimension':320})
                assert await s.collect(.4)
                await s.command('mouseDown',{'x':40,'y':50,'mac':'evil','device':'evil','width':999,'height':999})
                await s.command('mouseMove',{'path':[{'x':50,'y':60,'offset':.1},{'x':60,'y':70,'offset':.2}]})
                await s.command('mouseUp',{'x':60,'y':70})
                assert [a for a,_ in calls]==['mouseDown','mouseMove','mouseUp'],calls
                for _,body in calls:
                    assert body['width']==375 and body['height']==667 and body['orientation']=='portrait',body
                assert calls[1][1]['path']==[{'x':50,'y':60},{'x':60,'y':70}]
                assert not held
                print('PASS readiness, existing profile, logical geometry, input capabilities, ordered mouse API and target ownership')
                await s.command('disableLiveMouse')
                # Every supported mapping, and both aliases, without Live Mouse enabled.
                valid=''.join(chr(c) for c in range(32,127))+'\t\n'
                for action in ('typeText','tapText'):
                    response=await s.command(action,{'text':valid,'device':'evil','mac':'evil'})
                    assert response['status']=='ok' and response['value']['charactersSubmitted']==len(valid),response
                    assert typed[-1]==valid
                    assert (await s.command(action,{'text':''}))['value']['charactersSubmitted']==0
                    for bad in ['valid prefix 😀 suffix','abcé','abce\u0301','abc\r\n', 'a'*1025]+[
                        'valid prefix '+chr(c)+' suffix' for c in list(range(32))+[127] if c not in (9,10)]:
                        before=len(calls)
                        response=await s.command(action,{'text':bad})
                        assert response['status']=='error',response
                        assert len(calls)==before,'Invalid text reached the helper and could type a prefix'
                        assert 'valid prefix' not in response['error'],'Error must not echo supplied text'
                    for args in ({},{'text':None},{'text':123}):
                        before=len(calls)
                        assert (await s.command(action,args))['status']=='error'
                        assert len(calls)==before
                before=len(typed)
                response=await s.command('typeText',{'text':'helper rejects this'})
                assert response['status']=='error' and 'helper rejected whole string' in response['error'],response
                assert len(typed)==before
                assert sum(a=='type' and b['text']=='helper rejects this' for a,b in calls)==1,'Helper errors must not be retried'
                print('PASS both text aliases, full US ASCII keymap, empty text, whole-call rejection of unmappable characters, helper errors')
                if os.getenv('CF_SERVER_CHECKOUT'):
                    response=await s.command('getCFAgentCapabilities')
                    check=await asyncio.create_subprocess_exec('node',str(pathlib.Path(__file__).with_name('frontend-capabilities.cjs')),os.environ['CF_SERVER_CHECKOUT'],'1',stdin=asyncio.subprocess.PIPE)
                    await check.communicate(json.dumps(response).encode());assert check.returncode==0
                # Text waits for the helper's full call, not the two-second mouse timeout.
                text_delay=2.3;entered.clear()
                typing=asyncio.create_task(s.command('typeText',{'text':'This text takes longer than two seconds'}))
                await asyncio.wait_for(entered.wait(),2)
                started=time.monotonic();assert (await s.command('ping'))['value']=='pong'
                assert time.monotonic()-started<.4 and len(await s.collect(.5))>=5
                assert (await typing)['status']=='ok';text_delay=0
                keyboard_ready=False
                for _ in range(60):
                    caps=(await s.command('getCFAgentCapabilities'))['value']['capabilities']
                    if caps['tapTextCapable']==0:break
                    await asyncio.sleep(.05)
                assert caps['tapTextCapable']==caps['tapTextInitialized']==0
                assert (await s.command('typeText',{'text':'no keyboard'}))['status']=='error'
                keyboard_ready=True;pointer_ready=False
                await availability(False)
                assert (await s.command('tapText',{'text':'keyboard-only input'}))['status']=='ok'
                pointer_ready=True;await availability(True)
                assert any(j.get('type')=='CFAgentCapabilities' and j['value']['capabilities']['tapTextCapable']==0 for j in s.notices)
                assert any(j.get('type')=='CFAgentCapabilities' and j['value']['capabilities']['tapTextCapable']==1 for j in s.notices)
                print('PASS long text timeout, responsive video/ACKs, keyboard-only readiness and capability updates')
                await s.command('enableLiveMouse')

                # A slow helper must not block CF ping, JPEGs, or ACK processing.
                await s.command('mouseDown',{'x':80,'y':80})
                delay=.75;entered.clear()
                moving=asyncio.create_task(s.command('mouseMove',{'x':100,'y':100}))
                await asyncio.wait_for(entered.wait(),2)
                started=time.monotonic();assert (await s.command('ping'))['value']=='pong'
                assert time.monotonic()-started<.4
                assert len(await s.collect(.5))>=5
                assert (await moving)['status']=='ok'
                delay=0
                await s.command('disableLiveMouse');assert not held
                assert (await s.command('mouseDown',{'x':80,'y':80}))['status']=='error'
                print('PASS helper latency does not stall ping/video/ACKs; disable releases buttons and gates input')
                await s.command('enableLiveMouse');await s.command('mouseDown',{'x':80,'y':80})
                fail=True
                assert (await s.command('mouseMove',{'x':90,'y':90}))['status']=='error'
                await until(lambda:not held);fail=False
                assert (await s.command('mouseDown',{'x':80,'y':80}))['status']=='error'
                await s.command('enableLiveMouse');await s.command('mouseDown',{'x':80,'y':80})
                assert (await s.command('mouseMove',{'x':'invalid','y':80}))['status']=='error'
                await until(lambda:not held)
                print('PASS HTTP and argument errors release held buttons; explicit re-enable required')
                await s.command('enableLiveMouse');await s.command('mouseDown',{'x':80,'y':80})
                delay=1.25;entered.clear()
                moving=asyncio.create_task(s.command('mouseMove',{'x':90,'y':90}))
                await entered.wait()
                expired=asyncio.create_task(s.command('mouseMove',{'x':110,'y':110}))
                assert (await moving)['status']=='ok'
                expired_result=await expired
                assert 'expired' in expired_result.get('error',''),expired_result
                await until(lambda:not held);delay=0
                assert not any(a=='mouseMove' and b.get('x')==110 for a,b in calls)
                print('PASS stale queued movement expires rather than replaying late')
                peers.append('second-host');await availability(False)
                assert (await s.command('enableLiveMouse'))['status']=='error'
                peers[:]=['replacement-host'];await asyncio.sleep(1.2);await availability(False)
                assert (await s.command('enableLiveMouse'))['status']=='error'
                peers[:]=[host];await availability(True)
                print('PASS multiple phones and replacement identities rejected; original phone recovers')
                await s.command('enableLiveMouse');await s.command('mouseDown',{'x':80,'y':80})
                delay=.75;entered.clear()
                moving=asyncio.create_task(s.command('mouseMove',{'x':90,'y':90}))
                await entered.wait()
                # Deliberately exceed the bounded input queue using untracked tags.
                for i in range(80):
                    await s.ws.send_json({'action':'mouseMove','args':{'x':100,'y':100},'tag':f'overflow-{i}'})
                await until(lambda:any('queue full' in j.get('error','') for j in s.notices))
                delay=0;await until(lambda:not held)
                moving.cancel()
                try:await moving
                except asyncio.CancelledError:pass
                await asyncio.sleep(.3)
                print('PASS bounded input queue cancels overload and releases buttons')
                await s.command('enableLiveMouse');await s.command('mouseDown',{'x':80,'y':80})
                await s.ws.close();await until(lambda:not held)
                s=await asyncio.wait_for(sessions.get(),16)
                assert (await s.command('mouseDown',{'x':80,'y':80}))['status']=='error'
                await s.command('enableLiveMouse');await s.command('mouseDown',{'x':80,'y':80})
                process.send_signal(signal.SIGTERM);await asyncio.wait_for(process.wait(),10);stopped=True
                assert not held and process.returncode==0
                print('PASS provider reconnect and process shutdown release buttons without calibration')
            except BaseException:
                output.flush();output.seek(0);print(output.read(),file=sys.stderr);raise
            finally:
                if not stopped and process.returncode is None:
                    process.send_signal(signal.SIGTERM)
                    try:await asyncio.wait_for(process.wait(),10)
                    except asyncio.TimeoutError:process.kill();await process.wait()
    await runner.cleanup()

if __name__=='__main__':asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
