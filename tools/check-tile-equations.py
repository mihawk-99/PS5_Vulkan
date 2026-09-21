#!/usr/bin/env python3
# PS5 Vulkan - the tile swizzle equations against this driver's measured maps.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Why this exists: the driver's tiled maps were measured on the console one
# element size at a time (docs/HARDWARE_FINDINGS.md, the C7 address map), so each
# new mode is a console run away. The published table in tools/tile-equations.json
# covers every mode at once -- two dimensions, four fragment counts, five element
# sizes -- so the useful thing to know is whether the rows this driver already
# measured agree with it. If they do, a *new* row can be taken from the table and
# checked here instead of waiting for a probe; if one does not, the measured row
# wins and the table is wrong where they differ.
#
# The table is GPL-3.0 text from SharpProspero (see the file's own note); this
# script, and the driver rows it reads, are this repository's.
#
# It reads the rows out of driver/ps5vk_image.c rather than restating them, so a
# change to a map is compared here the moment it lands.
#
# Run from the repository root: python3 tools/check-tile-equations.py

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DRIVER = ROOT / "driver" / "ps5vk_image.c"

# ---------------------------------------------------------------- the equations
TABLE = json.loads((ROOT / "tools" / "tile-equations.json").read_text())
BASE_TERMS = [tuple(term) for term in TABLE["base_terms"]]
COLUMN_TERMS = [tuple(term) for term in TABLE["column_terms"]]
RENDER_TARGET_EQUATIONS = TABLE["render_target_equations"]
DEPTH_EQUATIONS = TABLE["depth_equations"]

# mkwii-ps5's transcription of "equation 27" (ps5/gpu/gx_depth_offset.h,
# GPL-3.0-only, Copyright (C) 2026 Phi1ow), evaluated as written so the claim
# "the depth target is tiled by equation 27" can be checked rather than
# believed. Its extra x terms are what the comparison below is for.
def mkwii_depth_offset(x, y, blocks_per_row):
    block = (y >> 7) * blocks_per_row + (x >> 7)
    in_block = (((y << 3) & 0x8) ^ ((y << 4) & 0x20) ^ ((y << 5) & 0xF80) ^ ((y << 9) & 0x1000)
                ^ ((y << 8) & 0x4000) ^ ((x << 2) & 0x4) ^ ((x << 3) & 0x10) ^ ((x << 4) & 0x440)
                ^ ((x << 5) & 0x300) ^ ((x << 6) & 0x800) ^ ((x << 9) & 0xA000))
    return (block << 16) + in_block


def equation_offset(index, x, y, z=0, slice=0):
    value = 0
    for coord, shift, mask in BASE_TERMS[index]:
        source = {1: y, 2: z, 3: slice}[coord]
        value ^= ((source << shift) if shift > 0 else (source >> -shift)) & mask
    for shift, mask in COLUMN_TERMS[index]:
        value ^= ((x << shift) if shift > 0 else (x >> -shift)) & mask
    return value


# ------------------------------------------------------- the driver's own rows
def read_terms(name):
    text = DRIVER.read_text()
    match = re.search(r"static const struct ps5vk_tiled_term " + name + r"\[\] = \{(.*?)\};",
                      text, re.S)
    if not match:
        raise SystemExit(f"{DRIVER}: no table {name}")
    return [(int(c), int(s), int(m, 0))
            for c, s, m in re.findall(r"\{(\d+),\s*(\d+),\s*(0x[0-9a-fA-F]+)u\}", match.group(1))]


def read_mask_array(name):
    text = DRIVER.read_text()
    match = re.search(r"static const uint16_t " + name + r"\[7\] = \{(.*?)\};", text, re.S)
    if not match:
        raise SystemExit(f"{DRIVER}: no array {name}")
    return [int(m, 0) for m in re.findall(r"(0x[0-9a-fA-F]+)", match.group(1))]


def terms_offset(terms, x, y):
    value = 0
    for coord, shift, mask in terms:
        source = x if coord == 0 else y
        value ^= (source << shift) & mask
    return value


def depth1_offset(x, y):
    value = 0
    for bit, (xm, ym) in enumerate(zip(read_mask_array("x_masks"), read_mask_array("y_masks"))):
        if (x >> bit) & 1:
            value ^= xm
        if (y >> bit) & 1:
            value ^= ym
    return value


