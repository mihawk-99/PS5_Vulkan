#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Strictly replay the R25 depth cases, including the formerly missing reset."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root = Path.cwd()
work = Path(tempfile.mkdtemp(prefix='r25-replay-', dir=root / 'build'))
print(work, flush=True)
cases = ('c5-depth-detach', 'c5-depth-detach-16', 'c5-depth',
         'c5-depth-16', 'c5-depth-nodepth', 'c1-readback', 'v0-stencil', 'v0-depth-bias')
selected = sys.argv[1:] or cases
assert all(case in cases for case in selected)
for case in selected:
    golden = root / ('golden/r25-depth-stencil/run-1.json' if case.startswith('v0-')
                     else 'golden/r25-depth-detach/run-1.json')
    directory = work / case
    directory.mkdir()
    queue = directory / 'queue.txt'
    queue.write_text(f'capture\n{case}\n')
    replay = directory / 'replay.txt'
    dump = directory / 'submission.dump'
    if case.startswith('v0-'):
        # PID 245 recorded driver tables, but no native defaults canary. Reuse
        # PID 244's same-build m2-solid defaults as host input; comparisons of
        # PID 245's complete command words and register tables remain strict.
        sys.path.insert(0, str(root / 'tools'))
        import golden as golden_tool
        defaults = golden_tool.register_defaults(json.loads(
            (root / 'golden/r25-depth-detach/m2-solid-1.json').read_text()))
        assert defaults is not None
        replay.write_text(golden_tool.driver_replay_text(json.loads(golden.read_text()),
                                                         defaults, case))
    else:
        subprocess.run(['python3', 'tools/golden.py', 'replay', str(golden), str(replay),
                        '--test', case], check=True)
    if case == 'c1-readback':
        run = json.loads(golden.read_text())
        first = next(x for x in run['submissions']
                     if x['test'] == case and x['label'].endswith('flip'))
        capture = json.loads((golden.parent / first['file']).read_text())
        counter = int(capture['words'][17], 16)
        assert 0x08000101 <= counter <= 0x080001ff
        replay.write_text(replay.read_text().replace('flips 0\n',
                          f'flips {counter - 0x08000101}\n', 1))
    with (directory / 'host.log').open('w') as out, (directory / 'host.err').open('w') as err:
        subprocess.run(['build/host/runner_host_driver', '--replay', str(replay),
                        '--memory', 'free', '--cases', 'driver', '--app0', str(root),
                        '--download0', str(directory), '--queue', str(queue)],
                       env={**os.environ, 'PS5_HOST_SUBMISSION_DUMP': str(dump)},
                       stdout=out, stderr=err, check=True)
    subprocess.run(['python3', 'tools/golden.py', 'compare-run', str(golden), str(dump),
                    '--test', case], check=True)
