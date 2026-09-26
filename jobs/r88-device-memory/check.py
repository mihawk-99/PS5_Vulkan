#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R88's console run: VkDeviceMemory outside the window by default, the
heap the whole pool, the memory budget, and the regression set."""
import json
from pathlib import Path
import sys

records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
GIB = 1 << 30


def values(probe):
    return {r['field']: r['value'] for r in records if r.get('probe') == probe and 'field' in r}


memory = values('device_report_memory')
assert memory['heap0_size'] == 12 * GIB, memory
extensions = [r['value'] for r in records if r.get('probe') == 'device_report_extension'
              and r.get('field') == 'device']
assert 'VK_EXT_memory_budget' in extensions, extensions

r88 = values('r88_memory')
assert r88['reported_heap_bytes'] == 12 * GIB
assert r88['allocation_refusal'] == -2  # VK_ERROR_OUT_OF_DEVICE_MEMORY
assert r88['allocated_bytes'] == 11 * GIB + 7 * 128 * (1 << 20)
# The budget's usage: what VkDeviceMemory holds, up by exactly the memory under
# test at the ceiling and back afterwards; the budget itself never above the heap.
assert r88['budget_usage_at_ceiling'] == r88['budget_usage_before'] + r88['allocated_bytes']
assert r88['budget_usage_after'] == r88['budget_usage_before']
assert r88['budget_before'] <= 12 * GIB and r88['budget_at_ceiling'] <= 12 * GIB
assert r88['budget_right'] == 1
assert r88['words_checked'] == r88['words_expected'] == r88['slices'] * (128 * (1 << 20) // 4)
assert r88['mismatches'] == 0 and r88['short_slices'] == 0
assert r88['outside_mappings'] == 18
assert r88['available_after'] == r88['available_before']

tests = {r['detail']: r['status'] for r in records if r.get('probe') == 'runner_test'}
assert len(tests) == 30 and set(tests.values()) == {'PASS'}, tests
summary = [r['detail'] for r in records if r.get('probe') == 'runner_summary' and 'detail' in r]
assert summary == ['30 of 30 queued tests passed'], summary
print(f"PASS: heap {memory['heap0_size']} bytes with VK_EXT_memory_budget; "
      f"{r88['allocated_bytes']} bytes through the default placement, "
      f"{r88['words_checked']} words written, inverted and compared by the GPU; "
      f"the budget's usage followed it exactly; 30 of 30 cases")
