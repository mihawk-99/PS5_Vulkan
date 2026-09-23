#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Require all acquired swapchain copies and the original presentation regression."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ps5vk_log

run = ps5vk_log.read_runs(sys.argv[1])[-1]
assert run['ended'], 'incomplete capture'
assert not run['statuses'].get('FAIL'), run['statuses']
passed = {r['detail'] for r in run['tests'] if r['status'] == 'PASS'}
assert {'m2-solid', 'c1-triangle', 'c1-readback'} <= passed, passed
for field, expected in [('frame', [0, 1, 2, 3]), ('pixels', [8294400] * 4),
                        ('mismatches', [0] * 4)]:
    values = [r['value'] for r in run['records']
              if r.get('probe') == 'agc_display_readback' and r.get('field') == field]
    assert values == expected, (field, values)
print(f"PID {run['pid']}: four acquired images copied pixel-exactly and presented; regression PASS")
