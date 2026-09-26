#!/usr/bin/env python3
# PS5 Vulkan - a multiview pipeline's view index, and where it reaches a stage.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

Vulkan 1.1 requires multiview (R84, docs/M5_PHASE_C.md): a render pass whose
subpass has a view mask draws every draw once per view, and a shader reads the
view it is drawing as `gl_ViewIndex`. The compiler is RADV's front end, which
already declares the index as a user-data argument (`AC_UD_VIEW_INDEX`,
`src/amd/vulkan/radv_shader_args.c`) and lowers the intrinsic to it
(`ac_nir_lower_intrinsics_to_args`) -- but the standalone path asks
`radv_shader_spirv_to_nir` to lower every view index to zero, which is right
only outside a multiview pass, and `PsbcShaderMetadata` does not say where the
argument went, so the driver would have nothing to write. Five edits:

  1. `PsbcCompileOptions` gains `multiview`, appended at the end: the pipeline
     renders into a subpass with a view mask. False, what a zero-initialised
     options structure holds, keeps the lowering to zero -- a view index outside
     a multiview pass is zero, and the stage spends no user SGPR on it.

  2. The stage import lowers the view index to zero only without `multiview`.

  3. `PsbcShaderMetadata` gains `view_index_valid` and
     `view_index_user_data_dword`, appended at the end (the version stays 14,
     as tooling/psbc/patch-push-constant-location.py explains), filled from the
     same `user_sgprs_locs` table as the other locations.

  4. The SPIR-V capabilities the front end accepts gain `MultiView`, which a
     stage reading `gl_ViewIndex` declares, and `GroupNonUniform`, the basic
     subgroup operations Vulkan 1.1 requires in compute. The front end warned
     "Unsupported SPIR-V capability" for each while RADV's own list, which the
     standalone path's replaces, has both.

  5. A pixel stage that reads `gl_ViewIndex` gets the same user-data argument
     the vertex stages have. RADV declares none there: its pipeline code
     rewrites a pixel stage's view index into a read of the layer the last
     vertex stage exports (`radv_nir_lower_view_index`), which the standalone
     path never runs, so the intrinsic reached `ac_nir_lower_intrinsics_to_args`
     with no argument and the compile aborted. A driver that replays each draw
     per view (driver/ps5vk_draw.c) knows the view when it writes the pixel
     stage's user data too, and needs no layer export for it.
"""
import sys
from pathlib import Path

OPTIONS_ANCHOR = """    const void* specialization_data;
} PsbcCompileOptions;
"""
OPTIONS_PATCHED = """    const void* specialization_data;
    /* PS5 Vulkan: the pipeline draws into a subpass with a view mask, so a
     * stage's gl_ViewIndex is the view being drawn, which the driver writes into
     * the user-data dword the metadata names; false lowers it to zero
     * (tooling/psbc/patch-view-index.py, docs/M5_PHASE_C.md, R84). */
    bool        multiview;
} PsbcCompileOptions;
"""

METADATA_ANCHOR = """    uint32_t             compute_private_stride; /* Ordinary storage; reserved user words 0/1. */
} PsbcShaderMetadata;
"""
METADATA_PATCHED = """    uint32_t             compute_private_stride; /* Ordinary storage; reserved user words 0/1. */
    /* PS5 Vulkan: where a multiview stage's view index reaches it
     * (tooling/psbc/patch-view-index.py, R84). */
    bool                 view_index_valid;
    uint32_t             view_index_user_data_dword;
} PsbcShaderMetadata;
"""

LOWER_ANCHOR = """    const struct radv_spirv_to_nir_options spirv_options = {
        .lower_view_index_to_zero = true,
        .lower_view_index_to_device_index = false,
    };
"""
LOWER_PATCHED = """    /* PS5 Vulkan: a multiview pipeline keeps gl_ViewIndex, which RADV's ABI
     * then declares as AC_UD_VIEW_INDEX (tooling/psbc/patch-view-index.py). */
    const struct radv_spirv_to_nir_options spirv_options = {
        .lower_view_index_to_zero = !opts->multiview,
        .lower_view_index_to_device_index = false,
    };
"""

FILL_ANCHOR = """    if (has_vertex_inputs && ctx->rargs->instance_id_bias.used) {"""
FILL_PATCHED = """    if (ctx->rargs->ac.view_index.used) {
        metadata->view_index_valid = true;
        metadata->view_index_user_data_dword =
            ctx->rargs->user_sgprs_locs.shader_data[AC_UD_VIEW_INDEX].sgpr_idx;
    }
    if (has_vertex_inputs && ctx->rargs->instance_id_bias.used) {"""


CAPS_ANCHOR = """    compiler_info->spirv_caps.Shader = true;
"""
CAPS_PATCHED = """    compiler_info->spirv_caps.Shader = true;
    /* PS5 Vulkan (R84, tooling/psbc/patch-view-index.py): Vulkan 1.1's
     * multiview and basic subgroup operations. */
    compiler_info->spirv_caps.MultiView = true;
    compiler_info->spirv_caps.GroupNonUniform = true;
"""


PS_ARGS_ANCHOR = """   case MESA_SHADER_FRAGMENT:
      declare_global_input_sgprs(state, gfx_level, info, user_sgpr_info);
"""
PS_ARGS_PATCHED = """   case MESA_SHADER_FRAGMENT:
      declare_global_input_sgprs(state, gfx_level, info, user_sgpr_info);

      /* PS5 Vulkan (R84, tooling/psbc/patch-view-index.py): the view index as
       * user data, which the driver writes per view, in place of RADV's read
       * of the layer the vertex stage exports. */
      if (info->uses_view_index) {
         RADV_ADD_UD_ARG(state, 1, AC_ARG_VALUE, ac.view_index, AC_UD_VIEW_INDEX);
      }
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
    edits = replace_once(tree / "libpsbc/psbc_compile.h", OPTIONS_ANCHOR, OPTIONS_PATCHED,
                         "    bool        multiview;", "options structure end")
    edits += replace_once(tree / "libpsbc/psbc_compile.h", METADATA_ANCHOR, METADATA_PATCHED,
                          "view_index_user_data_dword;", "metadata structure end")
    edits += replace_once(tree / "libpsbc/psbc_compile.c", LOWER_ANCHOR, LOWER_PATCHED,
                          ".lower_view_index_to_zero = !opts->multiview", "view index lowering")
    edits += replace_once(tree / "libpsbc/psbc_compile.c", FILL_ANCHOR, FILL_PATCHED,
                          "shader_data[AC_UD_VIEW_INDEX]", "metadata fill")
    edits += replace_once(tree / "libpsbc/psbc_compile.c", CAPS_ANCHOR, CAPS_PATCHED,
                          "spirv_caps.MultiView = true", "SPIR-V capabilities")
    edits += replace_once(tree / "src/amd/vulkan/radv_shader_args.c", PS_ARGS_ANCHOR,
                          PS_ARGS_PATCHED, "in place of RADV's read", "pixel stage arguments")
    return edits


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-view-index.py <opengnm-psbc tree>")
    count = patch(Path(sys.argv[1]))
    print(f"view index: {count} edits (metadata version stays 14)")
