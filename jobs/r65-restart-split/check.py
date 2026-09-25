#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the complete console readback, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r65_restart_split']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
frames = 3
assert field('mismatches') == [0] * frames and field('pixels') == [8294400] * frames
assert field('missing') == [0] * frames and field('stray') == [0] * frames
assert field('cells') == [], 'a failing frame logs its cell map'
assert sum(r.get('status') == 'PASS' for r in values) == frames
assert field('passed_frames') == [frames]
assert not any(r.get('status') == 'FAIL' for r in records)
print('PASS: restart strips across a submission split, every pixel of three frames')
