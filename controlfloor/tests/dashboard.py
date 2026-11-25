#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Dashboard/API/persistence integration against real coordinator + mock HID.
Use --demo to leave an isolated, harmless dashboard running for browser checks.
"""
import asyncio
import base64
import json
import pathlib
import socket
import sqlite3
import sys
import tempfile
import time
from aiohttp import ClientSession, web
from calibration_client import complete_calibration

async def main(binary, demo=False):
    host='keyboard-A'; inventory=[host]; calls=[]; timed_calls=[]; tasks=[]; outputs=[]
    request=None; code=''; process=None; reader=None; base=''; token=''
    capture='idle'; connected=False; cf=False; ready=False; worker='receiver-1'
    status_task=None; mirror_count=0; serial='TEST23456789'; mode='manual'
    calibration_release=asyncio.Event();calibration_release.set();calibration_urls=[]
    hold_about=False; about_fetched=False; about_release=asyncio.Event()
    async def until(predicate, seconds=10):
        deadline=time.monotonic()+seconds
        while True:
            value=predicate()
            if asyncio.iscoroutine(value): value=await value
            if value: return value
            assert time.monotonic()<deadline, ('timeout',calls[-6:],outputs[-3:])
            await asyncio.sleep(.05)
    async def fetch():
        async with http.get(base+'/shortcuts/next') as r:
            value=await r.json(); assert r.status==200,value; return value
    async def report(req, **extra):
        async with http.post(base+'/shortcuts/report',json=dict(protocolVersion=2,shortcutVersion='0.4.19',status='ok',**req,**extra)) as r:
            return r.status, await r.json()
    async def event(kind, message=None, id=None, peer='127.0.0.1'):
        if message is not None:message.setdefault('token',code)
        process.stdin.write((json.dumps(dict(type=kind,worker=id or worker,peer=peer,message=message or {}))+'\n').encode())
        await process.stdin.drain()
    async def replay():
        nonlocal capture,connected,ready
        await event('starting'); connected=True; ready=False
        await status()
        req=await fetch(); assert req['command']=='prefs-general-about',req
        assert (await report(req))[0]==200
        await asyncio.sleep(.1); capture='idle'; ready=True
    async def status():
        if connected:
            await event('message',dict(type='status',connection=1,connected=True,ready=ready,
                providerConnected=cf,providerTransportOpen=ready and any(v['message'].get('admit') and v['message'].get('token')==code for v in outputs),
                identity=dict(keyboardId=code,name='Fixture phone',modelName='iPhone SE',serialNumber=serial)))
    async def statuses():
        while True:
            await status(); await asyncio.sleep(.1)
    async def devices(_):
        return web.json_response({'bluetoothHosts':[dict(bluetoothHost=h,keyboard=True,consumerControls=True,liveMouse=True,
            mouse=dict(logicalWidth=375,logicalHeight=667,down=False,capture=dict(state=capture,error=None))) for h in inventory]})
    async def post(r):
        nonlocal request,code,capture,mirror_count,ready,about_fetched
        body=await r.json(); action=r.match_info['action'];calls.append((action,body));timed_calls.append((time.monotonic(),action,body))
        if body['device']!=host:return web.json_response({'submitted':True,'mouse':{'down':action=='mouseDown'}})
        if action=='mouseUp' and (mode in ('pair','reuse') or ready):
            fetched=await fetch()
            if fetched['command']=='initiate-pairing':request=fetched
            elif fetched['command']=='open-url':
                calibration_urls.append(fetched['url']);assert (await report(fetched))[0]==200
                tasks.append(asyncio.create_task(complete_calibration(http,base,fetched['url'],calibration_release)))
            else:
                assert fetched['command']=='prefs-general-about',fetched
                assert (await report(fetched))[0]==200
                ready=True;about_fetched=True
                if hold_about:await about_release.wait()
        if action=='type':code=body['text']
        if action=='typeSpecialKey':
            key=body['key']
            if key=='Enter':
                assert request and request['command']=='initiate-pairing',request
                assert (await report(request,pairingId=code,model='iPhone SE',iosVersion='26.6'))[0]==200
            elif key=='replayCapture':
                mirror_count+=1; capture='replaying';tasks.append(asyncio.create_task(replay()))
            elif key=='endCapture':capture='idle'
        return web.json_response({'submitted':True,'mouse':{'down':action=='mouseDown'}})
    app=web.Application();app.router.add_get('/devices',devices);app.router.add_post('/{action}',post)
    runner=web.AppRunner(app);await runner.setup();site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
    helper=f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}'
    async def snapshot():
        nonlocal token
        async with http.get(base+'/api/keyboards') as r:
            data=await r.json();assert r.status==200,data;token=data['mutationToken'];return data
    async def row():return (await snapshot())['keyboards'][0]
    async def matches(**expected):
        data=await row();return data if all(data[k]==v for k,v in expected.items()) else None
    async def control(action, expected=202, **extra):
        async with http.post(base+'/api/keyboards/control',json=dict(id=host,action=action,**extra),headers={'X-UxPlay-Token':token}) as r:
            data=await r.json();assert r.status==expected,(r.status,data);return data
    async def disconnect_client(id, revision, expected):
        async with http.post(base+'/api/airplay/disconnect',json={'worker':id,'connection':revision},headers={'X-UxPlay-Token':token}) as r:
            value=await r.json();assert r.status==expected,(r.status,value)
    async def launch(directory, log):
        nonlocal process,base,reader,status_task
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0));port=listener.getsockname()[1]
        process=await asyncio.create_subprocess_exec(binary,helper,directory,str(port),"--startup-timing",stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.PIPE,stderr=log)
        first=json.loads(await asyncio.wait_for(process.stdout.readline(),10));base=f'http://127.0.0.1:{first["port"]}'
        async def read():
            nonlocal code,ready,cf
            while line:=await process.stdout.readline():
                item=json.loads(line);outputs.append(item)
                if item['message'].get('type')=='binding':code=item['message']['token']
                elif item['message'].get('type')=='revoke' and item['message'].get('token')==code:
                    ready=False;cf=False
        reader=asyncio.create_task(read());status_task=asyncio.create_task(statuses())
        await until(lambda:matches(inputReady=True))
    async def stop():
        nonlocal connected
        connected=False
        if status_task:status_task.cancel();await asyncio.gather(status_task,return_exceptions=True)
        if process and process.returncode is None:
            process.stdin.write(b'{"type":"quit"}\n');await process.stdin.drain()
            try:await asyncio.wait_for(process.wait(),15)
            except asyncio.TimeoutError:process.kill();await process.wait()
        if reader:await reader
        if process:assert process.returncode==0,process.returncode
    with tempfile.TemporaryDirectory() as directory, open(pathlib.Path(directory)/'stderr','w+') as log:
        db=pathlib.Path(directory)/'keyboards.sqlite3'
        with sqlite3.connect(db) as conn:
            conn.execute('CREATE TABLE keyboards(source TEXT NOT NULL,host TEXT NOT NULL,record TEXT NOT NULL,PRIMARY KEY(source,host))')
            records=[dict(host=host,autoConnect=False,label='Desk 1',lastIP='')]
            if demo:
                records+=[dict(host='keyboard-B',autoConnect=False,label='Shelf 2',lastIP='192.168.1.82',identity=dict(name='Blue phone',serialNumber='DEMO23456789',modelName='iPhone SE',osVersion='26.6')),
                          dict(host='keyboard-C',autoConnect=False,label='Spare keyboard',lastIP='')]
                inventory.append('keyboard-B')
                records += [dict(host=f'keyboard-D-{i:032x}',autoConnect=False,label=f'Shelf {i//10+1} · Phone {i%10+1}',
                    lastIP=f'192.168.1.{i+10}',identity=dict(name=f'Phone {i+4}',serialNumber=f'DEMO{i:08d}',modelName='iPhone SE',osVersion='26.6')) for i in range(197)]
            for record in records:conn.execute('INSERT INTO keyboards VALUES(?,?,?)',('iphone-use:'+helper,record['host'],json.dumps(record)))
        try:
            async with ClientSession() as http:
                await launch(directory,log)
                if demo:
                    for n in range(2):
                        await event('starting',id=f'unpaired-{n}',peer=f'169.254.234.{129+n}')
                        await event('message',dict(type='status',connection=1,connected=True,
                            airplay=dict(id=f'22:70:9B:0E:36:{50+n}',name=['Red phone','Blue phone'][n],model='iPhone12,8')),id=f'unpaired-{n}',peer=f'169.254.234.{129+n}')
                    print(base,flush=True);await asyncio.Event().wait()
                # Expose only active, unbound AirPlay clients; timestamps describe
                # the connection, not each periodic status report.
                async def unpaired():return (await snapshot())['unpairedAirplay']
                await event('starting',id='loose-worker',peer='127.0.0.2')
                assert await unpaired()==[]
                status_value=dict(type='status',connection=1,connected=True,
                    airplay=dict(id='22:70:9B:0E:36:5D',name='<b>Unpaired phone</b>',model='iPhone12,8'))
                await event('message',status_value,id='loose-worker',peer='127.0.0.2')
                loose=(await until(unpaired))[0]
                assert loose['ip']=='127.0.0.2' and loose['clientName']=='<b>Unpaired phone</b>' and loose['model']=='iPhone12,8'
                assert loose['connectedAt']>0
                await event('message',status_value,id='loose-worker',peer='127.0.0.2')
                assert (await unpaired())[0]['connectedAt']==loose['connectedAt']
                await event('message',dict(status_value,connected=False),id='loose-worker',peer='127.0.0.2')
                async def no_unpaired():return not await unpaired()
                await until(no_unpaired)
                await event('message',dict(status_value,connection=2),id='loose-worker',peer='127.0.0.2')
                reconnected=(await until(unpaired))[0]
                assert reconnected['connection']==2 and reconnected['connectedAt']>loose['connectedAt']
                await disconnect_client('loose-worker',1,409)
                assert len(await unpaired())==1
                await disconnect_client('loose-worker',2,202)
                await until(no_unpaired)
                await until(lambda:any(v['worker']=='loose-worker' and v['disconnect'] for v in outputs))
                await disconnect_client('loose-worker',2,409)
                await event('stopped',id='loose-worker',peer='127.0.0.2')
                print('PASS unpaired AirPlay inventory includes client metadata, stable connection time, reconnect and disconnect lifecycle')
                async with http.get(base+'/') as r:
                    html=await r.text();assert r.status==200 and 'text/html' in r.headers['Content-Type'] and '<title>Keyboards' in html
                async with http.get(base+'/health') as r:assert (await r.json())['protocolVersion']==2
                async with http.post(base+'/api/keyboards/control',json={'id':host,'action':'reset'}) as r:assert r.status==403
                async with http.post(base+'/api/keyboards/control',json={'id':host,'action':'reset'},headers={'X-UxPlay-Token':token,'Origin':'http://other.test'}) as r:assert r.status==403
                # Every application request is logged, including invalid JSON, unknown
                # routes, binary bodies, and oversized callbacks rejected before parsing.
                probes=[('POST','/unknown?probe=one','hello café\nfrom shortcut'.encode(),404),
                        ('POST','/shortcuts/report?probe=invalid',b'{broken json',400),
                        ('POST','/shortcuts/report?probe=large',b'x'*65537,413),
                        ('POST','/unknown?probe=binary',b'\xff\x00\xfe',404),
                        ('OPTIONS','/unknown?probe=options',b'',404),
                        ('HEAD','/shortcuts/next?probe=head',b'',200)]
                for method,uri,body,expected in probes:
                    async with http.request(method,base+uri,data=body,headers={'X-Debug-Source':'shortcut-fixture'}) as r:
                        assert r.status==expected
                        await r.read()
                records=[json.loads(line) for line in (pathlib.Path(directory)/'http.jsonl').read_text().splitlines()]
                for method,uri,body,expected in probes:
                    incoming=next(v for v in records if v.get('uri')==uri and v['event']=='http-request')
                    outgoing=next(v for v in records if v.get('httpId')==incoming['httpId'] and v['event']=='http-response')
                    raw=base64.b64decode(incoming['body']) if incoming['bodyEncoding']=='base64' else incoming['body'].encode()
                    assert raw==body and incoming['bodyBytes']==len(body)
                    assert incoming['method']==method and incoming['peer']=='127.0.0.1'
                    assert {k.lower():v for k,v in incoming['headers'].items()}['x-debug-source']=='shortcut-fixture'
                    assert outgoing['status']==expected and outgoing['durationMs']>=0
                assert any(v.get('uri')=='/api/keyboards' and v['event']=='http-request' for v in records)
                print('PASS complete incoming HTTP bodies/headers, query strings, invalid JSON, binary payloads, oversized callbacks and response correlation')
                assert not calls,'Persisted disabled keyboard must not start automation'
                await control('settings',label='<b>Desk 1</b>')
                await control('settings',expected=400,autoConnect='yes')
                await control('mouse8')
                await until(lambda:any(a=='mouseUp' for a,_ in calls))
                await asyncio.sleep(.3)
                assert [a for a,_ in calls]==['mouseDown','mouseUp'],calls
                assert not (await row())['autoConnect']
                print('PASS dashboard HTML, health, mutation protection, settings validation and one-shot Mouse 8')
                pairing_call_start=len(timed_calls)
                mode='pair';await control('pair')
                bound=await until(lambda:matches(phase='paired'))
                assert bound['ip']=='127.0.0.1' and bound['bindingVerified'] and bound['pairingCode']==code
                assert mirror_count==0,'Manual Pair Keyboard must stop at callback, before replay'
                startup=timed_calls[pairing_call_start:]
                assert [a for _,a,_ in startup]==['typeSpecialKey','typeSpecialKey','mouseDown','mouseUp','type','typeSpecialKey'],startup
                assert [b['key'] for _,a,b in startup if a=='typeSpecialKey']==['GoHome','GoHome','Enter']
                assert startup[1][0]-startup[0][0]>=1, startup
                assert startup[2][0]-startup[1][0]>=1, startup
                assert startup[4][0]-startup[3][0]>=5, startup
                assert startup[4][2]['text']==code
                print('PASS real startup timings: Home, 1s, Home, 1s, Mouse 8, 5s, armed pairing code, Enter')
                assert not bound['autoConnect']
                mode='manual';calibration_release.clear();await control('mirror')
                blocked=await until(lambda:matches(phase='calibration'))
                assert blocked['identity']['serialNumber']==serial
                assert blocked['calibration']['requiredForAdmission']
                assert not any(v['message'].get('admit') for v in outputs)
                await until(lambda:calibration_urls)
                old_calibration=calibration_urls[-1].split('#')[1]
                await control('cancel-calibration')
                await until(lambda:matches(phase='calibration-required'))
                assert not any(v['message'].get('admit') for v in outputs)
                calibration_release.set()
                async def calibration_available():return (await row())['actions']['calibrate']
                await until(calibration_available);await control('calibrate')
                await until(lambda:matches(phase='live'))
                assert len(calibration_urls)==2
                for action in ('calibrate','calibrate-new'):
                    assert not (await row())['actions'][action]
                    await control(action,expected=409)
                print('PASS OCR launches calibration; cancellation blocks admission; retry commits before Home/admission; connection-in-progress rejects calibration')
                assert await unpaired()==[], 'Bound streams must not appear as unpaired'
                assert (await row())['displayPhase']=='connecting-to-controlfloor'
                cf=True
                await until(lambda:matches(displayPhase='connected-to-controlfloor'))
                await control('calibrate',expected=409)
                for sid in (old_calibration,calibration_urls[-1].split('#')[1]):
                    async with http.post(base+'/api/calibration',json={'session':sid,'operation':'begin'}) as r:assert r.status==409
                print('PASS connected ControlFloor rejects dashboard calibration and old page mutations')
                print('PASS manual pairing stops at IP callback; mirroring reaches OCR/admission and waits for server confirmation')
                await disconnect_client(worker,1,409) # Pairing raced the UI: don't kick a bound client.
                await control('settings',autoConnect=False)
                await until(lambda:matches(phase='idle',ip='',savedIP='127.0.0.1'))
                # A late status from the revoked worker must never restore its binding.
                await status();await asyncio.sleep(.3)
                assert not (await row())['airplayConnected']
                assert any(v['worker']==worker for v in await unpaired())
                assert not any(v['worker']==worker and v['disconnect'] for v in outputs)
                # Reuse an existing stream, including a fast callback while Mouse 8 is
                # still in flight. OCR/admission must wait for input completion.
                old_code=code;before_replay=mirror_count;mode='reuse';hold_about=True;about_fetched=False
                async def mirror_ready():return (await row())['actions']['mirror']
                await until(mirror_ready);await control('mirror')
                await until(lambda:about_fetched)
                await asyncio.sleep(.2)
                assert (await row())['phase']=='waiting-for-about'
                assert not (await row())['providerConnected']
                about_release.set();hold_about=False
                await until(lambda:matches(phase='live'))
                assert code!=old_code and mirror_count==before_replay
                before_input=len(calls)
                await event('message',dict(type='input',connection=1,token=old_code,
                    request={'action':'typeSpecialKey','tag':'stale','args':{'key':'Backspace'}},args={'key':'Backspace'}))
                await event('message',dict(type='reset-input',connection=1,token=old_code))
                await asyncio.sleep(.2)
                assert len(calls)==before_input and (await row())['phase']=='live'
                # Reset discards the IP but keeps the stream. Automatic rediscovery
                # must type a new token, then reuse video without running replay.
                await control('reset');await until(lambda:matches(phase='idle',ip='',savedIP=''))
                await until(unpaired)
                mode='pair';old_code=code;await control('resume')
                await until(lambda:matches(phase='live'))
                assert code!=old_code and mirror_count==before_replay
                assert not await unpaired()
                print('PASS retained AirPlay, About-only reuse, input-completion gate, stale-token rejection and automatic reset/re-pair without replay')
                mode='manual';await control('settings',autoConnect=False)
                await until(lambda:matches(phase='idle',ip='',savedIP='127.0.0.1'))
                keyboard_log=pathlib.Path(directory)/('keyboard-'+host+'.jsonl')
                history_before=keyboard_log.read_bytes()
                http_before=(pathlib.Path(directory)/'http.jsonl').read_bytes()
                assert {p.name for p in pathlib.Path(directory).glob('keyboard-*.jsonl')}=={'keyboard-'+host+'.jsonl'}
                await stop();cf=False;ready=False;worker='receiver-2'
                previous=len(calls);await launch(directory,log)
                restored=await row()
                assert restored['phase']=='remembered' and restored['savedIP']=='127.0.0.1'
                assert restored['ip']=='' and restored['pairingCode']=='' and not restored['providerConnected']
                assert restored['label']=='<b>Desk 1</b>' and not restored['autoConnect'] and len(calls)==previous
                assert restored['identity']['serialNumber']==serial
                assert keyboard_log.read_bytes().startswith(history_before)
                assert (pathlib.Path(directory)/'http.jsonl').read_bytes().startswith(http_before)
                # An explicit saved-IP resume re-identifies the same phone successfully.
                await control('mirror')
                await until(lambda:matches(phase='live',bindingVerified=True))
                assert (await row())['identity']['serialNumber']==serial
                assert keyboard_log.read_bytes().startswith(history_before) and len(keyboard_log.read_bytes())>len(history_before)
                # Pause and resume again in the same coordinator lifetime.
                await control('settings',autoConnect=False)
                connected=False;ready=False;await event('stopped');worker='receiver-3'
                await until(lambda:matches(phase='idle',ip=''))
                async def manual_ready():return (await row())['actions']['mirror']
                await until(manual_ready)
                # Explicitly resuming the saved binding must refuse a different phone.
                serial='DIFFERENT123';await control('mirror')
                await until(lambda:matches(phase='failed'))
                assert 'different phone' in (await row())['error']
                print('PASS restart persists settings/binding, never restores live authority; changed saved-IP phone is rejected')
                await control('reset');await until(lambda:matches(phase='idle',ip='',savedIP='',pairingCode=''))
                assert (await row())['identity']=={} and not (await row())['autoConnect']
                connected=False;await stop();await launch(directory,log)
                assert not (await row())['savedIP'] and not (await row())['autoConnect']
                # Inventory discovery defaults ON, even when other saved keyboards are OFF.
                inventory.append('keyboard-new')
                async def discovered():
                    return next((k for k in (await snapshot())['keyboards'] if k['id']=='keyboard-new'),None)
                new=await until(discovered);assert new['autoConnect']
                inventory.remove('keyboard-new');inventory.remove(host)
                await until(lambda:matches(present=False,displayPhase='offline'))
                await control('mouse8',expected=409)
                print('PASS reset is durable, newly discovered keyboards default ON, missing keyboards remain listed and cannot receive input')
        except BaseException:
            if not demo:
                log.flush();log.seek(0);print(log.read(),file=sys.stderr)
                for task in tasks:
                    if task.done() and not task.cancelled() and task.exception():print(repr(task.exception()),file=sys.stderr)
            raise
        finally:
            for task in tasks:task.cancel()
            await asyncio.gather(*tasks,return_exceptions=True)
            await stop();await runner.cleanup()
if __name__=='__main__':asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve()),'--demo' in sys.argv))
