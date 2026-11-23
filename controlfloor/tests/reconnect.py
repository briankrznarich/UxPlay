#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bad credentials, rejected/closed WS and stalled CF command loop (~2 minutes).
Only localhost mocks; never registers an account or repairs credentials.
"""
import asyncio, json, pathlib, signal, sys, tempfile, time
from aiohttp import web
from integration import Session

async def main(binary):
    phase = 'login-rejected'
    logins, upgrades, opened = asyncio.Queue(), asyncio.Queue(), asyncio.Queue()
    login_times = []
    registrations = 0
    async def login(r):
        data = await r.post()
        assert data['username'] == 'ad-hoc' and data['password'] == 'invalid-in-new-db'
        login_times.append(time.monotonic()); await logins.put(phase)
        if phase == 'login-rejected': return web.Response(status=403)
        response = web.Response(text='ok'); response.set_cookie('session', 'test')
        return response
    async def register(r):
        nonlocal registrations
        registrations += 1
        return web.Response(status=500)
    async def provider(r):
        assert r.query['udid'] == 'airplay-test-12345'
        assert r.cookies.get('session') == 'test'
        mode = phase
        await upgrades.put(mode)
        if mode == 'upgrade-rejected': return web.Response(status=403)
        ws = web.WebSocketResponse(); await ws.prepare(r)
        session = Session(ws); await opened.put((mode, session))
        if mode == 'closed':
            await ws.close(code=1008, message=b'provider unavailable')
        else:
            await session.read()
        return ws
    app = web.Application()
    app.router.add_post('/login', login); app.router.add_post('/admin/registerProvider', register)
    app.router.add_get('/provider/ws', provider)
    runner = web.AppRunner(app); await runner.setup()
    site = web.TCPSite(runner, '127.0.0.1', 0); await site.start()
    port = site._server.sockets[0].getsockname()[1]
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        config = root/'credentials.json'
        config.write_text(json.dumps(dict(server=f'http://127.0.0.1:{port}', username='ad-hoc', password='invalid-in-new-db')))
        original = config.read_bytes()
        logs = root/'logs'
        with (root/'process.log').open('w+') as output:
            p = await asyncio.create_subprocess_exec(binary, '--controlfloor-test-source',
                '--controlfloor-config', str(config), '--controlfloor-log-dir', str(logs),
                stdout=output, stderr=output)
            async def get(queue, timeout=20): return await asyncio.wait_for(queue.get(), timeout)
            try:
                assert await get(logins) == 'login-rejected'
                assert await get(logins) == 'login-rejected'
                assert login_times[1] - login_times[0] >= 9.5
                assert upgrades.empty()
                print('PASS invalid credentials retry at ten seconds without opening a socket', flush=True)
                phase = 'upgrade-rejected'
                assert await get(upgrades) == phase
                assert await get(upgrades) == phase
                print('PASS provider WebSocket HTTP 403 retries', flush=True)
                phase = 'closed'
                mode, _ = await get(opened); assert mode == phase
                phase = 'silent'
                mode, _ = await get(opened); assert mode == phase
                print('PASS upgrade followed by remote close retries', flush=True)
                phase = 'active'
                mode, session = await get(opened, 75); assert mode == phase
                assert (await session.command('ping'))['value'] == 'pong'
                info = (await session.command('getDeviceInformation'))['value']
                assert info['udid'] == 'airplay-test-12345'
                assert info['name'] == 'synthetic-airplay-id'
                print('PASS silent command loop times out after 60s and reconnects with the same identity', flush=True)
                records = [json.loads(line) for path in logs.glob('*.jsonl') for line in path.read_text().splitlines()]
                for event in ('provider-connect-attempt', 'provider-connect-failed', 'provider-websocket-error',
                              'provider-websocket-close', 'provider-heartbeat-timeout'):
                    assert any(r['event'] == event for r in records), event
                assert any(r['event']=='provider-websocket-error' and r['detail']['httpStatus']==403 for r in records)
                assert any(r['event']=='provider-websocket-close' and r['detail']['code']==1008 for r in records)
                assert any(r['event']=='provider' and 'protocol active' in r['detail'] for r in records)
                assert sum(r['event']=='controlfloor-admitted' for r in records) == 1
                assert registrations == 0 and config.read_bytes() == original
                assert 'invalid-in-new-db' not in json.dumps(records)
                print('PASS diagnostic events, no repeated admission, no credential mutation/registration', flush=True)
            except BaseException:
                output.flush(); output.seek(0); print(output.read(), file=sys.stderr); raise
            finally:
                if p.returncode is None: p.send_signal(signal.SIGTERM)
                try: await asyncio.wait_for(p.wait(), 15)
                except asyncio.TimeoutError: p.kill(); await p.wait()
                assert p.returncode == 0, p.returncode
                await runner.cleanup()
if __name__ == '__main__': asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
