#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - check the tiled mip chain layout.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase C7 (docs/M5_PHASE_C.md). The console lays a tiled mip
# chain out by AddrLib's rule, and the console's own address map measured what
# that rule produces for a five-level 256x256 chain
# (docs/HARDWARE_FINDINGS.md): level 0 at 0x20000, level 1 at 0x10000, level 2
# at 0x8400, level 3 at 0x4800 and level 4 at 0x800, inside 0x60000 bytes.
#
# This check builds the pinned AddrLib (.deps/native/mesa,
# tools/setup-native-dependencies.sh) as a host tool and holds that measurement
# against it: the rule is what the probe's tables and the driver's tiled upload
# will be written from, so the measurement has to be AddrLib's answer, not a
# reading of it. The rest of the shape matrix it prints is the reference for
# chains the console has not measured yet (the six-level chain the runner's
# c7-mip-pages-six frames walk, above all).
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mesa="$root/.deps/native/mesa/mesa-26.2.0"
addrlib="$mesa/src/amd/addrlib"
if [[ ! -d $addrlib ]]; then
    echo "missing $addrlib; run tools/setup-native-dependencies.sh" >&2
    exit 2
fi
cxx=${HOST_CXX:-}
if [[ -z $cxx ]]; then
    for candidate in clang++ clang++-18 g++; do
        if command -v "$candidate" >/dev/null; then
            cxx=$candidate
            break
        fi
    done
fi
[[ -n $cxx ]] || { echo "no host C++ compiler found; set HOST_CXX" >&2; exit 2; }

output="$root/build/host/mip-layout-oracle"
mkdir -p "$(dirname -- "$output")"
mapfile -t sources < <(find "$addrlib/src" -maxdepth 2 -name '*.cpp' -print | sort)
# The same definitions Mesa builds AddrLib with (src/amd/addrlib/meson.build):
# no regparm, allow the SIMD swizzlers, and the platform's endianness.
"$cxx" -std=c++17 -O1 -w \
    -DADDR_FASTCALL= -DADDR_ALLOW_SIMD=0 -DLITTLEENDIAN_CPU -DDEBUG=0 \
    -I "$addrlib/inc" -I "$addrlib/src" -I "$addrlib/src/core" \
    -I "$addrlib/src/chip/gfx9" -I "$addrlib/src/chip/gfx10" -I "$addrlib/src/chip/gfx11" \
    -I "$addrlib/src/chip/gfx12" -I "$addrlib/src/chip/r800" \
    "$root/tools/mip-layout-oracle.cpp" "${sources[@]}" -o "$output"

echo "== AddrLib's tile extents, as the driver sizes its images"
# Every tile is 64 KiB of elements, whatever the element size; the 1x row is the
# driver's ps5vk_tile_extent and the SDK's ps5_tiled_color_tile table, and
# AddrLib refuses a four-sample shape, so the 4x row stays the SDK's
# ps5_tiled_color_msaa4_tile (docs/HARDWARE_FINDINGS.md).
tiles=$("$output" tiles)
echo "$tiles"
expected_tiles="1 256x256
2 256x128
4 128x128
8 128x64
16 64x64"
got_tiles=$(echo "$tiles" | awk '/^   [0-9]+ /{print $1, $2}')
if [[ $got_tiles != "$expected_tiles" ]]; then
    echo "AddrLib's tile extents are not the driver's:" >&2
    printf '  AddrLib:\n%s\n  driver:\n%s\n' "$got_tiles" "$expected_tiles" >&2
    exit 1
fi
echo "PASS: the 64 KiB tile extents are AddrLib's"
echo "== AddrLib's chain layout, as the probe's chains need it"
"$output"
echo "== AddrLib's array slices, as D1's arrays and cubemaps need them"
# A slice is a chain of its own, so an array is consecutive chains and the one
# number the driver needs is sliceSize; and AddrLib's ADDR2 API has no cube flag
# -- a cube is a six-slice array to it, which is what the cube row has to keep
# saying. The console has measured no array yet: this is the reference Battery
# 1's array question is asked against (docs/M5_REFERENCE.md, V0-unknowns).
python3 "$root/tools/array-layout-check.py" "$output"

echo "== The console's measured five-level 256x256 chain"
# Measured by the address map (Klog_Logs/c7-mip-run33.log, run 33): the five
# pinned LODs' frames fetched 0x2f0fc, 0x13cfc, 0xb4fc, 0x58fc and 0x8fc, and
# each is a level's base plus the driver's map of the texel that frame sampled
# (docs/HARDWARE_FINDINGS.md).
measured="{256, 256, 5, 0x60000, {0x20000, 0x10000, 0x8400, 0x4800, 0x800}}, /* firstMipInTail 2 */"
if ! "$output" 256 256 5 | grep -Fq "{256, 256, 5, 0x60000, {0x20000, 0x10000, 0x8400, 0x4800, 0x800}}"; then
    echo "AddrLib's five-level chain is not the console's measurement:" >&2
    echo "  AddrLib:    $("$output" 256 256 5 | head -n 1)" >&2
    echo "  console:    $measured" >&2
    exit 1
