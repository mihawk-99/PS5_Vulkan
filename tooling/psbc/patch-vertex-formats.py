#!/usr/bin/env python3
# PS5 Vulkan - the vertex formats libpsbc's enum cannot express.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

Vulkan requires VERTEX_BUFFER for every format whose components the hardware can
fetch, and the hardware's vertex fetch reads the same GFX10 format words the
texture path does -- the table KytyPS5 carries as `BufferFormat` numbers
(k8UNorm 1, k8SNorm 2, k8UInt 5, k8SInt 6, k8_8UNorm 14, k8_8SNorm 15, k8_8UInt
18, k8_8SInt 19, k16UNorm 7 .. k16Float 13, k16_16UNorm 23 .. k16_16Float 29,
k16_16_16_16UNorm 65 .. k16_16_16_16Float 71), and the words this repository's
own `ps5vk_formats` fetch words come from. The compiler reaches them through
Mesa's pipe formats: `gfx_state.vi` takes a `pipe_format` a vertex attribute and
Mesa's vertex-element emission turns it into the descriptor word, so an enum
value and its pipe format are the whole mechanism.

The script inserts the values before the enum's closing brace and the cases
before the switch's `default:`, one at a time, and skips a value that is already
there -- so it is idempotent and works on a fresh SDK copy and on an
already-patched work copy alike.
"""
import sys
from pathlib import Path

# (PsbcVertexFormat value, the Mesa pipe format it fetches as)
FORMATS = [
    # The eight-bit component layouts (hardware words 1, 2, 5, 6, 14, 15, 18, 19).
    ("PSBC_VERTEX_FORMAT_R8_UNORM", "PIPE_FORMAT_R8_UNORM"),
    ("PSBC_VERTEX_FORMAT_R8_SNORM", "PIPE_FORMAT_R8_SNORM"),
    ("PSBC_VERTEX_FORMAT_R8_UINT", "PIPE_FORMAT_R8_UINT"),
    ("PSBC_VERTEX_FORMAT_R8_SINT", "PIPE_FORMAT_R8_SINT"),
    ("PSBC_VERTEX_FORMAT_R8G8_UNORM", "PIPE_FORMAT_R8G8_UNORM"),
    ("PSBC_VERTEX_FORMAT_R8G8_SNORM", "PIPE_FORMAT_R8G8_SNORM"),
    ("PSBC_VERTEX_FORMAT_R8G8_UINT", "PIPE_FORMAT_R8G8_UINT"),
    ("PSBC_VERTEX_FORMAT_R8G8_SINT", "PIPE_FORMAT_R8G8_SINT"),
    # The sixteen-bit component layouts (hardware words 7, 8, 11, 12, 13,
    # 23..29 and 65..71).
    ("PSBC_VERTEX_FORMAT_R16_UNORM", "PIPE_FORMAT_R16_UNORM"),
    ("PSBC_VERTEX_FORMAT_R16_SNORM", "PIPE_FORMAT_R16_SNORM"),
    ("PSBC_VERTEX_FORMAT_R16_UINT", "PIPE_FORMAT_R16_UINT"),
    ("PSBC_VERTEX_FORMAT_R16_SINT", "PIPE_FORMAT_R16_SINT"),
    ("PSBC_VERTEX_FORMAT_R16_SFLOAT", "PIPE_FORMAT_R16_FLOAT"),
    ("PSBC_VERTEX_FORMAT_R16G16_UNORM", "PIPE_FORMAT_R16G16_UNORM"),
    ("PSBC_VERTEX_FORMAT_R16G16_SNORM", "PIPE_FORMAT_R16G16_SNORM"),
    ("PSBC_VERTEX_FORMAT_R16G16_UINT", "PIPE_FORMAT_R16G16_UINT"),
    ("PSBC_VERTEX_FORMAT_R16G16_SINT", "PIPE_FORMAT_R16G16_SINT"),
    ("PSBC_VERTEX_FORMAT_R16G16_SFLOAT", "PIPE_FORMAT_R16G16_FLOAT"),
    ("PSBC_VERTEX_FORMAT_R16G16B16A16_UNORM", "PIPE_FORMAT_R16G16B16A16_UNORM"),
    ("PSBC_VERTEX_FORMAT_R16G16B16A16_SNORM", "PIPE_FORMAT_R16G16B16A16_SNORM"),
    ("PSBC_VERTEX_FORMAT_R16G16B16A16_UINT", "PIPE_FORMAT_R16G16B16A16_UINT"),
    ("PSBC_VERTEX_FORMAT_R16G16B16A16_SINT", "PIPE_FORMAT_R16G16B16A16_SINT"),
    ("PSBC_VERTEX_FORMAT_R16G16B16A16_SFLOAT", "PIPE_FORMAT_R16G16B16A16_FLOAT"),
    # The 8888 SNORM, SINT and UINT layouts (hardware words 57, 61, 60). Vulkan's
    # R8G8B8A8_X and A8B8G8R8_X_PACK32 name the same memory order -- the packed
    # form counts its bytes from the most significant end -- so one value closes
    # both rows, as the UNORM pair already shares one (round 3).
    ("PSBC_VERTEX_FORMAT_R8G8B8A8_SNORM", "PIPE_FORMAT_R8G8B8A8_SNORM"),
    ("PSBC_VERTEX_FORMAT_R8G8B8A8_SINT", "PIPE_FORMAT_R8G8B8A8_SINT"),
    ("PSBC_VERTEX_FORMAT_R8G8B8A8_UINT", "PIPE_FORMAT_R8G8B8A8_UINT"),
]

ENUM_CLOSE = "} PsbcVertexFormat;"
# The 0.3.0 fork maps the enum in a function of its own,
# `static enum pipe_format psbc_vertex_pipe_format(PsbcVertexFormat format)`,
# whose cases return their pipe format and whose default is this line. Anchor on
# the whole line, including the newline that bounds it: the 0.2.0-era anchor was
# eight spaces and `default:`, which is a *substring* of a deeper switch's
# `                default:` -- in the fork that is the NIR intrinsic switch
# inside psbc_tess_input_supported, so the cases landed where no `format`
# variable exists and the build failed with "use of undeclared identifier
# 'format'". A line-exact anchor cannot repeat that.
SWITCH_DEFAULT = "\n    default: return PIPE_FORMAT_NONE;\n"
NOTE = (
    "    /* PS5 Vulkan: the vertex formats the enum could not express, added by\n"
    "     * tooling/psbc/patch-vertex-formats.py; the descriptor word comes from\n"
    "     * Mesa's vertex-element table. */\n"
)


def patch_header(path):
    text = path.read_text()
    added = 0
    for name, _ in FORMATS:
        if f"    {name},\n" in text:
            continue
        if text.count(ENUM_CLOSE) != 1:
            raise SystemExit(f"{path}: expected one {ENUM_CLOSE}")
        text = text.replace(ENUM_CLOSE, f"    {name},\n{ENUM_CLOSE}")
        added += 1
    if added:
        path.write_text(text)
    return added


def patch_source(path):
    text = path.read_text()
    added = 0
    for name, pipe in FORMATS:
        if f"\n    case {name}:" in text:
            continue
        if text.count(SWITCH_DEFAULT) != 1:
            raise SystemExit(f"{path}: expected one vertex-format switch default")
        block = NOTE if added == 0 else ""
        text = text.replace(
            SWITCH_DEFAULT,
            f"\n{block}    case {name}: return {pipe};\n"
            "    default: return PIPE_FORMAT_NONE;\n",
        )
        added += 1
    if added:
        path.write_text(text)
    return added


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-vertex-formats.py <opengnm-psbc tree>")
    tree = Path(sys.argv[1])
    values = patch_header(tree / "libpsbc/psbc_compile.h")
    cases = patch_source(tree / "libpsbc/psbc_compile.c")
    print(
        f"vertex formats: {values} enum values and {cases} switch cases added to the "
        f"compiler work copy ({len(FORMATS)} in the table)"
    )
