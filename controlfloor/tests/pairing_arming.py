#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pairing arming against the real coordinator and mock HID, with no real phone."""
import asyncio
import json
import pathlib
import socket
import sys
import tempfile
import time
from aiohttp import ClientSession, web

async def main(binary):
    inventory = ['keyboard-A']
    calls = []
    mode = 'silent'
    request = None
    typed = ''
    base = ''
    process = None
    async with ClientSession() as http:
        async def fetch(method='GET'):
            async with http.request(method, base + '/shortcuts/next') as response:
                assert response.status == 200
                return await response.json() if method != 'HEAD' else None
        async def devices(_):
            return web.json_response({'bluetoothHosts': [dict(bluetoothHost=h, keyboard=True,
                consumerControls=True, liveMouse=True, mouse=dict(logicalWidth=375,
                logicalHeight=667, down=False, capture=dict(state='idle', error=None))) for h in inventory]})
        async def input_request(r):
            nonlocal request, typed
            body = await r.json()
            action = r.match_info['action']
            calls.append((action, body))
            if action == 'mouseUp':
                if mode == 'head':
                    await fetch('HEAD')
                elif mode == 'pair':
                    request = await fetch()
                    assert request['command'] == 'initiate-pairing', request
                elif mode == 'bound':
                    assert (await fetch())['command'] == 'ping'
            if action == 'type':
                typed = body['text']
            if action == 'typeSpecialKey' and body['key'] == 'Enter':
                assert mode == 'pair' and request and typed
                async with http.post(base + '/shortcuts/report', json=dict(protocolVersion=2,
                    shortcutVersion='test', status='ok', **request, pairingId=typed,
                    model='iPhone SE', iosVersion='26.6')) as response:
                    assert response.status == 200, await response.text()
            return web.json_response({'submitted': True, 'mouse': {'down': action == 'mouseDown'}})
        app = web.Application()
        app.router.add_get('/devices', devices)
        app.router.add_post('/{action}', input_request)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, '127.0.0.1', 0)
        await site.start()
        helper = f'http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}'
        async def snapshot():
            async with http.get(base + '/api/keyboards') as response:
                return await response.json()
        async def control(action, host='keyboard-A', **fields):
            state = await snapshot()
            async with http.post(base + '/api/keyboards/control', json=dict(id=host, action=action, **fields),
                headers={'X-UxPlay-Token': state['mutationToken']}) as response:
                assert response.status == 202, await response.text()
        async def wait_phase(phase, host='keyboard-A', start=0):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                state = await snapshot()
                for row in state['keyboards']:
                    if row['id'] == host and row['phase'] == phase and len(calls) > start:
                        return row
                await asyncio.sleep(.03)
            raise AssertionError((phase, state, calls[-8:]))
        def no_token(start):
            sent = calls[start:]
            assert any(a == 'mouseUp' for a, _ in sent), sent
            assert not any(a == 'type' or (a == 'typeSpecialKey' and b['key'] == 'Enter') for a, b in sent), sent
        with tempfile.TemporaryDirectory(prefix='pairing-arming-') as directory:
            with open(pathlib.Path(directory) / 'harness.log', 'w+') as log:
                try:
                    with socket.socket() as reservation:
                        reservation.bind(('127.0.0.1', 0))
                        port = reservation.getsockname()[1]
                    process = await asyncio.create_subprocess_exec(binary, helper, directory, str(port),
                        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=log)
                    first = json.loads(await asyncio.wait_for(process.stdout.readline(), 10))
                    base = f'http://127.0.0.1:{first["port"]}'
                    row = await wait_phase('failed')
                    assert 'not armed' in row['error'] and row['retryIn'] > 0
                    no_token(0)
                    await control('settings', autoConnect=False)
                    print('PASS no shortcut: Home/Home/M8, no typing or Enter, automatic retry scheduled')
                    mode = 'head'
                    start = len(calls)
                    await control('pair')
                    await wait_phase('failed', start=start)
                    no_token(start)
                    print('PASS HEAD and dashboard traffic do not arm typing')
                    request = await fetch()
                    assert request['command'] == 'initiate-pairing'
                    # An unbound ping response must not renew the original prompt's window.
                    await asyncio.sleep(6.15)
                    assert (await fetch())['command'] == 'ping'
                    mode = 'silent'
                    start = len(calls)
                    await control('pair')
                    await wait_phase('failed', start=start)
                    no_token(start)
                    print('PASS stale unpaired request and repeated ping do not arm typing')
                    await asyncio.sleep(2.1)  # Expire the mock's eight-second bootstrap lifetime.
                    mode = 'pair'
                    start = len(calls)
                    await control('pair')
                    row = await wait_phase('paired', start=start)
                    assert row['ip'] == '127.0.0.1' and row['bindingVerified']
                    assert typed == row['pairingCode']
                    print('PASS fresh unbound request permits the token and Enter; callback binds IP')
                    # Keep a paired phone requesting commands throughout expiry of the
                    # global arming window. Then discover a new unconfigured keyboard.
                    for _ in range(32):
                        assert (await fetch())['command'] == 'ping'
                        await asyncio.sleep(.2)
                    mode = 'bound'
                    start = len(calls)
                    inventory.append('keyboard-B')
                    row = await wait_phase('failed', host='keyboard-B', start=start)
                    assert 'not armed' in row['error']
                    no_token(start)
                    print('PASS bound-phone requests cannot arm a newly discovered keyboard')
                except BaseException:
                    log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
                    raise
                finally:
                    if process and process.returncode is None:
                        process.stdin.write(b'{"type":"quit"}\n')
                        await process.stdin.drain()
                        try:
                            await asyncio.wait_for(process.wait(), 10)
                        except asyncio.TimeoutError:
                            process.kill(); await process.wait()
                    await runner.cleanup()
                assert process.returncode == 0

if __name__ == '__main__':
    asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve())))
