#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R83's console readbacks of D32_SFLOAT_S8_UINT's two aspects, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r83_depth_stencil']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
assert field('frame') == [0, 1]
# Depth frame: 0.375 of 255 (96) outside the quadrant, 0 in it. Stencil frame:
# 0 outside, 0x5a in it, alpha 1.
assert field('corner') == ['0xff000060', '0x1000000']
assert field('quadrant') == ['0xff000000', '0x100005a']
assert field('sample_mismatches') == [0, 0]
assert field('depth_readback_mismatches') == [0, 0]
assert field('stencil_readback_mismatches') == [0, 0]
assert field('pixels') == [8294400] * 2
assert field('passed_frames') == [2]
row = [r['detail'] for r in records if r.get('probe') == 'v0_formats_format'
       and r['detail'].startswith('D32_SFLOAT_S8_UINT:')]
assert row == ['D32_SFLOAT_S8_UINT: optimal 0x0000c201, buffer 0x00000000'], row
assert not any(r.get('status') == 'FAIL' for r in records)
print('PASS: depth and stencil aspects sampled and read back exact over 8294400 pixels')
