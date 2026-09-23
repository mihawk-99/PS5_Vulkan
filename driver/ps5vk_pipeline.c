/*
 * PS5 Vulkan driver - graphics pipelines.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B6 (docs/M5_PHASE_B.md). A graphics pipeline compiles its
 * vertex and fragment SPIR-V with the opengnm-psbc compiler and packages each
 * stage with ps5-opengl's C writer, as the console compiled the probe shaders
 * byte for byte in Phase A3. The compiler options come from the pipeline:
 * - vertex stage: NGG, 32-bit GPU pointers at the address high word, and the
 *   vertex attributes of the vertex input state;
 * - both stages: the descriptor bindings of set 0 that name the stage, at
 *   their table offsets and strides (ps5vk_descriptor_set_layout.c);
 * - pixel stage: one SPI_SHADER_COL_FORMAT export per colour attachment,
 *   FP16_ABGR (4) where the attachment blends and 32_ABGR (9) where it does
 *   not, or the compiler's legacy default (0) when nothing blends. Blending
 *   an 8-bit UNORM target is exact only with FP16_ABGR exports (M4 step 2),
 *   while the other probes compiled with the default.
 * Before compiling, each stage's SPIR-V must be a well-formed instruction
 * stream declaring the named entry point for the stage: libpsbc checks only
 * the magic number and crashes on a module without one, so invalid SPIR-V
 * yields VK_ERROR_UNKNOWN here instead.
 * AGC shader objects are created the first time a command buffer draws with
 * the pipeline (ps5vk_pipeline_prepare_shaders, Phase B7), in a 64 KiB stage
 * workspace laid out as the test runner lays it out, so pipelines that never
 * draw use no GPU memory. Neither the runner nor ps5-opengl destroys AGC
 * shader objects, so destroying a pipeline releases only its workspace.
 * Draws refuse pipeline state the hardware has not rendered through the
 * driver yet (ps5vk_draw_refusal), while creation still succeeds, so every
 * package can be checked. What these probes have not proven is refused with VK_ERROR_UNKNOWN
 * and a logged reason: other topologies, multisampling, instanced vertex
 * input, descriptor sets past the four this driver advertises, descriptor types
 * without a proven table entry, and pipelines without a vertex stage.
 */

#include "ps5vk_private.h"
#include "ps5vk_debug.h"
#include "ps5vk_shader_cache.h"

#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps5_agc_package.h"
#include "util/detect_os.h"
#include "util/log.h"
#include "vk_alloc.h"
#include "vk_render_pass.h"
#include "vk_util.h"

#if DETECT_OS_LINUX
#include <stdio.h>
#endif

/* The compiler's ESGS ring item size for packages, as in Phase A3. */
#define PS5VK_ESGS_RING_ITEM_SIZE 1
/* The compiler's per-MRT colour export nibbles (SPI_SHADER_COL_FORMAT). */
#define PS5VK_EXPORT_32_ABGR 0x9u
#define PS5VK_EXPORT_FP16_ABGR 0x4u
#define PS5VK_MAX_COLOR_EXPORTS 8
/* Vertex attribute formats the M3 and M4 probes drew with; each component is
 * 4 bytes, the vertex-binding alignment the compiler takes. */
#define PS5VK_VERTEX_COMPONENT_ALIGNMENT 4

static const struct {
   VkFormat format;
   PsbcVertexFormat compiler_format;
} ps5vk_vertex_formats[] = {
   {VK_FORMAT_R32G32_SFLOAT, PSBC_VERTEX_FORMAT_R32G32_FLOAT},
   {VK_FORMAT_R32G32B32_SFLOAT, PSBC_VERTEX_FORMAT_R32G32B32_FLOAT},
   {VK_FORMAT_R32G32B32A32_SFLOAT, PSBC_VERTEX_FORMAT_R32G32B32A32_FLOAT},
   {VK_FORMAT_R32G32B32A32_UINT, PSBC_VERTEX_FORMAT_R32G32B32A32_UINT},
   /* V0-formats: the three-component integer formats, the last two
    * VERTEX_BUFFER-only rows docs/V0_FORMATS_AUDIT.md lists that the compiler
    * can express. The console probe is the runner's v0-vertex-sint and
    * v0-vertex-uint cases, whose third component reaches the readback as the
    * fragment's alpha. */
   {VK_FORMAT_R32G32B32_SINT, PSBC_VERTEX_FORMAT_R32G32B32_SINT},
   {VK_FORMAT_R32G32B32_UINT, PSBC_VERTEX_FORMAT_R32G32B32_UINT},
   /* The single-channel 32-bit float, whose VERTEX_BUFFER bit the CTS requires
    * for R32_SFLOAT (dEQP-VK.api.info.format_properties.r32_sfloat). The console
    * probe is the runner's v0-vertex-bytes-float case, whose row writes 0.25 as
    * 0x3E800000 and reads the four components Vulkan's fill rule gives an
    * attribute whose format has one (docs/M5_PHASE_C.md, CTS round 9). */
   {VK_FORMAT_R32_SFLOAT, PSBC_VERTEX_FORMAT_R32_FLOAT},
   /* Rung round 3: the nine rows docs/V0_FORMATS_AUDIT.md leaves probe-reachable
    * whose type a PsbcVertexFormat names, so a VkFormat the compiler has no word
    * for stays out of this table and the assert below keeps it that way. The two
    * packed 8888 layouts map by memory order: VK_FORMAT_A8B8G8R8_UNORM_PACK32
    * names its bytes from the most significant end, so its lowest byte is red
    * exactly as R8G8B8A8_UNORM's is (ps5-opengl's ps5_vertex_format maps the
    * same two pipe formats), and VK_FORMAT_A2B10G10R10_UNORM_PACK32 keeps red in
    * the low ten bits, which is PIPE_FORMAT_R10G10B10A2_UNORM's layout. The
    * console probe is the runner's v0-vertex-formats case, one frame a row. */
   {VK_FORMAT_R32_SINT, PSBC_VERTEX_FORMAT_R32_SINT},
   {VK_FORMAT_R32_UINT, PSBC_VERTEX_FORMAT_R32_UINT},
   {VK_FORMAT_R32G32_SINT, PSBC_VERTEX_FORMAT_R32G32_SINT},
   {VK_FORMAT_R32G32_UINT, PSBC_VERTEX_FORMAT_R32G32_UINT},
   {VK_FORMAT_R32G32B32A32_SINT, PSBC_VERTEX_FORMAT_R32G32B32A32_SINT},
   {VK_FORMAT_R8G8B8A8_UNORM, PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM},
   {VK_FORMAT_A8B8G8R8_UNORM_PACK32, PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM},
   {VK_FORMAT_B8G8R8A8_UNORM, PSBC_VERTEX_FORMAT_B8G8R8A8_UNORM},
   {VK_FORMAT_A2B10G10R10_UNORM_PACK32, PSBC_VERTEX_FORMAT_R10G10B10A2_UNORM},
   /* The eight-bit component layouts (round 1 of the blocker work): the
    * compiler's enum names them now (tooling/psbc/patch-vertex-formats.py) and
    * the hardware fetches them -- the GFX10 format words 1, 2, 5, 6 and 14, 15,
    * 18, 19 the vertex descriptor's DATA_FORMAT field takes. The console probe
    * is v0-vertex-formats' eight new rows, one frame each. */
   {VK_FORMAT_R8_UNORM, PSBC_VERTEX_FORMAT_R8_UNORM},
   {VK_FORMAT_R8_SNORM, PSBC_VERTEX_FORMAT_R8_SNORM},
   {VK_FORMAT_R8_UINT, PSBC_VERTEX_FORMAT_R8_UINT},
   {VK_FORMAT_R8_SINT, PSBC_VERTEX_FORMAT_R8_SINT},
   {VK_FORMAT_R8G8_UNORM, PSBC_VERTEX_FORMAT_R8G8_UNORM},
   {VK_FORMAT_R8G8_SNORM, PSBC_VERTEX_FORMAT_R8G8_SNORM},
   {VK_FORMAT_R8G8_UINT, PSBC_VERTEX_FORMAT_R8G8_UINT},
   {VK_FORMAT_R8G8_SINT, PSBC_VERTEX_FORMAT_R8G8_SINT},
   /* The sixteen-bit component layouts (blocker round 2): the enum names them
    * now and the hardware's words 7, 8, 11, 12, 13, 23..29 and 65..71 are the
    * ones Mesa's vertex-element table writes. The console probe is
    * v0-vertex-formats-16's fifteen rows, one frame each. */
   {VK_FORMAT_R16_UNORM, PSBC_VERTEX_FORMAT_R16_UNORM},
   {VK_FORMAT_R16_SNORM, PSBC_VERTEX_FORMAT_R16_SNORM},
   {VK_FORMAT_R16_UINT, PSBC_VERTEX_FORMAT_R16_UINT},
   {VK_FORMAT_R16_SINT, PSBC_VERTEX_FORMAT_R16_SINT},
   {VK_FORMAT_R16_SFLOAT, PSBC_VERTEX_FORMAT_R16_SFLOAT},
   {VK_FORMAT_R16G16_UNORM, PSBC_VERTEX_FORMAT_R16G16_UNORM},
   {VK_FORMAT_R16G16_SNORM, PSBC_VERTEX_FORMAT_R16G16_SNORM},
   {VK_FORMAT_R16G16_UINT, PSBC_VERTEX_FORMAT_R16G16_UINT},
   {VK_FORMAT_R16G16_SINT, PSBC_VERTEX_FORMAT_R16G16_SINT},
   {VK_FORMAT_R16G16_SFLOAT, PSBC_VERTEX_FORMAT_R16G16_SFLOAT},
   {VK_FORMAT_R16G16B16A16_UNORM, PSBC_VERTEX_FORMAT_R16G16B16A16_UNORM},
   {VK_FORMAT_R16G16B16A16_SNORM, PSBC_VERTEX_FORMAT_R16G16B16A16_SNORM},
   {VK_FORMAT_R16G16B16A16_UINT, PSBC_VERTEX_FORMAT_R16G16B16A16_UINT},
   {VK_FORMAT_R16G16B16A16_SINT, PSBC_VERTEX_FORMAT_R16G16B16A16_SINT},
   {VK_FORMAT_R16G16B16A16_SFLOAT, PSBC_VERTEX_FORMAT_R16G16B16A16_SFLOAT},
   /* The 8888 SNORM, SINT and UINT layouts (blocker round 3): Vulkan's
    * R8G8B8A8_X and A8B8G8R8_X_PACK32 name one memory order -- the packed form
    * counts its bytes from the most significant end -- so three values close six
    * rows, as the UNORM pair already shares one. The console probe is
    * v0-vertex-formats-8888's six rows. */
   {VK_FORMAT_R8G8B8A8_SNORM, PSBC_VERTEX_FORMAT_R8G8B8A8_SNORM},
   {VK_FORMAT_A8B8G8R8_SNORM_PACK32, PSBC_VERTEX_FORMAT_R8G8B8A8_SNORM},
   {VK_FORMAT_R8G8B8A8_SINT, PSBC_VERTEX_FORMAT_R8G8B8A8_SINT},
   {VK_FORMAT_A8B8G8R8_SINT_PACK32, PSBC_VERTEX_FORMAT_R8G8B8A8_SINT},
   {VK_FORMAT_R8G8B8A8_UINT, PSBC_VERTEX_FORMAT_R8G8B8A8_UINT},
   {VK_FORMAT_A8B8G8R8_UINT_PACK32, PSBC_VERTEX_FORMAT_R8G8B8A8_UINT},
};

/* SPIR-V's header, OpEntryPoint, OpExecutionMode and the execution models are
 * in ps5vk_private.h: the compute path checks its own stage with the same
 * calls. */

/* Neither the compiler nor AGC's shader creation is known to be reentrant:
 * compilations and creations take turns. The compute path lives in
 * ps5vk_compute.c and takes the same lock. */
once_flag ps5vk_compile_once = ONCE_FLAG_INIT;
mtx_t ps5vk_compile_mutex;

void
ps5vk_compile_mutex_init(void)
{
   mtx_init(&ps5vk_compile_mutex, mtx_plain);
}

void
ps5vk_pipeline_free(struct ps5vk_device *device, struct ps5vk_pipeline *pipeline,
                    const VkAllocationCallbacks *allocator)
{
   /* A pipeline that never drew has no stage mapping registered. */
   if (pipeline->stage_registered) {
      struct ps5vk_pipeline **at = &device->stages;
      while (*at != NULL && *at != pipeline)
         at = &(*at)->next_stage;
      if (*at == pipeline)
         *at = pipeline->next_stage;
      pipeline->stage_registered = false;
   }
   for (unsigned stage = 0; stage < PS5VK_PIPELINE_STAGE_COUNT; stage++)
      free(pipeline->stages[stage].data);
   ps5vk_direct_mapping_destroy(&pipeline->compute.code);
   ps5vk_direct_mapping_destroy(&pipeline->shaders.stage);
   mtx_destroy(&pipeline->shaders.lock);
   vk_object_free(&device->vk, allocator, pipeline);
}

