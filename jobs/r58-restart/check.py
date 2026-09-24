#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the two complete console readbacks, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r58_restart']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
assert field('mismatches') == [0, 0]
assert field('pixels') == [8294400] * 2
assert field('gap') == ['0xffff8040'] * 2, 'the gap between the quads is the clear colour'
assert len(set(field('drawn'))) == 1 and field('drawn')[0] != '0xffff8040'
assert field('passed_frames') == [2]
assert not any(r.get('status') == 'FAIL' for r in records)
print('PASS: restarted strip and degenerate-joined strip identical, gap clear, every pixel')
