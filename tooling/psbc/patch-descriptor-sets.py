#!/usr/bin/env python3
# PS5 Vulkan - compile one descriptor set layout per descriptor set.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

An application is allowed four descriptor sets (`maxBoundDescriptorSets`), and a
program that uses more than one is refused by this driver today. The refusal is
not a hardware limit: RADV's ABI already declares **one descriptor-set pointer
per set bit** (`src/amd/vulkan/radv_shader_args.c`, `declare_global_input_sgprs`
-> `add_descriptor_set`), and its descriptor lowering indexes the caller's
layouts by set number (`layout->set[desc_set].layout`, `MAX_SETS` = 32,
`src/amd/vulkan/radv_constants.h`). What is single-set is this wrapper: it builds
one flat set-0 table, asserts `binding->set == 0`, and hands the ABI `num_sets =
1`.

The bindings a caller passes already carry their own set index
(`PsbcDescriptorBinding.set`), so what the wrapper has to do is split the table
per set, size each set from *that set's own* bindings -- a set may hold several
bindings, and two sets cannot share one table -- and report one pointer per set.

Edits, each with a job:

  1. `PSBC_MAX_DESCRIPTOR_SETS` is this wrapper's own cap. The core's is 32; the
     driver that consumes this advertises four. The wrapper is a general
     facility, so it does not take the driver's number, but its storage blob is
     a fixed array, so it does not take the core's either.

  2. `psbc_descriptor_options_valid` checks the set index against that cap
     instead of requiring 0, and bounds the **total** binding slots across sets:
     each set's table is indexed by its own binding numbers, so two sets may each
     name binding 127 while the caller's array holds two entries.

  3. `psbc_descriptor_layout` builds one layout per set, in one blob, each
     followed by its own binding table, each chunk rounded up to the layout's
     alignment. A set with no bindings gets an empty layout rather than a NULL
     pointer, so a shader that reads it fails in the lowering, not in a later
     dereference.

  4. Every set the caller binds gets its bit in `desc_set_used_mask` -- the mask
     the ABI turns into one pointer per set -- instead of only set 0.

  5. `PsbcShaderMetadata` reports one user-data dword per set, appended at the
     end of the struct (version stays 14, see the note below). Set 0 repeats the
     existing singular fields so a reader built against the v14 prefix keeps
     working.

`PSBC_SHADER_METADATA_VERSION` stays 14: the fields are appended, and a consumer
compiled against 14 -- ps5-opengl's AGC package writer, built from the SDK's own
header -- reads the v14 prefix unchanged. Bumping it made that writer refuse
every shader ("the AGC package writer failed: -2") while the host build, whose
writer is compiled against this header, was happy.
"""
import sys
from pathlib import Path

# 2. The wrapper's own cap, beside the binding budget it shares a blob with.
SETS_ANCHOR = """#define PSBC_MAX_DESCRIPTOR_BINDINGS 128
"""
SETS_PATCHED = """#define PSBC_MAX_DESCRIPTOR_BINDINGS 128
/* PS5 Vulkan: how many descriptor sets this wrapper compiles for, and how many
 * binding slots it will hold across all of them. Both numbers are the wrapper's
 * own; neither is a hardware or API limit.
 *
 * The core allows 32 sets (MAX_SETS, src/amd/vulkan/radv_constants.h) and the
 * driver that consumes this advertises four
 * (VkPhysicalDeviceLimits.maxBoundDescriptorSets). This cap is neither: it is
 * the size of a fixed blob (PSBC_DESCRIPTOR_LAYOUT_STORAGE_BYTES below), chosen
 * so the blob stays small. **It is not a measurement.** The measurement that
 * does bound sets is the ABI's user data: RADV declares one user-data dword per
 * set (add_descriptor_set, src/amd/vulkan/radv_shader_args.c) out of the 32
 * user SGPRs a non-compute stage has -- 16 for compute -- shared with every
 * other argument the stage takes, and `driver/tests/vk_psbc_multiset_test.c`
 * measures the per-set cost on the host. When the pointers stop fitting, RADV
 * does not fail: `remaining_sgprs < num_desc_set` switches the whole ABI to its
 * *indirect* descriptor form (radv_shader_args.c:1013), which this driver does
 * not implement -- and no per-set pointer is declared, so the metadata reports
 * none and the driver refuses it by name rather than binding a set nowhere.
 *
 * The slot budget is the caller's own `descriptor_bindings[]` array length
 * (PsbcCompileOptions), a single-set cap before this patch and now a total
 * across sets: each set's table runs to its own highest binding number, so the
 * sum is what the blob holds. A caller that spreads many bindings over several
 * sets can reach 128 sooner than a per-set cap would allow; the consumer this
 * driver has uses about eight slots across three sets, so it is not close.
 * See tooling/psbc/patch-descriptor-sets.py, docs/M5_PHASE_C.md, R7. */