/* R9: specialization constants, core Vulkan 1.0. The compiler is RADV's front
 * end, whose spirv_to_nir applies a stage's VkSpecializationInfo by SpecId; the
 * options carry it there (tooling/psbc/patch-specialization.py). The compiler's
 * entry is VkSpecializationMapEntry's own layout, so the application's array is
 * handed over as it is. The disk shader cache hashes these entries and values,
 * independently of their process addresses (ps5vk_shader_cache.c). */
_Static_assert(sizeof(PsbcSpecializationEntry) == sizeof(VkSpecializationMapEntry) &&
                  offsetof(PsbcSpecializationEntry, constant_id) ==
                     offsetof(VkSpecializationMapEntry, constantID) &&
                  offsetof(PsbcSpecializationEntry, offset) ==
                     offsetof(VkSpecializationMapEntry, offset) &&
                  offsetof(PsbcSpecializationEntry, size) ==
                     offsetof(VkSpecializationMapEntry, size),
               "the compiler's specialization entry is VkSpecializationMapEntry");

VkResult
ps5vk_specialization_options(struct ps5vk_device *device, const VkSpecializationInfo *info,
                             const char *stage_name, PsbcCompileOptions *options)
{
   if (info == NULL || info->mapEntryCount == 0)
      return VK_SUCCESS;
   /* Valid usage: every entry's value lies inside pData (offset + size <=
    * dataSize), and pData is present when dataSize is not 0. */
   if (info->pMapEntries == NULL || (info->dataSize != 0 && info->pData == NULL))
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "%s stage: pSpecializationInfo names %u entries without their map or data",
                       stage_name, info->mapEntryCount);
   for (uint32_t i = 0; i < info->mapEntryCount; i++) {
      const VkSpecializationMapEntry *const entry = &info->pMapEntries[i];
      if (entry->offset > info->dataSize || entry->size > info->dataSize - entry->offset)
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "%s stage: specialization constant %u reads %zu bytes at offset %u, past "
                          "the %zu bytes of pData",
                          stage_name, entry->constantID, entry->size, entry->offset,
                          info->dataSize);
   }
   options->specialization_entry_count = info->mapEntryCount;
   options->specialization_entries = (const PsbcSpecializationEntry *)info->pMapEntries;
   options->specialization_data_size = info->dataSize;
   options->specialization_data = info->pData;
   return VK_SUCCESS;
}

/* The descriptor bindings every set the stage reads uses, into the compiler
 * options: one entry per binding, each carrying its own set, and the compiler
 * builds one table per set from them (tooling/psbc/patch-descriptor-sets.py).
 * The driver's own layout offsets are per set -- ps5vk_descriptor_set_layout
 * starts each set's table at zero -- so each set's entries are sized from that
 * set's own bindings, which is what the compiler requires. */
VkResult
ps5vk_descriptor_options(struct ps5vk_device *device, const struct vk_pipeline_layout *layout,
                         VkShaderStageFlags stage_bit, PsbcCompileOptions *options)
{
   for (uint32_t set = 0; layout && set < layout->set_count; set++) {
      if (!layout->set_layouts[set])
         continue;
      const struct ps5vk_descriptor_set_layout *const set_layout =
         container_of(layout->set_layouts[set], struct ps5vk_descriptor_set_layout, vk);
      for (uint32_t index = 0; index < set_layout->binding_count; index++) {
         const struct ps5vk_descriptor_binding *const binding = &set_layout->bindings[index];
         if (binding->count == 0 || !(binding->stages & stage_bit))
            continue;
         if (set >= PS5VK_DESCRIPTOR_SET_COUNT)
            return vk_errorf(device, VK_ERROR_UNKNOWN,
                             "descriptor set %u: more than the %u sets this driver advertises "
                             "(VkPhysicalDeviceLimits.maxBoundDescriptorSets)",
                             set, PS5VK_DESCRIPTOR_SET_COUNT);
         if (binding->stride == 0)
            return vk_errorf(device, VK_ERROR_UNKNOWN,
                             "set %u binding %u: descriptor type %d has no proven table entry",
                             set, index, binding->type);
         if (options->descriptor_binding_count == PSBC_MAX_DESCRIPTOR_BINDINGS)
            return vk_errorf(device, VK_ERROR_UNKNOWN, "more than %d descriptor bindings",
                             PSBC_MAX_DESCRIPTOR_BINDINGS);
         options->descriptor_bindings[options->descriptor_binding_count++] =
            (PsbcDescriptorBinding){
               .set = (uint8_t)set,
               .binding = (uint8_t)index,
               /* A dynamic uniform buffer is a uniform buffer to the
                * compiler: the offset is the application's and never part of
                * the shader's binding (Phase D1). */
               .type = (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                        binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                          ? PSBC_DESCRIPTOR_UNIFORM_BUFFER
                          : (binding->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                ? PSBC_DESCRIPTOR_STORAGE_BUFFER
                                : (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                                      ? PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER
                                      : (binding->type ==
                                               VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                            ? PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER
                                            : (binding->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                                                       binding->type ==
                                                          VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
                                                  ? PSBC_DESCRIPTOR_STORAGE_IMAGE
                                                  : PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER)))),
               .array_size = binding->count,
               .offset = binding->offset,
               .stride = binding->stride,
            };
      }
   }
   return VK_SUCCESS;
}

/* The vertex input state's attributes, into the compiler options, and its
 * bindings' strides, which a draw's vertex-buffer descriptors need. */
static VkResult
ps5vk_vertex_input_options(struct ps5vk_device *device,
                           const VkPipelineVertexInputStateCreateInfo *input,
                           struct ps5vk_vertex_binding *bindings, PsbcCompileOptions *options)
{
   for (uint32_t i = 0; input && i < input->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *const binding = &input->pVertexBindingDescriptions[i];
      if (binding->binding >= PS5VK_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_UNKNOWN, "vertex binding %u out of range",
                          binding->binding);
      bindings[binding->binding] =
         (struct ps5vk_vertex_binding){.stride = binding->stride, .used = true};
   }
   for (uint32_t i = 0; input && i < input->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *const attribute =
         &input->pVertexAttributeDescriptions[i];
      const VkVertexInputBindingDescription *binding = NULL;
      for (uint32_t b = 0; b < input->vertexBindingDescriptionCount; b++) {
         if (input->pVertexBindingDescriptions[b].binding == attribute->binding)
            binding = &input->pVertexBindingDescriptions[b];
      }
      /* Valid usage: every attribute names a described binding. */
      assert(binding);
      if (binding->inputRate != VK_VERTEX_INPUT_RATE_VERTEX)
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "binding %u: per-instance vertex input is not supported",
                          binding->binding);
      PsbcVertexFormat format = PSBC_VERTEX_FORMAT_NONE;
      for (size_t f = 0; f < ARRAY_SIZE(ps5vk_vertex_formats); f++) {
         if (ps5vk_vertex_formats[f].format == attribute->format)
            format = ps5vk_vertex_formats[f].compiler_format;
      }
      /* Valid usage: the format has VERTEX_BUFFER features, which only these
       * formats report (ps5vk_image.c). */
      assert(format != PSBC_VERTEX_FORMAT_NONE);
      if (attribute->location >= PSBC_MAX_VERTEX_ATTRIBUTES || attribute->binding >= 32)
         return vk_errorf(device, VK_ERROR_UNKNOWN, "vertex attribute %u out of range",
                          attribute->location);
      options->vertex_attributes[options->vertex_attribute_count++] = (PsbcVertexAttribute){
         .location = (uint8_t)attribute->location,
         .binding = (uint8_t)attribute->binding,
         .format = format,
         .offset = attribute->offset,
         .stride = binding->stride,
         .alignment = PS5VK_VERTEX_COMPONENT_ALIGNMENT,
      };
   }
   return VK_SUCCESS;
}

/* The blend factors and equations this driver can program, as AMD's gfx103
 * register headers number them for CB_BLEND0_CONTROL (V_028780_BLEND_* and
 * V_028780_COMB_*). VkBlendFactor's numbering is not AMD's: Vulkan's 4 and 5 are
 * the destination-colour factors where AMD's are the source-alpha ones, so the
 * mapping is a table rather than a cast. The constant and SRC1 factors would
 * need the blend-constant registers or a second colour source, which this
 * driver programs neither of: they are refused by name. */
static bool
ps5vk_blend_factor(VkBlendFactor factor, uint32_t *value)
{
   switch (factor) {
   case VK_BLEND_FACTOR_ZERO:
      *value = 0; /* V_028780_BLEND_ZERO */
      return true;
   case VK_BLEND_FACTOR_ONE:
      *value = 1; /* V_028780_BLEND_ONE */
      return true;
   case VK_BLEND_FACTOR_SRC_COLOR:
      *value = 2;
      return true;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
      *value = 3;
      return true;
   case VK_BLEND_FACTOR_SRC_ALPHA:
      *value = 4;
      return true;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
      *value = 5;
      return true;
   case VK_BLEND_FACTOR_DST_ALPHA:
      *value = 6;
      return true;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
      *value = 7;
      return true;
   case VK_BLEND_FACTOR_DST_COLOR:
      *value = 8;
      return true;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
      *value = 9;
      return true;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
      *value = 10;
      return true;
   /* The constant factors read CB_BLEND_RED/GREEN/BLUE/ALPHA, which a blending
    * draw records from the pipeline's blendConstants when one of them is used
    * (ps5vk_draw.c). AMD numbers them 13, 14, 19 and 20 in the same field. */
   case VK_BLEND_FACTOR_CONSTANT_COLOR:
      *value = 13; /* V_028780_BLEND_CONSTANT_COLOR */
      return true;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
      *value = 14; /* V_028780_BLEND_ONE_MINUS_CONSTANT_COLOR */
      return true;
   case VK_BLEND_FACTOR_CONSTANT_ALPHA:
      *value = 19; /* V_028780_BLEND_CONSTANT_ALPHA */
      return true;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
      *value = 20; /* V_028780_BLEND_ONE_MINUS_CONSTANT_ALPHA */
      return true;
   default:
      /* The SRC1 factors (15 to 18) are the ones left: Vulkan requires
       * dualSrcBlend for them, this device reports maxFragmentDualSrcAttachments
       * 0, and a valid application cannot ask for them. */
      return false;
   }
}

/* Whether the attachment's state reads the pipeline's blend constants. */
static bool
ps5vk_blend_uses_constants(const VkPipelineColorBlendAttachmentState *attachment)
{
   const VkBlendFactor factors[4] = {attachment->srcColorBlendFactor,
                                     attachment->dstColorBlendFactor,
                                     attachment->srcAlphaBlendFactor,
                                     attachment->dstAlphaBlendFactor};
   for (unsigned index = 0; index < 4; index++) {
      switch (factors[index]) {
      case VK_BLEND_FACTOR_CONSTANT_COLOR:
      case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
      case VK_BLEND_FACTOR_CONSTANT_ALPHA:
      case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
         return true;
      default:
         break;
      }
   }
   return false;
}

static bool
ps5vk_blend_function(VkBlendOp op, uint32_t *value)
{
   switch (op) {
   case VK_BLEND_OP_ADD:
      *value = 0; /* V_028780_COMB_DST_PLUS_SRC */
      return true;
   case VK_BLEND_OP_SUBTRACT:
      *value = 1; /* V_028780_COMB_SRC_MINUS_DST */
      return true;
   case VK_BLEND_OP_REVERSE_SUBTRACT:
      *value = 4; /* V_028780_COMB_DST_MINUS_SRC */
      return true;
   case VK_BLEND_OP_MIN:
      *value = 2; /* V_028780_COMB_MIN_DST_SRC */
      return true;
   case VK_BLEND_OP_MAX:
      *value = 3; /* V_028780_COMB_MAX_DST_SRC */
      return true;
   default:
      return false;
   }
}

/* The CB_BLEND0_CONTROL word the attachment's state becomes: 0 when it does not
 * blend, and false for a state whose factors or equations this driver cannot
 * program. The fields are the register's own: colour source and destination
 * factors in bits 0-4 and 8-12, colour equation in bits 5-7, and the alpha
 * half at 16, 24 and 21 with SEPARATE_ALPHA_BLEND (bit 29) set only when it
 * differs from the colour half, which is the word the M4 blend canary proved on
 * the console (docs/HARDWARE_FINDINGS.md). */
