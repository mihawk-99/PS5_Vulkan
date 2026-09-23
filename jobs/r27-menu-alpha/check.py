#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the four complete console readbacks, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r27_menu_alpha']
assert [r['value'] for r in values if r.get('field') == 'frame'] == list(range(4))
assert [r['value'] for r in values if r.get('field') == 'mismatches'] == [0] * 4
assert [r['value'] for r in values if r.get('field') == 'pixels'] == [8294400] * 4
assert [r['value'] for r in values if r.get('field') == 'passed_frames'] == [1, 3]
assert sum(r.get('status') == 'PASS' for r in values) == 4
assert not any(r.get('status') == 'FAIL' for r in records)
print('PASS: four complete 8294400-pixel alpha frames, no mismatches')
