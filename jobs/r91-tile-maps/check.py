#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R91's console runs: every tiled colour map is the hardware's, the
eight-byte one after its twist, and R90's clears read back exactly in all ten
formats once the one-byte copy moves eight bytes a run."""
import json
from pathlib import Path
import sys

here = Path(__file__).parent


def records(name):
    return [json.loads(line) for line in (here / name).read_text().splitlines()]


def statuses(rows, probe):
    return {r['detail'].split(':')[0]: (r['status'], r['detail'])
            for r in rows if r.get('probe') == probe and r.get('event') == 'probe'}


TEXELS = '8294400 of 8294400 texels'

# PID 237, the map as it was: every size exact but the eight-byte one, whose
# odd tile rows sat 0x800 bytes -- in-tile x bit 5 -- from where the driver
# looked, in both tile-column parities and nowhere else.
measured = records('readback-pid237.txt')
maps = statuses(measured, 'r91_tile_map')
for name in ('sixteen-byte', 'four-byte', 'two-byte', 'one-byte x', 'one-byte y'):
    assert maps[name][0] == 'PASS' and maps[name][1].endswith(TEXELS), maps[name]
assert maps['eight-byte'] == ('FAIL', "eight-byte: the driver's map names the GPU's place for "
                              "4177920 of 8294400 texels"), maps['eight-byte']
twists = [r['detail'] for r in measured if r.get('probe') == 'r91_tile_map_difference'
          and r['detail'].startswith('eight-byte')]
assert twists == ['eight-byte: offset xor 0x800, tile column 0 row 1 parity: 2058240 texels, '
                  'first (0,64) in-tile (0,0)',
                  'eight-byte: offset xor 0x800, tile column 1 row 1 parity: 2058240 texels, '
                  'first (128,64) in-tile (0,0)'], twists

# PID 238, with the twist and the one-byte runs: every map exact, and every
# clear of R90's case, the one- and eight-byte formats included.
final = records(sys.argv[1] if len(sys.argv) > 1 else 'readback.txt')
tests = {r['detail']: r['status'] for r in final if r.get('probe') == 'runner_test'}
assert tests == {name: 'PASS' for name in ('r91-tile-maps', 'r90-image-clears', 'v0-targets',
                                           'v0-targets-uint')}, tests
maps = statuses(final, 'r91_tile_map')
assert len(maps) == 6 and all(s == 'PASS' and d.endswith(TEXELS) for s, d in maps.values()), maps
clears = statuses(final, 'r90_clear_check')
assert len(clears) == 30 and all(s == 'PASS' and d.endswith('227688 of 227688 texels')
                                 for s, d in clears.values()), clears
for name in ('R16G16B16A16_UNORM clear 1', 'R16G16B16A16_UNORM clear 2',
             'R16G16B16A16_SFLOAT clear 1', 'R16G16B16A16_SFLOAT clear 2', 'R8_UNORM clear 1',
             'R8_UNORM clear 2'):
    assert name in clears, name
print('PASS: the eight-byte map twisted x bit 5 on odd tile rows (PID 237); with the twist and '
      'eight-byte runs for one-byte maps every element size names the GPU\'s place for all '
      '8294400 texels and 30 of 30 GPU clears read back exactly (PID 238)')
