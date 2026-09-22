#!/usr/bin/env python3
# PS5 Vulkan - read a subpass input through its descriptor, not the tile.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

`subpassLoad` is core Vulkan 1.0, and this driver's input attachments are
descriptors like every other image it binds: a 32-byte image entry the draw
builds from the subpass's input attachment view (driver/ps5vk_draw.c). The
fork's RADV front end lowers the read the other way -- to the tile coordinate
intrinsic:

    NIR_PASS(_, nir, nir_lower_input_attachments,
             &(nir_input_attachment_options){ .use_ia_coord_intrin = true });

which turns `load_input_attachment` into `nir_load_input_attachment_coord(index)`
plus a load. This fork's ACO has no case for that intrinsic, so a shader that
reads a subpass input does not compile at all: on the console the port's
`postprocess_frag` reached

    ACO ERROR: aco_select_nir_intrinsics.cpp:5132
        Unimplemented intrinsic instr: div 32x3 %1 = @load_input_attachment_coord

and the title died (docs/M5_PHASE_C.md, R10).

`nir_lower_input_attachments` has the other form built in: with
`use_ia_coord_intrin` false, `load_coord` computes the coordinate from the
fragment's own position and layer and the read becomes an ordinary
`nir_texop_txf` through the image's **descriptor** -- which is exactly what a
driver that binds input attachments as descriptors wants, and what Intel's
driver does with the same option. The ACO path for that form is the texture
instruction path the compiler already uses for every other image read.

So the edit is one field. Nothing else in the pass changes, and a shader with no
input attachment is untouched by it: the pass rewrites only image loads whose
dimension is `GLSL_SAMPLER_DIM_SUBPASS(_MS)`.

`PSBC_SHADER_METADATA_VERSION` is untouched: nothing here changes the metadata.
The driver's half is `ps5vk_spirv_input_attachments` (driver/ps5vk_pipeline.c)
and the draw-time binding in driver/ps5vk_draw.c.
docs/M5_PHASE_C.md, R10 of the vkQuake port's requests.
"""
import sys
from pathlib import Path

ANCHOR = """      if (nir->info.stage == MESA_SHADER_FRAGMENT)
         NIR_PASS(_, nir, nir_lower_input_attachments,
                  &(nir_input_attachment_options){
                     .use_ia_coord_intrin = true,
                  });
"""

PATCHED = """      if (nir->info.stage == MESA_SHADER_FRAGMENT)
         /* PS5 Vulkan: the descriptor form of the subpass read, not the tile
          * coordinate intrinsic, which this fork's ACO cannot select
          * (tooling/psbc/patch-subpass-input.py). The driver binds an input
          * attachment as a 32-byte image entry and the read becomes a texel
          * fetch through it. */
         NIR_PASS(_, nir, nir_lower_input_attachments,
                  &(nir_input_attachment_options){
                     .use_ia_coord_intrin = false,
                  });
"""


def replace_once(path, anchor, replacement, marker, what):
    text = path.read_text()
    if marker in text:
        return 0
    if text.count(anchor) != 1:
        raise SystemExit(f"{path}: {what}: {text.count(anchor)} matches for the anchor")
    path.write_text(text.replace(anchor, replacement, 1))
    return 1


def patch(tree):
    return replace_once(
        tree / "src/amd/vulkan/radv_shader.c",
        ANCHOR,
        PATCHED,
        ".use_ia_coord_intrin = false,",
        "input attachment lowering",
    )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-subpass-input.py <opengnm-psbc tree>")
    count = patch(Path(sys.argv[1]))
    print(f"subpass input read: {count} edits (metadata version stays 14)")