#define PSBC_MAX_DESCRIPTOR_SETS 8
"""

# 5. One pointer per set, appended to the metadata (version stays 14).
METADATA_ANCHOR = """    uint32_t             descriptor_binding_count;
    PsbcDescriptorBinding descriptor_bindings[PSBC_MAX_DESCRIPTOR_BINDINGS];
    bool                 base_vertex_valid;
"""
METADATA_PATCHED = """    /* PS5 Vulkan: where each descriptor set's pointer reached the stage. The
     * ABI declares one user-data dword per set bit, so a program that binds more
     * than set 0 has more than one pointer, and one field cannot describe them.
     * Index 0 repeats `descriptor_set0_*` above for readers built against the
     * v14 prefix; entries are valid only where `descriptor_sets_valid` is set
     * (tooling/psbc/patch-descriptor-sets.py, docs/M5_PHASE_C.md, R7). */
    bool                 descriptor_sets_valid[PSBC_MAX_DESCRIPTOR_SETS];
    uint32_t             descriptor_sets_user_data_dword[PSBC_MAX_DESCRIPTOR_SETS];
    uint32_t             descriptor_binding_count;
    PsbcDescriptorBinding descriptor_bindings[PSBC_MAX_DESCRIPTOR_BINDINGS];
    bool                 base_vertex_valid;
"""

# 2. Validate against the wrapper's set cap and the shared slot budget.
VALIDATE_ANCHOR = """static bool psbc_descriptor_options_valid(const PsbcCompileOptions* opts) {
    if (opts->descriptor_binding_count > PSBC_MAX_DESCRIPTOR_BINDINGS)
        return false;
    for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i) {
        const PsbcDescriptorBinding* binding = &opts->descriptor_bindings[i];
        const bool valid_type =
            binding->type == PSBC_DESCRIPTOR_UNIFORM_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
            binding->type == PSBC_DESCRIPTOR_STORAGE_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_STORAGE_IMAGE;
        const uint32_t expected_stride =
            binding->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ? 48u :
            binding->type == PSBC_DESCRIPTOR_STORAGE_IMAGE ? 32u :
            16u;
        if (binding->set != 0 || binding->binding >= PSBC_MAX_DESCRIPTOR_BINDINGS ||
            !valid_type || !binding->array_size ||
            binding->stride != expected_stride ||
            (binding->offset & 15u) ||
            (uint64_t)binding->offset +
                    (uint64_t)binding->array_size * binding->stride >
                UINT32_MAX)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (opts->descriptor_bindings[j].set == binding->set &&
                opts->descriptor_bindings[j].binding == binding->binding)
                return false;
    }
    return true;
}
"""
VALIDATE_PATCHED = """static bool psbc_descriptor_options_valid(const PsbcCompileOptions* opts) {
    if (opts->descriptor_binding_count > PSBC_MAX_DESCRIPTOR_BINDINGS)
        return false;
    for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i) {
        const PsbcDescriptorBinding* binding = &opts->descriptor_bindings[i];
        const bool valid_type =
            binding->type == PSBC_DESCRIPTOR_UNIFORM_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
            binding->type == PSBC_DESCRIPTOR_STORAGE_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_STORAGE_IMAGE;
        const uint32_t expected_stride =
            binding->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ? 48u :
            binding->type == PSBC_DESCRIPTOR_STORAGE_IMAGE ? 32u :
            16u;
        if (binding->set >= PSBC_MAX_DESCRIPTOR_SETS ||
            binding->binding >= PSBC_MAX_DESCRIPTOR_BINDINGS ||
            !valid_type || !binding->array_size ||
            binding->stride != expected_stride ||
            (binding->offset & 15u) ||
            (uint64_t)binding->offset +
                    (uint64_t)binding->array_size * binding->stride >
                UINT32_MAX)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (opts->descriptor_bindings[j].set == binding->set &&
                opts->descriptor_bindings[j].binding == binding->binding)
                return false;
    }
    /* The binding budget is a total across sets, not a per-set cap: each set's
     * table is sized by its own highest binding number, so a caller may name
     * binding 127 in two sets from two array entries. What has to fit is the sum
     * of those tables, because it is what the layout blob holds. The number is
     * PSBC_MAX_DESCRIPTOR_BINDINGS -- the caller's own array length, and the old
     * single-set cap, now spent across sets -- so 128 slots is a total where it
     * used to be a per-set limit (tooling/psbc/patch-descriptor-sets.py, R7). */
    uint32_t table_slots = 0;
    for (uint32_t set = 0; set < PSBC_MAX_DESCRIPTOR_SETS; ++set) {
        uint32_t highest_slot = 0;
        for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i)
            if (opts->descriptor_bindings[i].set == set)
                highest_slot = MAX2(highest_slot,
                                    (uint32_t)opts->descriptor_bindings[i].binding + 1u);
        table_slots += highest_slot;
    }
    return table_slots <= PSBC_MAX_DESCRIPTOR_BINDINGS;
}
"""

# 3 and 4. The layout builder, its storage size, and the used-set mask.
LAYOUT_ANCHOR = """static void psbc_descriptor_layout(const PsbcCompileOptions* opts,
                                   void* descriptor_set0_storage,
                                   struct radv_shader_layout* layout) {
    if (opts->descriptor_binding_count) {
        struct radv_descriptor_set_layout* set_layout =
            (struct radv_descriptor_set_layout*)descriptor_set0_storage;
        uint32_t binding_count = 0;
        uint32_t set_size = 0;
        for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i) {
            const PsbcDescriptorBinding* source =
                &opts->descriptor_bindings[i];
            struct radv_descriptor_set_binding_layout* target =
                &set_layout->binding[source->binding];
            target->type = source->type == PSBC_DESCRIPTOR_UNIFORM_BUFFER
                               ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                           : source->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER
                               ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                           : source->type == PSBC_DESCRIPTOR_STORAGE_BUFFER
                               ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                           : source->type == PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER
                               ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                           : source->type == PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER
                               ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                           : source->type == PSBC_DESCRIPTOR_STORAGE_IMAGE
                               ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            target->array_size = source->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER
                ? contiguous_sampler_count(opts, source) : source->array_size;
            target->offset = source->offset;
            target->size = source->stride;
            binding_count = MAX2(binding_count, source->binding + 1u);
            set_size = MAX2(set_size, source->offset +
                                      source->array_size * source->stride);
        }
        set_layout->binding_count = binding_count;
        set_layout->size = set_size;
        layout->num_sets = 1;
        layout->set[0].layout = set_layout;
    }
}
"""
LAYOUT_PATCHED = """/* PS5 Vulkan: one descriptor set layout per set, in one blob, each followed by
 * that set's own binding table. RADV's lowering looks a set's layout up by set
 * number (`layout->set[desc_set].layout`, src/amd/vulkan/radv_shader.c), so a
 * program with several sets needs several layouts, and each has to be sized
 * from its own bindings: a set is not limited to one binding, and two sets
 * cannot share one table (tooling/psbc/patch-descriptor-sets.py, R7). */
