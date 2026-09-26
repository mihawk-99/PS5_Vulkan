#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check R89's console run: a non-indexed draw's first vertex and a first
instance draw what they name."""
import json
from pathlib import Path
import sys

records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
events = lambda probe: [r for r in records if r.get('probe') == probe and r.get('event') == 'probe']
status = lambda probe: [r['status'] for r in events(probe)]

tests = {r['detail']: r['status'] for r in events('runner_test')}
assert tests == {name: 'PASS' for name in ('r89-first-vertex', 'r89-first-instance', 'c2-base-vertex',
                                           'c2-instancing', 'c2-indexed', 'b7-triangle')}, tests
assert status('r89_first_vertex') == ['PASS'] and status('r89_first_instance') == ['PASS']
# The first-vertex frames: the first quad's red (0x80) from vertex 0, the
# second's (0x20) from vertex 6, every pixel of the square.
colours = [r['detail'] for r in events('agc_vertex_colour')]
assert colours[:2] == ['2073600 of 2073600 pixels with red 0x80, alpha 0xff and gradients within 1',
                       '2073600 of 2073600 pixels with red 0x20, alpha 0xff and gradients within 1'], colours
# The first-instance frames: exactly the boxes of the instances each starts at.
boxes = [r['detail'] for r in events('agc_instance_square') if r['detail'].startswith('first instance')]
assert boxes == ['first instance 1: nothing in instance 0\'s box',
                 'first instance 1: instance 1 drew its own box',
                 'first instance 1: instance 2 drew its own box',
                 'first instance 2: nothing in instance 0\'s box',
                 'first instance 2: nothing in instance 1\'s box',
                 'first instance 2: instance 2 drew its own box'], boxes
assert all(r['status'] == 'PASS' for r in events('agc_instance_square'))
print('PASS: vkCmdDraw from vertex 0 and 6 drew each quad exactly; firstInstance 1 and 2 drew '
      'exactly their instances\' boxes; the indexed, instanced and triangle controls pass')