static bool
ps5vk_blend_control(const VkPipelineColorBlendAttachmentState *attachment, uint32_t *word)
{
   if (!attachment || !attachment->blendEnable) {
      *word = 0;
      return true;
   }
   uint32_t src_colour, dst_colour, src_alpha, dst_alpha, function_colour, function_alpha;
   if (!ps5vk_blend_factor(attachment->srcColorBlendFactor, &src_colour) ||
       !ps5vk_blend_factor(attachment->dstColorBlendFactor, &dst_colour) ||
       !ps5vk_blend_factor(attachment->srcAlphaBlendFactor, &src_alpha) ||
       !ps5vk_blend_factor(attachment->dstAlphaBlendFactor, &dst_alpha) ||
       !ps5vk_blend_function(attachment->colorBlendOp, &function_colour) ||
       !ps5vk_blend_function(attachment->alphaBlendOp, &function_alpha))
      return false;
   uint32_t state = (UINT32_C(1) << 30) | src_colour | (function_colour << 5) | (dst_colour << 8);
   if (src_alpha != src_colour || dst_alpha != dst_colour || function_alpha != function_colour)
      state |= (UINT32_C(1) << 29) | (src_alpha << 16) | (function_alpha << 21) | (dst_alpha << 24);
   *word = state;
   return true;
}

/* Colour exports: FP16_ABGR where an attachment blends, 32_ABGR elsewhere,
 * the legacy default 0 when nothing blends. */
static VkResult
ps5vk_color_export_options(struct ps5vk_device *device, const VkGraphicsPipelineCreateInfo *info,
                           uint32_t *spi_shader_col_format)
{
   const VkPipelineRenderingCreateInfo *const rendering =
      vk_get_pipeline_rendering_create_info(info);
   const uint32_t count = rendering ? rendering->colorAttachmentCount : 0;
   const VkPipelineColorBlendStateCreateInfo *const blend = info->pColorBlendState;
   if (count > PS5VK_MAX_COLOR_EXPORTS)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "more than %d colour attachments",
                       PS5VK_MAX_COLOR_EXPORTS);

   /* A format whose table entry names an export takes it whatever the blend
    * state -- the integer targets export UINT16_ABGR or SINT16_ABGR, and a
    * single-channel 32-bit one 32_R -- and every other format keeps the legacy
    * 32_ABGR default until its attachment blends, when the pixel stage exports
    * FP16_ABGR: what Mesa's ac_choose_spi_color_formats picks for the
    * normalized, sRGB and half-float classes. A pipeline whose attachments all
    * take the default passes 0, which is the compiler's own default. */
   bool named = false;
   uint32_t exports = 0;
   for (uint32_t index = 0; index < PS5VK_MAX_COLOR_EXPORTS; index++) {
      const bool used = index < count &&
                        rendering->pColorAttachmentFormats[index] != VK_FORMAT_UNDEFINED;
      const bool blends = used && blend && index < blend->attachmentCount &&
                          blend->pAttachments[index].blendEnable;
      const struct ps5vk_colour_format *const colour =
         used ? ps5vk_find_colour_format(rendering->pColorAttachmentFormats[index]) : NULL;
      if (used && colour == NULL)
         return vk_errorf(device, VK_ERROR_UNKNOWN, "colour attachment %u: format %d", index,
                          rendering->pColorAttachmentFormats[index]);
      const uint32_t format = used && colour->export_format != 0
                                 ? colour->export_format
                                 : (blends ? PS5VK_EXPORT_FP16_ABGR : PS5VK_EXPORT_32_ABGR);
      named = named || (used && (colour->export_format != 0 || blends));
      exports |= format << (4 * index);
   }
   *spi_shader_col_format = named ? exports : 0;
   return VK_SUCCESS;
}

/* Whether the module is a well-formed SPIR-V instruction stream in host byte
 * order that declares an entry point named name for the execution model. */
bool
ps5vk_spirv_has_entry_point(const struct ps5vk_shader_module *module, uint32_t model,
                            const char *name)
{
   const size_t count = module->size / sizeof(uint32_t);
   if (count < PS5VK_SPIRV_HEADER_WORDS || module->words[0] != PS5VK_SPIRV_MAGIC)
      return false;
   bool found = false;
   for (size_t at = PS5VK_SPIRV_HEADER_WORDS; at < count;) {
      const uint32_t word_count = module->words[at] >> 16;
      const uint32_t opcode = module->words[at] & 0xffff;
      if (word_count == 0 || word_count > count - at)
         return false;
      /* OpEntryPoint: execution model, function, name, interface. */
      if (opcode == PS5VK_SPIRV_OP_ENTRY_POINT && word_count >= 4 && module->words[at + 1] == model) {
         const char *const literal = (const char *)&module->words[at + 3];
         const size_t bytes = (word_count - 3) * sizeof(uint32_t);
         found = found || (memchr(literal, '\0', bytes) && strcmp(literal, name) == 0);
      }
      at += word_count;
   }
   return found;
}

/* R10: what this compiler has no path for, refused by name before it runs. A
 * shader that asks for one of these used to reach the compiler, which printed
 * its own error and then killed the process -- on the console, minutes into a
 * title's start-up, with no VkResult and nothing the application could act on.
 *
 * Every entry is measured on the host against this same compiler, with the
 * shaders named, rather than inferred from a capability's name: the audit of
 * the 67 shaders a vkQuake port deploys (docs/M5_PHASE_C.md, R10) and the
 * negative probe `driver/tests/vk_v0_capability_test.c`.
 *
 * The two checks are the two shapes the failures actually have, not the two
 * capabilities they happen to declare:
 *
 * - The **addressing model**, because that is what the front end rejects:
 *   "AddressingModelPhysicalStorageBuffer64 not supported"
 *   (spirv_to_nir.c's OpMemoryModel case), which is where the port's
 *   skinning, mesh-interpolate and lightmap readers stop. Declaring
 *   PhysicalStorageBufferAddresses without that model compiles -- measured --
 *   so a refusal on the declaration alone would refuse a shader that works.
 * - The one capability this compiler has no lowering for anywhere:
 *   PhysicalStorageBufferAddressesEXT, which the port's ray-debug and
 *   lightmap kernels declare and whose ACO has no case for the
 *   `@bindless_image_store` they reach.
 *
 * What is *not* here matters as much. The capabilities a vkQuake port deploys
 * beyond Shader are SampleRateShading (35), InputAttachment (40), SampledBuffer
 * (46), StorageImageExtendedFormats (49), ImageQuery (50), GroupNonUniform
 * (61) and GroupNonUniformShuffle (65) -- and every one of those but
 * InputAttachment is lowered by this compiler, measured the same way, so
 * refusing them would refuse working shaders. (The values are the SPIR-V
 * specification's; a capability list that numbers them by neighbour is how the
 * port's own scanner came to call 46 a storage-image write.)
 *
 * The upgrade path, when another case appears: the front end already knows
 * (`spirv_to_nir.c`'s `supported_capabilities`), so a compiler entry point that
 * answered the question would replace this table rather than duplicate it. */
#define PS5VK_SPIRV_ADDRESSING_MODEL_PHYSICAL_STORAGE_BUFFER_64 5348u

struct ps5vk_refused_capability
{
   uint32_t value;
   const char *name;
   const char *reason;
};

static const struct ps5vk_refused_capability ps5vk_refused_capabilities[] = {
   {4472, "PhysicalStorageBufferAddressesEXT",
    "buffer device address is not advertised by this physical device, and this compiler's ACO has "
    "no case for the bindless image store the shaders that use it reach"},
};

/* The names of the capabilities this driver knows, for the sentences below. A
 * capability outside the list is named by its number alone rather than guessed
 * at. */
static const struct ps5vk_refused_capability ps5vk_known_capabilities[] = {
   {1, "Shader", ""},
   {25, "ImageGatherExtended", ""},
   {35, "SampleRateShading", ""},
   {40, "InputAttachment", ""},
   {46, "SampledBuffer", ""},
   {49, "StorageImageExtendedFormats", ""},
   {50, "ImageQuery", ""},
   {61, "GroupNonUniform", ""},
   {64, "GroupNonUniformBallot", ""},
   {65, "GroupNonUniformShuffle", ""},
   {4472, "PhysicalStorageBufferAddressesEXT", ""},
   {5347, "PhysicalStorageBufferAddresses", ""},
};

static const struct ps5vk_refused_capability *
ps5vk_capability_in(const struct ps5vk_refused_capability *table, size_t count, uint32_t value)
{
   for (size_t at = 0; at < count; at++) {
      if (table[at].value == value)
         return &table[at];
   }
   return NULL;
}

const char *
ps5vk_capability_name(uint32_t value)
{
   const struct ps5vk_refused_capability *const known =
      ps5vk_capability_in(ps5vk_known_capabilities,
                          sizeof(ps5vk_known_capabilities) / sizeof(ps5vk_known_capabilities[0]),
                          value);
   return known != NULL ? known->name : NULL;
}

/* Whether this driver refuses the module before compiling it, and why, into
 * reason: one sentence naming the limit, the way every other refusal in this
 * driver does. False, with reason untouched, for a module that is not SPIR-V at
 * all -- ps5vk_spirv_has_entry_point refuses those by name first. */
bool
ps5vk_spirv_refusal(const uint32_t *words, size_t size, char *reason, size_t reason_size)
{
   const size_t count = size / sizeof(uint32_t);
   if (words == NULL || count < PS5VK_SPIRV_HEADER_WORDS || words[0] != PS5VK_SPIRV_MAGIC)
      return false;
   for (size_t at = PS5VK_SPIRV_HEADER_WORDS; at < count;) {
      const uint32_t word_count = words[at] >> 16;
      const uint32_t opcode = words[at] & 0xffff;
      if (word_count == 0 || word_count > count - at)
         return false;
      /* OpMemoryModel: addressing model, memory model. */
      if (opcode == PS5VK_SPIRV_OP_MEMORY_MODEL && word_count >= 3 &&
          words[at + 1] == PS5VK_SPIRV_ADDRESSING_MODEL_PHYSICAL_STORAGE_BUFFER_64) {
         snprintf(reason, reason_size,
                  "the shader's addressing model is PhysicalStorageBuffer64 (%u), which this "
                  "compiler's SPIR-V front end rejects (\"AddressingModelPhysicalStorageBuffer64 "
                  "not supported\") and which the buffer device address this device does not "
                  "advertise would need",
                  PS5VK_SPIRV_ADDRESSING_MODEL_PHYSICAL_STORAGE_BUFFER_64);
         return true;
      }
      /* OpCapability: one capability. */
      if (opcode == PS5VK_SPIRV_OP_CAPABILITY && word_count >= 2) {
         const uint32_t value = words[at + 1];
         const struct ps5vk_refused_capability *const refused =
            ps5vk_capability_in(ps5vk_refused_capabilities,
                                sizeof(ps5vk_refused_capabilities) /
                                   sizeof(ps5vk_refused_capabilities[0]),
                                value);
         if (refused != NULL) {
            snprintf(reason, reason_size, "the shader declares SpvCapability%s (%u), which this "
                                          "driver does not support: %s",
                     refused->name, value, refused->reason);
            return true;
         }
      }
      at += word_count;
   }
   return false;
}

/* R10: the bindings a stage reads as input attachments, which is one thing the
 * application never writes: Vulkan forbids a descriptor write of type
 * VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, so the draw has to build the entry from
 * the subpass's own input attachment instead (ps5vk_draw.c). What says which
 * subpass input a binding is is the shader's own InputAttachmentIndex
 * decoration, and what says where it is read from is its DescriptorSet and
 * Binding decorations -- all three are OpDecorate on the variable, so the scan
 * is flat: no type resolution, no instruction beyond those three.
 *
 * The scan is over the module the pipeline was created from, taken once at
 * creation because the application may destroy the module afterwards. */
/* The bindings a module declares, as its own OpDecorate instructions name them:
 * the pairs a stage's compiled table may hold. A layout binding the shader never
 * reads is not one of them, and the draw must not demand an application write
 * for it -- which is what an input attachment would need and never has (R10). */
uint32_t
ps5vk_spirv_bindings(const uint32_t *words, size_t size, struct ps5vk_spirv_binding *out,
                     uint32_t capacity)
{
   const size_t count = size / sizeof(uint32_t);
   if (words == NULL || count < PS5VK_SPIRV_HEADER_WORDS || words[0] != PS5VK_SPIRV_MAGIC)
      return 0;
   struct decorated
   {
      uint32_t id;
      uint32_t set;
      uint32_t binding;
      bool have_set;
      bool have_binding;
   } slots[PS5VK_MAX_SPIRV_BINDINGS];
   uint32_t seen = 0;
   for (size_t at = PS5VK_SPIRV_HEADER_WORDS; at < count;) {
      const uint32_t word_count = words[at] >> 16;
      const uint32_t opcode = words[at] & 0xffff;
      if (word_count == 0 || word_count > count - at)
         break;
      if (opcode == PS5VK_SPIRV_OP_DECORATE && word_count >= 4) {
         const uint32_t decoration = words[at + 2];
         if (decoration != PS5VK_SPIRV_DECORATION_DESCRIPTOR_SET &&
             decoration != PS5VK_SPIRV_DECORATION_BINDING) {
            at += word_count;
            continue;
         }
         uint32_t slot = 0;
         while (slot < seen && slots[slot].id != words[at + 1])
            slot++;
         if (slot == seen) {
            if (seen == PS5VK_MAX_SPIRV_BINDINGS)
               break;
            slots[seen] = (struct decorated){.id = words[at + 1]};
            slot = seen++;
         }
         if (decoration == PS5VK_SPIRV_DECORATION_DESCRIPTOR_SET) {
            slots[slot].set = words[at + 3];
            slots[slot].have_set = true;
         } else {
            slots[slot].binding = words[at + 3];
            slots[slot].have_binding = true;
         }
      }
      at += word_count;
   }
   uint32_t found = 0;
   for (uint32_t slot = 0; slot < seen && found < capacity; slot++) {
      if (!slots[slot].have_set || !slots[slot].have_binding)
         continue;
      out[found++] = (struct ps5vk_spirv_binding){
         .set = (uint8_t)slots[slot].set,
         .binding = (uint8_t)slots[slot].binding,
      };
   }
   return found;
}

