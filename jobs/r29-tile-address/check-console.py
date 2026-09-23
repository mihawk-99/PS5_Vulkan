#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the retained console pixel and regression evidence."""
import json
from pathlib import Path
root = Path(__file__).resolve().parent
for name, pid, tests in [('readback.txt', 267, 2), ('regression.txt', 268, 5)]:
    records = [json.loads(line) for line in (root / name).read_text().splitlines()]
    assert any(r.get('event') == 'run_start' and r.get('pid') == pid for r in records)
    assert not any(r.get('status') == 'FAIL' for r in records)
    assert sum(r.get('probe') == 'runner_test' and r.get('status') == 'PASS' for r in records) == tests
    assert any(r.get('probe') == 'runner_summary' and r.get('status') == 'PASS' for r in records)
    if name == 'readback.txt':
        frames = [r['value'] for r in records if r.get('probe') == 'agc_solid_readback' and r.get('field') == 'matching_pixels']
        assert frames == [8294400] * 5, frames  # Original solid plus four mip levels.
        assert sum(r.get('probe') == 'r16_mip_texels' and r.get('field') == 'mismatches' and r.get('value') == 0 for r in records) == 4
    print(f'PASS: PID {pid}, {tests} complete cases, no failed probes')
