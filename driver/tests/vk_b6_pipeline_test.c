/*
 * PS5 Vulkan driver - Phase B6 test: graphics pipelines from SPIR-V.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B6 (docs/M5_PHASE_B.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * On the PC it builds, through the Vulkan API, the pipeline each probe set
 * describes (probes/<set>/compile.txt: vertex input, descriptor bindings,
 * blending) from the set's SPIR-V. It requires the driver's packages
 * (PS5VK_PIPELINE_DUMP) to equal the set's vertex.bin and pixel.bin, the
 * packages the hardware ran. PS5VK_PROBES names the probes directory. The
 * PS5 build only links the pipeline path; it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "ps5vk_test.h"

static VkInstance g_instance;
static VkPhysicalDevice g_physical;
static VkDevice g_device;

#define NO_DESCRIPTOR VK_DESCRIPTOR_TYPE_MAX_ENUM

/* One pipeline as a probe set's compile.txt describes it. */
struct pipeline_description {
   const char *set;
   uint32_t attribute_count;
   VkVertexInputAttributeDescription attributes[2];
   uint32_t stride;
   VkDescriptorType descriptor;
   bool blend;
   VkPrimitiveTopology topology;
   /* Both stages' entry point; NULL means "main". */
   const char *entrypoint;
   /* The rasterization state's lineWidth; 0 means 1.0. */
   float line_width;
   bool primitive_restart;
   /* Which stages the pipeline has: both (0), the vertex stage alone (1) or the
    * fragment stage alone (2). */
   unsigned stages;
   /* R71: blend with the second source's factors (SRC1_COLOR and
    * ONE_MINUS_SRC1_COLOR, SRC1_ALPHA and ONE_MINUS_SRC1_ALPHA). */
   bool dual_source;
};

static const struct pipeline_description kProbeSets[] = {
   {"m2", 0, {{0}}, 0, NO_DESCRIPTOR, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0.0f, false, 0, false},
   {"m3", 0, {{0}}, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0.0f, false, 0, false},
   {"m3-vertex", 2,
    {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8}}, 24,
    NO_DESCRIPTOR, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0.0f, false, 0, false},
   {"m3-texture", 2, {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}}, 16,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0.0f, false, 0, false},
   {"m4-depth", 2,
    {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}}, 28,
    NO_DESCRIPTOR, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0.0f, false, 0, false},
   {"m4-blend", 2,
    {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}}, 28,
    NO_DESCRIPTOR, true, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0.0f, false, 0, false},
   /* Phase B7's orientation probe: the M2 set's options, a half-target triangle. */
   {"b7-corner", 0, {{0}}, 0, NO_DESCRIPTOR, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0.0f, false, 0, false},
   /* R71: a second colour source, blended with the SRC1 factors. The probe
    * build compiled its pixel stage with MRT0 FP16 and checked the packed
    * SPI_SHADER_COL_FORMAT 0x44 -- the second source exported to MRT1 in
    * MRT0's format -- so an equal package is the driver's dual-source path. */
   {"r71-dual-source", 2,
    {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}}, 28,
    NO_DESCRIPTOR, true, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL, 0.0f, false, 0, true},
};

static VkShaderModule
create_module(const uint32_t *words, size_t bytes)
{
   const VkShaderModuleCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = bytes,
      .pCode = words,
   };
   VkShaderModule module = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateShaderModule)(g_device, &info, NULL, &module) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   return module;
}

/* Creates the pipeline of description from two shader modules, with every
 * other object it needs, and destroys those again; the pipeline outlives
 * them. */
