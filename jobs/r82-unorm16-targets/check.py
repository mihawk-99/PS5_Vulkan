#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R82's console readbacks of the 16-bit UNORM targets, never host execution."""
import json
from pathlib import Path
import sys
records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
targets = [r['detail'] for r in records if r.get('probe') == 'agc_target_format' and 'detail' in r]
assert targets[-4:-1] == ['R16_UNORM', 'R16G16_UNORM', 'R16G16B16A16_UNORM'], targets
assert all(r['status'] == 'PASS' for r in records if r.get('probe') == 'agc_target_format' and 'status' in r)
assert len(targets) == 19
# Every readback of every target, solid and blended, matches in all pixels.
for probe in ('agc_narrow_readback', 'agc_wide_readback'):
    rows = [r for r in records if r.get('probe') == probe]
    matching = [r['value'] for r in rows if r.get('field') == 'matching']
    of = [r['value'] for r in rows if r.get('field') == 'of']
    assert matching and matching == of == [8294400] * len(of), probe
# Values half floats cannot hold, stored exact: 0x1235 alone, 0x1000 + 0x0234
# blended, and the four-channel blend's two words.
narrow = [r['value'] for r in records if r.get('probe') == 'agc_narrow_readback'
          and r.get('field') == 'expected_texel']
assert '0x1235' in narrow and '0x1234' in narrow, narrow
wide = [(r.get('field'), r['value']) for r in records if r.get('probe') == 'agc_wide_readback' and 'value' in r]
assert ('expected_word', '0x24561234') in wide and ('expected_word_hi', '0xc0003678') in wide
for name in ('R16_UNORM', 'R16G16_UNORM', 'R16G16B16A16_UNORM'):
    row = [r['detail'] for r in records if r.get('probe') == 'v0_formats_format'
           and r['detail'].startswith(name + ':')]
    assert row == [f'{name}: optimal 0x0000dd81, buffer 0x00000040'], row
assert not any(r.get('status') == 'FAIL' for r in records)
print('PASS: R16, R16G16 and R16G16B16A16 UNORM render and blend exact; features 0xdd81')
