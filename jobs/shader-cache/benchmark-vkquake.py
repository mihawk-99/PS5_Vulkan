#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from PS5_vkQuake after its gates, scan and verified deployment:
# python3 ../PS5_Vulkan/jobs/shader-cache/benchmark-vkquake.py cold
# Repeat unchanged with warm. Each invocation owns one launch, listener,
# first-presentation timing, closure and two final trace reads.
import importlib.util, io, json, re, socket, sys, threading, time, hashlib
from pathlib import Path
root = Path.cwd()
sys.path.insert(0, str(root.parent / 'PS5_Vulkan/tools'))
import ps5_console as console
spec = importlib.util.spec_from_file_location('fetch_trace', root / 'tools/fetch-trace.py')
fetcher = importlib.util.module_from_spec(spec); spec.loader.exec_module(fetcher)
settings = console.load_settings(); ftp_settings = fetcher.deploy_tool().load_settings()
label = sys.argv[1]
assert label in ('cold', 'warm')
identity = re.search(r'build identity: ([a-f0-9]{64})', Path('build/title_build_identity.h').read_text())[1]
assert 'count=0' in console.ps5vkctl_command(settings, 'procs')
before = fetcher.fetch('PPSA99010', ftp_settings)
boot_count = before.count(b'build identity: ')
base = Path('klog/cache-' + label)
stop = threading.Event()
sock = socket.create_connection((settings['host'], settings['klog_port']), timeout=10)
sock.settimeout(.2)
log = open(str(base)+'.klog', 'wb')
def drain():
    while not stop.is_set():
        try: data = sock.recv(65536)
        except socket.timeout: continue
        except OSError: break
        if not data: break
        log.write(data); log.flush()
thread = threading.Thread(target=drain); thread.start()
time.sleep(2)
mark = log.tell()
started = time.monotonic(); presented = None; seen = None; launched = False
try:
    reply = console.ps5vkctl_command(settings, 'launch PPSA99010')
    launched = True
    assert 'ok launched' in reply and '0x80940010' not in reply, reply
    print('launched', label, 'identity', identity, flush=True)
    last_report = 0
    while time.monotonic() - started < 900:
        raw = fetcher.fetch('PPSA99010', ftp_settings)
        if raw.count(b'build identity: ') > boot_count:
            latest = raw.decode(errors='replace').rsplit('build identity: ', 1)[-1]
            assert latest.splitlines()[0].strip() == identity
            seen = seen or time.monotonic()
            compiles = latest.count('[ps5vk] compile done: result=0')
            hits = latest.count('[ps5vk] shader cache hit')
            stores = latest.count('[ps5vk] shader cache stored')
            if time.monotonic() - last_report > 25:
                print(f'{time.monotonic()-started:.1f}s compiles={compiles} hits={hits} stores={stores}', flush=True)
                last_report = time.monotonic()
            if 'vkQueuePresentKHR -> 0' in latest:
                presented = time.monotonic()
                break
            if 'assertion failed:' in latest or 'QUAKE ERROR:' in latest:
                raise RuntimeError('failed before presenting')
        time.sleep(1)
    assert presented, 'no presentation within 900s'
    print('presentation detected at', round(presented-started, 2), 'seconds', flush=True)
    time.sleep(2)
finally:
    if launched:
        status = console.ps5vkctl_command(settings, 'procs')
        if 'count=0' not in status:
            console.ps5vkctl_command(settings, 'kill PPSA99010')
            time.sleep(4)
    stop.set(); thread.join(timeout=3); sock.close(); log.close()
assert 'count=0' in console.ps5vkctl_command(settings, 'procs')
a = fetcher.fetch('PPSA99010', ftp_settings); b = fetcher.fetch('PPSA99010', ftp_settings)
Path(str(base)+'-a.txt').write_bytes(a); Path(str(base)+'-b.txt').write_bytes(b)
assert a == b
latest = a.decode(errors='replace').rsplit('build identity: ', 1)[-1]
assert latest.splitlines()[0].strip() == identity
kernel = Path(str(base)+'.klog').read_bytes()[mark:].decode(errors='replace')
pids = re.findall(r'<(\d+)> EXEC /app0/eboot.bin', kernel)
assert len(pids) == 1, pids
stats = dict(label=label, identity=identity, pid=int(pids[0]),
    launch_to_present_seconds=round(presented-started, 3), polling_interval_seconds=1,
    first_trace_seen_seconds=round(seen-started, 3),
    compiles=latest.count('[ps5vk] compile done: result=0'),
    cache_hits=latest.count('[ps5vk] shader cache hit'),
    cache_stores=latest.count('[ps5vk] shader cache stored'),
    cache_invalid=latest.count('[ps5vk] shader cache invalid'),
    spirv_compiles=len(re.findall(r'compile start: nir=0 words=', latest)),
    nir_compiles=len(re.findall(r'compile start: nir=(?!0 )', latest)),
    trace_sha256=hashlib.sha256(a).hexdigest(), two_reads_identical=True, console_idle=True,
    later_assertion='assertion failed:' in latest,
    recording_refusals=latest.count('recording refusal'))
Path(str(base)+'.json').write_text(json.dumps(stats, indent=2)+'\n')
print(json.dumps(stats), flush=True)