static VkResult
create_pipeline(const struct pipeline_description *description, VkShaderModule vertex,
                VkShaderModule pixel, VkPipeline *pipeline)
{
   const __typeof__(&vkCreateGraphicsPipelines) create =
      VK_FUNCTION(g_instance, CreateGraphicsPipelines);
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkRenderPass render_pass = VK_NULL_HANDLE;
   *pipeline = VK_NULL_HANDLE;

   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = description->descriptor,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   const bool has_set = description->descriptor != NO_DESCRIPTOR;
   if (has_set &&
       VK_FUNCTION(g_instance, CreateDescriptorSetLayout)(g_device, &set_info, NULL, &set_layout) !=
          VK_SUCCESS)
      return VK_ERROR_INITIALIZATION_FAILED;
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = has_set ? 1 : 0,
      .pSetLayouts = &set_layout,
   };
   const VkAttachmentDescription attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   const VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   const VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &reference,
   };
   const VkRenderPassCreateInfo pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkResult result = VK_FUNCTION(g_instance, CreatePipelineLayout)(g_device, &layout_info, NULL, &layout);
   if (result == VK_SUCCESS)
      result = VK_FUNCTION(g_instance, CreateRenderPass)(g_device, &pass_info, NULL, &render_pass);
   if (result == VK_SUCCESS) {
      const char *const name = description->entrypoint ? description->entrypoint : "main";
      const VkPipelineShaderStageCreateInfo stages[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = name},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = pixel, .pName = name},
      };
      /* A single stage is the first element (the vertex stage) or the second. */
      const uint32_t stage_count = description->stages == 0 ? 2u : 1u;
      const VkPipelineShaderStageCreateInfo *const first_stage =
         description->stages == 2 ? &stages[1] : &stages[0];
      const VkVertexInputBindingDescription vertex_binding = {0, description->stride,
                                                              VK_VERTEX_INPUT_RATE_VERTEX};
      const VkPipelineVertexInputStateCreateInfo vertex_input = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
         .vertexBindingDescriptionCount = description->attribute_count ? 1 : 0,
         .pVertexBindingDescriptions = &vertex_binding,
         .vertexAttributeDescriptionCount = description->attribute_count,
         .pVertexAttributeDescriptions = description->attributes,
      };
      const VkPipelineInputAssemblyStateCreateInfo assembly = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = description->topology,
         .primitiveRestartEnable = description->primitive_restart,
      };
      const VkViewport viewport = {0, 0, 3840, 2160, 0, 1};
      const VkRect2D scissor = {{0, 0}, {3840, 2160}};
      const VkPipelineViewportStateCreateInfo viewport_state = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
         .viewportCount = 1,
         .pViewports = &viewport,
         .scissorCount = 1,
         .pScissors = &scissor,
      };
      const VkPipelineRasterizationStateCreateInfo rasterization = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .polygonMode = VK_POLYGON_MODE_FILL,
         .cullMode = VK_CULL_MODE_NONE,
         .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
         .lineWidth = description->line_width != 0.0f ? description->line_width : 1.0f,
      };
      const VkPipelineMultisampleStateCreateInfo multisample = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      };
      const VkPipelineColorBlendAttachmentState blend_attachment = {
         .blendEnable = description->blend,
         .srcColorBlendFactor = description->dual_source ? VK_BLEND_FACTOR_SRC1_COLOR
                                                         : VK_BLEND_FACTOR_SRC_ALPHA,
         .dstColorBlendFactor = description->dual_source ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR
                                                         : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .colorBlendOp = VK_BLEND_OP_ADD,
         .srcAlphaBlendFactor = description->dual_source ? VK_BLEND_FACTOR_SRC1_ALPHA
                                                         : VK_BLEND_FACTOR_ONE,
         .dstAlphaBlendFactor = description->dual_source ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA
                                                         : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .alphaBlendOp = VK_BLEND_OP_ADD,
         .colorWriteMask = 0xf,
      };
      const VkPipelineColorBlendStateCreateInfo blend = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &blend_attachment,
      };
      const VkGraphicsPipelineCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = stage_count,
         .pStages = first_stage,
         .pVertexInputState = &vertex_input,
         .pInputAssemblyState = &assembly,
         .pViewportState = &viewport_state,
         .pRasterizationState = &rasterization,
         .pMultisampleState = &multisample,
         .pColorBlendState = &blend,
         .layout = layout,
         .renderPass = render_pass,
         .subpass = 0,
      };
      result = create(g_device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
   }
   VK_FUNCTION(g_instance, DestroyRenderPass)(g_device, render_pass, NULL);
   VK_FUNCTION(g_instance, DestroyPipelineLayout)(g_device, layout, NULL);
   VK_FUNCTION(g_instance, DestroyDescriptorSetLayout)(g_device, set_layout, NULL);
   return result;
}

static void
destroy_pipeline(VkPipeline pipeline)
{
   VK_FUNCTION(g_instance, DestroyPipeline)(g_device, pipeline, NULL);
}