uint32_t
ps5vk_spirv_input_attachments(const uint32_t *words, size_t size, uint8_t stage,
                              struct ps5vk_input_attachment *out, uint32_t capacity)
{
   const size_t count = size / sizeof(uint32_t);
   if (words == NULL || count < PS5VK_SPIRV_HEADER_WORDS || words[0] != PS5VK_SPIRV_MAGIC)
      return 0;
   /* The three decorations are on the same result id, and this walk sees them
    * in whatever order the module wrote them: each id gets a slot the first
    * time one of the three is seen, and a slot that never gathers all three is
    * an ordinary binding the application writes rather than an input
    * attachment. */
   struct decorated
   {
      uint32_t id;
      uint32_t set;
      uint32_t binding;
      uint32_t index;
      bool have_set;
      bool have_binding;
      bool have_index;
   } slots[PS5VK_MAX_INPUT_ATTACHMENTS];
   uint32_t seen = 0;
   for (size_t at = PS5VK_SPIRV_HEADER_WORDS; at < count;) {
      const uint32_t word_count = words[at] >> 16;
      const uint32_t opcode = words[at] & 0xffff;
      if (word_count == 0 || word_count > count - at)
         break;
      /* OpDecorate: target, decoration, value. */
      if (opcode == PS5VK_SPIRV_OP_DECORATE && word_count >= 4) {
         const uint32_t decoration = words[at + 2];
         if (decoration != PS5VK_SPIRV_DECORATION_DESCRIPTOR_SET &&
             decoration != PS5VK_SPIRV_DECORATION_BINDING &&
             decoration != PS5VK_SPIRV_DECORATION_INPUT_ATTACHMENT_INDEX) {
            at += word_count;
            continue;
         }
         uint32_t slot = 0;
         while (slot < seen && slots[slot].id != words[at + 1])
            slot++;
         if (slot == seen) {
            if (seen == PS5VK_MAX_INPUT_ATTACHMENTS)
               break;
            slots[seen] = (struct decorated){.id = words[at + 1]};
            slot = seen++;
         }
         if (decoration == PS5VK_SPIRV_DECORATION_DESCRIPTOR_SET) {
            slots[slot].set = words[at + 3];
            slots[slot].have_set = true;
         } else if (decoration == PS5VK_SPIRV_DECORATION_BINDING) {
            slots[slot].binding = words[at + 3];
            slots[slot].have_binding = true;
         } else {
            slots[slot].index = words[at + 3];
            slots[slot].have_index = true;
         }
      }
      at += word_count;
   }
   uint32_t found = 0;
   for (uint32_t slot = 0; slot < seen && found < capacity; slot++) {
      if (!slots[slot].have_index || !slots[slot].have_set || !slots[slot].have_binding)
         continue;
      out[found++] = (struct ps5vk_input_attachment){
         .set = (uint8_t)slots[slot].set,
         .binding = (uint8_t)slots[slot].binding,
         .stage = stage,
         .index = slots[slot].index,
      };
   }
   return found;
}

/* Every capability the module declares, named where this driver knows the name
 * and numbered where it does not, into out: "SpvCapabilityShader,
 * SpvCapabilityInputAttachment (40)". It is what a refusal that cannot say
 * *which* capability is at fault says instead, so the log still names what the
 * shader asked the compiler for. */
void
ps5vk_spirv_capability_list(const uint32_t *words, size_t size, char *out, size_t out_size)
{
   const size_t count = size / sizeof(uint32_t);
   size_t used = 0;
   out[0] = '\0';
   if (words == NULL || count < PS5VK_SPIRV_HEADER_WORDS || words[0] != PS5VK_SPIRV_MAGIC) {
      snprintf(out, out_size, "no capabilities (not SPIR-V)");
      return;
   }
   for (size_t at = PS5VK_SPIRV_HEADER_WORDS; at < count;) {
      const uint32_t word_count = words[at] >> 16;
      const uint32_t opcode = words[at] & 0xffff;
      if (word_count == 0 || word_count > count - at)
         break;
      if (opcode == PS5VK_SPIRV_OP_CAPABILITY && word_count >= 2) {
         const uint32_t value = words[at + 1];
         const char *const name = ps5vk_capability_name(value);
         const int written = name != NULL
                                ? snprintf(out + used, out_size - used, "%sSpvCapability%s (%u)",
                                           used != 0 ? ", " : "", name, value)
                                : snprintf(out + used, out_size - used, "%scapability %u",
                                           used != 0 ? ", " : "", value);
         if (written < 0 || (size_t)written >= out_size - used)
            break;
         used += (size_t)written;
      }
      at += word_count;
   }
   if (used == 0)
      snprintf(out, out_size, "no capabilities");
}

/* The compiler recurses deeply -- NIR passes over big shaders, then ACO's
 * optimizer, register allocation and scheduler -- and an application's own
 * thread may carry a stack too small for it. The stack is allocated here and
 * handed to the thread with pthread_attr_setstack rather than requested with
 * setstacksize: a runtime that caps the size a thread asks for would leave the
 * recursion as unbounded as it was, while a stack this repository owns cannot
 * be capped.
 *
 * This is robustness rather than a repair. The console fault this wrapper was
 * written for -- SIGFPE, "integer divide fault", after the V0-formats sampled
 * unsigned case -- is not the compiler's and not a stack: it is the test
 * runner dividing the packed texel buffer's size by a value-initialized row's
 * zero texel size (src/diagnostics.cpp, docs/HARDWARE_FINDINGS.md,
 * 2026-09-20). */
#define PS5VK_COMPILE_STACK_BYTES (32u * 1024u * 1024u)

struct ps5vk_compile_call
{
   struct nir_shader *nir; /* one of the two inputs */
   const uint32_t *words;
   size_t size;
   const PsbcCompileOptions *options;
   PsbcShaderOutput *output;
   PsbcResult result;
   /* R10: the compiler trapped or aborted on this shader rather than returning
    * a result (see ps5vk_compile_worker). */
   bool aborted;
};

/* R10: a shader the compiler cannot lower has to come back as a VkResult.
 * `unreachable()` and `vtn_fail` in the ACO and SPIR-V front ends print their
 * own message and then raise, so a title that meets one dies minutes into
 * start-up having been told nothing it can act on -- and `vkCreatePipelines`
 * has no result to return. The raise happens on the thread that compiles
 * (ps5vk_compile_shader_deep's own, or the caller's when a thread cannot be
 * created), so the guard lives in that thread's frame and turns the signal into
 * a result the caller turns into a refusal.
 *
 * The ceiling, stated where it is: the aborted compile's allocations are leaked
 * -- the compiler's arenas are in whatever state the raise left them -- and the
 * driver does not retry the shader. A title that meets one gets an error for
 * that pipeline and keeps running; it does not get the memory back. Only these
 * two signals are caught: a real segmentation fault in the compiler is still a
 * crash this driver has to be fixed for, not something to report as the
 * shader's fault. */
static sigjmp_buf ps5vk_compile_jump;
static volatile sig_atomic_t ps5vk_compile_raised;

static void
ps5vk_compile_signal(int signo)
{
   (void)signo;
   ps5vk_compile_raised = 1;
   siglongjmp(ps5vk_compile_jump, 1);
}

static void *
ps5vk_compile_worker(void *argument)
{
   struct ps5vk_compile_call *const call = argument;
   /* One line per compile while the fault is open: it says whether a crash in
    * the compiler happened on this thread (with PS5VK_COMPILE_STACK_BYTES under
    * it) or before the compile started at all. Mesa's log reaches the console's
    * klog; the title's own stderr goes to its trace file instead. */
   printf("[ps5vk] compile start: nir=%p words=%p\n", (void *)call->nir,
          (const void *)call->words);
   fflush(stdout);
   struct sigaction action, saved_abort, saved_trap;
   memset(&action, 0, sizeof(action));
   action.sa_handler = ps5vk_compile_signal;
   sigemptyset(&action.sa_mask);
   const bool caught_abort = sigaction(SIGABRT, &action, &saved_abort) == 0;
   const bool caught_trap = sigaction(SIGTRAP, &action, &saved_trap) == 0;
   ps5vk_compile_raised = 0;
   if (!caught_abort && !caught_trap) {
      call->result = call->nir ? psbc_compile_nir(call->nir, call->options, call->output)
                               : psbc_compile_shader(call->words, call->size, call->options,
                                                     call->output);
   } else if (sigsetjmp(ps5vk_compile_jump, 1) == 0) {
      call->result = call->nir ? psbc_compile_nir(call->nir, call->options, call->output)
                               : psbc_compile_shader(call->words, call->size, call->options,
                                                     call->output);
   } else {
      /* The compiler raised: no package was written and the result is the
       * caller's to turn into a sentence. */
      call->aborted = true;
      call->result = PSBC_RESULT_COMPILE_ACO;
      printf("[ps5vk] compile raised: the shader compiler aborted on this shader\n");
      fflush(stdout);
   }
   if (caught_trap)
      sigaction(SIGTRAP, &saved_trap, NULL);
   if (caught_abort)
      sigaction(SIGABRT, &saved_abort, NULL);
   printf("[ps5vk] compile done: result=%d\n", (int)call->result);
   fflush(stdout);
   return NULL;
}

/* Runs one compile on a thread with the stack above and returns the compiler's
 * result. Every step falls back to the next: our own stack, then the size the
 * attributes ask for, then the caller's stack, which is what every compile did
 * before this existed -- so a platform that refuses one of them keeps working
 * and the fault stays a possibility rather than becoming a new failure. */
static PsbcResult
ps5vk_compile_shader_uncached_untimed(struct nir_shader *nir, const uint32_t *words, size_t size,
                          const PsbcCompileOptions *options, PsbcShaderOutput *output,
                          bool *aborted)
{
   struct ps5vk_compile_call call = {.nir = nir,
                                     .words = words,
                                     .size = size,
                                     .options = options,
                                     .output = output,
                                     .result = PSBC_RESULT_INTERNAL_ERROR};
   if (aborted != NULL)
      *aborted = false;
   pthread_attr_t attributes;
   pthread_t thread;
   char *const stack = malloc(PS5VK_COMPILE_STACK_BYTES);
   if (pthread_attr_init(&attributes) != 0)
   {
      free(stack);
      ps5vk_compile_worker(&call);
      if (aborted != NULL)
         *aborted = call.aborted;
      return call.result;
   }
   int created = -1;
   if (stack)
      created = pthread_attr_setstack(&attributes, stack, PS5VK_COMPILE_STACK_BYTES);
   if (created != 0)
      created = pthread_attr_setstacksize(&attributes, PS5VK_COMPILE_STACK_BYTES);
   if (created == 0)
      created = pthread_create(&thread, &attributes, ps5vk_compile_worker, &call);
   pthread_attr_destroy(&attributes);
   if (created == 0)
   {
      pthread_join(thread, NULL);
      free(stack);
      if (aborted != NULL)
         *aborted = call.aborted;
      return call.result;
   }
   /* The console's pthreads refused both forms: say so once for the log, then
    * compile on this thread. */
   fprintf(stderr, "[ps5vk] compile thread refused (%d); compiling on the caller's stack\n",
           created);
   free(stack);
   ps5vk_compile_worker(&call);
   if (aborted != NULL)
      *aborted = call.aborted;
   return call.result;
}

/* Timed for the hitch report (ps5vk_queue.c). */
static PsbcResult
ps5vk_compile_shader_uncached(struct nir_shader *nir, const uint32_t *words, size_t size,
                          const PsbcCompileOptions *options, PsbcShaderOutput *output,
                          bool *aborted)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const PsbcResult result = ps5vk_compile_shader_uncached_untimed(nir, words, size, options, output, aborted);
   ps5vk_hitch_end(PS5VK_HITCH_COMPILE, hitch);
   return result;
}

/* Both callers hold ps5vk_compile_mutex. Only immutable compiler output is
 * cached; GPU addresses, AGC objects and pipeline state are always rebuilt. */
PsbcResult
ps5vk_compile_shader_deep(struct nir_shader *nir, const uint32_t *words, size_t size,
                          const PsbcCompileOptions *options, PsbcShaderOutput *output,
                          bool *aborted)
{
   struct ps5vk_shader_cache_key key;
   const bool cacheable = !nir && ps5vk_shader_cache_key(words, size, options, &key);
   if (aborted)
      *aborted = false;
   if (cacheable && ps5vk_shader_cache_load(&key, output))
      return PSBC_RESULT_OK;
   const PsbcResult result = ps5vk_compile_shader_uncached(nir, words, size, options, output, aborted);
   if (cacheable && result == PSBC_RESULT_OK)
      ps5vk_shader_cache_store(&key, output);
   return result;
}

