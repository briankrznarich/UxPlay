#!/usr/bin/env python3
"""Black-box registration, JPEG, configuration, ACK/reconnect tests. Requires aiohttp, Pillow."""
import asyncio, contextlib, io, json, os, pathlib, signal, sys, tempfile, time
from aiohttp import web, WSMsgType
from PIL import Image

async def main(binary):
    native=(608,1080) if os.getenv('CF_TEST_H264') else (720,1280)
    sessions = asyncio.Queue()
    async def login(r):
        data = await r.post()
        assert data['username'] in ('admin', 'mock-provider')
        response = web.Response(text='ok')
        response.set_cookie('session', 'test-session')
        return response
    async def register(r):
        assert r.cookies.get('session') == 'test-session'
        return web.json_response({'Username':'mock-provider','Password':'mock-password','Existed':False})
    async def provider(r):
        assert r.query['udid'] == 'airplay-test-12345'
        assert r.cookies.get('session') == 'test-session'
        ws = web.WebSocketResponse(); await ws.prepare(r)
        session = Session(ws); await sessions.put(session)
        await session.read()
        return ws
    app=web.Application();app.router.add_post('/login',login);app.router.add_post('/admin/registerProvider',register);app.router.add_get('/provider/ws',provider)
    runner=web.AppRunner(app);await runner.setup();site=web.TCPSite(runner,'127.0.0.1',0);await site.start()
    port=site._server.sockets[0].getsockname()[1]
    with tempfile.TemporaryDirectory() as directory:
        config=str(pathlib.Path(directory)/'credentials.json')
        args=[binary,'--controlfloor-log-dir',str(pathlib.Path(directory)/'logs'),'--controlfloor-server',f'http://127.0.0.1:{port}','--controlfloor-config',config]
        env=dict(os.environ,CF_ADMIN_USERNAME='admin',CF_ADMIN_PASSWORD='test-password')
        p=await asyncio.create_subprocess_exec(*args,'--controlfloor-register',env=env,stdout=asyncio.subprocess.DEVNULL)
        assert await asyncio.wait_for(p.wait(),15)==0
        assert os.stat(config).st_mode & 0o777 == 0o600
        with open(pathlib.Path(directory)/'provider.log','w+') as output:
            p=await asyncio.create_subprocess_exec(*args,'--controlfloor-test-source',stdout=output,stderr=output)
            try:
                s=await asyncio.wait_for(sessions.get(),15)
                await asyncio.sleep(.2)
                assert (await s.command('ping'))['value']=='pong'
                assert (await s.command('getDeviceInformation'))['value']['name']=='synthetic-airplay-id'
                assert (await s.command('tap',{'x':1,'y':1}))['status']=='error'
                assert (await s.command('startAudio'))['status']=='error'
                response=await s.command('getCFAgentCapabilities')
                assert response['type']=='CFAgentCapabilities' and response['status']=='ok'
                caps=response['value']['capabilities']
                assert caps['tapTextCapable']==caps['tapTextInitialized']==caps['initializationState']==0
                assert caps['keyboards']==[]
                assert caps['video'] and not any(caps[key] for key in ('audio','input','nativeIOS'))
                for action in ('getStagedFileList','getCFAgentDeviceInformation','getSimulatedLocation'):
                    assert (await s.command(action))['status']=='error'
                if os.getenv('CF_SERVER_CHECKOUT'):
                    check=await asyncio.create_subprocess_exec('node',str(pathlib.Path(__file__).with_name('frontend-capabilities.cjs')),os.environ['CF_SERVER_CHECKOUT'],stdin=asyncio.subprocess.PIPE)
                    await check.communicate(json.dumps(response).encode())
                    assert check.returncode==0
                print('PASS nested capability contract and explicit unavailable startup services')
                # Match the HTML viewer's bootstrap: geometry is required before
                # it opens the image socket or sends startVideo.
                for attempt in range(30):
                    cfg=(await s.command('getVideoStreamConfiguration'))['value']
                    if cfg['nativeHeightPortrait']>0: break
                    await asyncio.sleep(.1)
                assert (cfg['nativeWidthPortrait'],cfg['nativeHeightPortrait'])==native,cfg
                device=(await s.command('getDeviceInformation'))['value']
                assert (device['logicalWidthPortrait'],device['logicalHeightPortrait'])==native
                assert (await s.command('getVideoStreamStatus'))['value']['enabled']==0
                debug=(await s.command('getVideoTransportDebug'))['value']
                assert debug['framesEncoded']==0 and debug['framesSent']==0,debug
                assert not await s.collect(.1)
                print('PASS viewer bootstrap: native dimensions without starting or encoding video')
                result=await s.command('startVideo',{'maxDimension':640,'jpegQuality':35,'maxFramesPerSecond':8,'flowControlVersion':2})
                assert result['status']=='ok',result
                frames=await s.collect(1.6)
                assert 6<=len(frames)<=16,len(frames)
                low_quality_table=Image.open(io.BytesIO(frames[-1][1])).quantization[0]
                for f,b in frames:
                    image=Image.open(io.BytesIO(b));image.load()
                    assert max(image.size)<=640,image.size
                    assert len(b)==f['binaryLength']==f['value']['size']
                print('PASS registration/cookies, ping/routing, unsupported input/audio, decoded JPEG dimensions, ACK streaming')
                if os.getenv('CF_TEST_GL'):
                    encoder=(await s.command('getVideoTransportDebug'))['value']['encoder']
                    assert 'memory:GLMemory' in encoder['rawCaps'],encoder
                    assert encoder['encodeFailures']==0,encoder
                    print('PASS GPU-backed NV12 frames copied to CPU JPEG encoder')
                assert (await s.command('setVideoStreamConfiguration',{'maxDimension':320,'jpegQuality':90,'clientWidthPortrait':150}))['status']=='ok'
                await s.collect(.3);frames=await s.collect(.5)
                assert frames
                assert sum(Image.open(io.BytesIO(frames[-1][1])).quantization[0])<sum(low_quality_table)
                for _,b in frames: assert Image.open(io.BytesIO(b)).width<=150
                cfg=(await s.command('getVideoStreamConfiguration'))['value']
                assert cfg['jpegQuality']==90 and cfg['maxDimension']==320
                assert (await s.command('setVideoStreamConfiguration',{'jpegQuality':101}))['status']=='error'
                assert (await s.command('getVideoStreamConfiguration'))['value']['jpegQuality']==90
                await s.command('setVideoStreamConfiguration',{'maxDimension':-2,'clientWidthPortrait':-1})
                assert (await s.command('getVideoStreamConfiguration'))['value']['maxDimension']==1280
                print('PASS live configuration, client dimensions, transactional validation and sentinel values')
                await s.command('stopVideo');await s.collect(.2)
                assert not await s.collect(.4)
                shot=await s.command('getScreenshot')
                assert shot['type']=='Screenshot' and shot['binaryLength']>100
                assert not await s.collect(.2)
                assert (await s.command('setVideoStreamConfiguration',{'vidstream':{'maxDimension':400,'burst':{'maxFramesPerSecond':10},'burstDuration':1},'maxVideoCPUPercentage':40}))['status']=='ok'
                assert (await s.command('getVideoStreamConfiguration'))['value']['maxDimension']==400
                assert (await s.command('setVideoStreamConfiguration',{'maxDimension':123.5}))['status']=='error'
                await s.command('resetVideoStreamConfiguration')
                print('PASS one-shot screenshot while stopped, nested Vidstream settings, config reset')
                s.acks=False;await s.command('startVideo',{'maxFramesPerSecond':30})
                frames=await s.collect(2.2)
                assert 2<=len(frames)<=6,len(frames)
                debug=(await s.command('getVideoTransportDebug'))['value']
                assert debug['framesEncoded']>debug['framesSent']
                assert debug['inFlightFrames']<=2
                # A stale-generation ACK cannot release the current window.
                before=debug['lastAckedFrameId']
                await s.command('ackVideoFrame',{'frameId':999999,'source':'UxPlay','sourceGeneration':'stale'})
                assert (await s.command('getVideoTransportDebug'))['value']['lastAckedFrameId']==before
                s.acks=True;await s.command('resetVideoTransport');assert len(await s.collect(.8))>=5
                print('PASS stop/resume, bounded missing-ACK probes, stale ACK rejection, transport reset')
                if not os.getenv('CF_TEST_STATIC'):
                    before=(await s.command('getVideoTransportDebug'))['value']['encoder']
                    frames=await s.collect(1.2)
                    after=(await s.command('getVideoTransportDebug'))['value']['encoder']
                    captured=after['capturedFrames']-before['capturedFrames']
                    assert captured>=28,(before,after)
                    assert after['encodeFailures']==0,after
                    assert len({body for _,body in frames})>=8,'Repeated cached frames hide a slow capture path'
                    print(f'PASS shared decoded-frame tap: {captured/1.2:.1f} captured FPS; changing JPEG content')
                await s.ws.close();s=await asyncio.wait_for(sessions.get(),15)
                await s.command('startVideo',{'maxDimension':480});assert await s.collect(.5)
                print('PASS authenticated reconnect and resumed video')
                await s.command('stopVideo')
            except BaseException:
                output.flush();output.seek(0);print(output.read(),file=sys.stderr)
                raise
            finally:
                p.send_signal(signal.SIGTERM)
                try: await asyncio.wait_for(p.wait(),15)
                except asyncio.TimeoutError: p.kill();await p.wait()
                output.seek(0);log=output.read()
                if p.returncode!=0: print(log,file=sys.stderr)
                assert p.returncode==0,p.returncode
        await runner.cleanup()