#if defined(__linux__)
static uint8_t *
read_file(const char *path, size_t *size)
{
   FILE *const file = fopen(path, "rb");
   if (!file)
      return NULL;
   fseek(file, 0, SEEK_END);
   const long length = ftell(file);
   fseek(file, 0, SEEK_SET);
   uint8_t *const data = length > 0 ? malloc((size_t)length) : NULL;
   const bool read = data && fread(data, 1, (size_t)length, file) == (size_t)length;
   fclose(file);
   if (!read) {
      free(data);
      return NULL;
   }
   *size = (size_t)length;
   return data;
}

/* Whether two files hold the same bytes; when they do not and report is set,
 * prints both sizes. */
static bool
same_bytes(const char *built, const char *expected, bool report)
{
   size_t built_size = 0;
   size_t expected_size = 0;
   uint8_t *const a = read_file(built, &built_size);
   uint8_t *const b = read_file(expected, &expected_size);
   const bool same = a && b && built_size == expected_size && memcmp(a, b, built_size) == 0;
   if (!same && report)
      printf("  (%s: %zu bytes, %s: %zu bytes)\n", built, built_size, expected, expected_size);
   free(a);
   free(b);
   return same;
}

static VkShaderModule
module_from_file(const char *probes, const char *set, const char *stage)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s/%s.spv", probes, set, stage);
   size_t size = 0;
   uint8_t *const words = read_file(path, &size);
   VkShaderModule module = VK_NULL_HANDLE;
   if (words && size % 4 == 0) {
      uint32_t *const aligned = malloc(size);
      memcpy(aligned, words, size);
      module = create_module(aligned, size);
      free(aligned);
   }
   free(words);
   return module;
}

/* Builds description's pipeline from spirv_set's SPIR-V, dumping its packages
 * to <directory>/<label>-{vertex,pixel}.bin. */
static VkResult
build_and_dump(const char *probes, const char *directory, const char *label, const char *spirv_set,
               const struct pipeline_description *description)
{
   char prefix[1024];
   snprintf(prefix, sizeof(prefix), "%s/%s", directory, label);
   setenv("PS5VK_PIPELINE_DUMP", prefix, 1);
   const VkShaderModule vertex = module_from_file(probes, spirv_set, "vertex");
   const VkShaderModule pixel = module_from_file(probes, spirv_set, "pixel");
   VkPipeline pipeline = VK_NULL_HANDLE;
   const VkResult result = vertex && pixel ? create_pipeline(description, vertex, pixel, &pipeline)
                                           : VK_ERROR_INITIALIZATION_FAILED;
   destroy_pipeline(pipeline);
   VK_FUNCTION(g_instance, DestroyShaderModule)(g_device, pixel, NULL);
   VK_FUNCTION(g_instance, DestroyShaderModule)(g_device, vertex, NULL);
   unsetenv("PS5VK_PIPELINE_DUMP");
   return result;
}

/* Whether <directory>/<label>-<stage>.bin equals probes/<set>/<stage>.bin;
 * report as for same_bytes. */
static bool
dump_matches(const char *probes, const char *directory, const char *label, const char *set,
             const char *stage, bool report)
{
   char built[1024];
   char expected[1024];
   snprintf(built, sizeof(built), "%s/%s-%s.bin", directory, label, stage);
   snprintf(expected, sizeof(expected), "%s/%s/%s.bin", probes, set, stage);
   return same_bytes(built, expected, report);
}

