/*
 * PS5 Vulkan driver - NIR shader stages.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C1b (docs/M5_PHASE_C.md). Mesa's vk_meta hands pipeline
 * stages to the driver as NIR rather than SPIR-V
 * (VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA), and the
 * shader compiler takes NIR through psbc_compile_nir. Two things must change
 * in that NIR before the compiler sees it.
 *
 * 1. Push constants become a uniform buffer. PsbcShaderMetadata reports every
 *    user-data location the compiler assigns except the push-constant one, so
 *    a shader the compiler placed push constants in would read a user SGPR the
 *    driver never wrote: an unmapped pointer, and a GPU fault. The set-0
 *    descriptor table, its 16-byte uniform-buffer entries and the pixel user
 *    data that names it are recorded instead (probes/m3/bindings.txt), so the
 *    driver lowers push constants to a load from one reserved binding and
 *    fills that buffer at the draw (ps5vk_draw.c). The load is written in the
 *    form libpsbc converts at its standalone NIR boundary
 *    (lower_gallium_ubo_index): a load_ubo whose buffer index is a single
 *    constant, which becomes set 0, binding PSBC_GALLIUM_UBO_BINDING_BASE
 *    plus that constant.
 * 2. The layer output goes away. vk_meta's rectangle vertex shader always
 *    writes gl_Layer, and the driver renders one layer
 *    (vkCmdBeginRendering refuses layerCount != 1), so the value is always 0.
 *    Dropping the store keeps the vertex stage to what the c1-clear probe
 *    recorded on the console: PA_CL_VS_OUT_CNTL 0, with no render-target-index
 *    export.
 *
 * The caller owns the NIR vk_meta built, so the driver clones it first;
 * libpsbc clones again before it lowers anything.
 */

#include "ps5vk_private.h"
#include "ps5vk_shader_cache.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_serialize.h"

/* Removes every store to the shader's layer output, then the variable. */
static bool
ps5vk_nir_drop_layer_store(nir_builder *b, nir_intrinsic_instr *intrinsic, void *data)
{
   const nir_variable *const layer = data;
   (void)b;
   if (intrinsic->intrinsic != nir_intrinsic_store_deref)
      return false;
   nir_deref_instr *const deref = nir_src_as_deref(intrinsic->src[0]);
   if (!deref || nir_deref_instr_get_variable(deref) != layer)
      return false;
   nir_instr_remove(&intrinsic->instr);
   return true;
}

static bool
ps5vk_nir_drop_layer_output(nir_shader *nir)
{
   nir_variable *layer = NULL;
   nir_foreach_shader_out_variable (variable, nir) {
      if (variable->data.location == VARYING_SLOT_LAYER)
         layer = variable;
   }
   if (!layer)
      return false;

   nir_shader_intrinsics_pass(nir, ps5vk_nir_drop_layer_store, nir_metadata_control_flow, layer);
   exec_node_remove(&layer->node);
   nir->info.outputs_written &= ~VARYING_BIT_LAYER;
   /* The derefs that named the variable are dead now, and libpsbc clones this
    * shader again before it lowers anything: a deref whose variable is no
    * longer in the shader's list trips nir_clone's remap assertion. Drop
    * them, so nothing refers to the removed variable. */
   while (nir_opt_dce(nir))
      ;
   return true;
}

/* load_push_constant, whose offset nir_lower_explicit_io has already made a
 * byte offset, into a load from the reserved uniform-buffer binding. */
static bool
ps5vk_nir_push_constant_to_ubo(nir_builder *b, nir_intrinsic_instr *intrinsic, void *data)
{
   const uint32_t *const bytes = data;
   if (intrinsic->intrinsic != nir_intrinsic_load_push_constant)
      return false;

   b->cursor = nir_before_instr(&intrinsic->instr);
   nir_def *const offset =
      nir_iadd_imm(b, intrinsic->src[0].ssa, nir_intrinsic_base(intrinsic));
   nir_def *const value = nir_load_ubo(
      b, intrinsic->def.num_components, intrinsic->def.bit_size,
      nir_imm_int(b, PS5VK_PUSH_CONSTANT_SLOT), offset, .align_mul = 4, .align_offset = 0,
      .range_base = 0, .range = *bytes);
   nir_def_replace(&intrinsic->def, value);
   return true;
}

nir_shader *
ps5vk_nir_prepare(const nir_shader *source, uint32_t push_constant_bytes)
{
   nir_shader *const nir = nir_shader_clone(NULL, source);
   if (!nir)
      return NULL;

   /* vk_meta builds its shaders without compiler options, and the passes
    * below read them (nir_lower_explicit_io's addressing limits): the
    * compiler's immutable options for the stage are the ones the rest of the
    * path compiles with. libpsbc replaces them with the same ones when it
    * takes the NIR. */
   nir->options = psbc_get_nir_options(nir->info.stage == MESA_SHADER_VERTEX
                                          ? PSBC_STAGE_VERTEX
                                          : PSBC_STAGE_FRAGMENT);

   if (nir->info.stage == MESA_SHADER_VERTEX)
      ps5vk_nir_drop_layer_output(nir);
   if (push_constant_bytes != 0) {
      /* The same lowering RADV runs before it reaches the compiler, so the
       * offsets libpsbc sees are the ones it would have produced itself.
       * Mesa's NIR_PASS macro keeps a progress variable the driver's -Werror
       * build would flag, so the passes are called directly. */
      (void)nir_lower_explicit_io(nir, nir_var_mem_push_const,
                                  nir_address_format_32bit_offset);
      (void)nir_shader_intrinsics_pass(nir, ps5vk_nir_push_constant_to_ubo,
                                       nir_metadata_control_flow, &push_constant_bytes);
   }
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   return nir;
}

/* AGC links both packages. Like RADV's noop FS, an empty fragment shader
 * leaves rasterized depth and fixed-function stencil intact without exports. */
nir_shader *
ps5vk_nir_noop_fragment(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, psbc_get_nir_options(PSBC_STAGE_FRAGMENT), "ps5vk_noop_fs");
   return b.shader;
}

void
ps5vk_nir_free(nir_shader *nir)
{
   ralloc_free(nir);
}

/* Reuse Mesa's pointer-free serialization and the same persistent-output key.
 * A zero prefix distinguishes this input from a valid SPIR-V module. */
bool
ps5vk_shader_cache_nir_key(const nir_shader *nir, const PsbcCompileOptions *options,
                           struct ps5vk_shader_cache_key *key)
{
   struct blob blob;
   blob_init(&blob);
   blob_write_uint32(&blob, 0);
   nir_serialize(&blob, nir, true);
   const bool valid = !blob.out_of_memory &&
      ps5vk_shader_cache_key((const uint32_t *)blob.data, blob.size, options, key);
   blob_finish(&blob);
   return valid;
}
