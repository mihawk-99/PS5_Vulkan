#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R86's console run: the GPU reads and writes VkDeviceMemory outside the
4 GiB address window."""
import json
import re
from pathlib import Path
import sys

records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
events = lambda probe: [r for r in records if r.get('probe') == probe and r.get('event') == 'probe']

# The kernel: GPU-visible memory (protection 0x33) mapped at every hint, at the
# hint itself, recorded with the protection of the window mapping the GPU is
# known to use, and read back by the CPU.
maps = [r['detail'] for r in events('r86_wide_map') if r['detail'].startswith('hint ')]
assert len(maps) == 5, maps
control = re.match(r'hint 0x0: .* at 0x2[0-9a-f]{8} \(window 1, at hint 0\), query 0x00000000 '
                   r'protection 0x33, cpu 1$', maps[0])
assert control, maps[0]
for hint in ('0x408000000', '0x1000000000', '0x8000000000', '0x100000000'):
    line = next(m for m in maps if m.startswith(f'hint {hint}:'))
    assert f'at {hint} (window 0, at hint 1), query 0x00000000 protection 0x33, cpu 1' in line, line

# The GPU: each existing test passed with its VkDeviceMemory at 0x10_0000_0000
# and up, and really had memory there.
wide = [r['detail'] for r in events('r86_wide')]
expected = {'d2-compute': 2, 'd2-compute-images': 3, 'c2-indexed': 3, 'c4-texture': 5,
            'c4-rtt': 7, 'c5-depth': 4}
for name, count in expected.items():
    line = f'{name} passed with {count} VkDeviceMemory mappings outside the address window'
    assert line in wide, (name, wide)
assert all(r['status'] == 'PASS' for r in events('r86_wide'))

tests = {r['detail']: r['status'] for r in events('runner_test')}
assert len(tests) == 8 and set(tests.values()) == {'PASS'}, tests
summary = [r['detail'] for r in events('runner_summary')]
assert summary == ['8 of 8 queued tests passed'], summary
print('PASS: GPU-visible memory maps at 0x1_0000_0000, 0x4_0800_0000, 0x10_0000_0000 and '
      '0x80_0000_0000 as in the window; storage buffers, storage images, vertex and index '
      'buffers, a sampled texture, render to texture and a depth attachment all work from '
      '0x10_0000_0000; d2-compute passes back in the window')