static void
check_probe_pipelines(void)
{
   const char *const probes = getenv("PS5VK_PROBES");
   char directory[] = "/tmp/ps5vk-b6-XXXXXX";
   if (!probes || !mkdtemp(directory)) {
      check(false, "PS5VK_PROBES names the probes directory, and a scratch directory exists");
      return;
   }

   VkFormatProperties properties;
   bool vertex_formats = true;
   const VkFormat formats[] = {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                               VK_FORMAT_R32G32B32A32_SFLOAT};
   for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
      VK_FUNCTION(g_instance, GetPhysicalDeviceFormatProperties)(g_physical, formats[i], &properties);
      /* The buffer bit of every one of them, and the two texel-buffer bits of
       * the ones whose required rows ask for them -- R32G32_SFLOAT and
       * R32G32B32A32_SFLOAT, fetched through a samplerBuffer in blocker round 5
       * and stored through an imageBuffer in round 6; R32G32B32_SFLOAT's row
       * requires neither. R32G32_SFLOAT and R32G32B32A32_SFLOAT also carry
       * V0-formats' sampled features now, so their optimal-tiling features are
       * no longer empty. */
      const bool texel_buffer = formats[i] != VK_FORMAT_R32G32B32_SFLOAT;
      const VkFormatFeatureFlags buffer =
         VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
         (texel_buffer ? VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                            VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT
                       : 0);
      vertex_formats = vertex_formats && properties.bufferFeatures == buffer;
   }
   check(vertex_formats, "R32G32, R32G32B32 and R32G32B32A32 SFLOAT are vertex-buffer formats");

   for (size_t i = 0; i < sizeof(kProbeSets) / sizeof(kProbeSets[0]); i++) {
      const struct pipeline_description *const set = &kProbeSets[i];
      const VkResult result = build_and_dump(probes, directory, set->set, set->set, set);
      const bool vertex_same =
         result == VK_SUCCESS && dump_matches(probes, directory, set->set, set->set, "vertex", true);
      const bool pixel_same =
         result == VK_SUCCESS && dump_matches(probes, directory, set->set, set->set, "pixel", true);
      char what[128];
      snprintf(what, sizeof(what),
               "%s: the pipeline's vertex and pixel packages equal the hardware-run packages",
               set->set);
      if (result != VK_SUCCESS)
         printf("  (%s: vkCreateGraphicsPipelines returned %d)\n", set->set, result);
      check(vertex_same && pixel_same, what);
   }

   /* The export-format rule from one SPIR-V: m4-depth's shaders with blending
    * enabled must give m4-blend's pixel package, and without it m4-depth's. */
   struct pipeline_description blended = kProbeSets[4];
   blended.blend = true;
   check(build_and_dump(probes, directory, "m4-depth-blended", "m4-depth", &blended) == VK_SUCCESS &&
            dump_matches(probes, directory, "m4-depth-blended", "m4-blend", "pixel", true) &&
            !dump_matches(probes, directory, "m4-depth-blended", "m4-depth", "pixel", false),
         "m4-depth's SPIR-V with blending compiles to m4-blend's FP16_ABGR pixel package");

   struct pipeline_description points = kProbeSets[0];
   points.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   check(build_and_dump(probes, directory, "m2-points", "m2", &points) == VK_ERROR_UNKNOWN,
         "a point-list pipeline is refused with VK_ERROR_UNKNOWN");

   /* The topologies (R6, R8): a strip compiles to exactly the list's packages --
    * nothing about three vertices a primitive changes -- while a line list's
    * vertex stage is compiled for two vertices a primitive and has to differ.
    * The topologies nothing has measured stay refused, and so do the line's
    * states this device does not advertise. */
   struct pipeline_description strip = kProbeSets[2];
   strip.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
   check(build_and_dump(probes, directory, "m3-vertex-strip", "m3-vertex", &strip) == VK_SUCCESS &&
            dump_matches(probes, directory, "m3-vertex-strip", "m3-vertex", "vertex", true) &&
            dump_matches(probes, directory, "m3-vertex-strip", "m3-vertex", "pixel", true),
         "a triangle-strip pipeline compiles to the list's packages");
   struct pipeline_description lines = kProbeSets[2];
   lines.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
   check(build_and_dump(probes, directory, "m3-vertex-lines", "m3-vertex", &lines) == VK_SUCCESS &&
            !dump_matches(probes, directory, "m3-vertex-lines", "m3-vertex", "vertex", false),
         "a line-list pipeline is created, its vertex stage compiled for two vertices a "
         "primitive rather than the list's three");
   static const struct {
      VkPrimitiveTopology topology;
      const char *what;
   } kRefused[] = {
      {VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, "a line-strip pipeline is refused"},
      {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, "a triangle-fan pipeline is refused"},
      {VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY, "a line list with adjacency is refused"},
      {VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, "a patch-list pipeline is refused"},
   };
   for (size_t at = 0; at < sizeof(kRefused) / sizeof(kRefused[0]); at++) {
      struct pipeline_description refused = kProbeSets[2];
      refused.topology = kRefused[at].topology;
      check(build_and_dump(probes, directory, "m3-vertex-refused", "m3-vertex", &refused) ==
               VK_ERROR_UNKNOWN,
            kRefused[at].what);
   }
   /* The fragment-less pipeline (the vkQuake sky-stencil shape): the vertex stage
    * alone is created, its vertex package is the one the set's pipeline has, and
    * the pixel package it is linked with is the driver's own empty fragment shader
    * (ps5vk_nir_noop_fragment), not the set's. A pipeline without a vertex stage
    * is still refused. */
   struct pipeline_description vertex_only = kProbeSets[4];
   vertex_only.stages = 1;
   check(build_and_dump(probes, directory, "m4-depth-vertex-only", "m4-depth", &vertex_only) ==
               VK_SUCCESS &&
            dump_matches(probes, directory, "m4-depth-vertex-only", "m4-depth", "vertex", true) &&
            !dump_matches(probes, directory, "m4-depth-vertex-only", "m4-depth", "pixel", false),
         "a pipeline with no fragment stage is created: the set's vertex package, linked with "
         "the driver's empty fragment shader");
   char noop[1024];
   snprintf(noop, sizeof(noop), "%s/m4-depth-vertex-only-pixel.bin", directory);
   size_t noop_size = 0;
   uint8_t *const noop_package = read_file(noop, &noop_size);
   /* Every stage's package is an ELF container whose .shader_header section
    * starts with AGC's header magic (ps5vk_package_sections checks the rest when
    * the pipeline is first drawn, which v0_fragmentless does). */
   static const uint8_t kElf[4] = {0x7f, 'E', 'L', 'F'};
   static const uint8_t kHeaderMagic[4] = {0x31, 0x32, 0x33, 0x34};
   bool header_magic = false;
   for (size_t at = 0; noop_package && at + 4 <= noop_size && !header_magic; at++)
      header_magic = memcmp(noop_package + at, kHeaderMagic, 4) == 0;
   check(noop_package && noop_size >= 4 && memcmp(noop_package, kElf, 4) == 0 && header_magic,
         "the empty fragment shader is an AGC package like any other stage's");
   free(noop_package);
   struct pipeline_description fragment_only = kProbeSets[4];
   fragment_only.stages = 2;
   check(build_and_dump(probes, directory, "m4-depth-fragment-only", "m4-depth", &fragment_only) ==
            VK_ERROR_UNKNOWN,
         "a pipeline with no vertex stage is refused");

   struct pipeline_description restart = lines;
   restart.primitive_restart = true;
   check(build_and_dump(probes, directory, "m3-vertex-restart", "m3-vertex", &restart) ==
            VK_ERROR_UNKNOWN,
         "a line list with primitive restart is refused");
   struct pipeline_description wide = lines;
   wide.line_width = 2.0f;
   check(build_and_dump(probes, directory, "m3-vertex-wide", "m3-vertex", &wide) ==
            VK_ERROR_UNKNOWN,
         "a line list 2.0 wide is refused: wideLines is not advertised");

   struct pipeline_description misnamed = kProbeSets[0];
   misnamed.entrypoint = "missing";
   check(build_and_dump(probes, directory, "m2-missing", "m2", &misnamed) == VK_ERROR_UNKNOWN,
         "stages naming an entry point their SPIR-V lacks are refused with VK_ERROR_UNKNOWN");

   char command[1100];
   snprintf(command, sizeof(command), "rm -rf '%s'", directory);
   if (system(command) != 0)
      printf("  (could not remove %s)\n", directory);
}
#endif

