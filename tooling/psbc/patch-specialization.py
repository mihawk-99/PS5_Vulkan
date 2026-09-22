#!/usr/bin/env python3
# PS5 Vulkan - carry an application's specialization constants into the compiler.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

Specialization constants are core Vulkan 1.0: a pipeline stage names
`OpSpecConstant*` values by SpecId and the application supplies them in
`VkPipelineShaderStageCreateInfo::pSpecializationInfo`. The compiler already has
the whole mechanism, because it *is* RADV's front end: `radv_shader_spirv_to_nir`
(`src/amd/vulkan/radv_shader.c`) converts the stage's `spec_info` with
`vk_spec_info_to_nir_spirv` and hands the result to `spirv_to_nir`, whose
`SpvDecorationSpecId` and `OpSpecConstant*` handling applies it. What the
standalone path lacked is plumbing, in three places:

  1. `PsbcCompileOptions` had no field for the values. It gains four, appended at
     the end of the structure: the entry count, the entries -- `PsbcSpecializationEntry`,
     laid out exactly as `VkSpecializationMapEntry` (constant id, offset, size) --
     and the data blob with its size. Every caller in this repository compiles
     against the installed patched header and zero-initialises its options, so an
     appended field reads 0 -- no specialization -- wherever nobody sets it.

  2. `psbc_compile.c` never set `stage->spec_info`. It now points it at a
     `VkSpecializationInfo` built from those fields for the stage being compiled
     (not a paired previous stage), on the SPIR-V path only -- internal NIR has
     no SPIR-V to specialise -- after checking every entry lies inside the data.

  3. `psbc_stubs.c` stubbed `vk_spec_info_to_nir_spirv` to return NULL, because
     its home, Mesa's `src/vulkan/util/vk_util.c`, is not part of the standalone
     build. The stub becomes Mesa's own body (MIT), built on `spirv_to_nir.c`'s
     `vtn_alloc_specialization` and `vtn_add_specialization_entry`, which the
     archive already carries. The driver renames the symbol in its copy of the
     archive (`psbc_vk_spec_info_to_nir_spirv`, tools/build-driver.sh), so the
     compiler calls its own copy and the Vulkan runtime keeps its.

`PSBC_SHADER_METADATA_VERSION` is untouched: nothing here changes the metadata.
docs/M5_PHASE_C.md, R9 of the vkQuake port's requests.
"""
import sys
from pathlib import Path

ENTRY_ANCHOR = """typedef struct {
    PsbcTarget  target;
    PsbcStage   stage;
"""
ENTRY_PATCHED = """/* PS5 Vulkan: one specialization constant, laid out as VkSpecializationMapEntry
 * (tooling/psbc/patch-specialization.py): the SpecId, the offset of its value in
 * the data blob and the value's size in bytes. */
typedef struct {
    uint32_t    constant_id;
    uint32_t    offset;
    size_t      size;
} PsbcSpecializationEntry;

typedef struct {
    PsbcTarget  target;
    PsbcStage   stage;
"""

OPTIONS_ANCHOR = """    bool        compute_buffer_spills; /* Opt-in GFX10 MUBUF register spills. */
} PsbcCompileOptions;
"""
OPTIONS_PATCHED = """    bool        compute_buffer_spills; /* Opt-in GFX10 MUBUF register spills. */
    /* PS5 Vulkan: the stage's specialization constants, VkSpecializationInfo's
     * four fields (tooling/psbc/patch-specialization.py). 0 entries -- what a
     * zero-initialised options structure holds -- specialises nothing. Applied
     * on the SPIR-V path only. */
    uint32_t    specialization_entry_count;
    const PsbcSpecializationEntry* specialization_entries;
    size_t      specialization_data_size;
    const void* specialization_data;
} PsbcCompileOptions;
"""

APPLY_ANCHOR = """    debug_stage(input_nir ? "import-nir-begin" : "import-spirv-begin");
    nir_shader* nir = prepare_stage_nir(
        compiler_info, stage, spirv, spirv_size, input_nir, opts
    );