#define PSBC_DESCRIPTOR_LAYOUT_ALIGN ((uint32_t)_Alignof(struct radv_descriptor_set_layout))
/* Worst case: every set present, every binding slot used. Each chunk is rounded
 * up to the layout's alignment because a binding table's size (a multiple of
 * four) need not be a multiple of the layout's. */
#define PSBC_DESCRIPTOR_LAYOUT_STORAGE_BYTES \\
    (PSBC_MAX_DESCRIPTOR_SETS * \\
         (sizeof(struct radv_descriptor_set_layout) + PSBC_DESCRIPTOR_LAYOUT_ALIGN) + \\
     PSBC_MAX_DESCRIPTOR_BINDINGS * sizeof(struct radv_descriptor_set_binding_layout))

/* PS5 Vulkan: the ABI declares one descriptor-set pointer per set bit
 * (`declare_global_input_sgprs`, src/amd/vulkan/radv_shader_args.c), so every
 * set the caller binds has to be named here -- not just set 0
 * (tooling/psbc/patch-descriptor-sets.py, R7). */
static uint32_t psbc_descriptor_set_mask(const PsbcCompileOptions* opts) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i)
        mask |= 1u << opts->descriptor_bindings[i].set;
    return mask;
}

static void psbc_descriptor_layout(const PsbcCompileOptions* opts,
                                   void* descriptor_set_storage,
                                   struct radv_shader_layout* layout) {
    if (!opts->descriptor_binding_count)
        return;
    uint8_t* storage = (uint8_t*)descriptor_set_storage;
    uint32_t highest_set = 0;
    for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i)
        highest_set = MAX2(highest_set, (uint32_t)opts->descriptor_bindings[i].set);
    for (uint32_t set = 0; set <= highest_set; ++set) {
        /* A set with no bindings still gets a layout: the lowering reads
         * layout->set[set].layout for every set the shader dereferences, and an
         * empty layout reports the missing binding instead of faulting. */
        struct radv_descriptor_set_layout* set_layout =
            (struct radv_descriptor_set_layout*)storage;
        uint32_t binding_count = 0;
        uint32_t set_size = 0;
        for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i) {
            const PsbcDescriptorBinding* source =
                &opts->descriptor_bindings[i];
            if (source->set != set)
                continue;
            struct radv_descriptor_set_binding_layout* target =
                &set_layout->binding[source->binding];
            target->type = source->type == PSBC_DESCRIPTOR_UNIFORM_BUFFER
                               ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                           : source->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER
                               ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                           : source->type == PSBC_DESCRIPTOR_STORAGE_BUFFER
                               ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                           : source->type == PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER
                               ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                           : source->type == PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER
                               ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                           : source->type == PSBC_DESCRIPTOR_STORAGE_IMAGE
                               ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            target->array_size = source->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER
                ? contiguous_sampler_count(opts, source) : source->array_size;
            target->offset = source->offset;
            target->size = source->stride;
            binding_count = MAX2(binding_count, source->binding + 1u);
            set_size = MAX2(set_size, source->offset +
                                      source->array_size * source->stride);
        }
        set_layout->binding_count = binding_count;
        set_layout->size = set_size;
        layout->set[set].layout = set_layout;
        layout->num_sets = set + 1u;
        const uint32_t chunk = sizeof(struct radv_descriptor_set_layout) +
            binding_count * sizeof(struct radv_descriptor_set_binding_layout);
        storage += (chunk + PSBC_DESCRIPTOR_LAYOUT_ALIGN - 1u) &
                   ~(PSBC_DESCRIPTOR_LAYOUT_ALIGN - 1u);
    }
}
"""

# The linked path's blob and its call.
LINKED_STORAGE_ANCHOR = """    _Alignas(struct radv_descriptor_set_layout)
        uint8_t descriptor_set0_storage[sizeof(struct radv_descriptor_set_layout) +
            PSBC_MAX_DESCRIPTOR_BINDINGS * sizeof(struct radv_descriptor_set_binding_layout)] = {0};
    psbc_descriptor_layout(&options->vertex, descriptor_set0_storage, &layout);