/* SPIR-V without an entry point: the driver must refuse it before the
 * compiler, which crashes on such a module. */
static void
check_invalid_spirv(void)
{
   static const uint32_t header_only[5] = {0x07230203, 0x00010000, 0, 1, 0};
   const VkShaderModule vertex = create_module(header_only, sizeof(header_only));
   const VkShaderModule pixel = create_module(header_only, sizeof(header_only));
   check(vertex != VK_NULL_HANDLE && pixel != VK_NULL_HANDLE, "shader modules are created");
   VkPipeline pipeline = VK_NULL_HANDLE;
   const VkResult result = create_pipeline(&kProbeSets[0], vertex, pixel, &pipeline);
   check(result == VK_ERROR_UNKNOWN && pipeline == VK_NULL_HANDLE,
         "SPIR-V without a shader is refused with VK_ERROR_UNKNOWN and no pipeline");
   destroy_pipeline(pipeline);
   VK_FUNCTION(g_instance, DestroyShaderModule)(g_device, pixel, NULL);
   VK_FUNCTION(g_instance, DestroyShaderModule)(g_device, vertex, NULL);
}

int
main(void)
{
   test_begin("B6 pipeline");
   if (test_create_device(&g_instance, &g_physical, &g_device)) {
#if defined(__linux__)
      check_probe_pipelines();
#endif
      check_invalid_spirv();
   }
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
