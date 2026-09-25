#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check a complete console readback, never host execution. With --before, the
readback of the driver before R66, whose loads past the bound range were not
checked."""
import json
from pathlib import Path
import sys
before = '--before' in sys.argv
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
values = [r for r in records if r.get('probe') == 'r66_uniform_bounds']
field = lambda name: [r['value'] for r in values if r.get('field') == name]
assert field('row') == [3, 17, 40, 63] * 2
assert field('pixels') == [8294400] * 2
if before:
    # Rows 17, 40 and 63 read their real colours past the 16 bound rows.
    assert field('band_mismatches') == [0, 2073600, 2073600, 2073600, 0, 0, 0, 0]
    assert field('center')[1:4] == ['0xffdfba45', '0xff615da2', '0xffe700ff']
    print('PASS (before R66): loads past the bound range read the rows beyond it')
else:
    assert field('band_mismatches') == [0] * 8 and field('mismatches') == [0, 0]
    assert field('center')[1:4] == ['0x0'] * 3, 'rows past the range read zero'
    assert field('passed_frames') == [2]
    assert not any(r.get('status') == 'FAIL' for r in records)
    print('PASS: rows past the bound range read zero; the whole range reads every row')
