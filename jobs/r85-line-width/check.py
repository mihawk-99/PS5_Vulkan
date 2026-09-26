#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R85's console run: a line width set by the draw draws the static frame."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
status = {r['probe']: r['status'] for r in records if r.get('event') == 'probe' and 'status' in r}
assert status['agc_lines_dynamic_width'] == 'PASS'
assert status['agc_lines_unculled'] == 'PASS' and status['agc_lines_culled'] == 'PASS'
frames = [r['detail'] for r in records if r.get('probe') == 'agc_lines' and r.get('status') == 'INFO']
assert frames[-1] == 'lines, width 1.0 set by the draw', frames
hashes = [r['value'] for r in records if r.get('field') == 'frame_fnv1a64']
assert len(hashes) == 5 and hashes[4] == hashes[0], hashes
# R8's reference (lines against rectangles) is the case's standing open item:
# both draw the reference's 2460 pixels elsewhere in the frame, since its first
# console run (docs/M5_PHASE_C.md). It is not what this round measures.
assert status['agc_lines_reference'] == 'FAIL'
print('PASS: the width set by the draw draws the static line frame word for word')