fi
echo "PASS: AddrLib lays the measured chain out at the measured bases"

echo "== The table each user of it holds"
# The probe's table fills the chain the console samples and the C7 test's fills
# the one the PC draws; both are this measurement, and neither may keep a base
# that is one of a level's addresses rather than the level's start -- which is
# exactly how level 2's 0x8000 was caught (docs/HARDWARE_FINDINGS.md).
# The driver's own table (driver/ps5vk_image.c, ps5vk_tiled_chains) is what its
# tiled upload places levels with, so every shape it carries is checked against
# the oracle too, not only the five-level one the console measured.
python3 - "$output" <<'PY'
import re
import subprocess
import sys
from pathlib import Path

oracle = sys.argv[1]
text = Path("driver/ps5vk_image.c").read_text(encoding="utf-8")
table = re.search(r"ps5vk_tiled_chains\[\] = \{(.*?)\n\};", text, re.S)
if table is None:
    raise SystemExit("  driver/ps5vk_image.c: no ps5vk_tiled_chains table")
entries = re.findall(r"\{(\d+), (\d+), (\d+), (0x[0-9a-f]+),\s*\{([^}]*)\}\}", table.group(1))
declared = len(re.findall(r"^   \{\d+, \d+, \d+,", table.group(1), re.M))
if not entries or len(entries) != declared:
    raise SystemExit(f"  driver/ps5vk_image.c: parsed {len(entries)} of {declared} chain entries")
