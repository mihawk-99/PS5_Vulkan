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
};

static const struct pipeline_description kProbeSets[] = {
   {"m2", 0, {{0}}, 0, NO_DESCRIPTOR, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL},
   {"m3", 0, {{0}}, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL},
   {"m3-vertex", 2,
    {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8}}, 24,
    NO_DESCRIPTOR, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL},
   {"m3-texture", 2, {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}}, 16,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL},
   {"m4-depth", 2,
    {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}}, 28,
    NO_DESCRIPTOR, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL},
   {"m4-blend", 2,
    {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}}, 28,
    NO_DESCRIPTOR, true, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL},
   /* Phase B7's orientation probe: the M2 set's options, a half-target triangle. */
   {"b7-corner", 0, {{0}}, 0, NO_DESCRIPTOR, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, NULL},
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
         .lineWidth = 1.0f,
      };
      const VkPipelineMultisampleStateCreateInfo multisample = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      };
      const VkPipelineColorBlendAttachmentState blend_attachment = {
         .blendEnable = description->blend,
         .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
         .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .colorBlendOp = VK_BLEND_OP_ADD,
         .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
         .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
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
         .stageCount = 2,
         .pStages = stages,
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
