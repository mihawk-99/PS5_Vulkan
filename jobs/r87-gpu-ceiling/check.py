#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R87's console run: the GPU memory ceiling is the direct-memory pool."""
import json
from pathlib import Path
import sys

records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
MIB, GIB = 1 << 20, 1 << 30


def values(probe):
    return {r['field']: r['value'] for r in records if r.get('probe') == probe and 'field' in r}


kernel = values('r87_ceiling_map')
assert kernel['direct_size'] == 12 * GIB
# GPU-visible memory maps until the pool is spent: less than one 2 MiB piece
# is left, every piece at its address from 0x40_0000_0000, none in the window,
# and everything comes back.
assert kernel['available_before'] - kernel['mapped_bytes'] == kernel['available_while_held']
assert kernel['available_while_held'] < 2 * MIB, kernel
assert kernel['pieces'] == kernel['at_hint'] and kernel['in_window'] == 0
assert kernel['cpu_words_right'] == kernel['cpu_words_checked'] > 0
assert kernel['available_after'] == kernel['available_before']

vk = values('r87_ceiling')
assert vk['reported_heap_bytes'] == 4 * GIB  # what the device reports before the change
assert vk['allocation_refusal'] == -2  # VK_ERROR_OUT_OF_DEVICE_MEMORY
assert vk['allocated_bytes'] == 11 * GIB + 7 * 128 * MIB, vk['allocated_bytes']
assert vk['available_at_ceiling'] < 128 * MIB
assert vk['tested_bytes'] == vk['slices'] * 128 * MIB and vk['tested_bytes'] > 11 * GIB
assert vk['words_checked'] == vk['words_expected'] == vk['slices'] * (128 * MIB // 4)
# first_bad_slice is UINT32_MAX when no slice differed (the runner logs the
# unsigned value).
assert vk['mismatches'] == 0 and vk['short_slices'] == 0 and vk['first_bad_slice'] == 0xffffffff
assert vk['outside_mappings'] == 18  # 11 of 1 GiB and 7 of 128 MiB
assert vk['available_after'] == vk['available_before']

tests = {r['detail']: r['status'] for r in records if r.get('probe') == 'runner_test'}
assert tests == {'r87-ceiling-map': 'PASS', 'r87-ceiling': 'PASS', 'd2-compute': 'PASS'}, tests
print(f"PASS: {kernel['mapped_bytes']} bytes GPU-visible (the pool, {kernel['available_while_held']} "
      f"left); vkAllocateMemory {vk['allocated_bytes']} bytes; the GPU wrote, inverted and "
      f"compared {vk['words_checked']} words ({vk['tested_bytes']} bytes), none differed")
