#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real LAN HTTP server + coordinator + input adapters, with simulated phones/HID.
No real helper, AirPlay, network configuration, credentials or phone input used.
"""
import asyncio, json, pathlib, sys, tempfile, time, socket, re
from aiohttp import web, ClientSession

async def main(binary):
    calls=[]; host='keyboard-A'; phone_ip='127.0.0.1'; inventory=[host,'keyboard-B']
    capture='idle'; token=''; pairing_request=None; about_request=None
    worker='receiver-1'; connection=1; connected=False; ready=False; allow_about=False; admitted=False
    outputs=[]; tasks=[]; auto_replay=True; auto_about=True; fail_replay=False
    recording_b=True
    record={}; http=None; base=''; p=None; output_task=None; status_task=None
    async def event(kind, message=None, id=None):
        p.stdin.write((json.dumps({'type':kind,'worker':id or worker,'peer':phone_ip,'message':message or {}})+'\n').encode())
        await p.stdin.drain()
    async def report(command, **extra):
        body=dict(protocolVersion=2,shortcutVersion='0.4.19',status='ok',**command,**extra)
        async with http.post(base+'/shortcuts/report',json=body) as r:return r.status, await r.json()
    async def fetch():
        async with http.get(base+'/shortcuts/next') as r:
            value=await r.json();assert r.status==200,(r.status,value);return value
    async def initial_fetch():
        nonlocal pairing_request
        pairing_request=await fetch();assert pairing_request['command']=='initiate-pairing',pairing_request
    async def replay():
        nonlocal capture,connected,about_request
        await asyncio.sleep(.15)
        await event('starting');connected=True
        await event('message',{'type':'status','connection':connection,'connected':True,'ready':False})
        # The final recorded Mouse 8 fetches work before replay's asynchronous completion.
        about_request=await fetch();assert about_request['command']=='prefs-general-about',about_request
        if auto_about:
            status,value=await report(about_request);assert status==200,(status,value)
        await asyncio.sleep(.25);capture='idle'
    async def devices(r):
        return web.json_response({'bluetoothHosts':[{'bluetoothHost':h,'keyboard':True,'consumerControls':True,'liveMouse':True,
            'mouse':{'logicalWidth':375,'logicalHeight':667,'down':False,'capture':{'state':'recording' if h=='keyboard-B' and recording_b else capture,'error':None}}} for h in inventory]})
    async def post(r):
        nonlocal token,capture
        body=await r.json();action=r.match_info['action'];calls.append((action,body))
        if body['device'] not in inventory:
            return web.json_response({'error':'Bluetooth host disconnected'},status=503)
        if body['device']=='keyboard-B':
            return web.json_response({'submitted':True,'mouse':{'down':action=='mouseDown'}})
        if action in ('mouseDown','mouseUp'):
            assert body['button']==8 and 'x' not in body and 'y' not in body
            if action=='mouseUp':
                if pairing_request is None:tasks.append(asyncio.create_task(initial_fetch()))
                elif admitted:
                    async def remote_shortcut():
                        req=await fetch(); record['remote']=req
                        await report(req,text='clipboard fixture')
                    tasks.append(asyncio.create_task(remote_shortcut()))
            return web.json_response({'mouse':{'down':action=='mouseDown'}})
        if action=='type':
            token=body['text'];assert re.fullmatch(r'[0-9]{2}(?:-[0-9]{2}){3}',token),token
        elif action=='typeSpecialKey':
            key=body['key']
            if key=='Enter':
                assert pairing_request and token
                # Invalid spellings must not bind or consume this request.
                for invalid in ('X='+token+'=X', token.replace('-',''), token.replace('-',':'), 12345678):
                    code,_=await report(pairing_request,pairingId=invalid,model='iPhone SE',iosVersion='26.6');assert code==409
                code,_=await report(pairing_request,pairingId=token,model='iPhone SE',iosVersion='26.6');assert code==200
            elif key=='replayCapture':
                if fail_replay:return web.json_response({'error':'capture is empty'},status=400)
                capture='replaying'
                if auto_replay:tasks.append(asyncio.create_task(replay()))
                return web.json_response({'capture':{'state':'replaying','durationMs':400}})
            elif key=='GoHome':assert allow_about and ready
            elif key=='endCapture':capture='idle'
            elif key=='Backspace':pass
            else:raise AssertionError(body)
        return web.json_response({'submitted':True})
    app=web.Application();app.router.add_get('/devices',devices);app.router.add_post('/{action}',post)
    runner=web.AppRunner(app);await runner.setup();site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
    helper=f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}'
    async def until(predicate,seconds=15):
        end=time.monotonic()+seconds
        while not predicate():
            assert time.monotonic()<end,('timed out',outputs[-8:],calls[-8:])
            await asyncio.sleep(.03)
    with tempfile.TemporaryDirectory() as directory, open(pathlib.Path(directory)/'stderr','w+') as log:
        try:
            async with ClientSession() as client:
                http=client
                with socket.socket() as listener:
                    listener.bind(('127.0.0.1',0));lan_port=listener.getsockname()[1]
                p=await asyncio.create_subprocess_exec(binary,helper,directory,str(lan_port),stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.PIPE,stderr=log)
                first=json.loads(await asyncio.wait_for(p.stdout.readline(),10));base=f'http://127.0.0.1:{first["port"]}'
                async def read():
                    nonlocal allow_about,admitted
                    while line:=await p.stdout.readline():
                        value=json.loads(line);outputs.append(value)
                        msg=value['message']
                        if msg.get('type')=='binding':
                            allow_about=msg['about'];admitted=msg['admit']
                output_task=asyncio.create_task(read())
                async def statuses():
                    while True:
                        if connected:
                            await event('message',{'type':'status','connection':connection,'connected':True,'ready':ready,
                                'identity':{'keyboardId':token,'name':'Fixture','modelName':'iPhone SE',
                                            'serialNumber':'TEST23456789','modelNumber':'MX9U2J/A','osVersion':'26.6'}})
                        await asyncio.sleep(.15)
                status_task=asyncio.create_task(statuses())
                await until(lambda:allow_about)
                assert not admitted
                assert not any(body['device']=='keyboard-B' for _,body in calls),'Do not type into an active recording'
                recording_b=False
                await until(lambda:any(action=='type' and body['device']=='keyboard-B' for action,body in calls))
                inventory.remove('keyboard-B')
                assert [body['key'] for action,body in calls if action=='typeSpecialKey' and body['device']==host]==['Enter','replayCapture']
                assert sum(action=='mouseUp' and body['device']==host for action,body in calls)==1,'Coordinator must not double-trigger replay final Mouse 8'
                assert (await report(pairing_request,pairingId=token,model='iPhone SE',iosVersion='26.6'))[1]['duplicate']
                code,_=await report(dict(command='prefs-general-about',requestId='stale'));assert code==409
                ready=True;await until(lambda:admitted)
                assert any(action=='typeSpecialKey' and body['key']=='GoHome' for action,body in calls)
                assert (await fetch())=={'command':'ping','requestId':''}
                print('PASS startup token callback, IP binding, asynchronous replay + final shortcut fetch, About gate, Home before admission')
                req={'action':'typeSpecialKey','args':{'key':'Backspace'},'tag':'remote-1','cf':{'d':'airplay-TEST23456789'}}
                await event('message',{'type':'input','connection':connection,'request':req,'args':req['args']})
                await until(lambda:any(v['message'].get('request',{}).get('tag')=='remote-1' for v in outputs))
                value=next(v['message'] for v in outputs if v['message'].get('request',{}).get('tag')=='remote-1')
                assert not value['error'] and value['request']=={k:v for k,v in req.items() if k in ('action','cf','tag','events')}
                req={'action':'getClipboardText','agent':'Shortcuts','args':{},'tag':'shortcut-1'}
                await event('message',{'type':'input','connection':connection,'request':req,'args':{}})
                await until(lambda:any(v['message'].get('type')=='input-result' and v['message'].get('request',{}).get('tag')=='shortcut-1' for v in outputs))
                value=next(v['message'] for v in outputs if v['message'].get('type')=='input-result' and v['message'].get('request',{}).get('tag')=='shortcut-1')
                assert value['value']=={'text':'clipboard fixture','hasText':True} and not value['error'],value
                assert record['remote']['command']=='clipboard-get'
                req={'action':'setBrightness','args':{'percent':42},'tag':'brightness-1'}
                await event('message',{'type':'input','connection':connection,'request':req,'args':req['args']})
                await until(lambda:any(v['message'].get('type')=='input-result' and v['message'].get('request',{}).get('tag')=='brightness-1' for v in outputs))
                assert record['remote']['command']=='brightness-set' and record['remote']['value']==42
                print('PASS matched keyboard routing, CF request correlation, queued stateless shortcut commands and callback responses')
                # A reconnect invalidates the whole old binding; old reports cannot complete a new session.
                connected=False
                await event('message',{'type':'status','connection':connection+1,'connected':False})
                await until(lambda:any(v['disconnect'] for v in outputs))
                previous=len(calls)
                await event('message',{'type':'input','connection':connection,'request':req,'args':{}})
                await asyncio.sleep(.3);assert len(calls)==previous
                async with http.post(base+'/shortcuts/report',json={'requestId':3}) as r:assert r.status==400
                async with http.get(base+'/does-not-exist') as r:assert r.status==404
                print('PASS disconnect revokes binding/input, stale messages ignored, malformed HTTP rejected')
                records=[json.loads(line) for path in pathlib.Path(directory).glob('keyboard-*.jsonl') for line in path.read_text().splitlines()]
                assert {'keyboard-ip-bound','airplay-ip-bound','replay-completed','about-ocr-enabled','device-identified','live','pairing-failed'} <= {v['event'] for v in records}
                print('PASS per-keyboard logs cover the complete lifecycle')
                # Retry automatically gets a fresh token. A missing recording fails
                # that attempt without admitting a device or keeping input held.
                previous_token=token;pairing_request=None;admitted=False;ready=False;fail_replay=True
                await until(lambda:token!=previous_token,seconds=12)
                await until(lambda:any(action=='typeSpecialKey' and body['key']=='endCapture' for action,body in calls))
                assert token!=previous_token
                failures=[json.loads(line) for path in pathlib.Path(directory).glob('keyboard-*.jsonl') for line in path.read_text().splitlines()
                          if json.loads(line)['event']=='pairing-failed']
                assert any('capture is empty' in str(r['detail']) for r in failures),failures
                print('PASS independent keyboard startup, disappeared keyboard isolation, fresh-token retry and missing replay failure')
                previous_token=token;pairing_request=None;auto_replay=False;fail_replay=False
                await until(lambda:token!=previous_token,seconds=12)
                await until(lambda:capture=='replaying')
                await until(lambda:capture=='idle',seconds=16)
                records=[json.loads(line) for path in pathlib.Path(directory).glob('keyboard-*.jsonl') for line in path.read_text().splitlines()]
                assert any(r['event']=='pairing-failed' and 'Timed out' in str(r['detail']) for r in records)
                assert not admitted
                paths=list(pathlib.Path(directory).glob('keyboard-*.jsonl'))
                assert {p.name for p in paths}=={'keyboard-keyboard-A.jsonl','keyboard-keyboard-B.jsonl'}
                attempts=[json.loads(line) for line in (pathlib.Path(directory)/'keyboard-keyboard-A.jsonl').read_text().splitlines()]
                assert len({r['pairingId'] for r in attempts if r['event']=='startup'})>=3
                print('PASS repeated pairing attempts append to the same keyboard log')
                print('PASS missing AirPlay/shortcut timeout cancels replay and never admits the device')
        except BaseException:
            log.flush();log.seek(0);print(log.read(),file=sys.stderr)
            for task in tasks:
                if task.done() and not task.cancelled() and task.exception():print(repr(task.exception()),file=sys.stderr)
            raise
        finally:
            for task in [status_task,*tasks]:
                if task:task.cancel()
            await asyncio.gather(*(task for task in [status_task,*tasks] if task),return_exceptions=True)
            if p and p.returncode is None:
                p.stdin.write(b'{"type":"quit"}\n');await p.stdin.drain()
                try:await asyncio.wait_for(p.wait(),15)
                except asyncio.TimeoutError:p.kill();await p.wait()
            if output_task:await output_task
            await runner.cleanup()
            if p:assert p.returncode==0,p.returncode
if __name__=='__main__':asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
