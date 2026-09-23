#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R19 packet evidence; --pixels also requires console readback."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ps5vk_log

run = ps5vk_log.read_runs(sys.argv[1])[-1]
assert run['ended'], 'incomplete run'
records = []
selected = False
for record in run['records']:
    if record.get('probe') == 'runner_test_start':
        selected = record.get('detail') == 'r19-index32'
    if selected:
        records.append(record)
packets = [r for r in records if r.get('probe') == 'r19_index_packets']
assert len(packets) == 3 and all(r['status'] == 'PASS' for r in packets), packets
if '--pixels' in sys.argv[2:]:
    pixels = [r for r in records if r.get('probe') == 'agc_solid_colour']
    assert len(pixels) == 3 and all(r['status'] == 'PASS' and r['detail'] ==
        '8294400 of 8294400 drawn pixels equal 0xffffffff' for r in pixels), pixels
    assert not any(r.get('status') == 'FAIL' for r in records), 'failed R19 record'
    print(f"PID {run['pid']}: three complete UINT32 pixel frames and packet checks PASS")
else:
    print('Three UINT32 packet/offset/bounds checks PASS; host pixels are not acceptance')
