#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R84's console run: a 1.1 device, multiview and basic subgroups, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
report = {r['field']: r['value'] for r in records if r.get('probe') == 'device_report'
          and 'field' in r}
assert report['instance_api_version'] == report['api_version'] == (1 << 22) | (1 << 12)
multiview = [r for r in records if r.get('probe') == 'r84_multiview']
field = lambda rows, name: [r['value'] for r in rows if r.get('field') == name]
assert field(multiview, 'layer') == [0, 1, 2]
# Layer 0: view 0's red quad left, the clear right. Layer 1: the clear left,
# view 1's green quad right. Layer 2: the harness's zero, which no view writes.
assert field(multiview, 'left') == ['0xff0000ff', '0xffff8040', '0x0']
assert field(multiview, 'right') == ['0xffff8040', '0xff00ff00', '0x0']
assert field(multiview, 'mismatches') == [0, 0, 0]
assert field(multiview, 'pixels') == [8294400]
subgroup = [r for r in records if r.get('probe') == 'r84_subgroup']
assert field(subgroup, 'ran') == [32] and field(subgroup, 'failed') == [0]
compute = [r['value'] for r in records if r.get('probe') == 'd2_compute' and r.get('field') == 'readback']
assert compute == ['0xa5a5a5a5'] * 4, compute  # r84-subgroup's two, then d2-compute's two
tests = [(r['detail'], r['status']) for r in records if r.get('probe') == 'runner_test']
assert len(tests) == 8 and all(status == 'PASS' for _, status in tests), tests
assert not any(r.get('status') == 'FAIL' for r in records)
print('PASS: Vulkan 1.1 device; two views in their own layers, the third untouched; '
      '34 four-subgroup dispatches exact; 8 of 8 cases')
