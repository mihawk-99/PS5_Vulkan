#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Require the complete target-flush regression battery and its readbacks."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import ps5vk_log

run = ps5vk_log.read_runs(sys.argv[1])[-1]
assert run['ended'], 'incomplete run'
assert not run['statuses'].get('FAIL'), run['statuses']
wanted = {'m2-solid', 'v0-two-passes', 'v0-subpass', 'c4-rtt',
          'v0-query-full', 'v0-timestamp-driver', 'c8-resolve'}
passed = {r['detail'] for r in run['tests'] if r['status'] == 'PASS'}
assert wanted <= passed, ('missing cases', wanted - passed)


def values(probe, field):
    return [r['value'] for r in run['records']
            if r.get('probe') == probe and r.get('field') == field]


assert values('agc_two_pass_shape', 'frames') == [40, 40, 40]
for field in ('differing_rows', 'differing_columns', 'differing_words', 'max_channel_error'):
    assert values('agc_resolve', field) == [0], field
for probe in ('agc_query_copy', 'agc_query_driver', 'agc_timestamp_driver', 'agc_c4_rtt'):
    assert any(r.get('probe') == probe and r.get('status') == 'PASS'
               for r in run['records']), probe
subprocess.run([sys.executable, str(ROOT / 'jobs/r20-subpass/check.py'), sys.argv[1]], check=True)
print(f"PID {run['pid']}: all seven cases, 120 repeated-pass frames and readbacks PASS")
