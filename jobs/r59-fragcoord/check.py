#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the complete console readback, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = {r['field']: r['value'] for r in records if r.get('probe') == 'r59_frag_coord' and 'field' in r}
assert values['mismatches'] == 0 and values['pixels'] == 8294400
assert values['left'] == '0xff008040' and values['right'] == '0xff0080bf'
assert any(r.get('probe') == 'r59_frag_coord' and r.get('status') == 'PASS' for r in records)
print('PASS: gl_FragCoord.z ramps 0.25-0.75 and .w is 0.5 on every pixel')
