#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the complete console readback, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r63_skinned_record']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
assert field('band_mismatches') == [0] * 4
assert field('mismatches') == [0] and field('pixels') == [8294400]
assert sum(r.get('status') == 'PASS' for r in values) == 1
assert not any(r.get('status') == 'FAIL' for r in values)
print("PASS: Dolphin's 36-byte skinned record, four bands, every pixel")
