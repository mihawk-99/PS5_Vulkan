#!/usr/bin/env python3
# PS5 Vulkan - a compute stage's subgroup is the size the device reports.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

Vulkan 1.1 reports one subgroup size, and a shader of SPIR-V 1.5 or older that
does not ask for another sees exactly that size in gl_SubgroupSize and has its
invocations grouped by it. The driver reports 32 (driver/ps5vk_physical_device.c),
compute's wave (`cs_wave_size`). In this tree `vk_set_subgroup_size`, where
RADV applies the API's subgroup size, is a stub that does nothing
(src/vulkan/runtime/vk_pipeline.h), so a shader's subgroup size stays
unconstrained and `radv_shader_choose_subgroup_size` falls back to RADV's wave
heuristics: wave64 for a compute stage whose workgroup is a multiple of 64 and
which uses subgroup-wide intrinsics -- which fits RADV, which reports 64 -- and
then reports that wave as the subgroup size. The console's r84-subgroup case
measured it: a workgroup of 64 was one subgroup of 64, one invocation elected,
every size and index check failing (R84, docs/M5_PHASE_C.md).

One edit: a stage with a workgroup that asks for no size of its own gets
`cs_wave_size` as its API, minimum and maximum subgroup size, set on the shader
where the stub would have, so its wave is exactly the size the device reports
whatever its workgroup or intrinsics. The graphics stages are untouched.
"""
import sys
from pathlib import Path

ANCHOR = """                        stage_key->subgroup_require_full);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
"""
PATCHED = """                        stage_key->subgroup_require_full);

   /* PS5 Vulkan (R84, tooling/psbc/patch-subgroup-size.py): vk_set_subgroup_size
    * is a stub in this tree, so a stage with a workgroup that asks for no size
    * gets the one the device reports, compute's wave, here. */
   if (mesa_shader_stage_uses_workgroup(nir->info.stage) && !rss_info.requiredSubgroupSize) {
      nir->info.api_subgroup_size = compiler_info->key.cs_wave_size;
      nir->info.min_subgroup_size = compiler_info->key.cs_wave_size;
      nir->info.max_subgroup_size = compiler_info->key.cs_wave_size;
   }

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
"""


def replace_once(path, anchor, patched, marker, what):
    text = path.read_text()
    if marker in text:
        return 0
    if text.count(anchor) != 1:
        raise SystemExit(f"{path}: expected one {what}, found {text.count(anchor)}")
    path.write_text(text.replace(anchor, patched))
    return 1


def patch(tree):
    edits = replace_once(tree / "src/amd/vulkan/radv_shader.c", ANCHOR, PATCHED,
                         "patch-subgroup-size.py): vk_set_subgroup_size", "subgroup size choice")
    return edits


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-subgroup-size.py <opengnm-psbc tree>")
    print(f"subgroup size: {patch(Path(sys.argv[1]))} edits")
