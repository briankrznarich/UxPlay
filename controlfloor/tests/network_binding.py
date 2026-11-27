#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real listener/discovery smoke test. Uses fake credentials and no phone/HID.
Usage: network_binding.py build/controlfloor/uxplay 192.168.1.13 en0
Briefly advertises a uniquely named receiver on the supplied LAN interface.
"""
import ctypes
import json
import pathlib
import queue
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'multidevice/tests'))
from test_receiver import info

binary, address, interface = sys.argv[1:4]
binary = str(pathlib.Path(binary).resolve())
name = 'UxPlay-bind-test-' + uuid.uuid4().hex[:8]
observed = []
dns = None
browse = ctypes.c_void_p()
# macOS Bonjour: verify actual advertised interface, not just the code setting.
if sys.platform == 'darwin':
    dns = ctypes.CDLL('/usr/lib/libSystem.B.dylib')
    callback_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_uint32, ctypes.c_int32, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_char_p, ctypes.c_void_p)
    @callback_type
    def found(ref, flags, index, error, service, regtype, domain, context):
        if not error and flags & 2 and service.decode() == name:
            observed.append(index)
    dns.DNSServiceBrowse.argtypes = [ctypes.POINTER(ctypes.c_void_p),ctypes.c_uint32,
        ctypes.c_uint32,ctypes.c_char_p,ctypes.c_char_p,callback_type,ctypes.c_void_p]
    dns.DNSServiceRefSockFD.argtypes = [ctypes.c_void_p]
    dns.DNSServiceProcessResult.argtypes = [ctypes.c_void_p]
    dns.DNSServiceRefDeallocate.argtypes = [ctypes.c_void_p]
    assert dns.DNSServiceBrowse(ctypes.byref(browse),0,0,b'_airplay._tcp',None,found,None)==0

def refused(host, port):
    try:
        with socket.create_connection((host,port),timeout=1): pass
    except OSError:
        return
    raise AssertionError(f'Unexpected listener at {host}:{port}')

try:
    with tempfile.TemporaryDirectory(prefix='uxplay-bind-') as tmp:
        root = pathlib.Path(tmp)
        (root/'network.conf').write_text(f'bind {address}\n')
        (root/'provider.json').write_text(json.dumps(dict(server='http://127.0.0.1:9',username='mock',password='mock')))
        with socket.socket() as reservation:
            reservation.bind((address,0)); http_port=reservation.getsockname()[1]
        arguments=[binary,'-rc',str(root/'network.conf'),'-multi','2','--controlfloor-lan',
            '--controlfloor-config',str(root/'provider.json'),'--controlfloor-mouse-url','http://127.0.0.1:9',
            '--controlfloor-shortcut-port',str(http_port),'--controlfloor-log-dir',str(root/'logs'),
            '-n',name,'-nh','-m','02:fa:ab:12:34:56','-vs','fakesink','-as','0','-vsync','no']
        process=subprocess.Popen(arguments,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        lines=queue.Queue(); transcript=[]
        def read():
            for line in process.stdout:
                transcript.append(line); lines.put(line)
        reader=threading.Thread(target=read,daemon=True);reader.start()
        try:
            deadline=time.monotonic()+15
            while True:
                line=lines.get(timeout=max(.1,deadline-time.monotonic()))
                if 'Multi-device receiver listening on port ' in line:
                    port=int(line.split('port ')[1].split(',')[0]);break
                assert time.monotonic()<deadline
            assert any(f'Network restricted to {address} on {interface}' in line for line in transcript)
            opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open(f'http://{address}:{http_port}/health',timeout=3) as response:
                assert json.load(response)['service']=='UxPlay ControlFloor LAN'
            refused('127.0.0.1',http_port);refused('::1',http_port)
            refused('127.0.0.1',port);refused('::1',port)
            print('PASS configured LAN HTTP/AirPlay listeners; no wildcard/loopback/IPv6 exposure')
            with socket.create_connection((address,port),timeout=10) as peer:
                peer.settimeout(10)
                assert info(peer)['name']==name
            print('PASS LAN AirPlay control reaches the worker through its private loopback listener')
            if dns:
                fd=dns.DNSServiceRefSockFD(browse)
                deadline=time.monotonic()+8
                while time.monotonic()<deadline:
                    if select.select([fd],[],[],.2)[0]:
                        assert dns.DNSServiceProcessResult(browse)==0
                    if observed and time.monotonic()>deadline-5: break
                assert observed and set(observed)=={socket.if_nametoindex(interface)},observed
                print('PASS Bonjour advertises only on the configured LAN interface',interface)
        except BaseException:
            print(''.join(transcript),file=sys.stderr);raise
        finally:
            process.send_signal(signal.SIGTERM)
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait();raise
            reader.join(timeout=2);process.stdout.close()
        assert process.returncode==0
        for target in ['192.0.2.254','does-not-exist','0.0.0.0','::1']:
            # Must fail before creating any listener or silently choosing another address.
            (root/'network.conf').write_text(f'bind {target}\n')
            result=subprocess.run([binary,'-rc',str(root/'network.conf')],capture_output=True,text=True,timeout=10)
            assert result.returncode!=0 and 'not an IPv4 address/interface' in result.stderr,(target,result.stderr)
        print('PASS unavailable/invalid address or interface fails closed')
finally:
    if dns and browse.value:dns.DNSServiceRefDeallocate(browse)