# name, tile width, tile height, ours(x, y), equation, what the row is
ROWS = [
    ("colour 1-byte", 256, 256, lambda x, y: terms_offset(read_terms("ps5vk_tiled_1b_terms"), x, y),
     RENDER_TARGET_EQUATIONS[0][0][0], "ps5vk_tiled_1b_terms"),
    ("colour 2-byte", 256, 128, lambda x, y: terms_offset(read_terms("ps5vk_tiled_2b_terms"), x, y),
     RENDER_TARGET_EQUATIONS[0][0][1], "ps5vk_tiled_2b_terms"),
    ("colour 4-byte", 128, 128, lambda x, y: terms_offset(read_terms("ps5vk_tiled_4b_terms"), x, y),
     RENDER_TARGET_EQUATIONS[0][0][2], "ps5vk_tiled_4b_terms"),
    ("colour 8-byte", 128, 64, lambda x, y: terms_offset(read_terms("ps5vk_tiled_8b_terms"), x, y),
     RENDER_TARGET_EQUATIONS[0][0][3], "ps5vk_tiled_8b_terms"),
    ("colour 16-byte", 64, 64, lambda x, y: terms_offset(read_terms("ps5vk_tiled_16b_terms"), x, y),
     RENDER_TARGET_EQUATIONS[0][0][4], "ps5vk_tiled_16b_terms"),
    ("depth 2-byte", 256, 128, lambda x, y: terms_offset(read_terms("ps5vk_tiled_depth2_terms"), x, y),
     DEPTH_EQUATIONS[0][0][1], "ps5vk_tiled_depth2_terms"),
    ("depth 4-byte", 128, 128, depth1_offset, DEPTH_EQUATIONS[0][0][2], "ps5vk_tiled_depth_offset"),
    # fragmentsLog2 indexes the fragment *count*: 0 is one sample, 1 two, 2 four.
    ("depth 4-byte, four samples", 64, 64,
     lambda x, y: terms_offset(read_terms("ps5vk_tiled_depth4_terms"), x, y),
     DEPTH_EQUATIONS[0][2][2], "ps5vk_tiled_depth4_terms"),
]


def main():
    failures = 0
    print("== this driver's measured tile maps against the published equations")
    for name, width, height, ours, equation, source in ROWS:
        differences = 0
        first = None
        for y in range(height):
            for x in range(width):
                if ours(x, y) != equation_offset(equation, x, y):
                    differences += 1
                    if first is None:
                        first = (x, y, ours(x, y), equation_offset(equation, x, y))
        state = "agrees" if differences == 0 else f"DIFFERS in {differences} of {width * height} positions"
        print(f"   {name:<28} equation {equation:<3} {state}")
        print(f"      ({source}, tile {width}x{height})")
        if first is not None:
            print(f"      first: ({first[0]}, {first[1]}) ours {first[2]:#06x}, "
                  f"equation {first[3]:#06x}")
            failures += 1

    # The transcription mkwii-ps5 uses for its D32Float depth target.
    print("== mkwii-ps5's depth transcription against equation 27")
    differences = 0
    first = None
    for y in range(128):
        for x in range(128):
            if mkwii_depth_offset(x, y, 1) != equation_offset(27, x, y):
                differences += 1
                if first is None:
                    first = (x, y, mkwii_depth_offset(x, y, 1), equation_offset(27, x, y))
    if differences == 0:
        print("   agrees over the tile's 16384 texels")
    else:
        print(f"   DIFFERS in {differences} of 16384 positions")
        print(f"      first: ({first[0]}, {first[1]}) theirs {first[2]:#06x}, "
              f"equation 27 {first[3]:#06x}")
        failures += 1

    # The modes this driver refuses because no probe has walked them: the table
    # names their equations, which is what a host check can extend coverage with.
    print("== the equations the table gives the modes this driver has not measured")
    print(f"   colour 4-byte, two samples    equation {RENDER_TARGET_EQUATIONS[0][1][2]}")
    print(f"   colour 4-byte, four samples   equation {RENDER_TARGET_EQUATIONS[0][2][2]}")
    print(f"   depth 4-byte, two samples     equation {DEPTH_EQUATIONS[0][1][2]}")
    print(f"   depth 4-byte, four samples    equation {DEPTH_EQUATIONS[0][2][2]}")

    print("tile equations: " + ("PASS" if failures == 0 else "FAIL"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
