#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Provider/WebSocket audio compatibility using headless synthetic GStreamer A/V.
No real device, server account, or local speaker output is used.
"""
import asyncio,io,json,os,pathlib,signal,struct,sys,tempfile,time
from aiohttp import web,WSMsgType
from PIL import Image
from integration import Session

class AVSession(Session):
    def __init__(self,ws):
        super().__init__(ws);self.audio=asyncio.Queue()
    async def read(self):
        header=None
        async for message in self.ws:
            if message.type==WSMsgType.TEXT:
                assert header is None,'Interleaved audio/video header'
                value=json.loads(message.data)
                if value.get('binaryLength'):header=value;continue
                tag=value.get('tag')
                if isinstance(tag,str) and tag in self.waiters:self.waiters.pop(tag).set_result(value)
                else:self.notices.append(value)
            elif message.type==WSMsgType.BINARY:
                assert header and len(message.data)==header['binaryLength']
                if header['type']=='AudioFrame':
                    assert header['cf']['d']=='airplay-test-12345'
                    await self.audio.put((header,message.data))
                else:
                    assert header['type']=='VideoFrame';Image.open(io.BytesIO(message.data)).load()
                    await self.frames.put((header,message.data))
                    if self.acks:
                        v=header['value'];await self.ws.send_json({'action':'ackVideoFrame','args':{'frameId':v['frameId'],'source':v['source'],'sourceGeneration':v['sourceGeneration']}})
                header=None
    async def sound(self,seconds):
        result=[];deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            try:result.append(await asyncio.wait_for(self.audio.get(),max(.001,deadline-time.monotonic())))
            except asyncio.TimeoutError:break
        return result

async def main(binary):
    sessions=asyncio.Queue()
    async def login(_):
        r=web.Response(text='ok');r.set_cookie('session','test');return r
    async def provider(r):
        ws=web.WebSocketResponse();await ws.prepare(r);s=AVSession(ws);await sessions.put(s);await s.read();return ws
    app=web.Application();app.router.add_post('/login',login);app.router.add_get('/provider/ws',provider)
    runner=web.AppRunner(app);await runner.setup();site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
    server=f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}'
    with tempfile.TemporaryDirectory(prefix='cf-audio-') as directory:
        root=pathlib.Path(directory);config=root/'config.json';config.write_text(json.dumps(dict(server=server,username='mock',password='mock')))
        processes=[];logs=[]
        async def launch(wave):
            log=open(root/(wave+'.log'),'w+');logs.append(log)
            p=await asyncio.create_subprocess_exec(binary,'--audio-delay-ms',os.environ.get('CF_TEST_AUDIO_DELAY_MS','0'),'--controlfloor-config',str(config),'--controlfloor-log-dir',str(root/wave),'--controlfloor-test-source',
                env=dict(os.environ,CF_TEST_AUDIO=wave),stdout=log,stderr=log)
            processes.append(p);return await asyncio.wait_for(sessions.get(),15)
        try:
            s=await launch('sine');await asyncio.sleep(.2)
            assert (await s.command('getCFAgentCapabilities'))['value']['capabilities']['audio']
            assert (await s.command('getVideoStreamStatus'))['value']['audioEnabled']==0
            assert not await s.sound(.15)
            assert (await s.command('startAudio',{'encoding':'PCM16-44100-2ch','silence':'include'}))['status']=='ok'
            packets=await s.sound(.6);assert len(packets)>30,len(packets)
            data=b''.join(p[1] for p in packets);values=struct.unpack('<'+'h'*(len(data)//2),data)
            assert max(values)>10000 and min(values)<-10000
            assert all(values[i]==values[i+1] for i in range(0,len(values),2))
            assert all(p[0]['value']['encoding']=='PCM16-44100-2ch' for p in packets)
            print('PASS real decoded PCM tap, stereo signed samples, native client headers, audio without JPEG demand')
            cfg=(await s.command('getVideoStreamConfiguration'))['value']
            assert len(cfg['supportedAudioEncodings'])==12 and cfg['audioEnabled']==1 and cfg['audioEncoding']=='PCM16-44100-2ch'
            for bits,rate,channels in [(8,22050,1),(16,11025,2),(16,22050,1)]:
                encoding=f'PCM{bits}-{rate}-{channels}ch'
                assert (await s.command('setAudioStreamConfiguration',dict(encoding=encoding)))['status']=='ok'
                await s.sound(.08);packets=await s.sound(.3)
                assert len(packets)>10 and all(p[0]['value']['encoding']==encoding for p in packets)
                # Buffer length varies by one sample because 441 input frames
                # don't evenly divide into the downsampling factor.
                expected=rate*channels*(bits//8)/100
                assert all(abs(len(p[1])-expected)<=channels*(bits//8) for p in packets)
            before=(await s.command('getAudioStreamConfiguration'))['value']
            assert (await s.command('setAudioStreamConfiguration',dict(encoding='AAC',silence='report')))['status']=='error'
            assert (await s.command('getAudioStreamConfiguration'))['value']==before
            await s.command('stopAudio');await s.sound(.15);assert not await s.sound(.25)
            assert (await s.command('getVideoStreamStatus'))['value']['audioEnabled']==0
            await s.command('startAudio',{'encoding':'','silence':''});assert len(await s.sound(.2))>5
            print('PASS encoding changes, blank-argument reuse, transactional errors, stop/resume playback')
            await s.command('startVideo',dict(maxDimension=320,maxFramesPerSecond=10))
            await s.command('startAudio',dict(encoding='PCM16-22050-1ch',silence='include'))
            assert len(await s.collect(.6))>3;assert len(await s.sound(.2))>10
            s.acks=False;await s.collect(.3);await s.sound(.05)
            assert len(await s.sound(.4))>20
            assert (await s.command('ping'))['type']=='pong'
            await s.command('stopAudio');await s.sound(.1);assert not await s.sound(.2)
            s.acks=True;assert await s.collect(1.2)
            print('PASS interleaved JPEG/audio framing, missing video ACKs do not stop audio, mute preserves video')
            await s.command('startAudio');await s.ws.close()
            s=await asyncio.wait_for(sessions.get(),15)
            await asyncio.sleep(.15);assert (await s.command('getVideoStreamStatus'))['value']['audioEnabled']==0
            assert not await s.sound(.2)
            await s.command('startAudio');assert await s.sound(.2)
            # A second receiver worker has its own audio and mute state.
            silent=await launch('silence');await silent.command('startAudio',dict(encoding='PCM8-11025-1ch',silence='include'))
            packets=await silent.sound(.3);assert packets and all(not any(p[1]) for p in packets)
            await silent.command('stopAudio');assert await s.sound(.2)
            await silent.command('startAudio',dict(silence='drop'));await silent.sound(1.25);assert not await silent.sound(.2)
            await silent.command('startAudio',dict(silence='report'))
            await silent.sound(3.4)
            reports=[n for n in silent.notices if n.get('type')=='AudioSilence'];assert reports,reports
            assert reports[-1]['binaryLength']==0 and reports[-1]['value']['bytes']>0
            assert reports[-1]['value']['durationMs']>=2000
            print('PASS reconnect defaults muted, isolated receiver audio state, silence include/drop/report')
            debug=(await silent.command('getVideoTransportDebug'))['value']['audio']
            assert debug['queuedBytes']<=debug['queueLimitBytes']
        except Exception:
            for log in logs:log.flush();log.seek(0);print(log.read()[-4000:],file=sys.stderr)
            raise
        finally:
            for p in processes:
                if p.returncode is None:p.send_signal(signal.SIGTERM)
            for p in processes:
                try:await asyncio.wait_for(p.wait(),10)
                except asyncio.TimeoutError:p.kill();await p.wait()
                assert p.returncode==0,p.returncode
            for log in logs:log.close()
            await runner.cleanup()

asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
