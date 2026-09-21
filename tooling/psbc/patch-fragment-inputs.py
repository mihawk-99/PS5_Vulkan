#!/usr/bin/env python3
# PS5 Vulkan - assign fragment input slots before standalone shader compilation.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree."""
import sys
from pathlib import Path

# The fork's standalone path takes the pointers it already holds (`compiler_info`,
# `layout`, `gfx_state`), a `stage` pointer, and picks the pipeline kind from the
# Mesa stage, so the call reads differently from the 0.2.0-era tree the anchor was
# first written against. The anchor is the call's opening lines, which are unique
# in the file; the fork's own fragment-stage lowering (`radv_nir_lower_opt_fs_frag_pos`)
# sits just above it, so inserting here keeps RADV's order -- lower fragment
# coordinates, assign input slots, then gather shader info.
ANCHOR = """    radv_nir_shader_info_pass(
        compiler_info, nir, layout, &stage->key, gfx_state,
"""
REPLACEMENT = """    /* PS5 Vulkan: standalone compilation needs RADV's fragment input mapping.
     * Without it every lowered input retains base 0: a second varying aliases
     * the first in ACO, and AGC input-semantic construction reports a collision.
     * Match radv_fill_shader_info: assign slots before gathering shader info. */
    if (mesa_stage == MESA_SHADER_FRAGMENT)
        NIR_PASS(_, nir, ac_nir_assign_fs_input_locations);

""" + ANCHOR

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-fragment-inputs.py <opengnm-psbc tree>")
    path = Path(sys.argv[1]) / "libpsbc/psbc_compile.c"
    text = path.read_text()
    if REPLACEMENT not in text:
        if text.count(ANCHOR) != 1:
            raise SystemExit(f"{path}: expected one fragment-input anchor")
        path.write_text(text.replace(ANCHOR, REPLACEMENT))
    print("fragment input locations assigned in compiler work copy")
