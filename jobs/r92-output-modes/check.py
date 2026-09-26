#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R92's console runs: VideoOut supports output mode selectors 1 and 15
only, 15 is 119.88 Hz once the title declares it, and no other selector from 0
to 63 is supported, declared or not."""
import json
from pathlib import Path

here = Path(__file__).parent


def records(name):
    return [json.loads(line) for line in (here / name).read_text().splitlines()]


def answers(rows):
    return {int(r['field'].split('_')[1]): r['value'] for r in rows
            if r.get('probe') == 'r92_output_modes' and r.get('field', '').startswith('supported_')}


REFUSED = 0x80290016 - (1 << 32)   # a mode the console knows and refuses here
INVALID = 0x8029001E - (1 << 32)   # a selector it does not take
UNAVAILABLE = {4, 7, 8, 12, 13, 14, 16, 17, 18, 19}
runs = {name: records(name) for name in ('readback-pid260.txt', 'readback-pid261.txt',
                                          'readback.txt')}
for name, rows in runs.items():
    got = answers(rows)
    assert len(got) == 64, (name, len(got))
    for mode, value in got.items():
        want = 1 if mode in (1, 15) else REFUSED if mode in UNAVAILABLE else INVALID
        assert value == want, (name, mode, value)

# Without the title's high-frame-rate declaration (attribute3 0x80040), 15 is
# refused with 0x80290016 on a used handle (PID 260) and on a fresh one (261).
for name in ('readback-pid260.txt', 'readback-pid261.txt'):
    modes = [r['detail'] for r in runs[name] if r.get('probe') == 'r92_output_mode']
    assert any(d.startswith('mode 15: configure 0x80290016') for d in modes), (name, modes)

# With it (PID 262), 15 halves the vblank period, and 1 is the default.
final = runs['readback.txt']
modes = [r['detail'] for r in final if r.get('probe') == 'r92_output_mode']
assert modes[0].startswith('mode 1: configure 0x00000000, vblank 16683'), modes
assert modes[1].startswith('mode 15: configure 0x00000000, vblank 834') and \
    '(119.88' in modes[1], modes
assert [r['status'] for r in final if r.get('probe') == 'runner_test'] == ['PASS']
print('PASS: selectors 1 and 15 alone are supported, 15 at 8.34 ms once declared; '
      'the rest are refused, 4, 7, 8, 12-14 and 16-19 with the code 15 gets undeclared')
