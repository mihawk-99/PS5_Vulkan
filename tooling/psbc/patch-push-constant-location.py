#!/usr/bin/env python3
# PS5 Vulkan - report where the compiler put an application's push constants.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

An application's SPIR-V that declares `layout(push_constant)` compiles to a load
through a **user-data dword the driver never writes**. The compiler's ABI takes
RADV's two forms for push constants (`src/amd/vulkan/radv_shader_args.c`): the
values inlined into user SGPRs (`AC_UD_INLINE_PUSH_CONSTANTS`, chosen when the
caller hands it an inline mask), or a 64-bit pointer to the data in a user-data
dword (`AC_UD_PUSH_CONSTANTS`). The standalone path passes no inline mask -- it
has no pipeline layout -- so it takes the pointer form, and
`PsbcShaderMetadata` reports every user-data location except that one, so the
driver has nothing to write and the shader reads an unwritten SGPR: a silent
zero, which is what the runner's v0-push-constant case measured on the console
(`docs/M5_PHASE_C.md`, R9).

Two edits, each with a job:

  1. `PsbcShaderMetadata` gains the location: `push_constant_valid` and
     `push_constant_user_data_dword` (the first of the two dwords the 64-bit
     pointer occupies), plus `push_constant_inline` and
     `push_constant_inline_count` so the other form is named rather than
     silently unsupported. The fields are appended at the end of the struct, so
     `PSBC_SHADER_METADATA_VERSION` stays 14 (see the note below).

  2. `psbc_compile.c` fills them beside the descriptor-set field it already
     computes from the same `user_sgprs_locs` table.

  3. And the arg has to exist for there to be a location: RADV's info pass
     decides `loads_push_constants` -- and with it whether the ABI declares
     `AC_UD_PUSH_CONSTANTS` -- by scanning for `load_push_constant`
     intrinsics, and `radv_postprocess_nir` (which lowers the push-constant
     *variable* into those intrinsics, `src/amd/vulkan/radv_shader.c:827`)
     runs *after* it in this standalone path. So the pointer was never
     declared, the lowering still produced loads, and ACO read the arg's
     SGPR anyway: an unwritten register, which is the silent zero. Hoisting
     the (idempotent) lowering above the info pass is the fix; the call in
     `radv_postprocess_nir` then finds nothing left to do.
"""
import sys
from pathlib import Path

# The version stays 14: the fields are appended, so a consumer that reads the v14
# prefix -- ps5-opengl's package writer, which the title build compiles against the
# SDK's own header -- keeps reading exactly what it read before. Bumping it made that
# writer refuse every shader ("the AGC package writer failed: -2") on the console while
# the host build, whose writer is compiled against the patched header, was happy, and
# the appended fields need no bump.
VERSION_KEEP = "#define PSBC_SHADER_METADATA_VERSION 14u"

FIELDS_ANCHOR = """    bool                 descriptor_set0_valid;
    uint32_t             descriptor_set0_user_data_dword;
"""
FIELDS_PATCHED = """    bool                 descriptor_set0_valid;
    uint32_t             descriptor_set0_user_data_dword;
    /* PS5 Vulkan: where an application's push constants reach the stage. The
     * standalone path takes RADV's pointer form (a 64-bit address in the first
     * two user-data dwords this names) because it passes no inline mask; the
     * inline fields are reported so the driver refuses that form by name
     * instead of writing nothing (tooling/psbc/patch-push-constant-location.py,
     * docs/M5_PHASE_C.md, R9). */
    bool                 push_constant_valid;
    uint32_t             push_constant_user_data_dword;
    uint32_t             push_constant_dword_count;
    bool                 push_constant_inline;
    uint32_t             push_constant_inline_count;
"""

HOIST_ANCHOR = """    radv_nir_shader_info_pass(
        compiler_info, nir, layout, &stage->key, gfx_state,
        mesa_stage == MESA_SHADER_COMPUTE ? RADV_PIPELINE_COMPUTE : RADV_PIPELINE_GRAPHICS,
        false, &stage->info
    );
