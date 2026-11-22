#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real OCR -> decoded-frame tap -> CF admission, with a localhost server.

Requires aiohttp/Pillow. Optional args: keyboard.png about.png (private fixtures
are read in place, never copied into the repository). Otherwise creates fake IDs.
"""
import asyncio, contextlib, io, json, pathlib, sys, tempfile, time
from aiohttp import web
from PIL import Image, ImageDraw, ImageFont
from integration import Session

async def main(binary, fixtures):
    sessions = asyncio.Queue()
    async def login(request):
        response = web.Response(text='ok')
        response.set_cookie('session', 'fixture')
        return response
    async def provider(request):
        ws = web.WebSocketResponse(); await ws.prepare(request)
        session = Session(ws)
        await sessions.put((request.query['udid'], session))
        await session.read()
        return ws
    app = web.Application()
    app.router.add_post('/login', login); app.router.add_get('/provider/ws', provider)
    runner = web.AppRunner(app); await runner.setup()
    site = web.TCPSite(runner, '127.0.0.1', 0); await site.start()
    port = site._server.sockets[0].getsockname()[1]
    children = []
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        config = root/'provider.json'
        config.write_text(json.dumps(dict(server=f'http://127.0.0.1:{port}', username='mock', password='mock')))
        logs = root/'logs'
        # Keep fixtures distributable: these are made-up IDs, not phone screenshots.
        try: font = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', 24)
        except OSError: font = ImageFont.truetype('DejaVuSans.ttf', 24)
        def picture(name, lines):
            image = Image.new('RGB', (650, 700), 'white'); draw = ImageDraw.Draw(image)
            for row, text in enumerate(lines): draw.text((25, 40 + row*60), text, fill='black', font=font)
            path = root/(name+'.png'); image.save(path); return path
        marker = picture('keyboard', ['Keyboard identifier (display only)', 'X=4523452345234=X'])
        about = picture('about', ['About', 'Name                Fixture phone', 'iOS Version         26.6',
            'Model Name          iPhone SE', 'Model Number        MX9U2J/A', 'Serial Number       TEST23456789'])
        if fixtures: marker, about = map(pathlib.Path, fixtures)
        async def probe(path):
            p = await asyncio.create_subprocess_exec(binary, '--ocr-file', str(path), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
            out, _ = await p.communicate(); assert p.returncode == 0
            return json.loads(out)
        keyboard = (await probe(marker))['keyboardId']; identity = await probe(about)
        assert keyboard and identity['serialNumber'] and identity['name'], (keyboard, identity)
        udid = 'airplay-'+identity['serialNumber']
        async def launch():
            output = (root/f'worker-{len(children)}.stderr').open('w+')
            p = await asyncio.create_subprocess_exec(binary, '--controlfloor-provider',
                '--controlfloor-config', str(config), '--controlfloor-log-dir', str(logs),
                '--controlfloor-ocr-interval', '.2', stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=output)
            children.append((p, output))
            assert await asyncio.wait_for(p.stdout.readline(), 15) == b'ready\n'
            return p
        async def command(p, action, **args):
            p.stdin.write((json.dumps(dict(action=action, **args))+'\n').encode()); await p.stdin.drain()
            if action != 'quit': assert await asyncio.wait_for(p.stdout.readline(), 15) == b'ok\n'
        def records(prefix):
            return [json.loads(line) for path in logs.glob(prefix+'*.jsonl') for line in path.read_text().splitlines()]
        async def wait_event(prefix, event, count=1):
            deadline = time.monotonic()+20
            while time.monotonic() < deadline:
                if sum(r['event'] == event for r in records(prefix)) >= count: return
                await asyncio.sleep(.05)
            raise AssertionError((event, records(prefix)))
        async def absent(seconds=.5):
            await asyncio.sleep(seconds); assert sessions.empty(), 'unidentified/duplicate device connected'
        try:
            p = await launch()
            await command(p, 'connect', id='22:70:AA:BB:CC:01')
            await command(p, 'frame', path=str(about))
            await wait_event('22_70', 'ocr'); await absent()
            print('PASS About page alone cannot bypass the keyboard gate')
            await command(p, 'frame', path=str(marker))
            await wait_event('22_70', 'keyboard-identified'); await absent()
            await command(p, 'frame', path=str(about))
            got, session = await asyncio.wait_for(sessions.get(), 20)
            assert got == udid
            info = (await session.command('getDeviceInformation'))['value']
            assert info['udid'] == udid and info['keyboardId'] == keyboard
            assert info['name'] == identity['name'] and info['serialNumber'] == identity['serialNumber']
            assert info['hardwareModelName'] == identity['modelName']
            await session.command('startVideo', {'maxDimension':400,'flowControlVersion':2})
            frames = await session.collect(.8); assert len(frames) >= 3
            image = Image.open(io.BytesIO(frames[0][1])); assert max(image.size) <= 400
            assert any(r['event']=='identity-candidate' for r in records('22_70'))
            print('PASS keyboard + two About reads admit serial ID; video/config/ACK work through the real frame tap')
            # A second receiver must not steal the same server device identity.
            p2 = await launch(); await command(p2, 'connect', id='22:70:AA:BB:CC:02')
            await command(p2, 'frame', path=str(marker)); await wait_event('22_70_AA_BB_CC_02', 'keyboard-identified')
            await command(p2, 'frame', path=str(about)); await wait_event('22_70_AA_BB_CC_02', 'device-identified')
            await absent()
            await command(p, 'disconnect')
            got2, second = await asyncio.wait_for(sessions.get(), 15); assert got2 == udid
            assert (await second.command('ping'))['value'] == 'pong'
            await command(p2, 'disconnect'); await asyncio.sleep(.2)
            print('PASS duplicate serial lease, disconnect, and release to another worker')
            # Reusing the same AirPlay address must repeat identification from scratch.
            await command(p, 'connect', id='22:70:AA:BB:CC:01')
            await command(p, 'frame', path=str(about)); await absent(.8)
            await command(p, 'frame', path=str(marker)); await wait_event('22_70_AA_BB_CC_01', 'keyboard-identified', 2)
            # Disconnect while an OCR result may be in flight. It cannot admit the next connection.
            await command(p, 'frame', path=str(about)); await command(p, 'disconnect')
            await command(p, 'connect', id='22:70:AA:BB:CC:03')
            await command(p, 'frame', path=str(about)); await absent(1.5)
            assert len(list(logs.glob('22_70_AA_BB_CC_01-*.jsonl'))) == 2
            print('PASS reconnect requires new keyboard gate; old OCR results cannot register a replacement; logs are per connection')
        finally:
            for p, output in children:
                if p.returncode is None:
                    with contextlib.suppress(Exception): await command(p, 'quit')
                    try: await asyncio.wait_for(p.wait(), 15)
                    except asyncio.TimeoutError: p.kill(); await p.wait()
                if p.returncode:
                    output.seek(0); print(output.read(), file=sys.stderr)
                output.close()
            await runner.cleanup()
        assert all(p.returncode == 0 for p, _ in children)

if __name__ == '__main__':
    asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve()), sys.argv[2:]))
