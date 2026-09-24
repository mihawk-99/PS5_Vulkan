#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the complete console readback, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r62_uniform_index']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
assert field('frame') == [0, 1, 2]
assert field('mismatches') == [0, 0, 0] and field('pixels') == [8294400] * 3
assert field('band_mismatches') == [0] * 12
assert sum(r.get('status') == 'PASS' for r in values) == 3
assert not any(r.get('status') == 'FAIL' for r in values)
print('PASS: rows 3, 17, 40, 63, row 0 and row 1 read by an attribute index, every pixel')
