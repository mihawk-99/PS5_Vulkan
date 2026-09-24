#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the five complete console readbacks, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r57_border']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
assert field('frame') == list(range(5))
assert field('mismatches') == [0] * 5
assert field('pixels') == [8294400] * 5
assert field('edge_pixels_skipped') == [0] + [512160] * 4
assert field('expected_outside') == field('corner'), 'the border colour is the corner pixel'
assert field('passed_frames') == [5]
assert sum(r.get('status') == 'PASS' for r in values) == 5
assert not any(r.get('status') == 'FAIL' for r in records)
print('PASS: control and four border frames exact outside the 1/16-texel edge band')
