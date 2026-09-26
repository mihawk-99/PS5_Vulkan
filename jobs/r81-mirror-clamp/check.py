#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R81's three complete console readbacks, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r81_mirror_clamp']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
assert field('frame') == [0, 1, 2]
# Clamp-to-edge, mirrored repeat, mirror-clamp-to-edge: the corner (texel -8)
# reads texel 0, texel 0 and texel 3; the left edge's middle row the same on U.
assert field('corner') == ['0xff802020', '0xff802020', '0xff80e0e0']
assert field('left_middle') == ['0xff80a020', '0xff80a0a0', '0xff80a0e0']
assert field('mismatches') == [0, 0, 0]
assert field('pixels') == [8294400] * 3
assert field('passed_frames') == [3]
assert not any(r.get('status') == 'FAIL' for r in records)
assert sum(r.get('probe') == 'runner_test' and r.get('status') == 'PASS' for r in records) == 3
print('PASS: clamp, mirrored repeat and mirror-clamp-to-edge, three 8294400-pixel frames exact')
