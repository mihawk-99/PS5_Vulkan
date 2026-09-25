#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R79's twelve complete console readbacks, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r79_lod_bias_range']
expected = ['0xff6e6e6e', '0xff323232', '0xff0a0a0a', '0xffbebebe', '0xffb4b4b4', '0xff3c3c3c',
            '0xff282828', '0xff0a0a0a', '0xffbebebe', '0xff0a0a0a', '0xffbebebe', '0xff6e6e6e']
assert [r['value'] for r in values if r.get('field') == 'frame'] == list(range(12))
assert [r['value'] for r in values if r.get('field') == 'expected'] == expected
assert [r['value'] for r in values if r.get('field') == 'center'] == expected
assert [r['value'] for r in values if r.get('field') == 'mismatches'] == [0] * 12
assert [r['value'] for r in values if r.get('field') == 'pixels'] == [8294400] * 12
assert [r['value'] for r in values if r.get('field') == 'passed_frames'] == [12]
assert sum(r.get('status') == 'PASS' for r in values) == 12
assert not any(r.get('status') == 'FAIL' for r in records)
print('PASS: twelve complete 8294400-pixel frames, biases -16 to +16, no mismatches')
