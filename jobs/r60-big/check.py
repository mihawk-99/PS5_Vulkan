#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the complete console readback, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = {r['field']: r['value'] for r in records if r.get('probe') == 'r60_big_shader' and 'field' in r}
assert values['mismatches'] == 0 and values['pixels'] == 8294400 and values['center'] == '0xffbf8040'
assert any(r.get('probe') == 'r60_big_shader' and r.get('status') == 'PASS' for r in records)
print('PASS: the 32 KiB pixel stage drew (0.25, 0.5, 0.75, 1) on every pixel')