/* A stage's shader, as SPIR-V from a shader module or as the NIR Mesa's meta
 * operations hand the driver directly
 * (VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA). */
static VkResult
ps5vk_compile_stage(struct ps5vk_device *device, const VkPipelineShaderStageCreateInfo *stage,
                    const PsbcCompileOptions *options, uint32_t push_constant_bytes,
                    struct ps5vk_shader_package *package)
{
   VK_FROM_HANDLE(ps5vk_shader_module, module, stage ? stage->module : VK_NULL_HANDLE);
   const VkPipelineShaderStageNirCreateInfoMESA *const nir_info =
      stage ? vk_find_struct_const(stage->pNext, PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA) : NULL;
   const bool vertex = options->stage == PSBC_STAGE_VERTEX;
   const char *const stage_name = vertex ? "vertex" : "fragment";
   if (stage && !module && !nir_info)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "shader stages without a shader module are not supported");
   if (stage && !nir_info && !ps5vk_spirv_has_entry_point(module,
                                                 vertex ? PS5VK_SPIRV_EXECUTION_MODEL_VERTEX
                                                        : PS5VK_SPIRV_EXECUTION_MODEL_FRAGMENT,
                                                 stage->pName))
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "%s stage: not well-formed SPIR-V with an entry point \"%s\"", stage_name,
                       stage->pName);

   /* R10: a capability this compiler cannot lower is refused here, before the
    * compiler runs, which is what makes the refusal a VkResult the application
    * can act on rather than a process that dies with the compiler's own message
    * on its way out. */
   if (stage && !nir_info) {
      char reason[PS5VK_CAPABILITY_LIST_BYTES];
      if (ps5vk_spirv_refusal(module->words, module->size, reason, sizeof(reason)))
         return vk_errorf(device, VK_ERROR_UNKNOWN, "%s stage: %s (docs/M5_PHASE_C.md, R10)",
                          stage_name, reason);
   }

   /* R9: the stage's own specialization constants. Mesa's meta stages are NIR,
    * which has none to apply. */
   PsbcCompileOptions stage_options = *options;
   if (stage && !nir_info) {
      const VkResult specialized = ps5vk_specialization_options(
         device, stage->pSpecializationInfo, stage_name, &stage_options);
      if (specialized != VK_SUCCESS)
         return specialized;
   }

   /* Lowering runs outside the compiler lock: it touches only this clone. */
   struct nir_shader *const nir =
      !stage ? ps5vk_nir_noop_fragment()
             : (nir_info ? ps5vk_nir_prepare(nir_info->nir, push_constant_bytes) : NULL);
   if ((!stage || nir_info) && !nir)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   call_once(&ps5vk_compile_once, ps5vk_compile_mutex_init);
   mtx_lock(&ps5vk_compile_mutex);
   PsbcShaderOutput output;
   memset(&output, 0, sizeof(output));
   /* A stage Mesa's meta operations hand the driver is NIR and no module, so
    * the words are the module's only when there is one. Evaluating them
    * eagerly here dereferences a null module for every meta clear, blit and
    * resolve (the host's loader tests caught it as a SIGSEGV in this function,
    * 2026-09-20). */
   const uint32_t *const words = module ? module->words : NULL;
   const size_t size = module ? module->size : 0;
   bool aborted = false;
   const PsbcResult result = ps5vk_compile_shader_deep(nir, words, size, &stage_options, &output,
                                                      &aborted);
   const int written = result == PSBC_RESULT_OK
                          ? ps5_agc_package_build(&output, PS5VK_ESGS_RING_ITEM_SIZE,
                                                  &package->data, &package->size)
                          : -1;
   if (result == PSBC_RESULT_OK)
      package->metadata = output.metadata;
   psbc_free_output(&output);
   mtx_unlock(&ps5vk_compile_mutex);
   if (nir)
      ps5vk_nir_free(nir);

   if (result != PSBC_RESULT_OK) {
      /* R10: an abort is not a compile result, so it gets a sentence of its
       * own: the compiler never returned, and what the shader asked it for --
       * its capability list -- is the part of it the log can still name. */
      if (aborted) {
         char declared[PS5VK_CAPABILITY_LIST_BYTES];
         ps5vk_spirv_capability_list(words, size, declared, sizeof(declared));
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "%s stage: the shader compiler aborted on this shader instead of "
                          "returning a result, so no package was written: it cannot lower "
                          "something the shader uses (the shader declares %s; "
                          "docs/M5_PHASE_C.md, R10)",
                          stage_name, declared);
      }
      return vk_errorf(device, VK_ERROR_UNKNOWN, "%s stage: %s", stage_name,
                       psbc_result_string(result));
   }
   if (written != 0 || !package->data)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "the AGC package writer failed: %d", written);
   return VK_SUCCESS;
}

#if DETECT_OS_LINUX
/* PC checks compare packages with the probe sets: PS5VK_PIPELINE_DUMP names a
 * path prefix, and each new pipeline writes <prefix>-vertex.bin and
 * <prefix>-pixel.bin. */
static void
ps5vk_pipeline_dump(const struct ps5vk_pipeline *pipeline)
{
   const char *const prefix = getenv("PS5VK_PIPELINE_DUMP");
   static const char *const names[PS5VK_PIPELINE_STAGE_COUNT] = {"vertex", "pixel"};
   for (unsigned stage = 0; prefix && stage < PS5VK_PIPELINE_STAGE_COUNT; stage++) {
      char path[4096];
      snprintf(path, sizeof(path), "%s-%s.bin", prefix, names[stage]);
      FILE *const file = fopen(path, "wb");
      if (!file)
         continue;
      fwrite(pipeline->stages[stage].data, 1, pipeline->stages[stage].size, file);
      fclose(file);
   }
}
#endif

/* AGC shader packages as the test runner checks them before creating shaders
 * (validate_shader_package): the hardware stage byte of each header. */
#define PS5VK_PACKAGE_VERTEX_STAGE 2
#define PS5VK_PACKAGE_PIXEL_STAGE 1
#define PS5VK_PACKAGE_HEADER_MAGIC 0x34333231u
#define PS5VK_PACKAGE_MIN_HEADER_BYTES 96
/* Stage workspace regions start on 4 KiB boundaries (link_shader_packages). */
#define PS5VK_STAGE_REGION_ALIGNMENT 0x1000
/* The link's primitive types: AMD's DI_PT_* enumeration, VGT_PRIMITIVE_TYPE's
 * values (R_030908 in amdgfxregs.h), which AGC's sceAgcLinkShaders takes and
 * writes into that register -- the list's link leaves 0x242 = 4 among its
 * uniform records (golden/c1-triangle's stage image). ps5-opengl hands the
 * link the same enumeration (ps5_agc_native_runtime.c,
 * ps5_agc_gate2_set_draw_state: 5 TRIFAN, 6 TRISTRIP), and so does opengnm's
 * gnm_types.h (GNM_PT_TRIFAN = 0x5). R6 first passed 5 for the strip, which is
 * the *fan*: the strip is 6. */
#define PS5VK_LINK_LINE_LIST 2
#define PS5VK_LINK_TRIANGLE_LIST 4
#define PS5VK_LINK_TRIANGLE_STRIP 6
/* A shader object's context and SH table pointers and their record counts,
 * with the bounds the runner accepts (emit_linked_shader_state). */
#define PS5VK_SHADER_CX_TABLE_OFFSET 24
#define PS5VK_SHADER_SH_TABLE_OFFSET 32
#define PS5VK_SHADER_CX_COUNT_BYTE 91
#define PS5VK_SHADER_SH_COUNT_BYTE 92
#define PS5VK_SHADER_MAX_CX_RECORDS 32
#define PS5VK_SHADER_MAX_SH_RECORDS 16

static uint16_t
ps5vk_read16(const uint8_t *at)
{
   uint16_t value;
   memcpy(&value, at, sizeof(value));
   return value;
}

static uint32_t
ps5vk_read32(const uint8_t *at)
{
   uint32_t value;
   memcpy(&value, at, sizeof(value));
   return value;
}

static uint64_t
ps5vk_read64(const uint8_t *at)
{
   uint64_t value;
   memcpy(&value, at, sizeof(value));
   return value;
}

/* A register-table pointer stored in a shader object. */
static const struct ps5vk_agc_register *
ps5vk_read_table(const uint8_t *at)
{
   const struct ps5vk_agc_register *table;
   memcpy(&table, at, sizeof(table));
   return table;
}

/* A package's .shader_header and .shader_text sections: an ELF whose section
 * table and sections lie inside it, and a header with the expected magic,
 * sizes and hardware stage. */
static bool
ps5vk_package_sections(const struct ps5vk_shader_package *package, uint8_t hardware_stage,
                       const uint8_t **header, size_t *header_bytes, const uint8_t **code,
                       size_t *code_bytes)
{
   const uint8_t *const data = package->data;
   const size_t size = package->size;
   *header = NULL;
   *code = NULL;
   *header_bytes = 0;
   *code_bytes = 0;
   if (size < 64 || memcmp(data, "\x7f" "ELF", 4) != 0)
      return false;
   const uint64_t sections = ps5vk_read64(data + 40);
   const uint16_t entry_bytes = ps5vk_read16(data + 58);
   const uint16_t count = ps5vk_read16(data + 60);
   const uint16_t names_index = ps5vk_read16(data + 62);
   if (sections == 0 || entry_bytes < 64 || count == 0 || names_index >= count || sections > size ||
       count > (size - sections) / entry_bytes)
      return false;
   const uint8_t *const names_record = data + sections + (size_t)names_index * entry_bytes;
   const uint64_t names_offset = ps5vk_read64(names_record + 24);
   const uint64_t names_bytes = ps5vk_read64(names_record + 32);
   if (names_offset > size || names_bytes > size - names_offset)
      return false;
   const char *const names = (const char *)data + names_offset;
   for (uint16_t index = 0; index < count; index++) {
      const uint8_t *const record = data + sections + (size_t)index * entry_bytes;
      const uint32_t name = ps5vk_read32(record);
      const uint64_t offset = ps5vk_read64(record + 24);
      const uint64_t bytes = ps5vk_read64(record + 32);
      if (name >= names_bytes || offset > size || bytes > size - offset ||
          !memchr(names + name, '\0', names_bytes - name))
         return false;
      if (strcmp(names + name, ".shader_header") == 0) {
         *header = data + offset;
         *header_bytes = (size_t)bytes;
      } else if (strcmp(names + name, ".shader_text") == 0) {
         *code = data + offset;
         *code_bytes = (size_t)bytes;
      }
   }
   const uint8_t *const h = *header;
   return h && *code && *header_bytes >= PS5VK_PACKAGE_MIN_HEADER_BYTES && *code_bytes != 0 &&
          ps5vk_read32(h) == PS5VK_PACKAGE_HEADER_MAGIC && ps5vk_read32(h + 4) == 24 &&
          ps5vk_read64(h + 8) != 0 && ps5vk_read64(h + 16) == 0 &&
          ps5vk_read32(h + 64) == *header_bytes && ps5vk_read32(h + 68) == *code_bytes &&
          h[0x5a] == hardware_stage;
}

/* The runner's staging, creation and linking (link_shader_packages): vertex
 * header, vertex code, pixel header and pixel code on 4 KiB boundaries from
 * the start of the workspace; sceAgcCreateShader relocates each header in
 * place; sceAgcLinkShaders writes the linked context and uniforms. */