"""
LINKED_STORAGE_PATCHED = """    _Alignas(struct radv_descriptor_set_layout)
        uint8_t descriptor_set_storage[PSBC_DESCRIPTOR_LAYOUT_STORAGE_BYTES] = {0};
    psbc_descriptor_layout(&options->vertex, descriptor_set_storage, &layout);
"""

# The standalone path's blob, alias and call.
SCRATCH_STORAGE_ANCHOR = """    _Alignas(struct radv_descriptor_set_layout)
        uint8_t descriptor_set0_storage[
            sizeof(struct radv_descriptor_set_layout) +
            PSBC_MAX_DESCRIPTOR_BINDINGS *
                sizeof(struct radv_descriptor_set_binding_layout)];
"""
SCRATCH_STORAGE_PATCHED = """    _Alignas(struct radv_descriptor_set_layout)
        uint8_t descriptor_set_storage[PSBC_DESCRIPTOR_LAYOUT_STORAGE_BYTES];
"""
ALIAS_ANCHOR = """    uint8_t *descriptor_set0_storage = scratch->descriptor_set0_storage;
"""
ALIAS_PATCHED = """    uint8_t *descriptor_set_storage = scratch->descriptor_set_storage;
"""
CALL_ANCHOR = """    psbc_descriptor_layout(opts, descriptor_set0_storage, layout);
"""
CALL_PATCHED = """    psbc_descriptor_layout(opts, descriptor_set_storage, layout);
"""

# 4. The used-set mask, in the linked and the standalone path.
LINKED_MASK_ANCHOR = """        if (options->vertex.descriptor_binding_count)
            s->info.desc_set_used_mask |= 1u;
"""
LINKED_MASK_PATCHED = """        s->info.desc_set_used_mask |= psbc_descriptor_set_mask(&options->vertex);
"""
STANDALONE_MASK_ANCHOR = """    /* Legacy Gallium texture indices carry no Vulkan deref for RADV's info
     * pass to discover. The explicit PSBC layout still requires set 0. */
    if (opts->descriptor_binding_count)
        stage->info.desc_set_used_mask |= 1u;
    if (paired_geometry && opts->descriptor_binding_count)
        previous->info.desc_set_used_mask |= 1u;
