#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run upstream-compatible receiver smoke tests with CF enabled but unadmitted.
Creates a unique test advertisement, uses dummy local credentials, no phone I/O.
"""
import json, pathlib, runpy, shlex, sys, tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
receiver = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[2]/'multidevice/tests/test_receiver.py'))
with tempfile.TemporaryDirectory(prefix='cf-receiver-') as temp:
    directory = pathlib.Path(temp)
    config = directory/'provider.json'
    config.write_text(json.dumps(dict(server='http://127.0.0.1:9', username='mock', password='mock')))
    wrapper = directory/'receiver'
    argv = [str(binary), '--controlfloor-provider', '--controlfloor-config', str(config),
            '--controlfloor-log-dir', str(directory/'logs')]
    wrapper.write_text('#!/bin/sh\nexec '+shlex.join(argv)+' "$@"\n')
    wrapper.chmod(0o700)
    receiver['main'](str(wrapper))
    receiver['worker_parent_eof'](str(wrapper))
    receiver['single'](str(wrapper))
    files = list((directory/'logs').glob('*.jsonl'))
    assert len(files) >= 2
    for path in files:
        events = [json.loads(line)['event'] for line in path.read_text().splitlines()]
        assert 'connection-open' in events and 'controlfloor-admitted' not in events
        assert path.stat().st_mode & 0o777 == 0o600
    print('PASS CF-enabled real receiver workers, parent EOF cleanup, private logs, and no premature admission')