static VkResult
ps5vk_pipeline_create_shaders(struct ps5vk_device *device, struct ps5vk_pipeline *pipeline)
{
   struct ps5vk_pipeline_shaders *const shaders = &pipeline->shaders;
   const uint8_t *vertex_header, *vertex_code, *pixel_header, *pixel_code;
   size_t vertex_header_bytes, vertex_code_bytes, pixel_header_bytes, pixel_code_bytes;
   if (!ps5vk_package_sections(&pipeline->stages[PS5VK_PIPELINE_STAGE_VERTEX],
                               PS5VK_PACKAGE_VERTEX_STAGE, &vertex_header, &vertex_header_bytes,
                               &vertex_code, &vertex_code_bytes) ||
       !ps5vk_package_sections(&pipeline->stages[PS5VK_PIPELINE_STAGE_PIXEL],
                               PS5VK_PACKAGE_PIXEL_STAGE, &pixel_header, &pixel_header_bytes,
                               &pixel_code, &pixel_code_bytes))
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "the compiled shaders are not valid AGC shader packages");

   const size_t vertex_code_offset = ALIGN_POT(vertex_header_bytes, PS5VK_STAGE_REGION_ALIGNMENT);
   const size_t pixel_offset =
      vertex_code_offset + ALIGN_POT(vertex_code_bytes, PS5VK_STAGE_REGION_ALIGNMENT);
   const size_t pixel_code_offset =
      pixel_offset + ALIGN_POT(pixel_header_bytes, PS5VK_STAGE_REGION_ALIGNMENT);
   if (pixel_code_offset + pixel_code_bytes > PS5VK_STAGE_CONTEXT_OFFSET)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "shaders of %zu bytes do not fit before the linked context",
                       pixel_code_offset + pixel_code_bytes);

   const int32_t mapped = ps5vk_direct_mapping_create(&shaders->stage, PS5VK_STAGE_BYTES,
                                                      PS5VK_DIRECT_PAGE_BYTES);
   if (mapped != 0)
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "the stage workspace could not be mapped in the address window: 0x%08x",
                       (unsigned)mapped);
   uint8_t *const stage = shaders->stage.address;
   memset(stage, 0, PS5VK_STAGE_BYTES);
   memcpy(stage, vertex_header, vertex_header_bytes);
   memcpy(stage + vertex_code_offset, vertex_code, vertex_code_bytes);
   memcpy(stage + pixel_offset, pixel_header, pixel_header_bytes);
   memcpy(stage + pixel_code_offset, pixel_code, pixel_code_bytes);

   void *vertex_shader = NULL;
   void *pixel_shader = NULL;
   call_once(&ps5vk_compile_once, ps5vk_compile_mutex_init);
   mtx_lock(&ps5vk_compile_mutex);
   int32_t result = sceAgcCreateShader(&vertex_shader, stage, stage + vertex_code_offset);
   if (result == 0 && vertex_shader)
      result = sceAgcCreateShader(&pixel_shader, stage + pixel_offset, stage + pixel_code_offset);
   if (result == 0 && pixel_shader)
      result = sceAgcLinkShaders(stage + PS5VK_STAGE_CONTEXT_OFFSET,
                                 stage + PS5VK_STAGE_UNIFORM_OFFSET, NULL, vertex_shader,
                                 pixel_shader, pipeline->link_primitive_type);
   mtx_unlock(&ps5vk_compile_mutex);
   if (result != 0 || !vertex_shader || !pixel_shader) {
      ps5vk_direct_mapping_destroy(&shaders->stage);
      return vk_errorf(device, VK_ERROR_UNKNOWN, "AGC shader creation or linking failed: 0x%08x",
                       (unsigned)result);
   }

   const uint8_t *const objects[PS5VK_PIPELINE_STAGE_COUNT] = {vertex_shader, pixel_shader};
   for (unsigned index = 0; index < PS5VK_PIPELINE_STAGE_COUNT; index++) {
      struct ps5vk_shader_tables *const tables = &shaders->tables[index];
      tables->cx = ps5vk_read_table(objects[index] + PS5VK_SHADER_CX_TABLE_OFFSET);
      tables->sh = ps5vk_read_table(objects[index] + PS5VK_SHADER_SH_TABLE_OFFSET);
      tables->cx_count = objects[index][PS5VK_SHADER_CX_COUNT_BYTE];
      tables->sh_count = objects[index][PS5VK_SHADER_SH_COUNT_BYTE];
      /* The tables live in this stage mapping: AGC's relocation writes their
       * addresses into the header, so a header that was never relocated (a
       * PC model with no capture of this pipeline, say) names somewhere else.
       * Reading a table from there is a crash rather than a wrong frame, so
       * the addresses are checked, not just their counts. */
      const uintptr_t begin = (uintptr_t)stage;
      const uintptr_t end = begin + PS5VK_STAGE_BYTES;
      const uintptr_t cx_at = (uintptr_t)tables->cx;
      const uintptr_t sh_at = (uintptr_t)tables->sh;
      if (!tables->cx || !tables->sh || cx_at < begin || cx_at >= end || sh_at < begin ||
          sh_at >= end || tables->cx_count > PS5VK_SHADER_MAX_CX_RECORDS ||
          tables->sh_count > PS5VK_SHADER_MAX_SH_RECORDS) {
         ps5vk_direct_mapping_destroy(&shaders->stage);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "a created shader's register tables are out of bounds");
      }
   }
   ps5vk_flush_cpu_cache(stage, PS5VK_STAGE_BYTES);
   /* The runner's capture logs these mappings: a PC rebuild replays what AGC
    * wrote here, and a pipeline the capture does not carry cannot be modelled
    * (ps5vk_debug.h, ps5vk_debug_pipeline_stages). */
   pipeline->next_stage = device->stages;
   device->stages = pipeline;
   pipeline->stage_registered = true;
   return VK_SUCCESS;
}

uint32_t
ps5vk_debug_pipeline_stages(VkDevice _device, ps5vk_debug_stage *stages, uint32_t capacity)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   uint32_t count = 0;
   for (struct ps5vk_pipeline *pipeline = device ? device->stages : NULL; pipeline != NULL;
        pipeline = pipeline->next_stage)
      count++;
   if (stages == NULL || capacity == 0)
      return count;
   /* The list is newest first; the caller logs creation order, which is the
    * order the pipelines linked their stages. */
   uint32_t at = MIN2(count, capacity);
   for (struct ps5vk_pipeline *pipeline = device->stages; pipeline != NULL && at > 0;
        pipeline = pipeline->next_stage) {
      at--;
      /* A compute pipeline has no AGC stage workspace: its mapping is the
       * compiled ISA the dispatch points at (Phase D2). */
      const struct ps5vk_direct_mapping *const mapping =
         pipeline->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? &pipeline->compute.code
                                                                : &pipeline->shaders.stage;
      stages[at] = (ps5vk_debug_stage){
         .address = mapping->address,
         .bytes = mapping->bytes,
      };
   }
   return count;
}

uint32_t
ps5vk_debug_pipeline_primitive_type(VkPipeline _pipeline)
{
   VK_FROM_HANDLE(ps5vk_pipeline, pipeline, _pipeline);
   return pipeline != NULL && pipeline->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS
             ? pipeline->link_primitive_type
             : 0;
}

VkResult
ps5vk_pipeline_prepare_shaders(struct ps5vk_device *device, struct ps5vk_pipeline *pipeline)
{
   struct ps5vk_pipeline_shaders *const shaders = &pipeline->shaders;
   mtx_lock(&shaders->lock);
   if (!shaders->attempted) {
      shaders->attempted = true;
      shaders->result = ps5vk_pipeline_create_shaders(device, pipeline);
   }
   const VkResult result = shaders->result;
   mtx_unlock(&shaders->lock);
   return result;
}

/* Whether the pipeline declares a dynamic state. */
static bool
ps5vk_state_is_dynamic(const VkGraphicsPipelineCreateInfo *info, VkDynamicState state)
{
   for (uint32_t index = 0; info->pDynamicState && index < info->pDynamicState->dynamicStateCount;
        index++) {
      if (info->pDynamicState->pDynamicStates[index] == state)
         return true;
   }
   return false;
}

/* Why command buffers cannot draw with a pipeline of this state yet, or NULL.
 * The draw path encodes the vertex input, the reserved push-constant buffer
 * and the viewport and scissor the command buffer holds (ps5vk_draw.c); it
 * encodes no descriptor sets, no dynamic state beyond the viewport and
 * scissor, and no rasterization, depth, stencil or blend state beyond what
 * b4-headless and the c1-clear probe used. */
static const char *
ps5vk_draw_refusal(const VkGraphicsPipelineCreateInfo *info,
                   const struct vk_pipeline_layout *layout)
{
   const VkPipelineRasterizationStateCreateInfo *const raster = info->pRasterizationState;
   const VkPipelineDepthStencilStateCreateInfo *const depth = info->pDepthStencilState;
   const VkPipelineColorBlendStateCreateInfo *const blend = info->pColorBlendState;
   const VkPipelineViewportStateCreateInfo *const viewport = info->pViewportState;
   /* R7: every set the stage reads has its own table and its own user-data
    * pointer (ps5vk_draw.c), so a layout's *set count* is no longer a reason to
    * refuse a draw. The limit that still exists is the number of sets this
    * driver advertises, and it is named where it is reached: a binding a stage
    * reads in a set past it is refused when the compiler options are built
    * (ps5vk_descriptor_options), a bind of such a set when the set is bound
    * (ps5vk_CmdBindDescriptorSets), and a stage's metadata that names one when
    * the table would be built (ps5vk_draw.c). A layout that declares more sets
    * than that, none of which a shader reads, needs no table and draws. */
   for (uint32_t index = 0; info->pDynamicState && index < info->pDynamicState->dynamicStateCount;
        index++) {
      const VkDynamicState state = info->pDynamicState->pDynamicStates[index];
      if (state != VK_DYNAMIC_STATE_VIEWPORT && state != VK_DYNAMIC_STATE_SCISSOR &&
          state != VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE &&
          state != VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE &&
          state != VK_DYNAMIC_STATE_DEPTH_COMPARE_OP &&
          state != VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE &&
          state != VK_DYNAMIC_STATE_STENCIL_OP &&
          state != VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK &&
          state != VK_DYNAMIC_STATE_STENCIL_WRITE_MASK &&
          state != VK_DYNAMIC_STATE_STENCIL_REFERENCE &&
          state != VK_DYNAMIC_STATE_DEPTH_BIAS)
         return "drawing with dynamic state other than the viewport, scissor, depth and stencil "
                "state, and the depth bias (VK_DYNAMIC_STATE_DEPTH_BIAS), is not supported yet";
   }
   /* The depth bias's clamp, refused in the static form here and in the dynamic
    * form at the draw (ps5vk_draw.c): Vulkan clamps the bias to
    * +-depthBiasClamp, this hardware's PA_SU_POLY_OFFSET_CLAMP measured inert on
    * the D32 float path, and capping the bias in the driver would report a wrong
    * depth as a success. A pipeline that declares VK_DYNAMIC_STATE_DEPTH_BIAS
    * ignores this member (Valid Usage), so the refusal is the draw's there. */
   if (raster->depthBiasEnable && raster->depthBiasClamp != 0.0f &&
       !ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_DEPTH_BIAS))
      return "drawing with a depth bias clamped to a non-zero value: the clamp needs a runner "
             "probe, because this hardware's PA_SU_POLY_OFFSET_CLAMP (0x2df) measured inert on "
             "the D32 float path and capping the bias in the driver would report a wrong depth "
             "as a success (PS5_VULKAN_REQUESTSv2.md, the clamp decision)";
   /* Valid usage: rasterization state is always present. The two states left
    * are the ones a feature bit gates -- polygonMode by fillModeNonSolid and
    * depthClampEnable by depthClamp, both reported false -- so refusing them is
    * the specification's own answer. cullMode, rasterizerDiscardEnable and
    * depthBiasEnable are core Vulkan 1.0 with no feature bit at all, and R1
    * programs them where the pipeline is built (ps5vk_graphics_pipeline_create,
    * PS5_VULKAN_REQUESTS.md R1): PA_SU_SC_MODE_CNTL's CULL_FRONT, CULL_BACK and
    * FACE, PA_CL_CLIP_CNTL's DX_RASTERIZATION_KILL, and the
    * PA_SU_POLY_OFFSET_* block. */
   if (raster->depthClampEnable || raster->polygonMode != VK_POLYGON_MODE_FILL)
      return "drawing other than filled polygons, or with depth clamping, is not supported yet: "
             "both are gated by a feature this device reports false (fillModeNonSolid, "
             "depthClamp)";
   /* Depth bias is the third state R1 names and the one whose words are only
    * part of the pipeline's: the three factors are ps5-opengl's own floats and
    * ps5vk_graphics_pipeline_create keeps them, while the block's format word
    * belongs to the depth attachment, which a Vulkan 1.0 pipeline does not name
    * -- so the draw programs the block and refuses a depth format whose word is
    * not measured, by name (ps5vk_draw.c, PS5_VULKAN_REQUESTS.md, R1). */
   /* Depth test, write and compare are Phase C5 and the stencil test is round
    * 12: a pipeline that enables it records the three stencil state words from
    * the dynamic state beside its depth ones (ps5vk_stencil_registers), and a
    * rendering with no stencil attachment ignores the test the way Vulkan says
    * it does. Depth bounds is the state still refused by name. */
   if (depth && depth->depthBoundsTestEnable)
      return "drawing with a depth-bounds test is not supported yet";
   if (blend && blend->logicOpEnable)
      return "drawing with logic operations is not supported yet";
   const VkPipelineRenderingCreateInfo *const rendering =
      vk_get_pipeline_rendering_create_info(info);
   for (uint32_t index = 0; blend && index < blend->attachmentCount; index++) {
      uint32_t word = 0;
      if (!ps5vk_blend_control(&blend->pAttachments[index], &word))
         return "drawing with a blend factor this driver cannot program: the second-source "
                "factors need dual-source blending, which this device does not advertise "
                "(maxFragmentDualSrcAttachments 0)";
      /* Vulkan: blending is not supported for an integer attachment, and the CB
       * bypasses it for one (Mesa's ac_build_cb_state). A pipeline that asks for
       * both is refused rather than drawn unblended. */
      if (word != 0 && rendering != NULL && index < rendering->colorAttachmentCount) {
         const struct ps5vk_colour_format *const colour =
            ps5vk_find_colour_format(rendering->pColorAttachmentFormats[index]);
         if (colour != NULL &&
             (colour->cb_number_type == 4 /* UINT */ || colour->cb_number_type == 5 /* SINT */))
            return "drawing with blending into an integer colour attachment, which the "
                   "hardware bypasses and Vulkan forbids";
      }
      /* 0xf writes every channel and 0 writes none, which is what vk_meta's
       * depth clear asks for; anything between them would need the target's
       * channel order. */
      if (blend->pAttachments[index].colorWriteMask != 0xf &&
          blend->pAttachments[index].colorWriteMask != 0)
         return "drawing with colour write masks other than RGBA or none is not supported yet";
   }
   if (!viewport || viewport->viewportCount != 1 || viewport->scissorCount != 1)
      return "drawing without one viewport and one scissor is not supported yet";
   if ((!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_VIEWPORT) && !viewport->pViewports) ||
       (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_SCISSOR) && !viewport->pScissors))
      return "drawing without a static or dynamic viewport and scissor is not supported yet";
   return NULL;
}

