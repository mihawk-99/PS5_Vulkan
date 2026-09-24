#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the complete console readback, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r61_one_layer_array']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
assert field('mismatches') == [0, 0] and field('center') == ['0xffc08040'] * 2
assert sum(r.get('status') == 'PASS' for r in values) == 2
print('PASS: a one-layer 2D-array view samples its layer, linear and tiled, every pixel')
