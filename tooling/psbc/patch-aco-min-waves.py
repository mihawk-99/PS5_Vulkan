#!/usr/bin/env python3
# PS5 Vulkan - keep the standalone compiler's wave arithmetic out of a zero divide.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

This patch was written for a console SIGFPE that turned out not to be the
compiler's: that fault is the test runner dividing a sampled table row's buffer
by a zero texel size (src/diagnostics.cpp, docs/HARDWARE_FINDINGS.md,
2026-09-20). What is left is a latent divide the patch closes on its own merits
-- `get_addr_regs_from_waves` divides by its `waves` argument:

    uint16_t sgprs = std::min(program->dev.physical_sgprs / waves, 128);   (612-617)
    uint16_t vgprs = program->dev.physical_vgprs / waves;

The call sites hand it `program->min_waves`, and `calc_min_waves` derives that
from `program->workgroup_size`:

    unsigned workgroup_size =
       program->workgroup_size == UINT_MAX ? program->wave_size : program->workgroup_size;
    return align(workgroup_size, program->wave_size) / program->wave_size;

`Program::workgroup_size` is documented "if known; otherwise UINT_MAX"
(aco_ir.h), but the standalone path copies `program->info.workgroup_size` into it
verbatim, so a stage whose info never had a workgroup size -- a graphics stage
that is not NGG -- leaves 0 there, `calc_waves_per_workgroup` answers 0 and
`min_waves` becomes 0. The 0.3.0 fork does not change this: it renamed the field
the old tree called `num_waves` to `min_waves` and still divides unguarded.

Two edits, each with a job:

  1. the invariant: `workgroup_size` of 0 means "not known", which the field's
     own comment says must be UINT_MAX, so the wave size is used. This is the
     fix -- upstream's `assert(program->min_waves >= 1)` is the invariant it
     restores;
  2. the guard and the witness: `get_addr_regs_from_waves` clamps a zero `waves`
     to 1 and says so on stderr, so a divide that survives edit 1 is somewhere
     else, and the log names whether this point was reached at all.
"""
import sys
from pathlib import Path

ASSIGN = "   program->workgroup_size = program->info.workgroup_size;\n"
ASSIGN_FIXED = (
    "   program->workgroup_size = program->info.workgroup_size;\n"
    "   /* PS5 Vulkan: 0 is not the \"unknown\" value this field documents -- it\n"
    "    * would make calc_waves_per_workgroup answer 0 and min_waves divide by\n"
    "    * zero (tooling/psbc/patch-aco-min-waves.py). */\n"
    "   if (program->workgroup_size == 0)\n"
    "      program->workgroup_size = UINT_MAX;\n"
)

DIVIDE = "   uint16_t sgprs = std::min(program->dev.physical_sgprs / waves, 128);\n"
DIVIDE_GUARDED = (
    "   /* PS5 Vulkan: this division is unguarded upstream and the standalone\n"
    "    * path can hand it a zero `waves`; the witness says whether the\n"
    "    * workgroup_size fix above was enough (tooling/psbc/patch-aco-min-waves.py). */\n"
    "   if (waves == 0) {\n"
    "      fprintf(stderr, \"[psbc] get_addr_regs_from_waves: 0 waves, using 1\\n\");\n"
    "      waves = 1;\n"
    "   }\n"
) + DIVIDE

INCLUDE = '#include "aco_ir.h"\n'
INCLUDE_WITH_CSTDIO = '#include "aco_ir.h"\n\n#include <cstdio>\n'


def patch_isel(path):
    text = path.read_text()
    if ASSIGN_FIXED in text:
        return 0
    if text.count(ASSIGN) != 1:
        raise SystemExit(f"{path}: expected one workgroup_size assignment")
    path.write_text(text.replace(ASSIGN, ASSIGN_FIXED))
    return 1


def patch_waves(path):
    text = path.read_text()
    if DIVIDE_GUARDED in text:
        return 0
    if text.count(DIVIDE) != 1:
        raise SystemExit(f"{path}: expected one get_addr_regs_from_waves divide")
    if text.count(INCLUDE) != 1:
        raise SystemExit(f"{path}: expected one aco_ir.h include")
    text = text.replace(INCLUDE, INCLUDE_WITH_CSTDIO)
    path.write_text(text.replace(DIVIDE, DIVIDE_GUARDED))
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-aco-min-waves.py <opengnm-psbc tree>")
    tree = Path(sys.argv[1])
    isel = patch_isel(tree / "src/amd/compiler/instruction_selection/aco_isel_setup.cpp")
    waves = patch_waves(tree / "src/amd/compiler/aco_live_var_analysis.cpp")
    print(
        f"aco waves: {isel} workgroup_size fix, {waves} zero-waves guard added to the "
        f"compiler work copy"
    )
