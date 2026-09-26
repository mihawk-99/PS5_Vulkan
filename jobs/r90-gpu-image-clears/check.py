#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R90's console runs: attachments cleared on the GPU, read back exactly at
a split point, and the two first runs that found what the step end and the
one- and eight-byte maps owed."""
import json
from pathlib import Path
import sys

here = Path(__file__).parent


def records(name):
    return [json.loads(line) for line in (here / name).read_text().splitlines()]


def checks(rows):
    return {r['detail'].split(':')[0]: (r['status'], r['detail'])
            for r in rows if r.get('probe') == 'r90_clear_check'}


final = records(sys.argv[1] if len(sys.argv) > 1 else 'readback.txt')
tests = {r['detail']: r['status'] for r in final if r.get('probe') == 'runner_test'}
assert tests == {name: 'PASS' for name in ('r90-image-clears', 'c7-clear', 'c5-depth-clear',
                                           'v0-stencil-clear', 'v0-colour-clear', 'c1-clear')}, tests
final_checks = checks(final)
assert len(final_checks) == 24, len(final_checks)
for name, (status, detail) in final_checks.items():
    assert status == 'PASS' and detail.endswith('227688 of 227688 texels'), detail
assert [r['detail'] for r in final if r.get('probe') == 'r90_clears'] == \
    ['24 of 24 GPU image clear checks passed']
tiled = [r for r in final if r.get('probe') == 'c7_clear_check' and 'tiled_clear' in r['detail']]
assert [r['status'] for r in tiled] == ['PASS'], tiled

# PID 230, the step's completion at bottom of pipe: a fifth of a depth clear
# read stale, and the depth planes of D32_SFLOAT_S8_UINT with it.
first = checks(records('readback-pid230.txt'))
assert first['D32_SFLOAT clear 2'][0] == 'FAIL' and '46269 of 227688' in first['D32_SFLOAT clear 2'][1]
assert first['depth-only clear, depth 0.75'][0] == 'FAIL'
assert first['clear of both, depth 0.25'][0] == 'FAIL'
# PID 231, CACHE_FLUSH_AND_INV_TS_EVENT at the step's end: every depth and
# stencil check exact, and the one- and eight-byte colour formats as they were --
# the same texels in both runs, so a map rather than a cache.
second = checks(records('readback-pid231.txt'))
for name in ('D32_SFLOAT clear 1', 'D32_SFLOAT clear 2', 'depth-only clear, depth 0.75',
             'depth-only clear, stencil kept', 'stencil-only clear, depth kept',
             'stencil-only clear, stencil 0x33', 'clear of both, depth 0.25',
             'clear of both, stencil 0x7e'):
    assert second[name][0] == 'PASS', second[name]
for name in ('R16G16B16A16_UNORM clear 1', 'R16G16B16A16_UNORM clear 2',
             'R16G16B16A16_SFLOAT clear 1', 'R16G16B16A16_SFLOAT clear 2'):
    assert first[name] == second[name] and '664 of 227688 texels differ, first (604,64)' in \
        second[name][1], second[name]
for name in ('R8_UNORM clear 1', 'R8_UNORM clear 2'):
    assert first[name] == second[name] and 'first (8,356)' in second[name][1], second[name]
regression = records('regression/readback.txt')
regression_tests = [r for r in regression if r.get('probe') == 'runner_test']
assert len(regression_tests) == 29 and all(r['status'] == 'PASS' for r in regression_tests), \
    [r['detail'] for r in regression_tests if r['status'] != 'PASS']
print('PASS: 24 of 24 GPU clears read back exactly at a split point (PID 233); the depth '
      'clears PID 230 read stale are exact with the step ending in CACHE_FLUSH_AND_INV_TS_EVENT '
      '(PID 231); the one- and eight-byte formats miss the same texels in both, a map for R91; 29 of 29 existing cases pass on the new step end (PID 235)')
