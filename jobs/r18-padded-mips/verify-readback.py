#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Require every pixel of the three R18 hardware mip chains to match."""
import json
import re
import sys
from pathlib import Path

capture = json.loads(Path(sys.argv[1]).read_text())
assert capture['ended'] and not capture['statuses'].get('FAIL'), 'incomplete or failed run'
expected = {'r18-padded-mips': 8, 'r18-small-mips': 6, 'r18-aligned-mips': 5}
seen = {name: [] for name in expected}
passed = set()
test = None
level = None
for record in capture['records']:
    probe = record.get('probe')
    if probe == 'runner_test_start':
        test = record['detail']
        level = None
    elif probe == 'r18_mip' and record.get('field') == 'level':
        level = record['value']
    elif probe == 'agc_solid_colour' and test in expected:
        match = re.fullmatch(r'(\d+) of (\d+) drawn pixels equal (0x[0-9a-f]+)', record['detail'])
        assert match and record['status'] == 'PASS', (test, level, record)
        matched, total, colour = match.groups()
        assert int(matched) == int(total) == 8294400, (test, level, matched, total)
        assert int(colour, 16) == 0xff000000 | ((20 + level * 25) * 0x010101), (test, level)
        seen[test].append(level)
    elif probe == 'runner_test' and test in expected:
        assert record['status'] == 'PASS', test
        passed.add(test)
assert passed == set(expected), passed
for name, count in expected.items():
    assert seen[name] == list(range(count)), (name, seen[name])
print(f"PID {capture['pid']}: all 19 mip frames match all 8,294,400 pixels")