class Session:
    def __init__(self,ws):
        self.ws=ws;self.acks=True;self.counter=0;self.waiters={};self.frames=asyncio.Queue();self.notices=[]
    async def read(self):
        header=None
        async for message in self.ws:
            if message.type==WSMsgType.TEXT:
                assert header is None,'message interleaved between header and JPEG'
                j=json.loads(message.data)
                if j.get('binaryLength'): header=j;continue
                if isinstance(j.get('tag'),str) and j['tag'] in self.waiters:
                    waiter=self.waiters.pop(j['tag'])
                    if not waiter.done(): waiter.set_result(j)
                else:self.notices.append(j)
            elif message.type==WSMsgType.BINARY:
                assert header and header['type'] in ('VideoFrame','Screenshot')
                if header['type']=='Screenshot':
                    Image.open(io.BytesIO(message.data)).load()
                    waiter=self.waiters.pop(header['tag']);waiter.set_result(header)
                    header=None;continue
                await self.frames.put((header,message.data))
                if self.acks:
                    v=header['value']
                    await self.ws.send_json({'action':'ackVideoFrame','args':{'frameId':v['frameId'],'size':len(message.data),'source':v['source'],'sourceGeneration':v['sourceGeneration'],'flowControlVersion':2,'ackSummary':{'lastFrameId':v['frameId'],'framesReceived':1}}})
                header=None
    async def command(self,action,args=None):
        self.counter+=1;tag=str(self.counter);f=asyncio.get_running_loop().create_future();self.waiters[tag]=f
        request={'action':action,'tag':tag,'ack':1,'cf':{'s':self.counter,'c':47,'d':'airplay-test-12345'}}
        if args is not None:request['args']=args
        await self.ws.send_json(request)
        result=await asyncio.wait_for(f,5)
        assert result['cf']==request['cf'],result
        return result
    async def collect(self,seconds):
        result=[];deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            try:result.append(await asyncio.wait_for(self.frames.get(),max(.001,deadline-time.monotonic())))
            except asyncio.TimeoutError:break
        return result
if __name__=='__main__':asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