"""
HOIST_PATCHED = """    /* PS5 Vulkan: the info pass below decides from the *lowered* NIR whether the
     * ABI declares the push-constant pointer (it scans for load_push_constant),
     * and radv_postprocess_nir does that lowering later in this standalone path,
     * so the arg was never declared while ACO still read its unwritten SGPR --
     * a silent zero instead of the application's values. The lowering is
     * idempotent, so doing it here is the whole fix
     * (tooling/psbc/patch-push-constant-location.py, docs/M5_PHASE_C.md, R9). */
    NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
             nir_address_format_32bit_offset);

    radv_nir_shader_info_pass(
        compiler_info, nir, layout, &stage->key, gfx_state,
        mesa_stage == MESA_SHADER_COMPUTE ? RADV_PIPELINE_COMPUTE : RADV_PIPELINE_GRAPHICS,
        false, &stage->info
    );
    /* PS5 Vulkan: and the pointer is the only form this driver can program.
     * RADV's info pass inlines a shader's push constants into user SGPRs when
     * there are few enough of them, which the driver has no way to write: it
     * programs one 64-bit address, and 128 bytes of push constants (32 dwords)
     * exceed the 16 user-data dwords a stage has. So the inline heuristic is
     * off here and every push-constant load goes through the pointer
     * (tooling/psbc/patch-push-constant-location.py, docs/M5_PHASE_C.md, R9). */
    stage->info.inline_push_constant_mask = 0;
    stage->info.can_inline_all_push_constants = false;
"""
ASSIGN_ANCHOR = """    if (has_vertex_inputs && ctx->rargs->ac.vertex_buffers.used) {
"""
ASSIGN_PATCHED = """    if (ctx->rargs->ac.push_constants.used &&
        ctx->rargs->user_sgprs_locs.shader_data[AC_UD_PUSH_CONSTANTS].sgpr_idx !=
            UINT32_MAX) {
        metadata->push_constant_valid = true;
        metadata->push_constant_user_data_dword =
            ctx->rargs->user_sgprs_locs
                .shader_data[AC_UD_PUSH_CONSTANTS].sgpr_idx;
        metadata->push_constant_dword_count =
            ctx->rargs->ac.args[ctx->rargs->ac.push_constants.arg_index].size;
    }
    if (ctx->rargs->ac.inline_push_const_mask) {
        metadata->push_constant_inline = true;
        metadata->push_constant_inline_count =
            (uint32_t)__builtin_popcountll(ctx->rargs->ac.inline_push_const_mask);
    }
    if (has_vertex_inputs && ctx->rargs->ac.vertex_buffers.used) {
"""


def patch_fields(path):
    text = path.read_text()
    # The marker is one field, not the whole block: the block grows when this
    # script grows, and an anchor that includes every field would insert a second
    # copy the moment one was added (which is exactly what happened once).
    if "push_constant_valid;" in text:
        return 0
    if text.count(FIELDS_ANCHOR) != 1:
        raise SystemExit(f"{path}: expected one descriptor_set0 field pair")
    path.write_text(text.replace(FIELDS_ANCHOR, FIELDS_PATCHED))
    return 1


def patch_hoist(path):
    text = path.read_text()
    if "stage->info.inline_push_constant_mask = 0;" in text:
        return 0
    if text.count(HOIST_ANCHOR) != 1:
        raise SystemExit(f"{path}: expected one shader-info pass")
    path.write_text(text.replace(HOIST_ANCHOR, HOIST_PATCHED))
    return 1


def patch_assign(path):
    text = path.read_text()
    if "metadata->push_constant_valid = true;" in text:
        return 0
    if text.count(ASSIGN_ANCHOR) != 1:
        raise SystemExit(f"{path}: expected one vertex-buffer-table assignment")
    path.write_text(text.replace(ASSIGN_ANCHOR, ASSIGN_PATCHED))
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-push-constant-location.py <opengnm-psbc tree>")
    tree = Path(sys.argv[1])
    fields = patch_fields(tree / "libpsbc/psbc_compile.h")
    hoist = patch_hoist(tree / "libpsbc/psbc_compile.c")
    assign = patch_assign(tree / "libpsbc/psbc_compile.c")
    print(
        f"push constants: {fields} struct fields (metadata version stays 14), "
        f"{hoist} lowering hoisted, {assign} location report added to the compiler work copy"
    )
