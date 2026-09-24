#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay the r60-big capture through the host driver without tolerances."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path.cwd()
golden = root / 'golden/r60-big/run-1.json'
work = Path(tempfile.mkdtemp(prefix='r60-replay-', dir=root / 'build'))
case = 'r60-big'
directory = work / case
directory.mkdir()
queue = directory / 'queue.txt'
queue.write_text(f'capture\n{case}\n')
replay = directory / 'replay.txt'
dump = directory / 'submission.dump'
subprocess.run(['python3', 'tools/golden.py', 'replay', str(golden), str(replay),
                '--test', case], check=True)
with (directory / 'host.log').open('w') as out, (directory / 'host.err').open('w') as err:
    subprocess.run(['build/host/runner_host_driver', '--replay', str(replay),
                    '--memory', 'free', '--cases', 'driver', '--app0', str(root),
                    '--download0', str(directory), '--queue', str(queue)],
                   env={**os.environ, 'PS5_HOST_SUBMISSION_DUMP': str(dump)},
                   stdout=out, stderr=err, check=True)
subprocess.run(['python3', 'tools/golden.py', 'compare-run', str(golden), str(dump),
                '--test', case], check=True)
print('PASS: r60-big replays word for word')
