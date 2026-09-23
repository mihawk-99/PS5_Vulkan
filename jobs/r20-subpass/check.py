#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Require measured full-frame writer and reader results for both subpass frames."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import ps5vk_log

run = ps5vk_log.read_runs(sys.argv[1])[-1]
assert run['ended'], 'incomplete capture'
assert not run['statuses'].get('FAIL'), run['statuses']
for field in ('writer_samples', 'reader_samples', 'of'):
    values = [r['value'] for r in run['records']
              if r.get('probe') == 'agc_subpass' and r.get('field') == field]
    assert values == [8294400, 8294400], (field, values)
assert any(r.get('probe') == 'b7_map_subpass_memory' and r.get('status') == 'PASS'
           for r in run['records']), 'writer memory was not mapped'
print(f"PID {run['pid']}: both writers and readers match every pixel in two frames")
