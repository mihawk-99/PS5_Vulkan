#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the A/B console readback (PID 435): the same eleven frames under four
ways of ending a restart draw, set per variant through the driver's A/B flags in
a diagnostic build that is not committed."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
variant, statuses = None, {}
for record in records:
    if record.get('field') == 'variant':
        variant = record['value']
    if record.get('event') == 'probe' and record.get('probe') == 'r64_restart_strips':
        statuses.setdefault(variant, []).append((record['status'], record['detail']))
bare = statuses['0x0']
assert [detail for status, detail in bare if status == 'PASS'] == [
    'the 2138-index strips joined by degenerate triangles, no restart'], 'only the control passes'
assert sum(status == 'FAIL' for status, _ in bare) == 10
for fixed in ('0x800', '0x1000', '0x2000'):  # no write, SQ_NON_EVENT first, VS_PARTIAL_FLUSH first
    assert [status for status, _ in statuses[fixed]] == ['PASS'] * 11, fixed
print('PASS: a bare write after the draw fails every restart frame; none, SQ_NON_EVENT '
      'or VS_PARTIAL_FLUSH before it passes all eleven')
