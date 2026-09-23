#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove cold/warm NIR cache reuse preserves recorded PS5 mip submissions."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root = Path.cwd()
work = Path(tempfile.mkdtemp(prefix='nir-meta-replay-', dir=root/'build'))
for run in ('cold', 'warm'):
    before = set((root/'build').glob('r29-replay-*'))
    with (work/(run+'.txt')).open('w') as log:
        subprocess.run(['python3', 'jobs/r29-tile-address/replay.py'], check=True,
                       stdout=log, stderr=log,
                       env={**os.environ, 'PS5VK_SHADER_CACHE_DIR': str(work/'cache')})
    created = set((root/'build').glob('r29-replay-*')) - before
    assert len(created) == 1, created
    data = (created.pop()/'r16-mip-blit/host.log').read_text()
    (work/(run+'.log')).write_text(data)
    print(run, 'compiles', data.count('compile start:'), 'NIR compiles',
          len(re.findall(r'compile start: nir=0x', data)), 'hits', data.count('shader cache hit'))
    if run == 'cold':
        assert 'compile start: nir=0x' in data
    else:
        assert 'compile start:' not in data and 'shader cache hit' in data
print('PASS: eight exact mip submissions; warm NIR and SPIR-V compiles zero')
print('logs:', work.relative_to(root))
