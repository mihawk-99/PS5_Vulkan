#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay both presentation paths and compare every captured command word."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path.cwd()
golden = root / 'golden/r23-display-readback/run-1.json'
work = Path(tempfile.mkdtemp(prefix='r23-replay-', dir=root / 'build'))
print(work, flush=True)
for case in ('c1-triangle', 'c1-readback'):
    directory = work / case
    directory.mkdir()
    queue = directory / 'queue.txt'
    queue.write_text(f'capture\n{case}\n')
    replay = directory / 'replay.txt'
    dump = directory / 'submission.dump'
    subprocess.run(['python3', 'tools/golden.py', 'replay', str(golden), str(replay),
                    '--test', case], check=True)
    # driver_replay_text starts a fresh flip counter. This battery has earlier
    # flips (including the helper probe), so seed the host model from the first
    # captured flip's existing RELEASE_MEM counter encoding. Goldens stay intact.
    run = json.loads(golden.read_text())
    first = next(x for x in run['submissions']
                 if x['test'] == case and x['label'].endswith('flip'))
    capture = json.loads((golden.parent / first['file']).read_text())
    counter = int(capture['words'][17], 16)
    assert 0x08000101 <= counter <= 0x080001ff, hex(counter)
    seed = counter - 0x08000101
    replay.write_text(replay.read_text().replace('flips 0\n', f'flips {seed}\n', 1))
    print(f'{case}: captured flip counter seed {seed}', flush=True)
    with (directory / 'host.log').open('w') as out, (directory / 'host.err').open('w') as err:
        subprocess.run(['build/host/runner_host_driver', '--replay', str(replay),
                        '--memory', 'free', '--cases', 'driver', '--app0', str(root),
                        '--download0', str(directory), '--queue', str(queue)],
                       env={**os.environ, 'PS5_HOST_SUBMISSION_DUMP': str(dump)},
                       stdout=out, stderr=err, check=True)
    subprocess.run(['python3', 'tools/golden.py', 'compare-run', str(golden), str(dump),
                    '--test', case], check=True)