/* The state vkCmdBindPipeline puts into the command buffer: the viewport and
 * scissor a pipeline declares statically, and, for a pipeline that declares
 * them dynamic, the count alone. Vulkan's count is the pipeline's: every
 * viewport vkCmdSetViewport sets has to lie inside it, and this driver accepts
 * one viewport and one scissor (ps5vk_draw_refusal), so a pipeline names 1
 * whatever the command buffer set. What the pipeline declares dynamic stays
 * otherwise unset, so the command buffer keeps the arrays vkCmdSetViewport and
 * vkCmdSetScissor left there. */
static void
ps5vk_pipeline_dynamic_state(const VkGraphicsPipelineCreateInfo *info,
                             struct vk_dynamic_graphics_state *dynamic)
{
   vk_dynamic_graphics_state_init(dynamic);
   dynamic->vp.viewport_count = 1;
   BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT);
   dynamic->vp.scissor_count = 1;
   BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_VP_SCISSOR_COUNT);
   if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_VIEWPORT)) {
      dynamic->vp.viewports[0] = info->pViewportState->pViewports[0];
      BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_VP_VIEWPORTS);
   }
   if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_SCISSOR)) {
      dynamic->vp.scissors[0] = info->pViewportState->pScissors[0];
      BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_VP_SCISSORS);
   }
   /* R8: the depth bias's own state, static unless the pipeline declares
    * VK_DYNAMIC_STATE_DEPTH_BIAS -- the rule the viewport, scissor, depth and
    * stencil states above follow. A pipeline that declares it leaves the
    * command buffer's values alone, so vkCmdSetDepthBias is the only source;
    * the draw reads whichever arrived (ps5vk_draw.c), and both forms refuse the
    * same thing, a non-zero depthBiasClamp. */
   const VkPipelineRasterizationStateCreateInfo *const raster = info->pRasterizationState;
   if (raster != NULL) {
      /* Vulkan 1.0's VK_DYNAMIC_STATE_DEPTH_BIAS makes the three *factors*
       * dynamic and leaves depthBiasEnable static (the two are separate bits in
       * Mesa's state for that reason), so the enable always comes from the
       * pipeline and only the factors are skipped when they are dynamic. */
      dynamic->rs.depth_bias.enable = raster->depthBiasEnable;
      BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE);
      if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_DEPTH_BIAS)) {
         dynamic->rs.depth_bias.constant_factor = raster->depthBiasConstantFactor;
         dynamic->rs.depth_bias.clamp = raster->depthBiasClamp;
         dynamic->rs.depth_bias.slope_factor = raster->depthBiasSlopeFactor;
         BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS);
      }
   }
   /* The depth state a draw's DB_DEPTH_CONTROL comes from, static unless the
    * pipeline declares it dynamic (Phase C5, ps5vk_depth_control). */
   if (info->pDepthStencilState != NULL) {
      if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)) {
         dynamic->ds.depth.test_enable = info->pDepthStencilState->depthTestEnable;
         BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE);
      }
      if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE)) {
         dynamic->ds.depth.write_enable = info->pDepthStencilState->depthWriteEnable;
         BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE);
      }
      if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP)) {
         dynamic->ds.depth.compare_op = info->pDepthStencilState->depthCompareOp;
         BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP);
      }
      /* The stencil test's own state (round 12): the enable, the two faces'
       * operations and their reference, compare mask and write mask, which the
       * draw turns into DB_DEPTH_CONTROL's stencil bits, DB_STENCIL_CONTROL and
       * the two DB_STENCILREFMASK words (ps5vk_stencil_registers). Each is
       * static unless the pipeline declares that state dynamic, the same rule
       * the depth state above follows. */
      if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE)) {
         dynamic->ds.stencil.test_enable = info->pDepthStencilState->stencilTestEnable;
         BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE);
      }
      for (unsigned face = 0; face < 2; face++) {
         struct vk_stencil_test_face_state *const state =
            face == 0 ? &dynamic->ds.stencil.front : &dynamic->ds.stencil.back;
         const VkStencilOpState *const op =
            face == 0 ? &info->pDepthStencilState->front : &info->pDepthStencilState->back;
         if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_STENCIL_OP)) {
            state->op.fail = op->failOp;
            state->op.pass = op->passOp;
            state->op.depth_fail = op->depthFailOp;
            state->op.compare = op->compareOp;
            BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_DS_STENCIL_OP);
         }
         if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
            state->compare_mask = op->compareMask;
            BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK);
         }
         if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
            state->write_mask = op->writeMask;
            BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK);
         }
         if (!ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
            state->reference = (uint8_t)op->reference;
            BITSET_SET(dynamic->set, MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE);
         }
      }
   }
}

/* The push-constant bytes a layout declares and the stages that read them. */
static void
ps5vk_push_constant_range(const struct vk_pipeline_layout *layout, uint32_t *bytes,
                          VkShaderStageFlags *stages)
{
   *bytes = 0;
   *stages = 0;
   for (uint32_t index = 0; layout && index < layout->push_range_count; index++) {
      const VkPushConstantRange *const range = &layout->push_ranges[index];
      *bytes = MAX2(*bytes, range->offset + range->size);
      *stages |= range->stageFlags;
   }
}

/* The reserved uniform-buffer binding the push constants reach a stage
 * through, into the compiler options (ps5vk_nir.c). It is one descriptor at
 * the END of the set-0 table a draw builds: the application's own set-0
 * bindings start at the table's first entry (ps5vk_descriptor_options), and
 * two bindings cannot share one entry, so a stage that reads both a push
 * constant and an application uniform buffer gets a table holding both. A
 * layout with no set 0 leaves it at the start, where it has always been. */
static void
ps5vk_push_constant_options(const struct vk_pipeline_layout *layout, PsbcCompileOptions *options)
{
   uint32_t offset = 0;
   if (layout != NULL && layout->set_count > 0 && layout->set_layouts[0] != NULL) {
      const struct ps5vk_descriptor_set_layout *const set_layout =
         container_of(layout->set_layouts[0], struct ps5vk_descriptor_set_layout, vk);
      offset = set_layout->table_bytes;
   }
   options->descriptor_bindings[options->descriptor_binding_count++] = (PsbcDescriptorBinding){
      .set = 0,
      .binding = PS5VK_PUSH_CONSTANT_BINDING,
      .type = PSBC_DESCRIPTOR_UNIFORM_BUFFER,
      .array_size = 1,
      .offset = offset,
      .stride = PS5VK_UNIFORM_BUFFER_DESCRIPTOR_BYTES,
   };
}

/* The primitive type a topology is linked as, or 0 for one this driver does not
 * map. Mesa's META_RECT_LIST vertices already hold two triangles per rectangle,
 * so they are a list here, as they are in nvk (vk_to_nv9097_primitive_topology). */
static uint32_t
ps5vk_link_primitive_type(VkPrimitiveTopology topology)
{
   /* Not a switch: META_RECT_LIST is Mesa's value outside the enumeration. */
   if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
       topology == VK_PRIMITIVE_TOPOLOGY_META_RECT_LIST_MESA)
      return PS5VK_LINK_TRIANGLE_LIST;
   if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
      return PS5VK_LINK_TRIANGLE_STRIP;
   if (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
      return PS5VK_LINK_LINE_LIST;
   return 0;
}