"""
APPLY_PATCHED = """    /* PS5 Vulkan: the application's specialization constants reach RADV's front
     * end as the stage's VkSpecializationInfo, which radv_shader_spirv_to_nir
     * converts and spirv_to_nir applies (tooling/psbc/patch-specialization.py).
     * The paired previous stage, if any, is another shader and takes none. */
    _Static_assert(sizeof(PsbcSpecializationEntry) == sizeof(VkSpecializationMapEntry) &&
                   offsetof(PsbcSpecializationEntry, constant_id) ==
                      offsetof(VkSpecializationMapEntry, constantID) &&
                   offsetof(PsbcSpecializationEntry, offset) ==
                      offsetof(VkSpecializationMapEntry, offset) &&
                   offsetof(PsbcSpecializationEntry, size) ==
                      offsetof(VkSpecializationMapEntry, size),
                   "PsbcSpecializationEntry must be VkSpecializationMapEntry's layout");
    VkSpecializationInfo psbc_spec_info = {0};
    if (opts->specialization_entry_count != 0) {
        if (input_nir || !opts->specialization_entries ||
            (opts->specialization_data_size != 0 && !opts->specialization_data)) {
            psbc_shutdown();
            return PSBC_RESULT_INVALID_ARGUMENT;
        }
        for (uint32_t i = 0; i < opts->specialization_entry_count; i++) {
            const PsbcSpecializationEntry* entry = &opts->specialization_entries[i];
            if (entry->offset > opts->specialization_data_size ||
                entry->size > opts->specialization_data_size - entry->offset) {
                psbc_shutdown();
                return PSBC_RESULT_INVALID_ARGUMENT;
            }
        }
        psbc_spec_info.mapEntryCount = opts->specialization_entry_count;
        psbc_spec_info.pMapEntries =
            (const VkSpecializationMapEntry*)opts->specialization_entries;
        psbc_spec_info.dataSize = opts->specialization_data_size;
        psbc_spec_info.pData = opts->specialization_data;
        stage->spec_info = &psbc_spec_info;
    }
    debug_stage(input_nir ? "import-nir-begin" : "import-spirv-begin");
    nir_shader* nir = prepare_stage_nir(
        compiler_info, stage, spirv, spirv_size, input_nir, opts
    );
    stage->spec_info = NULL;
"""

STUB_ANCHOR = """struct nir_spirv_specialization *vk_spec_info_to_nir_spirv(const VkSpecializationInfo *vk_spec_info)
{
   return NULL;
}
"""
STUB_PATCHED = """/* PS5 Vulkan: Mesa's own body (src/vulkan/util/vk_util.c, MIT), not a stub --
 * the standalone path hands it the stage's specialization constants
 * (tooling/psbc/patch-specialization.py). */
#include "compiler/spirv/nir_spirv.h"
struct nir_spirv_specialization *vk_spec_info_to_nir_spirv(const VkSpecializationInfo *vk_spec_info)
{
   if (vk_spec_info == NULL || vk_spec_info->mapEntryCount == 0)
      return NULL;

   struct nir_spirv_specialization *spec =
      vtn_alloc_specialization(vk_spec_info->mapEntryCount);
   if (!spec)
      return NULL;

   for (uint32_t i = 0; i < vk_spec_info->mapEntryCount; i++) {
      const VkSpecializationMapEntry vk_entry = vk_spec_info->pMapEntries[i];
      const void *vk_data = (const uint8_t *)vk_spec_info->pData + vk_entry.offset;
      if (!vtn_add_specialization_entry(spec, i, vk_entry.constantID,
                                        (uint32_t)vk_entry.size, vk_data, false)) {
         vtn_free_specialization(spec);
         return NULL;
      }
   }
   return spec;
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
    edits = replace_once(
        tree / "libpsbc/psbc_compile.h",
        ENTRY_ANCHOR,
        ENTRY_PATCHED,
        "} PsbcSpecializationEntry;",
        "options structure",
    )
    edits += replace_once(
        tree / "libpsbc/psbc_compile.h",
        OPTIONS_ANCHOR,
        OPTIONS_PATCHED,
        "specialization_entry_count;",
        "options structure end",
    )
    edits += replace_once(
        tree / "libpsbc/psbc_compile.c",
        APPLY_ANCHOR,
        APPLY_PATCHED,
        "psbc_spec_info",
        "stage import",
    )
    edits += replace_once(
        tree / "psbc_stubs.c",
        STUB_ANCHOR,
        STUB_PATCHED,
        "vtn_alloc_specialization(vk_spec_info->mapEntryCount)",
        "vk_spec_info_to_nir_spirv stub",
    )
    return edits


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-specialization.py <opengnm-psbc tree>")
    count = patch(Path(sys.argv[1]))
    print(f"specialization constants: {count} edits (metadata version stays 14)")
