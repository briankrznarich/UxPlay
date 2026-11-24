#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual decoded-frame/OCR/CF worker gate with simulated coordinator IPC."""
import asyncio, json, pathlib, sys, tempfile, time
from aiohttp import web
from PIL import Image, ImageDraw, ImageFont
from integration import Session

async def main(binary):
    sessions=asyncio.Queue(); events=[]; lines=asyncio.Queue()
    async def login(r):
        value=web.Response(text='ok');value.set_cookie('session','mock');return value
    async def provider(r):
        ws=web.WebSocketResponse();await ws.prepare(r);s=Session(ws)
        await sessions.put((r.query['udid'],s));await s.read();return ws
    app=web.Application();app.router.add_post('/login',login);app.router.add_get('/provider/ws',provider)
    runner=web.AppRunner(app);await runner.setup();site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
    with tempfile.TemporaryDirectory() as directory:
        root=pathlib.Path(directory);config=root/'provider.json'
        config.write_text(json.dumps(dict(server=f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}',username='mock',password='mock')))
        image=Image.new('RGB',(650,700),'white');draw=ImageDraw.Draw(image)
        try:font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',24)
        except OSError:font=ImageFont.truetype('DejaVuSans.ttf',24)
        for row,text in enumerate(['About','Name                LAN Fixture','iOS Version         26.6',
            'Model Name          iPhone SE','Model Number        MX9U2J/A','Serial Number       LANTEST23456']):
            draw.text((25,40+row*60),text,fill='black',font=font)
        picture=root/'about.png';image.save(picture)
        with (root/'stderr').open('w+') as log:
            p=await asyncio.create_subprocess_exec(binary,'--controlfloor-lan','--controlfloor-config',str(config),
                '--controlfloor-log-dir',str(root/'log'),'--controlfloor-ocr-interval','.2',
                stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.PIPE,stderr=log)
            async def read():
                while raw:=await p.stdout.readline():
                    value=raw.decode().strip()
                    if value.startswith('pairing '):events.append(json.loads(value[8:]))
                    else:await lines.put(value)
            reader=asyncio.create_task(read())
            async def command(action,**args):
                p.stdin.write((json.dumps(dict(action=action,**args))+'\n').encode());await p.stdin.drain()
                if action!='quit':assert await asyncio.wait_for(lines.get(),10)=='ok'
            async def until(predicate):
                end=time.monotonic()+15
                while not predicate():
                    assert time.monotonic()<end,events[-4:]
                    await asyncio.sleep(.03)
            input_state={'available':True,'keyboardAvailable':True,'consumerControlsAvailable':True,
                         'logicalWidthPortrait':375,'logicalHeightPortrait':667,'enabled':False,'mouse':{}}
            binding=dict(type='binding',connection=1,token='01-17-45-07',keyboard='hardware-keyboard-A',
                         about=False,admit=False,input=input_state)
            try:
                assert await asyncio.wait_for(lines.get(),15)=='ready'
                await command('connect',id='00:11:22:33:44:55');await command('frame',path=str(picture))
                await until(lambda:any(e.get('connected') and e.get('airplay',{}).get('id')=='00:11:22:33:44:55' for e in events))
                advertised=next(e['airplay'] for e in events if e.get('airplay',{}).get('id')=='00:11:22:33:44:55')
                assert 'name' in advertised and 'model' in advertised
                await command('pairing',message=binding);await asyncio.sleep(.6)
                assert sessions.empty() and not any(e.get('ready') for e in events)
                binding['about']=True;await command('pairing',message=binding);await command('frame',path=str(picture))
                await until(lambda:any(e.get('ready') for e in events));assert sessions.empty()
                await command('pairing',message=dict(binding,connection=999,admit=True));await asyncio.sleep(.3);assert sessions.empty()
                binding['admit']=True;await command('pairing',message=binding)
                udid,s=await asyncio.wait_for(sessions.get(),15);assert udid=='airplay-LANTEST23456',udid
                info=(await s.command('getDeviceInformation'))['value'];assert info['keyboardId']=='hardware-keyboard-A',info
                assert (await s.command('getDeviceStatus'))['value']['liveMouse']
                print('PASS real About OCR without token display, explicit About/admission gates, hardware ID and CF registration')
                pending=asyncio.create_task(s.command('typeSpecialKey',{'key':'Backspace'}))
                await until(lambda:any(e.get('type')=='input' for e in events))
                emitted=next(e for e in events if e.get('type')=='input')
                assert emitted['args']['modelName']=='iPhone SE' and emitted['connection']==1
                await command('pairing',message={'type':'input-result','connection':1,'request':emitted['request'],
                    'value':{'submitted':True},'error':''})
                assert (await pending)['value']['submitted']
                shot=asyncio.create_task(s.command('getScreenshot'));await command('frame',path=str(picture))
                assert (await shot)['binaryLength']>100
                print('PASS paired-worker input/reply IPC, trusted model context, independent JPEG screenshot delivery')
                await command('disconnect');await command('connect',id='new-airplay-client')
                await command('pairing',message=binding);await command('frame',path=str(picture))
                await asyncio.sleep(.7);assert sessions.empty()
                assert not any(e.get('ready') for e in events if e.get('connection')==3)
                print('PASS reconnect requires new binding; stale IPC cannot re-admit or reuse old OCR identity')
            except BaseException:
                log.flush();log.seek(0);print(log.read(),file=sys.stderr);raise
            finally:
                if p.returncode is None:await command('quit')
                try:await asyncio.wait_for(p.wait(),15)
                except asyncio.TimeoutError:p.kill();await p.wait()
                await reader;assert p.returncode==0
    await runner.cleanup()
if __name__=='__main__':asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