static VkResult
ps5vk_graphics_pipeline_create(struct ps5vk_device *device, const VkGraphicsPipelineCreateInfo *info,
                               const VkAllocationCallbacks *allocator, VkPipeline *out_pipeline)
{
   VK_FROM_HANDLE(vk_pipeline_layout, layout, info->layout);
   const VkPipelineShaderStageCreateInfo *vertex = NULL;
   const VkPipelineShaderStageCreateInfo *pixel = NULL;
   for (uint32_t i = 0; i < info->stageCount; i++) {
      if (info->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT)
         vertex = &info->pStages[i];
      else if (info->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
         pixel = &info->pStages[i];
   }
   if (!vertex)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "a vertex stage is required");
   /* The topologies this driver links: a triangle list (which is what Mesa's
    * META_RECT_LIST vertices already are, as in nvk) and a triangle strip, which
    * is core Vulkan 1.0 with no feature bit gating it and what a tessellated mesh
    * is built as -- consecutive rows sharing an edge, which a list cannot express
    * without regenerating the mesh. The strip's alternating winding for its second
    * and later triangles is the hardware's own (the link's primitive type below),
    * so the front-face and cull state this driver programs is untouched and a
    * culled strip is culled by the same rule a list is. Every other topology is
    * still refused by name. */
   const VkPrimitiveTopology topology =
      info->pInputAssemblyState != NULL ? info->pInputAssemblyState->topology
                                        : VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
   const uint32_t link_primitive_type = ps5vk_link_primitive_type(topology);
   if (link_primitive_type == 0 || info->pInputAssemblyState->primitiveRestartEnable)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "only triangle lists, triangle strips and line lists without primitive "
                       "restart are supported");
   /* R8: a line list is core Vulkan 1.0 with no feature bit gating it -- the class
    * of the strip -- and it brings the state a triangle does not have: its width
    * and its rasterization rules. The width is the one this device advertises,
    * 1.0 (lineWidthRange, wideLines unclaimed), which Valid Usage requires of a
    * pipeline without wideLines anyway; anything else is refused by name rather
    * than drawn at 1.0. The rules are Vulkan's non-strict lines (strictLines is
    * reported false), programmed at the draw (ps5vk_draw.c). A four-sample line is
    * rasterized differently again and nothing has measured one here. */
   const bool line = link_primitive_type == PS5VK_LINK_LINE_LIST;
   if (line && info->pRasterizationState != NULL &&
       info->pRasterizationState->lineWidth != 1.0f &&
       !ps5vk_state_is_dynamic(info, VK_DYNAMIC_STATE_LINE_WIDTH))
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "a line width of %g needs wideLines, which this device does not "
                       "advertise (lineWidthRange is 1.0 to 1.0)",
                       (double)info->pRasterizationState->lineWidth);
   if (line && info->pMultisampleState != NULL &&
       info->pMultisampleState->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "a multisampled line list needs a probe of its own: only one-sample "
                       "lines are measured");
   /* One sample or four (C8): the colour target's register block carries the
    * count (ps5vk_draw.c, CB_COLOR0_ATTRIB.NUM_SAMPLES), and nothing in the
    * compiled shader does. */
   if (info->pMultisampleState &&
       info->pMultisampleState->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT &&
       info->pMultisampleState->rasterizationSamples != VK_SAMPLE_COUNT_4_BIT)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "only one-sample and four-sample pipelines are supported");

   uint32_t push_constant_bytes = 0;
   VkShaderStageFlags push_constant_stages = 0;
   ps5vk_push_constant_range(layout, &push_constant_bytes, &push_constant_stages);
   if (!pixel)
      push_constant_stages &= ~VK_SHADER_STAGE_FRAGMENT_BIT;
   if (push_constant_bytes > PS5VK_MAX_PUSH_CONSTANT_BYTES)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "push-constant ranges reach %u bytes, past %d",
                       push_constant_bytes, PS5VK_MAX_PUSH_CONSTANT_BYTES);

   /* The compiler is told the topology when it changes the shaders, which a
    * line list does: the vertex stage is an NGG primitive shader whose primitive
    * export carries the vertices of one primitive, three unless the compiler
    * knows better (radv_get_num_vertices_per_prim), and a line has two -- the
    * same key RADV compiles a line pipeline with (ia.topology, psbc_compile.c).
    * The fragment stage takes it too, as RADV's does: a line's fragments are
    * front-facing by definition. Triangles keep 0, so every triangle pipeline
    * compiles to exactly the packages the goldens hold. */
   const uint32_t compile_primitive_type = line ? PS5VK_LINK_LINE_LIST : 0u;
   PsbcCompileOptions vertex_options = {
      .target = PSBC_TARGET_PS5,
      .stage = PSBC_STAGE_VERTEX,
      .entrypoint = vertex->pName,
      .optimise = true,
      .ngg = true,
      .address32_hi = (uint32_t)PS5VK_ADDRESS_HIGH_WORD,
      .primitive_type = compile_primitive_type,
   };
   PsbcCompileOptions pixel_options = {
      .target = PSBC_TARGET_PS5,
      .stage = PSBC_STAGE_FRAGMENT,
      .entrypoint = pixel ? pixel->pName : "main",
      .optimise = true,
      .address32_hi = (uint32_t)PS5VK_ADDRESS_HIGH_WORD,
      .primitive_type = compile_primitive_type,
      /* The pipeline's sample count reaches the compiler as well as the
       * rasterizer registers (ps5vk_draw.c, ps5vk_multisample_registers): the
       * fragment stage's sample-mask and barycentric lowerings are built for
       * it. Only a four-sample pipeline sets it, so a one-sample pipeline
       * compiles to exactly the stage every frame before Phase C8 ran
       * (docs/M5_PHASE_C.md). */
      .rasterization_samples =
         info->pMultisampleState &&
               info->pMultisampleState->rasterizationSamples == VK_SAMPLE_COUNT_4_BIT
            ? 4u
            : 0u,
   };
   struct ps5vk_vertex_binding vertex_bindings[PS5VK_MAX_VERTEX_BINDINGS] = {0};
   VkResult result =
      ps5vk_vertex_input_options(device, info->pVertexInputState, vertex_bindings, &vertex_options);
   if (result == VK_SUCCESS)
      result = ps5vk_descriptor_options(device, layout, VK_SHADER_STAGE_VERTEX_BIT, &vertex_options);
   if (result == VK_SUCCESS && pixel)
      result = ps5vk_descriptor_options(device, layout, VK_SHADER_STAGE_FRAGMENT_BIT, &pixel_options);
   uint32_t exports = 0;
   if (result == VK_SUCCESS && pixel)
      result = ps5vk_color_export_options(device, info, &exports);
   if (result != VK_SUCCESS)
      return result;
   pixel_options.spi_shader_col_format = exports;
   if (push_constant_stages & VK_SHADER_STAGE_VERTEX_BIT)
      ps5vk_push_constant_options(layout, &vertex_options);
   if (push_constant_stages & VK_SHADER_STAGE_FRAGMENT_BIT)
      ps5vk_push_constant_options(layout, &pixel_options);

   struct ps5vk_pipeline *const pipeline =
      vk_object_zalloc(&device->vk, allocator, sizeof(*pipeline), VK_OBJECT_TYPE_PIPELINE);
   if (!pipeline)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   pipeline->spi_shader_col_format = exports;
   /* What the link is told: the topology's own DI_PT value, so the hardware
    * alternates a strip's winding the way the specification defines it. */
   pipeline->link_primitive_type = link_primitive_type;
   /* The colour write masks a draw programs, as one word: Vulkan gives **each**
    * attachment its own VkPipelineColorBlendAttachmentState::colorWriteMask, and
    * the two registers that carry them are per-target nibble fields --
    * CB_TARGET_MASK's TARGETn_ENABLE and CB_SHADER_MASK's OUTPUTn_ENABLE, four
    * bits each at 4n (R_028238, R_02823C). This used to be attachment 0's mask
    * alone on the assumption that the rest are the same, which holds for one
    * attachment and masks every other target's writes off for a rendering into
    * more than one (R7 step 1b, found by the vkQuake port project reading the
    * register database). A pipeline with no blend state writes RGBA to the first
    * attachment, which is the word a single-attachment draw has always had. */
   pipeline->colour_write_mask = 0;
   const uint32_t blend_attachments =
      info->pColorBlendState != NULL ? info->pColorBlendState->attachmentCount : 0;
   for (uint32_t at = 0; at < PS5VK_MAX_COLOR_TARGETS; at++) {
      const uint32_t mask = at < blend_attachments
                               ? (uint32_t)info->pColorBlendState->pAttachments[at].colorWriteMask
                               : (at == 0 ? 0xfu : 0u);
      pipeline->colour_write_mask |= (mask & 0xfu) << (4 * at);
   }
   /* No fragment stage means no colour output, even with attachment writes enabled. */
   if (!pixel)
      pipeline->colour_write_mask = 0;
   /* The word a blending draw records, 0 for one that does not blend. A state
    * this driver cannot program has already set draw_refusal below, so the word
    * only has to be safe here. A state that reads the blend constants takes
    * them from the pipeline: the draw records CB_BLEND_RED/GREEN/BLUE/ALPHA
    * beside the blend word when blend_uses_constants is set, and every pipeline
    * that does not leaves those registers alone, so no earlier draw's stream
    * changes. */
   uint32_t blend_control = 0;
   bool blend_uses_constants = false;
   if (info->pColorBlendState != NULL && info->pColorBlendState->attachmentCount > 0) {
      (void)ps5vk_blend_control(&info->pColorBlendState->pAttachments[0], &blend_control);
      blend_uses_constants =
         blend_control != 0 && ps5vk_blend_uses_constants(&info->pColorBlendState->pAttachments[0]);
   }
   /* R1's rasterization state, into the words the draw records. Each is
    * recorded only when the pipeline asks for something the hardware's defaults
    * do not already do, so every earlier draw's register table is unchanged.
    * The state's standing is the request's own point: cullMode,
    * rasterizerDiscardEnable and depthBiasEnable are core Vulkan 1.0, with no
    * feature bit to gate them, and ps5vk_draw_refusal no longer refuses them
    * (the depth bias's own gate is the depth attachment's format, which only
    * the rendering knows: ps5vk_draw.c, PS5_VULKAN_REQUESTS.md, R1). */
   const VkPipelineRasterizationStateCreateInfo *const raster = info->pRasterizationState;
   uint32_t rasterizer_word = 0;
   switch (raster->cullMode) {
   case VK_CULL_MODE_NONE:
      break;
   case VK_CULL_MODE_FRONT_BIT:
      rasterizer_word |= UINT32_C(1) << 0; /* CULL_FRONT */
      break;
   case VK_CULL_MODE_BACK_BIT:
      rasterizer_word |= UINT32_C(1) << 1; /* CULL_BACK */
      break;
   case VK_CULL_MODE_FRONT_AND_BACK:
      rasterizer_word |= UINT32_C(3) << 0;
      break;
   default:
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "cull mode %d is not a Vulkan cull mode", (int)raster->cullMode);
   }
   /* FACE: 0 is the hardware's counter-clockwise front face, 1 clockwise. The
    * viewport carries Vulkan's orientation (a negative y scale), and the
    * console measured what that does to culling: v0-cull's cull-back frame
    * keeps the triangle the specification says it should with this mapping, so
    * Vulkan's front face is the hardware's own here (docs/M5_PHASE_C.md, R1). */
   if (raster->frontFace == VK_FRONT_FACE_CLOCKWISE)
      rasterizer_word |= UINT32_C(1) << 2;
   else if (raster->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "front face %d is not a Vulkan front face",
                       (int)raster->frontFace);
   /* Culling is a polygon's: Vulkan's cullMode does not apply to lines (Face
    * Determination, which lines do not have), so a line pipeline programs none
    * whatever the application set, rather than rely on the rasterizer ignoring
    * the cull bits for a line. */
   if (line)
      rasterizer_word &= ~(UINT32_C(3) << 0);
   pipeline->rasterizer_word = rasterizer_word;
   pipeline->discard_rasterizer = raster->rasterizerDiscardEnable;
   pipeline->line_rasterizer = line;
   pipeline->blend_control = blend_control;
   pipeline->blend_uses_constants = blend_uses_constants;
   for (unsigned index = 0; index < 4; index++) {
      const float value = blend_uses_constants ? info->pColorBlendState->blendConstants[index] : 0.0f;
      memcpy(&pipeline->blend_constants[index], &value, sizeof(value));
   }
   pipeline->shaders.stage.start = -1;
   pipeline->push_constant_bytes = push_constant_bytes;
   pipeline->push_constant_stages = push_constant_stages;
   memcpy(pipeline->vertex_bindings, vertex_bindings, sizeof(vertex_bindings));
   mtx_init(&pipeline->shaders.lock, mtx_plain);
   pipeline->draw_refusal = ps5vk_draw_refusal(info, layout);
   if (!pipeline->draw_refusal)
      ps5vk_pipeline_dynamic_state(info, &pipeline->dynamic);

   result = ps5vk_compile_stage(device, vertex, &vertex_options, push_constant_bytes,
                                &pipeline->stages[PS5VK_PIPELINE_STAGE_VERTEX]);
   if (result == VK_SUCCESS)
      result = ps5vk_compile_stage(device, pixel, &pixel_options, push_constant_bytes,
                                   &pipeline->stages[PS5VK_PIPELINE_STAGE_PIXEL]);
   if (result != VK_SUCCESS) {
      ps5vk_pipeline_free(device, pipeline, allocator);
      return result;
   }
   /* R10: what the stages read, taken from the modules while they are still the
    * application's to destroy.
    *
    * Two things come out of the same scan. The input-attachment bindings are the
    * ones the draw fills from the subpass rather than from an application's
    * write (Vulkan forbids a write of that type), with the shader's own
    * InputAttachmentIndex saying which subpass input each one reads. And every
    * stage's binding list is narrowed to the bindings the module itself
    * declares: the driver hands the compiler every layout binding whose
    * stageFlags name the stage, and the compiler reports them all back in its
    * metadata, so a stage that never reads one would otherwise make the draw
    * demand a write for a descriptor no instruction fetches -- which for an
    * input attachment is a descriptor the application may not write at all.
    *
    * A stage whose module is NIR rather than SPIR-V (Mesa's meta stages) keeps
    * its metadata exactly as the compiler wrote it. */
   const VkPipelineShaderStageCreateInfo *const stage_infos[PS5VK_PIPELINE_STAGE_COUNT] = {
      vertex, pixel};
   for (unsigned index = 0; index < PS5VK_PIPELINE_STAGE_COUNT; index++) {
      VK_FROM_HANDLE(ps5vk_shader_module, module,
                     stage_infos[index] ? stage_infos[index]->module : VK_NULL_HANDLE);
      if (module == NULL)
         continue;
      const uint32_t room = PS5VK_MAX_INPUT_ATTACHMENTS - pipeline->input_attachment_count;
      pipeline->input_attachment_count +=
         ps5vk_spirv_input_attachments(module->words, module->size, (uint8_t)index,
                                       pipeline->input_attachments +
                                          pipeline->input_attachment_count,
                                       room);
      struct ps5vk_spirv_binding declared[PS5VK_MAX_SPIRV_BINDINGS];
      const uint32_t declared_count = ps5vk_spirv_bindings(
         module->words, module->size, declared, PS5VK_MAX_SPIRV_BINDINGS);
      PsbcShaderMetadata *const metadata = &pipeline->stages[index].metadata;
      uint32_t kept = 0;
      for (uint32_t b = 0; b < metadata->descriptor_binding_count; b++) {
         const PsbcDescriptorBinding *const binding = &metadata->descriptor_bindings[b];
         bool used = false;
         for (uint32_t d = 0; d < declared_count && !used; d++)
            used = declared[d].set == binding->set && declared[d].binding == binding->binding;
         if (used)
            metadata->descriptor_bindings[kept++] = *binding;
      }
      metadata->descriptor_binding_count = kept;
   }
#if DETECT_OS_LINUX
   ps5vk_pipeline_dump(pipeline);
#endif

   *out_pipeline = ps5vk_pipeline_to_handle(pipeline);
   return VK_SUCCESS;
}

static VkResult
ps5vk_CreateGraphicsPipelines_untimed(VkDevice _device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
                              const VkGraphicsPipelineCreateInfo *pCreateInfos,
                              const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* The Vulkan cache handle is unused; compiler outputs use the driver disk cache. */
   (void)pipelineCache;
   VkResult result = VK_SUCCESS;
   uint32_t index = 0;
   for (; index < createInfoCount; index++) {
      pPipelines[index] = VK_NULL_HANDLE;
      const VkResult created =
         ps5vk_graphics_pipeline_create(device, &pCreateInfos[index], pAllocator, &pPipelines[index]);
      if (created == VK_SUCCESS)
         continue;
      if (result == VK_SUCCESS)
         result = created;
      if (pCreateInfos[index].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)
         break;
   }
   for (; index < createInfoCount; index++)
      pPipelines[index] = VK_NULL_HANDLE;
   return result;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateGraphicsPipelines(VkDevice _device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
                              const VkGraphicsPipelineCreateInfo *pCreateInfos,
                              const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_CreateGraphicsPipelines_untimed(_device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
   ps5vk_hitch_end(PS5VK_HITCH_PIPELINE, hitch);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyPipeline(VkDevice _device, VkPipeline _pipeline, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_pipeline, pipeline, _pipeline);
   if (pipeline)
      ps5vk_pipeline_free(device, pipeline, pAllocator);
}