failed = False
for width, height, levels, size, bases in entries:
    out = subprocess.run([oracle, width, height], check=True, capture_output=True, text=True).stdout
    want = None
    for line in out.splitlines():
        m = re.match(r"\{%s, %s, %s, (0x[0-9a-f]+), \{([^}]*)\}\}" % (width, height, levels), line.strip())
        if m:
            want = (m.group(1), [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", m.group(2))])
            break
    if want is None:
        print(f"  {width}x{height} {levels} levels: the oracle prints no such chain", file=sys.stderr)
        failed = True
        continue
    got = [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", bases)]
    if int(size, 16) != int(want[0], 16) or got != want[1]:
        print(f"  {width}x{height} {levels} levels: driver {size} {[hex(v) for v in got]}, "
              f"oracle {want[0]} {[hex(v) for v in want[1]]}", file=sys.stderr)
        failed = True
    else:
        print(f"  driver/ps5vk_image.c: {width}x{height} {levels} levels is the oracle's chain")
if failed:
    raise SystemExit(1)
PY
python3 - "$root" "$("$output" 256 256 5 | head -n 1)" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
measured = re.search(r"\{0x[0-9a-f, 0x]+\}", sys.argv[2]).group(0)
want = [int(word, 16) for word in re.findall(r"0x[0-9a-f]+", measured)]
tables = [("driver/tests/vk_c7_tiled_mip_test.c", "kLevelBases"),
          ("src/diagnostics.cpp", "kMipLevelBases")]
failed = False
for name, symbol in tables:
    text = (root / name).read_text(encoding="utf-8")
    match = re.search(re.escape(symbol) + r"\[[^\]]*\]\s*=\s*\{([^}]*)\}", text)
    if match is None:
        print(f"  {name}: no initializer for {symbol}", file=sys.stderr)
        failed = True
        continue
    got = [int(word, 16) for word in re.findall(r"0x[0-9a-f]+", match.group(1))]
    if got != want:
        print(f"  {name}: {symbol} is {[hex(value) for value in got]}", file=sys.stderr)
        failed = True
    else:
        print(f"  {name}: {symbol} is the measured chain")
if failed:
    print(f"  the measurement is {[hex(value) for value in want]}", file=sys.stderr)
    raise SystemExit(1)
PY

echo "== The driver's texel maps, against the rows AddrLib derives for the console"
# The console's maps are AddrLib's own rows in *its* configuration: sixteen
# pipes, a non-RbPlus revision, no pipe/bank rotation (the oracle builds the
# library that way, and its four-byte row comes out as the console's measured
# map, `measured match` below). With a default one-pipe library AddrLib answers
# with a different map -- 16128 of a tile's 16384 texels differ -- which is why
# this check exists and why the two-byte row is a row of its own rather than a
# scaling of the four-byte one (docs/HARDWARE_FINDINGS.md).
python3 - "$output" <<'PY'
import re
import subprocess
import sys
from pathlib import Path

oracle = sys.argv[1]
text = Path("driver/ps5vk_image.c").read_text(encoding="utf-8")


def driver_terms(symbol):
    match = re.search(re.escape(symbol) + r"\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if match is None:
        raise SystemExit(f"  driver/ps5vk_image.c: no {symbol} table")
    found = re.findall(r"\{(\d+),\s*(\d+),\s*(0x[0-9a-f]+)u\}", match.group(1))
    if not found:
        raise SystemExit(f"  driver/ps5vk_image.c: {symbol} parses to nothing")
    return {(int(coord), int(shift), int(mask, 16)) for coord, shift, mask in found}


def oracle_terms(bpe, samples, mode):
    out = subprocess.run([oracle, "swizzle", str(bpe), str(samples), mode], check=True,
                         capture_output=True, text=True).stdout
    if "refused" in out:
        raise SystemExit(f"  the oracle refuses {bpe}-byte {samples}x {mode}")
    if "measured MISMATCH" in out:
        raise SystemExit("  the oracle's four-byte row is not the console's measured map: its "
                         "configuration is not the console's")
    return {(0 if coord == "x" else 1, int(shift), int(mask, 16))
            for coord, shift, mask in re.findall(r"\(([xy]), (-?\d+), (0x[0-9a-f]+)\)", out)}


failed = False
for symbol, bpe, samples, mode, what in [
        ("ps5vk_tiled_4b_terms", 4, 1, "64kb_r_x", "the measured four-byte colour map"),
        ("ps5vk_tiled_1b_terms", 1, 1, "64kb_r_x", "the one-byte colour map"),
        ("ps5vk_tiled_2b_terms", 2, 1, "64kb_r_x", "the two-byte colour map"),
        ("ps5vk_tiled_8b_terms", 8, 1, "64kb_r_x", "the eight-byte colour map"),
        ("ps5vk_tiled_16b_terms", 16, 1, "64kb_r_x", "the sixteen-byte colour map"),
        ("ps5vk_tiled_depth2_terms", 2, 1, "64kb_z_x", "the two-byte depth map"),
        ("ps5vk_tiled_depth4_terms", 4, 4, "64kb_z_x", "the four-sample depth map")]:
    want = oracle_terms(bpe, samples, mode)
    # The depth row's sample term is the byte offset the driver adds itself
    # (PS5VK_DEPTH_SAMPLE_BYTES, four bytes a sample), not a term of its table.
    want = {term for term in want if term[0] != 2}
    got = driver_terms(symbol)
    if got != want:
        print(f"  driver/ps5vk_image.c: {symbol} is not AddrLib's {bpe}-byte row ({what})",
              file=sys.stderr)
        print(f"    driver: {sorted(got)}", file=sys.stderr)
        print(f"    AddrLib: {sorted(want)}", file=sys.stderr)
        failed = True
    else:
        print(f"  driver/ps5vk_image.c: {symbol} is AddrLib's {bpe}-byte row, {len(got)} terms")
if failed:
    raise SystemExit(1)
PY

echo "== The four-sample depth map as the driver applies it, against AddrLib's offsets"
# The terms above are half of a depth map: a four-sample depth tile also turns
# its own place in the grid into bits 10 to 15 of the texel's position
# (ps5vk_tiled_depth4_offset). Both halves are read out of the driver here and
# evaluated over a 4x4-tile region -- every texel, every sample -- against the
# offsets AddrLib computes for the same region.
python3 - "$output" <<'PY'
import re
import subprocess
import sys
from pathlib import Path

oracle = sys.argv[1]
text = Path("driver/ps5vk_image.c").read_text(encoding="utf-8")
body = re.search(r"ps5vk_tiled_depth4_offset\(.*?\n\{(.*?)\n\}", text, re.S)
if body is None:
    raise SystemExit("  driver/ps5vk_image.c: no ps5vk_tiled_depth4_offset")
body = body.group(1)

terms = re.search(r"ps5vk_tiled_depth4_terms\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
if terms is None:
    raise SystemExit("  driver/ps5vk_image.c: no ps5vk_tiled_depth4_terms table")
term_list = [(int(coord), int(shift), int(mask, 16))
             for coord, shift, mask in re.findall(r"\{(\d+),\s*(\d+),\s*(0x[0-9a-f]+)u\}", terms.group(1))]

# The turn the function applies, as the lines that XOR it in: the coordinate it
# takes a bit from (the tile's column, its row, or the row's second bit) and the
# output bit it lands on.
turn = []
for name, shift in re.findall(
        r"local \^= \(uint64_t\)\((column|row|\(row >> 1\)) & 1u\) << (\d+);", body):
    turn.append(({"column": 0, "row": 1, "(row >> 1)": 2}[name], int(shift)))
if len(turn) != 4:
    raise SystemExit(f"  driver/ps5vk_image.c: parsed {len(turn)} turn lines, expected 4")

block = re.search(r"return \(\(uint64_t\)row \* blocks_per_row \+ column\) \* PS5VK_TILE_BYTES \+ local;",
                  body)
if block is None:
    raise SystemExit("  driver/ps5vk_image.c: the depth-4 offset's block index is not the expected one")

tile_width = 64
tile_height = 64
tiles = 4
level_width = tile_width * tiles


def driver(x, y, sample):
    in_x, in_y = x & (tile_width - 1), y & (tile_height - 1)
    column, row = x // tile_width, y // tile_height
    local = 0
    for coord, shift, mask in term_list:
        local ^= ((in_x if coord == 0 else in_y) << shift) & mask
    index = [column, row, row >> 1]
    for coord, shift in turn:
        local ^= (index[coord] & 1) << shift
    blocks_per_row = (level_width + tile_width - 1) // tile_width
    return (row * blocks_per_row + column) * 0x10000 + local + sample * 4


dump = subprocess.run([oracle, "coords", "4", "4", "64kb_z_x", str(tiles)], check=True,
                      capture_output=True, text=True).stdout.splitlines()
checked = 0
failed = False
for line in dump:
    x, y, sample, offset = (int(v, 16) if v.startswith("0x") else int(v) for v in line.split())
    if driver(x, y, sample) != offset:
        print(f"  the driver's four-sample depth map is not AddrLib's at "
              f"({x}, {y}) sample {sample}: driver 0x{driver(x, y, sample):x}, "
              f"AddrLib 0x{offset:x}", file=sys.stderr)
        failed = True
        break
    checked += 1
if failed:
    raise SystemExit(1)
print(f"  driver/ps5vk_image.c: the four-sample depth map is AddrLib's over a "
      f"{tiles}x{tiles}-tile region, {checked} texels and samples")
PY

# And the same maps against the published equation table, which is the other
# independent statement of them (SharpProspero's AgcTilingTables.cs, the same
# address library's generated equations). That check also names the equations
# for the modes nothing here has measured, which is what a probe would extend.
python3 "$root/tools/check-tile-equations.py"

# A compressed format's tile is its own, and no probe has walked one: the CTS
# requires one of the BC, ETC2 or ASTC sets in full (docs/M5_PHASE_C.md, CTS
# round 12), so the shape a driver map for it would be written from is asked of
# AddrLib here rather than guessed -- the element is a 4x4 texel block, so the
# row comes from the format rather than from a bytes-per-pixel number.
echo "== Compressed tiles, from AddrLib's own format table"
for spec in "bc1 64kb_r_x" "bc3 64kb_r_x" "bc7 64kb_r_x"; do
    "$output" compressed $spec | sed 's/^/   /'
done

# And the claim those numbers carry: a compressed format's tile is the row of its
# *block size*, with the coordinates in 4x4 blocks -- BC1's eight-byte block tile
# is the eight-byte element row, BC3's and BC7's sixteen-byte ones are the
# sixteen-byte row. That is what lets a driver map for a compressed format reuse
# a row this repository already measured and proved instead of inventing one
# (docs/M5_PHASE_C.md, CTS rounds 13 and 14), so it is checked here rather than
# asserted in prose.
python3 - "$output" <<'PY'
import re
import subprocess
import sys

oracle = sys.argv[1]


def tile(args):
    out = subprocess.run([oracle] + args, capture_output=True, text=True).stdout
    found = re.search(r"tile (\d+)x(\d+)", out)
    return (int(found.group(1)), int(found.group(2))) if found else None


def compressed(format_name):
    out = subprocess.run([oracle, "compressed", format_name, "64kb_r_x"],
                         capture_output=True, text=True).stdout
    found = re.search(r"block (\d+)x(\d+) texels", out)
    return (int(found.group(1)), int(found.group(2))) if found else None


rows = {size: tile(["swizzle", str(size), "1", "64kb_r_x"]) for size in (8, 16)}
pairs = [("bc1", 8), ("bc2", 16), ("bc3", 16), ("bc4", 8), ("bc5", 16), ("bc6", 16), ("bc7", 16)]
bad = 0
for format_name, block in pairs:
    got, want = compressed(format_name), rows[block]
    ok = got is not None and got == want
    bad += 0 if ok else 1
    print(f"   {format_name}: {block}-byte blocks -> tile {got}, the {block}-byte row is {want}"
          f"{' OK' if ok else ' MISMATCH'}")
raise SystemExit(1 if bad else 0)
PY
