#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Require depth-detachment pixel checks and existing depth/display regressions."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ps5vk_log
run = ps5vk_log.read_runs(sys.argv[1])[-1]
assert run['ended'], 'incomplete capture'
assert not run['statuses'].get('FAIL'), run['statuses']
passed = {r['detail'] for r in run['tests'] if r['status'] == 'PASS'}
assert {'m2-solid', 'c5-depth-detach', 'c5-depth-detach-16', 'c5-depth',
        'c5-depth-16', 'c5-depth-nodepth', 'c1-readback'} <= passed, passed
for field in ('colour_mismatching', 'depth_mismatching'):
    values = [r['value'] for r in run['records']
              if r.get('probe') == 'agc_c5_depth_readback' and r.get('field') == field]
    assert values == [0] * 5, (field, values)
values = [r['value'] for r in run['records']
          if r.get('probe') == 'agc_display_readback' and r.get('field') == 'mismatches']
assert values == [0] * 4, values
print(f"PID {run['pid']}: five colour and four depth checks and four swapchain copies PASS")