"""
STANDALONE_MASK_PATCHED = """    /* Legacy Gallium texture indices carry no Vulkan deref for RADV's info
     * pass to discover. The explicit PSBC layouts still require every set the
     * caller bound. */
    const uint32_t descriptor_set_mask = psbc_descriptor_set_mask(opts);
    stage->info.desc_set_used_mask |= descriptor_set_mask;
    if (paired_geometry)
        previous->info.desc_set_used_mask |= descriptor_set_mask;
"""

# 5. Report one location per set.
REPORT_ANCHOR = """    if (ctx->rargs->descriptors[0].used &&
        ctx->rargs->user_sgprs_locs.descriptor_sets[0].sgpr_idx !=
            UINT32_MAX) {
        metadata->descriptor_set0_valid = true;
        metadata->descriptor_set0_user_data_dword =
            ctx->rargs->user_sgprs_locs.descriptor_sets[0].sgpr_idx;
    }
"""
REPORT_PATCHED = """    for (uint32_t set = 0; set < PSBC_MAX_DESCRIPTOR_SETS; ++set) {
        if (!ctx->rargs->descriptors[set].used ||
            !(ctx->rargs->user_sgprs_locs.descriptor_sets_enabled & (1u << set)) ||
            ctx->rargs->user_sgprs_locs.descriptor_sets[set].sgpr_idx ==
                UINT32_MAX)
            continue;
        metadata->descriptor_sets_valid[set] = true;
        metadata->descriptor_sets_user_data_dword[set] =
            ctx->rargs->user_sgprs_locs.descriptor_sets[set].sgpr_idx;
        if (set == 0) {
            metadata->descriptor_set0_valid = true;
            metadata->descriptor_set0_user_data_dword =
                metadata->descriptor_sets_user_data_dword[0];
        }
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


def patch_header(path):
    edits = replace_once(
        path,
        SETS_ANCHOR,
        SETS_PATCHED,
        "#define PSBC_MAX_DESCRIPTOR_SETS ",
        "descriptor binding budget",
    )
    edits += replace_once(
        path,
        METADATA_ANCHOR,
        METADATA_PATCHED,
        "descriptor_sets_valid[",
        "metadata descriptor-binding block",
    )
    return edits


def patch_source(path):
    edits = replace_once(
        path, VALIDATE_ANCHOR, VALIDATE_PATCHED, "table_slots", "descriptor option validator"
    )
    edits += replace_once(
        path,
        LAYOUT_ANCHOR,
        LAYOUT_PATCHED,
        "psbc_descriptor_set_mask",
        "descriptor layout builder",
    )
    edits += replace_once(
        path,
        LINKED_STORAGE_ANCHOR,
        LINKED_STORAGE_PATCHED,
        "descriptor_set_storage[PSBC_DESCRIPTOR_LAYOUT_STORAGE_BYTES] = {0}",
        "linked descriptor storage",
    )
    edits += replace_once(
        path,
        SCRATCH_STORAGE_ANCHOR,
        SCRATCH_STORAGE_PATCHED,
        "descriptor_set_storage[PSBC_DESCRIPTOR_LAYOUT_STORAGE_BYTES];",
        "standalone descriptor storage",
    )
    edits += replace_once(
        path, ALIAS_ANCHOR, ALIAS_PATCHED, "scratch->descriptor_set_storage", "storage alias"
    )
    edits += replace_once(
        path, CALL_ANCHOR, CALL_PATCHED, "psbc_descriptor_layout(opts, descriptor_set_storage",
        "standalone layout call",
    )
    edits += replace_once(
        path,
        LINKED_MASK_ANCHOR,
        LINKED_MASK_PATCHED,
        "psbc_descriptor_set_mask(&options->vertex)",
        "linked used-set mask",
    )
    edits += replace_once(
        path,
        STANDALONE_MASK_ANCHOR,
        STANDALONE_MASK_PATCHED,
        "descriptor_set_mask = psbc_descriptor_set_mask(opts)",
        "standalone used-set mask",
    )
    edits += replace_once(
        path,
        REPORT_ANCHOR,
        REPORT_PATCHED,
        "metadata->descriptor_sets_valid[set] = true;",
        "descriptor-set location report",
    )
    return edits


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-descriptor-sets.py <opengnm-psbc tree>")
    tree = Path(sys.argv[1])
    header = patch_header(tree / "libpsbc/psbc_compile.h")
    source = patch_source(tree / "libpsbc/psbc_compile.c")
    print(
        f"descriptor sets: {header} header edits, {source} compiler edits "
        f"(metadata version stays 14)"
    )
