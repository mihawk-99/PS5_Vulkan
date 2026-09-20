#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - check AddrLib's array and cube layout.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""PS5 Vulkan compatibility probe - hold the array reference against the oracle.

Phase D1 (docs/M5_REFERENCE.md). The console has measured no array, so D1's
arrays are written from AddrLib's rule: a slice is a whole chain of its own, the
layers are consecutive chains, and -- the ADDR2 API having no cube flag -- a cube
is a six-slice array with no layout of its own. This reads
tools/mip-layout-oracle's output (built by tools/check-mip-layout.sh from the
pinned AddrLib) and requires every array row to be its own chain's size times the
slice count, at consecutive multiples, with the cube row equal to the six-slice
array row. Anything else means the reference D1 is written from has moved.

Run through tools/check-mip-layout.sh, which passes the oracle's path.
"""

import re
import subprocess
import sys

out = subprocess.run([sys.argv[1]], check=True, capture_output=True, text=True).stdout
chains = {}
for line in out.splitlines():
    m = re.match(r"\{(\d+), (\d+), (\d+), (0x[0-9a-f]+), \{", line.strip())
    if m:
        chains[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = int(m.group(4), 16)
arrays = {}
for line in out.splitlines():
    m = re.match(r"\{(\d+), (\d+), (\d+), (\d+), (array|cube), (0x[0-9a-f]+), (0x[0-9a-f]+)\},"
                 r" /\* slice bases: ([^*]*)\*/", line.strip())
    if m:
        key = (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5))
        bases = [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", m.group(8))]
        arrays[key] = (int(m.group(6), 16), int(m.group(7), 16), bases)

if not arrays:
    raise SystemExit("  the oracle printed no array shape")
failed = False
for (width, height, levels, slices, kind), (slice_size, total, bases) in sorted(arrays.items()):
    chain = chains.get((width, height, levels))
    if chain is None:
        print(f"  {width}x{height} {levels} levels {slices} {kind}: no chain to compare",
              file=sys.stderr)
        failed = True
        continue
    consecutive = bases == [slice_size * i for i in range(slices)]
    if slice_size != chain or total != slice_size * slices or not consecutive:
        print(f"  {width}x{height} {levels} levels {slices} {kind}: slice 0x{slice_size:x}, "
              f"chain 0x{chain:x}, total 0x{total:x}", file=sys.stderr)
        failed = True
    else:
        print(f"  {width}x{height} {levels} levels x {slices} {kind}: slice 0x{slice_size:x} "
              f"is the chain, layers consecutive")
cube = arrays.get((64, 64, 1, 6, "cube"))
if cube != arrays.get((64, 64, 1, 6, "array")):
    print("  the cube row is not the six-slice array row: AddrLib has no cube flag",
          file=sys.stderr)
    failed = True
else:
    print("  a cube is the six-slice array AddrLib lays out: no cube-specific layout")
if failed:
    raise SystemExit(1)
print("PASS: every slice is a chain and the layers are consecutive")
