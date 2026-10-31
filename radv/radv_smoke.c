/*
 * PS5 Vulkan - RADV smoke test.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The first program RADV runs on the console (docs/VULKAN_1_4_PLAN.md, Phase 1):
 * an instance and a device through the RADV archive the title links, then four
 * results a working GPU path has to produce exactly -- a buffer fill, a buffer
 * copy, a compute dispatch and a triangle drawn over a clear -- each read back
 * and compared on the CPU. Every line goes to klog and to radv-smoke.txt beside
 * the title (/app0), which FTP reaches.
 *
 * It links no Vulkan loader: every command comes from RADV's
 * vk_icdGetInstanceProcAddr, as a title that links the archive gets them.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "radv_smoke_shaders.h"
#include "../src/present_metadata.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

#if defined(__PROSPERO__)
#include <sys/stat.h>
#include <unistd.h>

#define RESULTS_PATH "/app0/radv-smoke.txt"
/* libkernel's klog writer: a title's stderr does not reach klog, and klog is
 * what tools/run-title.py watches for the run's end. */
int sceKernelDebugOutText(int channel, const char *text);
/* ps5platform/klog.h: the driver's messages on standard error, in klog. */
int ps5_klog_capture_stderr(const char *prefix);
#else
#define RESULTS_PATH "radv-smoke.txt"
#endif

#ifndef MIN2
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#endif

static FILE *results;
static unsigned passed, failed;

static void
report(const char *format, ...)
{
   char line[512];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line), format, args);
   va_end(args);
   fprintf(stderr, "[radv-smoke] %s\n", line);
   fflush(stderr);
#if defined(__PROSPERO__)
   char klog_line[600];
   snprintf(klog_line, sizeof(klog_line), "[radv-smoke] %s\n", line);
   sceKernelDebugOutText(0, klog_line);
#endif
   if (results) {
      fprintf(results, "%s\n", line);
      fflush(results);
   }
}

static void
check(bool ok, const char *what)
{
   report("%s %s", ok ? "PASS" : "FAIL", what);
   if (ok)
      passed++;
   else
      failed++;
}

/* ------------------------------------------------------------ entry points */

#define INSTANCE_COMMANDS(X)                                                                       \
   X(DestroyInstance)                                                                              \
   X(EnumeratePhysicalDevices)                                                                     \
   X(GetPhysicalDeviceProperties2)                                                                 \
   X(GetPhysicalDeviceQueueFamilyProperties)                                                       \
   X(GetPhysicalDeviceMemoryProperties)                                                            \
   X(GetPhysicalDeviceFeatures)                                                                    \
   X(GetPhysicalDeviceFeatures2)                                                                   \
   X(CreateDevice)                                                                                 \
   X(EnumerateDeviceExtensionProperties)                                                           \
   X(GetDeviceProcAddr)

#define DEVICE_COMMANDS(X)                                                                         \
   X(DestroyDevice)                                                                                \
   X(GetDeviceQueue)                                                                               \
   X(QueueSubmit)                                                                                  \
   X(QueueWaitIdle)                                                                                \
   X(DeviceWaitIdle)                                                                               \
   X(CreateFence)                                                                                  \
   X(DestroyFence)                                                                                 \
   X(WaitForFences)                                                                                \
   X(ResetFences)                                                                                  \
   X(CreateCommandPool)                                                                            \
   X(DestroyCommandPool)                                                                           \
   X(AllocateCommandBuffers)                                                                       \
   X(BeginCommandBuffer)                                                                           \
   X(EndCommandBuffer)                                                                             \
   X(ResetCommandBuffer)                                                                           \
   X(CreateBuffer)                                                                                 \
   X(DestroyBuffer)                                                                                \
   X(GetBufferMemoryRequirements)                                                                  \
   X(CreateImage)                                                                                  \
   X(DestroyImage)                                                                                 \
   X(GetImageMemoryRequirements)                                                                   \
   X(CreateImageView)                                                                              \
   X(DestroyImageView)                                                                             \
   X(AllocateMemory)                                                                               \
   X(FreeMemory)                                                                                   \
   X(BindBufferMemory)                                                                             \
   X(BindImageMemory)                                                                              \
   X(MapMemory)                                                                                    \
   X(CmdFillBuffer)                                                                                \
   X(CmdCopyBuffer)                                                                                \
   X(CmdCopyImageToBuffer)                                                                         \
   X(CmdPipelineBarrier2)                                                                          \
   X(CmdBindPipeline)                                                                              \
   X(CmdBindDescriptorSets)                                                                        \
   X(CmdDispatch)                                                                                  \
   X(CmdPushConstants)                                                                             \
   X(CmdBeginRendering)                                                                            \
   X(CmdEndRendering)                                                                              \
   X(CmdDraw)                                                                                      \
   X(CmdDrawIndexed)                                                                               \
   X(CmdDrawIndirect)                                                                              \
   X(CmdDrawIndexedIndirect)                                                                       \
   X(CmdDrawIndirectCount)                                                                         \
   X(CmdBindIndexBuffer)                                                                           \
   X(CmdSetViewport)                                                                               \
   X(CmdSetScissor)                                                                                \
   X(CreateShaderModule)                                                                           \
   X(DestroyShaderModule)                                                                          \
   X(CreateDescriptorSetLayout)                                                                    \
   X(DestroyDescriptorSetLayout)                                                                   \
   X(CreatePipelineLayout)                                                                         \
   X(DestroyPipelineLayout)                                                                        \
   X(CreateComputePipelines)                                                                       \
   X(CreateGraphicsPipelines)                                                                      \
   X(DestroyPipeline)                                                                              \
   X(CreateDescriptorPool)                                                                         \
   X(DestroyDescriptorPool)                                                                        \
   X(AllocateDescriptorSets)                                                                       \
   X(UpdateDescriptorSets)

/* Loaded when c->mesh. */
#define MESH_COMMANDS(X)                                                                           \
   X(CmdDrawMeshTasksEXT)                                                                          \
   X(CmdDrawMeshTasksIndirectEXT)

#define DECLARE(name) static PFN_vk##name vk##name;
INSTANCE_COMMANDS(DECLARE)
DEVICE_COMMANDS(DECLARE)
MESH_COMMANDS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance;

static bool
load_instance(VkInstance instance)
{
   bool ok = true;
#define LOAD(name)                                                                                 \
   vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name);                       \
   if (!vk##name) {                                                                                \
      report("missing vk" #name);                                                                  \
      ok = false;                                                                                  \
   }
   INSTANCE_COMMANDS(LOAD)
#undef LOAD
   return ok;
}

static bool
load_device(VkDevice device)
{
   bool ok = true;
#define LOAD(name)                                                                                 \
   vk##name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name);                               \
   if (!vk##name) {                                                                                \
      report("missing vk" #name);                                                                  \
      ok = false;                                                                                  \
   }
   DEVICE_COMMANDS(LOAD)
#undef LOAD
   return ok;
}

/* ----------------------------------------------------------------- context */

struct context {
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
   VkQueue queue;
   uint32_t family;
   VkPhysicalDeviceMemoryProperties memory;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   /* VK_KHR_fragment_shader_barycentric is reported and enabled. */
   bool barycentric;
   /* VK_EXT_provoking_vertex is reported and enabled. */
   bool provoking;
   /* VK_KHR_fragment_shading_rate is reported, and its pipeline rate enabled. */
   bool shading_rate;
   /* VK_KHR_acceleration_structure is reported and enabled, with buffer
    * device addresses. */
   bool acceleration_structure;
   /* VK_KHR_display is reported and enabled with VK_KHR_surface, and
    * VK_KHR_swapchain with them. */
   bool display;
   /* VK_EXT_descriptor_buffer is reported and enabled (test_sparse_timing). */
   bool descriptor_buffer;
   /* VK_EXT_mesh_shader is reported and its mesh shaders enabled (test_mesh),
    * and its task shaders when reported (test_task). */
   bool mesh;
   bool task;
   /* How render_readback_with records its draw (test_geometry's draw checks):
    * directly by default. Indices are 16-bit. */
   struct {
      enum {
         DRAW_DIRECT,
         DRAW_INDEXED,
         DRAW_INDIRECT,
         DRAW_INDEXED_INDIRECT,
         DRAW_INDIRECT_COUNT,
         DRAW_MESH,
         DRAW_MESH_INDIRECT,
      } mode;
      /* DRAW_MESH's workgroups. */
      uint32_t groups[3];
      bool restart;
      bool provoking_last;
      VkBuffer indices;
      VkBuffer args;
      VkBuffer count;
      uint32_t first_index;
      int32_t vertex_offset;
      uint32_t max_draws;
   } draw;
};

static uint32_t
memory_type(const struct context *c, uint32_t bits, VkMemoryPropertyFlags wanted)
{
   for (uint32_t i = 0; i < c->memory.memoryTypeCount; i++) {
      if ((bits & (1u << i)) && (c->memory.memoryTypes[i].propertyFlags & wanted) == wanted)
         return i;
   }
   return UINT32_MAX;
}

struct buffer {
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *map;
   VkDeviceSize size;
};

static bool
buffer_create(struct context *c, VkDeviceSize size, VkBufferUsageFlags usage, struct buffer *out)
{
   *out = (struct buffer){.size = size};
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (vkCreateBuffer(c->device, &info, NULL, &out->buffer) != VK_SUCCESS)
      return false;
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->device, out->buffer, &req);
   const uint32_t type = memory_type(c, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   const VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   if (type == UINT32_MAX || vkAllocateMemory(c->device, &alloc, NULL, &out->memory) != VK_SUCCESS ||
       vkBindBufferMemory(c->device, out->buffer, out->memory, 0) != VK_SUCCESS ||
       vkMapMemory(c->device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->map) != VK_SUCCESS)
      return false;
   return true;
}

static void
buffer_destroy(struct context *c, struct buffer *b)
{
   vkDestroyBuffer(c->device, b->buffer, NULL);
   vkFreeMemory(c->device, b->memory, NULL);
}

static bool
begin(struct context *c)
{
   const VkCommandBufferBeginInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   return vkResetCommandBuffer(c->cmd, 0) == VK_SUCCESS && vkBeginCommandBuffer(c->cmd, &info) == VK_SUCCESS;
}

/* Ends, submits and waits for the command buffer, and makes what the GPU wrote
 * visible to the host's reads. */
static bool
submit_and_wait(struct context *c, const char *what)
{
   const VkMemoryBarrier2 host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
   };
   const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &host,
   };
   vkCmdPipelineBarrier2(c->cmd, &dependency);
   VkResult result = vkEndCommandBuffer(c->cmd);
   if (result != VK_SUCCESS) {
      report("%s: vkEndCommandBuffer returned %d", what, result);
      return false;
   }
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &c->cmd,
   };
   vkResetFences(c->device, 1, &c->fence);
   result = vkQueueSubmit(c->queue, 1, &submit, c->fence);
   if (result != VK_SUCCESS) {
      report("%s: vkQueueSubmit returned %d", what, result);
      return false;
   }
   result = vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_C(20000000000));
   if (result != VK_SUCCESS) {
      report("%s: vkWaitForFences returned %d", what, result);
      return false;
   }
   return true;
}

/* ------------------------------------------------------------------- tests */

static void
test_fill(struct context *c)
{
   struct buffer b;
   const VkDeviceSize size = 65536;
   if (!buffer_create(c, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b)) {
      check(false, "fill: buffer");
      return;
   }
   memset(b.map, 0xcd, size);
   bool ok = begin(c);
   vkCmdFillBuffer(c->cmd, b.buffer, 0, size, 0x12345678u);
   ok = ok && submit_and_wait(c, "fill");
   uint32_t wrong = 0, first_wrong = UINT32_MAX;
   const uint32_t *words = b.map;
   for (uint32_t i = 0; ok && i < size / 4; i++) {
      if (words[i] != 0x12345678u) {
         if (first_wrong == UINT32_MAX)
            first_wrong = i;
         wrong++;
      }
   }
   if (wrong)
      report("fill: %u of %u words wrong, the first at %u reads 0x%08x", wrong, (unsigned)(size / 4), first_wrong,
             words[first_wrong]);
   check(ok && wrong == 0, "fill: vkCmdFillBuffer of 64 KiB reads back exactly");
   buffer_destroy(c, &b);
}

static void
test_copy(struct context *c)
{
   struct buffer src, dst;
   const VkDeviceSize size = 1u << 20;
   if (!buffer_create(c, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src) ||
       !buffer_create(c, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst)) {
      check(false, "copy: buffers");
      return;
   }
   uint32_t *const in = src.map;
   for (uint32_t i = 0; i < size / 4; i++)
      in[i] = i * 2654435761u;
   memset(dst.map, 0, size);
   bool ok = begin(c);
   const VkBufferCopy region = {.srcOffset = 0, .dstOffset = 0, .size = size};
   vkCmdCopyBuffer(c->cmd, src.buffer, dst.buffer, 1, &region);
   ok = ok && submit_and_wait(c, "copy");
   const bool same = ok && memcmp(src.map, dst.map, size) == 0;
   if (ok && !same) {
      const uint32_t *const out = dst.map;
      for (uint32_t i = 0; i < size / 4; i++) {
         if (out[i] != in[i]) {
            report("copy: word %u reads 0x%08x, not 0x%08x", i, out[i], in[i]);
            break;
         }
      }
   }
   check(same, "copy: vkCmdCopyBuffer of 1 MiB reads back exactly");
   buffer_destroy(c, &src);
   buffer_destroy(c, &dst);
}

static VkShaderModule
shader(struct context *c, const uint32_t *code, size_t bytes)
{
   const VkShaderModuleCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = bytes,
      .pCode = code,
   };
   VkShaderModule module = VK_NULL_HANDLE;
   vkCreateShaderModule(c->device, &info, NULL, &module);
   return module;
}

static void
test_compute(struct context *c)
{
   enum { COUNT = 4096 };
   struct buffer b;
   if (!buffer_create(c, COUNT * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &b)) {
      check(false, "compute: buffer");
      return;
   }
   memset(b.map, 0, COUNT * 4);
   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkDescriptorSet set = VK_NULL_HANDLE;
   VkShaderModule module = shader(c, radv_smoke_comp, sizeof(radv_smoke_comp));
   bool ok = module && vkCreateDescriptorSetLayout(c->device, &set_info, NULL, &set_layout) == VK_SUCCESS;
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   ok = ok && vkCreatePipelineLayout(c->device, &layout_info, NULL, &layout) == VK_SUCCESS;
   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
         },
      .layout = layout,
   };
   ok = ok && vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS;
   check(ok, "compute: SPIR-V compiles to a pipeline on the console");
   const VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &size,
   };
   ok = ok && vkCreateDescriptorPool(c->device, &pool_info, NULL, &pool) == VK_SUCCESS;
   const VkDescriptorSetAllocateInfo set_alloc = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   ok = ok && vkAllocateDescriptorSets(c->device, &set_alloc, &set) == VK_SUCCESS;
   if (ok) {
      const VkDescriptorBufferInfo buffer_info = {.buffer = b.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
      const VkWriteDescriptorSet write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &buffer_info,
      };
      vkUpdateDescriptorSets(c->device, 1, &write, 0, NULL);
      ok = begin(c);
      vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
      vkCmdDispatch(c->cmd, COUNT / 64, 1, 1);
      ok = ok && submit_and_wait(c, "compute");
   }
   uint32_t wrong = 0;
   const uint32_t *const values = b.map;
   for (uint32_t i = 0; ok && i < COUNT; i++) {
      if (values[i] != i * 3u + 7u) {
         if (wrong == 0)
            report("compute: value %u reads %u, not %u", i, values[i], i * 3u + 7u);
         wrong++;
      }
   }
   check(ok && wrong == 0, "compute: 64 workgroups of 64 write every value their index names");
   vkDestroyPipeline(c->device, pipeline, NULL);
   vkDestroyDescriptorPool(c->device, pool, NULL);
   vkDestroyPipelineLayout(c->device, layout, NULL);
   vkDestroyDescriptorSetLayout(c->device, set_layout, NULL);
   vkDestroyShaderModule(c->device, module, NULL);
   buffer_destroy(c, &b);
}

enum { TARGET_SIZE = 256 };

/* Draws vertex_count vertices with the stages into a cleared blue 256-square
 * RGBA8 target and reads the target back into readback. */
static bool
render_readback_with(struct context *c, const char *what, const VkPipelineShaderStageCreateInfo *stages,
                     uint32_t stage_count, VkPrimitiveTopology topology, uint32_t patch_points,
                     uint32_t vertex_count, struct buffer *readback, VkBuffer storage, const void *pipeline_next)
{
   const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory image_memory = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkDescriptorSet set = VK_NULL_HANDLE;
   memset(readback->map, 0, TARGET_SIZE * TARGET_SIZE * 4);
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {TARGET_SIZE, TARGET_SIZE, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   bool ok = vkCreateImage(c->device, &image_info, NULL, &image) == VK_SUCCESS;
   if (ok) {
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(c->device, image, &req);
      const VkMemoryAllocateInfo alloc = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = memory_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      };
      ok = vkAllocateMemory(c->device, &alloc, NULL, &image_memory) == VK_SUCCESS &&
           vkBindImageMemory(c->device, image, image_memory, 0) == VK_SUCCESS;
   }
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   ok = ok && vkCreateImageView(c->device, &view_info, NULL, &view) == VK_SUCCESS;
   if (storage) {
      /* One storage buffer at set 0, binding 0, for every graphics stage. */
      const VkDescriptorSetLayoutBinding binding = {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | (c->mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : 0),
      };
      const VkDescriptorSetLayoutCreateInfo set_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings = &binding,
      };
      const VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
      const VkDescriptorPoolCreateInfo pool_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1,
         .poolSizeCount = 1,
         .pPoolSizes = &size,
      };
      ok = ok && vkCreateDescriptorSetLayout(c->device, &set_info, NULL, &set_layout) == VK_SUCCESS &&
           vkCreateDescriptorPool(c->device, &pool_info, NULL, &pool) == VK_SUCCESS;
      const VkDescriptorSetAllocateInfo set_alloc = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout,
      };
      ok = ok && vkAllocateDescriptorSets(c->device, &set_alloc, &set) == VK_SUCCESS;
      if (ok) {
         const VkDescriptorBufferInfo buffer_info = {.buffer = storage, .offset = 0, .range = VK_WHOLE_SIZE};
         const VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_info,
         };
         vkUpdateDescriptorSets(c->device, 1, &write, 0, NULL);
      }
   }
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = storage ? 1 : 0,
      .pSetLayouts = &set_layout,
   };
   ok = ok && vkCreatePipelineLayout(c->device, &layout_info, NULL, &layout) == VK_SUCCESS;
   const VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   const VkPipelineInputAssemblyStateCreateInfo assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = topology,
      .primitiveRestartEnable = c->draw.restart,
   };
   const VkPipelineTessellationStateCreateInfo tessellation = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
      .patchControlPoints = patch_points,
   };
   const VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
   };
   const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
      .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT,
   };
   const VkPipelineRasterizationStateCreateInfo raster = {
      .pNext = c->draw.provoking_last ? &provoking_state : NULL,
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
   const VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xf};
   const VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
   };
   const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   const VkPipelineDynamicStateCreateInfo dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dynamic_states,
   };
   const VkPipelineRenderingCreateInfo rendering_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .pNext = pipeline_next,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &format,
   };
   const VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_info,
      .stageCount = stage_count,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &assembly,
      .pTessellationState = patch_points ? &tessellation : NULL,
      .pViewportState = &viewport_state,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic,
      .layout = layout,
   };
   ok = ok && vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS;
   char label[96];
   snprintf(label, sizeof(label), "%s: a graphics pipeline compiles on the console", what);
   check(ok, label);

   if (ok) {
      ok = begin(c);
      const VkImageMemoryBarrier2 to_attachment = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .image = image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      const VkDependencyInfo first = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .imageMemoryBarrierCount = 1,
         .pImageMemoryBarriers = &to_attachment,
      };
      vkCmdPipelineBarrier2(c->cmd, &first);
      const VkRenderingAttachmentInfo attachment = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = view,
         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .clearValue = {.color = {.float32 = {0.0f, 0.0f, 1.0f, 1.0f}}},
      };
      const VkRenderingInfo rendering = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = {{0, 0}, {TARGET_SIZE, TARGET_SIZE}},
         .layerCount = 1,
         .colorAttachmentCount = 1,
         .pColorAttachments = &attachment,
      };
      vkCmdBeginRendering(c->cmd, &rendering);
      vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (set)
         vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
      const VkViewport viewport = {0.0f, 0.0f, (float)TARGET_SIZE, (float)TARGET_SIZE, 0.0f, 1.0f};
      const VkRect2D scissor = {{0, 0}, {TARGET_SIZE, TARGET_SIZE}};
      vkCmdSetViewport(c->cmd, 0, 1, &viewport);
      vkCmdSetScissor(c->cmd, 0, 1, &scissor);
      if (c->draw.indices)
         vkCmdBindIndexBuffer(c->cmd, c->draw.indices, 0, VK_INDEX_TYPE_UINT16);
      switch (c->draw.mode) {
      case DRAW_DIRECT:
         vkCmdDraw(c->cmd, vertex_count, 1, 0, 0);
         break;
      case DRAW_INDEXED:
         vkCmdDrawIndexed(c->cmd, vertex_count, 1, c->draw.first_index, c->draw.vertex_offset, 0);
         break;
      case DRAW_INDIRECT:
         vkCmdDrawIndirect(c->cmd, c->draw.args, 0, c->draw.max_draws, sizeof(VkDrawIndirectCommand));
         break;
      case DRAW_INDEXED_INDIRECT:
         vkCmdDrawIndexedIndirect(c->cmd, c->draw.args, 0, c->draw.max_draws, sizeof(VkDrawIndexedIndirectCommand));
         break;
      case DRAW_INDIRECT_COUNT:
         vkCmdDrawIndirectCount(c->cmd, c->draw.args, 0, c->draw.count, 0, c->draw.max_draws,
                                sizeof(VkDrawIndirectCommand));
         break;
      case DRAW_MESH:
         vkCmdDrawMeshTasksEXT(c->cmd, c->draw.groups[0], c->draw.groups[1], c->draw.groups[2]);
         break;
      case DRAW_MESH_INDIRECT:
         vkCmdDrawMeshTasksIndirectEXT(c->cmd, c->draw.args, 0, c->draw.max_draws,
                                       sizeof(VkDrawMeshTasksIndirectCommandEXT));
         break;
      }
      vkCmdEndRendering(c->cmd);
      const VkImageMemoryBarrier2 to_transfer = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .image = image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      const VkDependencyInfo second = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .imageMemoryBarrierCount = 1,
         .pImageMemoryBarriers = &to_transfer,
      };
      vkCmdPipelineBarrier2(c->cmd, &second);
      const VkBufferImageCopy copy = {
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {TARGET_SIZE, TARGET_SIZE, 1},
      };
      vkCmdCopyImageToBuffer(c->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &copy);
      ok = ok && submit_and_wait(c, what);
   }

   vkDestroyPipeline(c->device, pipeline, NULL);
   vkDestroyPipelineLayout(c->device, layout, NULL);
   vkDestroyDescriptorPool(c->device, pool, NULL);
   vkDestroyDescriptorSetLayout(c->device, set_layout, NULL);
   vkDestroyImageView(c->device, view, NULL);
   vkDestroyImage(c->device, image, NULL);
   vkFreeMemory(c->device, image_memory, NULL);
   return ok;
}

static bool
render_readback(struct context *c, const char *what, const VkPipelineShaderStageCreateInfo *stages,
                uint32_t stage_count, VkPrimitiveTopology topology, uint32_t patch_points, uint32_t vertex_count,
                struct buffer *readback)
{
   return render_readback_with(c, what, stages, stage_count, topology, patch_points, vertex_count, readback,
                               VK_NULL_HANDLE, NULL);
}

static VkPipelineShaderStageCreateInfo
stage_info(VkShaderStageFlagBits stage, VkShaderModule module)
{
   return (VkPipelineShaderStageCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = stage,
      .module = module,
      .pName = "main",
   };
}

static void
test_triangle(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "triangle: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_vert, sizeof(radv_smoke_vert));
   VkShaderModule frag = shader(c, radv_smoke_frag, sizeof(radv_smoke_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   bool ok = vert && frag &&
             render_readback(c, "triangle", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 3, &readback);

   /* The triangle covers x/128 + y/256 < 1 (Vulkan's y runs down the target);
    * every other texel keeps the clear. */
   uint32_t wrong = 0;
   const uint32_t red = 0xff0000ffu, blue = 0xffff0000u;
   const uint32_t *const texels = readback.map;
   for (uint32_t y = 0; ok && y < TARGET_SIZE; y++) {
      for (uint32_t x = 0; x < TARGET_SIZE; x++) {
         const double edge = (x + 0.5) / 128.0 + (y + 0.5) / 256.0;
         /* Texels whose centre is within half a texel of the edge may go
          * either way. */
         if (edge > 0.995 && edge < 1.005)
            continue;
         const uint32_t expected = edge < 1.0 ? red : blue;
         if (texels[y * TARGET_SIZE + x] != expected) {
            if (wrong < 4)
               report("triangle: texel (%u, %u) reads 0x%08x, not 0x%08x", x, y, texels[y * TARGET_SIZE + x],
                      expected);
            wrong++;
         }
      }
   }
   if (wrong)
      report("triangle: %u texels wrong", wrong);
   check(ok && wrong == 0, "triangle: a clear and a draw read back texel for texel");

   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* The coherence probe (docs/VULKAN_1_4_PLAN.md, S7) on the memory the driver
 * maps for the host, with no flush and no invalidate between the CPU and the
 * GPU, which is what HOST_COHERENT promises. Each part keeps the other
 * direction out of its way:
 * - the CPU writes a source the GPU then copies at once, into a region the CPU
 *   has never touched, so its lines are still in the CPU's caches and the
 *   destination's are not;
 * - the GPU fills a region the CPU has just read, so the CPU's caches hold the
 *   old words;
 * and the CPU's reads and writes of mapped memory are timed against its own
 * heap's, which says whether the mapping is cached. */
static uint64_t
now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t coherence_sink;

static uint64_t
read_ns(const void *memory, size_t bytes)
{
   const uint64_t *words = memory;
   uint64_t sum = 0;
   const uint64_t start = now_ns();
   for (size_t i = 0; i < bytes / 8; i++)
      sum += words[i];
   const uint64_t end = now_ns();
   coherence_sink += sum;
   return end - start;
}

static uint64_t
write_ns(void *memory, size_t bytes)
{
   uint64_t *words = memory;
   const uint64_t start = now_ns();
   for (size_t i = 0; i < bytes / 8; i++)
      words[i] = i;
   return now_ns() - start;
}

static void
test_coherence(struct context *c)
{
   enum { ROUNDS = 64, PIECE = 4096, STRIDE = 65536 };
   struct buffer src, dst;
   if (!buffer_create(c, PIECE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src) ||
       !buffer_create(c, (VkDeviceSize)ROUNDS * STRIDE, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst)) {
      check(false, "coherence: buffers");
      return;
   }

   /* The CPU's writes, read by the GPU. */
   unsigned stale_rounds = 0, stale_words = 0;
   bool ok = true;
   for (uint32_t round = 0; ok && round < ROUNDS; round++) {
      uint32_t *const in = src.map;
      for (uint32_t i = 0; i < PIECE / 4; i++)
         in[i] = (round + 1) * 0x9e3779b9u ^ i;
      ok = begin(c);
      const VkBufferCopy region = {.srcOffset = 0, .dstOffset = (VkDeviceSize)round * STRIDE, .size = PIECE};
      vkCmdCopyBuffer(c->cmd, src.buffer, dst.buffer, 1, &region);
      ok = ok && submit_and_wait(c, "coherence");
      const uint32_t *const out = (const uint32_t *)((const uint8_t *)dst.map + (size_t)round * STRIDE);
      unsigned wrong = 0;
      for (uint32_t i = 0; ok && i < PIECE / 4; i++)
         wrong += out[i] != ((round + 1) * 0x9e3779b9u ^ i);
      stale_rounds += wrong != 0;
      stale_words += wrong;
   }
   if (stale_rounds)
      report("coherence: %u of %u rounds read stale source words (%u words)", stale_rounds, ROUNDS, stale_words);
   check(ok && stale_rounds == 0, "coherence: the GPU reads what the CPU just wrote, without a flush");

   /* The GPU's writes over lines the CPU holds. */
   stale_rounds = 0;
   stale_words = 0;
   for (uint32_t round = 0; ok && round < ROUNDS; round++) {
      const uint32_t *const words = (const uint32_t *)((const uint8_t *)dst.map + (size_t)round * STRIDE);
      uint32_t held = 0;
      for (uint32_t i = 0; i < PIECE / 4; i++)
         held += words[i];
      coherence_sink += held;
      ok = begin(c);
      vkCmdFillBuffer(c->cmd, dst.buffer, (VkDeviceSize)round * STRIDE, PIECE, 0xa5a50000u | round);
      ok = ok && submit_and_wait(c, "coherence");
      unsigned wrong = 0;
      for (uint32_t i = 0; ok && i < PIECE / 4; i++)
         wrong += words[i] != (0xa5a50000u | round);
      stale_rounds += wrong != 0;
      stale_words += wrong;
   }
   if (stale_rounds)
      report("coherence: %u of %u rounds read stale words the CPU held (%u words)", stale_rounds, ROUNDS,
             stale_words);
   check(ok && stale_rounds == 0, "coherence: the CPU reads what the GPU wrote over lines it held, without an "
                                  "invalidate");
   buffer_destroy(c, &src);
   buffer_destroy(c, &dst);

   /* Cached or not: 16 MiB read and written, mapped and on the heap, the best
    * of three each. */
   enum { TIMED = 16 << 20 };
   struct buffer timed;
   void *const heap = malloc(TIMED);
   if (heap && buffer_create(c, TIMED, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &timed)) {
      uint64_t mapped_read = UINT64_MAX, heap_read = UINT64_MAX, mapped_write = UINT64_MAX,
               heap_write = UINT64_MAX;
      for (int i = 0; i < 3; i++) {
         mapped_write = MIN2(mapped_write, write_ns(timed.map, TIMED));
         heap_write = MIN2(heap_write, write_ns(heap, TIMED));
         mapped_read = MIN2(mapped_read, read_ns(timed.map, TIMED));
         heap_read = MIN2(heap_read, read_ns(heap, TIMED));
      }
      report("coherence: 16 MiB read in %.2f ms mapped, %.2f ms on the heap; written in %.2f ms mapped, %.2f ms "
             "on the heap",
             mapped_read / 1e6, heap_read / 1e6, mapped_write / 1e6, heap_write / 1e6);
      buffer_destroy(c, &timed);
   }
   free(heap);
}

/* The triangle of test_triangle, shaded by a fragment shader whose private
 * array of n floats (a specialisation constant) is written and read at
 * indices that depend on the texel: past a few dozen floats the compiler keeps
 * it in scratch memory. Each texel's colour is the array's sum. */
static void
test_scratch(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "scratch: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_vert, sizeof(radv_smoke_vert));
   VkShaderModule frag = shader(c, radv_smoke_scratch_frag, sizeof(radv_smoke_scratch_frag));
   static const int32_t sizes[] = {16, 64, 256, 1024};
   for (unsigned v = 0; v < sizeof(sizes) / sizeof(sizes[0]); v++) {
      const int32_t n = sizes[v];
      const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(n)};
      const VkSpecializationInfo specialisation = {
         .mapEntryCount = 1,
         .pMapEntries = &entry,
         .dataSize = sizeof(n),
         .pData = &n,
      };
      VkPipelineShaderStageCreateInfo stages[2] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
      };
      stages[1].pSpecializationInfo = &specialisation;
      char what[64];
      snprintf(what, sizeof(what), "scratch with %d floats", (int)n);
      bool ok = vert && frag &&
                render_readback(c, what, stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 3, &readback);
      uint32_t wrong = 0;
      const uint32_t blue = 0xffff0000u;
      const uint32_t *const texels = readback.map;
      for (uint32_t y = 0; ok && y < TARGET_SIZE; y++) {
         for (uint32_t x = 0; x < TARGET_SIZE; x++) {
            const double edge = (x + 0.5) / 128.0 + (y + 0.5) / 256.0;
            if (edge > 0.995 && edge < 1.005)
               continue;
            uint32_t sum = 0;
            for (int32_t i = 0; i < n; i++)
               sum += (uint32_t)((i * 7 + (int32_t)x) & 15);
            const uint32_t expected =
               edge < 1.0 ? (sum & 0xffu) | (((sum >> 8) & 0xffu) << 8) | 0xff000000u : blue;
            if (texels[y * TARGET_SIZE + x] != expected) {
               if (wrong < 4)
                  report("%s: texel (%u, %u) reads 0x%08x, not 0x%08x", what, x, y, texels[y * TARGET_SIZE + x],
                         expected);
               wrong++;
            }
         }
      }
      if (wrong)
         report("%s: %u texels wrong", what, wrong);
      char description[96];
      snprintf(description, sizeof(description), "%s: every texel has its array's sum", what);
      check(ok && wrong == 0, description);
   }
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* test_triangle's triangle, whose vertices name themselves 1, 2 and 3; the
 * fragment shader writes the three values it reads per vertex as its red,
 * green and blue bytes. With the first vertex provoking, vertex_id[i] is
 * vertex i's. */
static void
test_barycentric(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "barycentric: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_bary_vert, sizeof(radv_smoke_bary_vert));
   VkShaderModule frag = shader(c, radv_smoke_bary_frag, sizeof(radv_smoke_bary_frag));
   for (uint32_t winding = 0; winding < 2; winding++) {
   const VkBool32 other_winding = winding;
   const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(other_winding)};
   const VkSpecializationInfo specialisation = {
      .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(other_winding), .pData = &other_winding};
   VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   stages[0].pSpecializationInfo = &specialisation;
   const char *const what = winding ? "barycentric, other winding" : "barycentric";
   bool ok = vert && frag &&
             render_readback(c, what, stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 3, &readback);
   uint32_t wrong = 0;
   const uint32_t expected_inside = 0xff030201u, blue = 0xffff0000u;
   const uint32_t *const texels = readback.map;
   for (uint32_t y = 0; ok && y < TARGET_SIZE; y++) {
      for (uint32_t x = 0; x < TARGET_SIZE; x++) {
         const double edge = (x + 0.5) / 128.0 + (y + 0.5) / 256.0;
         if (edge > 0.995 && edge < 1.005)
            continue;
         const uint32_t expected = edge < 1.0 ? expected_inside : blue;
         if (texels[y * TARGET_SIZE + x] != expected) {
            if (wrong < 4)
               report("%s: texel (%u, %u) reads 0x%08x, not 0x%08x", what, x, y, texels[y * TARGET_SIZE + x],
                      expected);
            wrong++;
         }
      }
   }
   if (wrong)
      report("%s: %u texels wrong", what, wrong);
   char description[128];
   snprintf(description, sizeof(description), "%s: each vertex's value read per vertex, in the triangle's vertex order",
            what);
   check(ok && wrong == 0, description);
   }
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* dEQP-VK.fragment_shading_barycentric's triangle list: two triangles over
 * the whole target, split along the diagonal, the right-hand corners at
 * w = 16; vertex i names itself i + 1 and the fragment shader writes the
 * three values it reads per vertex as its red, green and blue bytes. */
static void
test_barycentric_pair(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "barycentric pair: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_bary6_vert, sizeof(radv_smoke_bary6_vert));
   VkShaderModule frag = shader(c, radv_smoke_bary_frag, sizeof(radv_smoke_bary_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   bool ok = vert && frag &&
             render_readback(c, "barycentric pair", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 6, &readback);
   uint32_t wrong = 0;
   const uint32_t *const texels = readback.map;
   for (uint32_t y = 0; ok && y < TARGET_SIZE; y++) {
      for (uint32_t x = 0; x < TARGET_SIZE; x++) {
         if (x == y || x + 1 == y || y + 1 == x)
            continue;
         /* Below the diagonal: the first triangle (vertices 1, 2, 3). */
         const uint32_t expected = x < y ? 0xff030201u : 0xff060504u;
         if (texels[y * TARGET_SIZE + x] != expected) {
            if (wrong < 4)
               report("barycentric pair: texel (%u, %u) reads 0x%08x, not 0x%08x", x, y,
                      texels[y * TARGET_SIZE + x], expected);
            wrong++;
         }
      }
   }
   if (wrong)
      report("barycentric pair: %u texels wrong", wrong);
   check(ok && wrong == 0, "barycentric pair: each triangle's values read per vertex, in its vertex order");
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* The order the pixel shader reads a primitive's per-vertex inputs in, against the order the primitive was drawn in
 * (docs/BARYCENTRIC_GFX101.md). The shader (bary.frag) writes vertex_id[0], [1] and [2] as red, green and blue; every
 * vertex names itself, so a texel says which vertices arrived in which slot, and the three values name the primitive.
 * This only reports: what the console does decides the fix. One line a primitive and order seen; a primitive whose
 * texels disagree among themselves shows two lines. perm=abc means slot j holds the primitive's vertex "abc[j]" of
 * its natural order (vertices first, second, third as drawn), so 012 is in order, 201 is rotated by one. */
static void
survey_case(struct context *c, const char *name, uint32_t which, VkPrimitiveTopology topology, uint32_t vertices,
            VkShaderModule vert, VkShaderModule frag, struct buffer *readback)
{
   const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(which)};
   const VkSpecializationInfo specialisation = {
      .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(which), .pData = &which};
   VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   stages[0].pSpecializationInfo = &specialisation;
   if (!render_readback(c, name, stages, 2, topology, 0, vertices, readback)) {
      check(false, name);
      return;
   }
   /* A texel's three values add up to a number no other primitive's do. */
   enum { KEYS = 256, ORDERS = 6 };
   static uint32_t seen[KEYS][ORDERS], count[KEYS][ORDERS];
   memset(seen, 0, sizeof(seen));
   memset(count, 0, sizeof(count));
   const uint32_t *const texels = readback->map;
   for (uint32_t i = 0; i < TARGET_SIZE * TARGET_SIZE; i++) {
      const uint32_t t = texels[i];
      if ((t >> 24) != 0xff || t == 0xffff0000u)   /* not drawn: the target's blue */
         continue;
      const uint32_t a = t & 0xff, b = (t >> 8) & 0xff, d = (t >> 16) & 0xff;
      const uint32_t key = a + b + d;
      if (key >= KEYS)
         continue;
      for (uint32_t o = 0; o < ORDERS; o++) {
         if (!count[key][o]) {
            seen[key][o] = t & 0xffffff;
            count[key][o] = 1;
            break;
         }
         if (seen[key][o] == (t & 0xffffff)) {
            count[key][o]++;
            break;
         }
      }
   }
   uint32_t primitives = 0, in_order = 0;
   for (uint32_t key = 0; key < KEYS; key++) {
      if (!count[key][0])
         continue;
      primitives++;
      /* The primitive's vertices in the order the specification gives the pixel shader, the provoking vertex first
       * (the CTS's formulas): a list's p-th is 3p, 3p+1, 3p+2 (ids one more); a strip's t-th is t, t+1, t+2 and, when t is
       * odd, t, t+2, t+1; a fan's is t+1, t+2, 0. Its key is the sum of the three ids. */
      uint32_t natural[3];
      if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
         const uint32_t p = (key - 6) / 9;
         natural[0] = 3 * p + 1, natural[1] = 3 * p + 2, natural[2] = 3 * p + 3;
      } else if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) {
         const uint32_t t = (key - 6) / 3;
         natural[0] = t + 1, natural[1] = (t & 1) ? t + 3 : t + 2, natural[2] = (t & 1) ? t + 2 : t + 3;
      } else {
         const uint32_t t = (key - 6) / 2;
         natural[0] = t + 2, natural[1] = t + 3, natural[2] = 1;
      }
      for (uint32_t o = 0; o < ORDERS && count[key][o]; o++) {
         const uint32_t slots[3] = {seen[key][o] & 0xff, (seen[key][o] >> 8) & 0xff, (seen[key][o] >> 16) & 0xff};
         char perm[4] = "???";
         for (uint32_t j = 0; j < 3; j++)
            for (uint32_t n = 0; n < 3; n++)
               if (slots[j] == natural[n])
                  perm[j] = (char)('0' + n);
         if (!strcmp(perm, "012") && o == 0 && !count[key][1])
            in_order++;
         report("survey %s: primitive %u (vertices %u %u %u): perm=%s in %u texels", name, key, natural[0], natural[1],
                natural[2], perm, count[key][o]);
      }
   }
   report("survey %s: %u primitives seen, %u in order", name, primitives, in_order);
   char description[160];
   snprintf(description, sizeof(description), "survey %s: every primitive's vertices read in its own order", name);
   check(primitives >= 16 && in_order == primitives, description);
}

/* The same list triangles as survey case 0 and 1, read through gl_BaryCoord: with the corners in API order, a
 * pixel's coordinates are its weights on the first, second and third vertex as drawn. For each primitive, which of
 * the six orders of the three channels matches the weights the pixel has, and in how many of its texels. */
static void
survey_bary_coord(struct context *c, const char *name, uint32_t which, VkShaderModule vert, VkShaderModule frag,
                  struct buffer *readback)
{
   const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(which)};
   const VkSpecializationInfo specialisation = {
      .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(which), .pData = &which};
   VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   stages[0].pSpecializationInfo = &specialisation;
   if (!render_readback(c, name, stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 48, readback)) {
      check(false, name);
      return;
   }
   static const float corners[3][2] = {{8.0f, 8.0f}, {8.0f, 56.0f}, {56.0f, 40.0f}};
   static const uint32_t orders[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {2, 1, 0}, {1, 0, 2}};
   static const uint32_t channels[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {2, 1, 0}, {1, 0, 2}};
   const uint8_t *const bytes = readback->map;
   uint32_t good = 0;
   for (uint32_t p = 0; p < 16; p++) {
      float px[3], py[3];
      for (uint32_t i = 0; i < 3; i++) {
         const uint32_t k = which == 0 ? i : orders[p % 6][i];
         px[i] = (float)(p % 4) * 64.0f + corners[k][0];
         py[i] = (float)(p / 4) * 64.0f + corners[k][1];
      }
      const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
      if (getenv("RADV_PS5_BARY_DEBUG")) {
         /* The raw texels three pixels in from each corner, toward the centre. */
         const float cx = (px[0] + px[1] + px[2]) / 3.0f, cy = (py[0] + py[1] + py[2]) / 3.0f;
         char raw[96] = "";
         for (uint32_t i = 0; i < 3; i++) {
            const float dx = cx - px[i], dy = cy - py[i], len = sqrtf(dx * dx + dy * dy);
            const uint32_t x = (uint32_t)(px[i] + 4.0f * dx / len), y = (uint32_t)(py[i] + 4.0f * dy / len);
            const uint8_t *t = bytes + (y * TARGET_SIZE + x) * 4;
            snprintf(raw + strlen(raw), sizeof(raw) - strlen(raw), " v%u(%u,%u)=%02x%02x%02x", i, x, y, t[0], t[1], t[2]);
         }
         report("survey %s: triangle %u raw:%s", name, p, raw);
      }
      uint32_t match[6] = {0}, texels = 0;
      for (uint32_t y = (p / 4) * 64; y < (p / 4) * 64 + 64; y++) {
         for (uint32_t x = (p % 4) * 64; x < (p % 4) * 64 + 64; x++) {
            const uint8_t *t = bytes + (y * TARGET_SIZE + x) * 4;
            if (t[3] != 0xff)
               continue;
            const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float w[3];
            w[1] = ((fx - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (fy - py[0])) / area;
            w[2] = ((px[1] - px[0]) * (fy - py[0]) - (fx - px[0]) * (py[1] - py[0])) / area;
            w[0] = 1.0f - w[1] - w[2];
            if (w[0] < 0.1f || w[1] < 0.1f || w[2] < 0.1f)
               continue;   /* near an edge a half texel moves the weights */
            texels++;
            for (uint32_t m = 0; m < 6; m++) {
               bool all = true;
               for (uint32_t j = 0; j < 3; j++) {
                  const float seen = (float)t[j] / 255.0f, want = w[channels[m][j]];
                  all = all && seen > want - 0.03f && seen < want + 0.03f;
               }
               match[m] += all;
            }
         }
      }
      uint32_t best = 0;
      for (uint32_t m = 1; m < 6; m++)
         if (match[m] > match[best])
            best = m;
      good += best == 0 && match[0] == texels;
      report("survey %s: triangle %u: channels read as vertices %u%u%u in %u of %u texels", name, p,
             channels[best][0], channels[best][1], channels[best][2], match[best], texels);
   }
   report("survey %s: %u of 16 triangles read in API order", name, good);
   char description[160];
   snprintf(description, sizeof(description), "survey %s: every triangle's barycentric coordinates follow its vertices' order", name);
   check(good == 16, description);
}

/* Lines: two vertices, so two slots. The pixel shader writes slot 0 and slot 1; the key is their sum, which no other line's
 * is. A list's p-th line is vertices 2p, 2p+1 (ids 2p+1, 2p+2), a strip's is p, p+1. perm=01 is in order, 10 swapped. */
static void
survey_lines(struct context *c, const char *name, uint32_t which, VkPrimitiveTopology topology, uint32_t vertices,
             bool provoking_last, VkShaderModule vert, VkShaderModule frag, struct buffer *readback)
{
   const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(which)};
   const VkSpecializationInfo specialisation = {
      .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(which), .pData = &which};
   VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   stages[0].pSpecializationInfo = &specialisation;
   c->draw.provoking_last = provoking_last;
   const bool ok = render_readback(c, name, stages, 2, topology, 0, vertices, readback);
   c->draw.provoking_last = false;
   if (!ok) {
      check(false, name);
      return;
   }
   uint32_t seen[256][2] = {{0}}, count[256][2] = {{0}}, lines = 0, in_order = 0;
   const uint32_t *const texels = readback->map;
   for (uint32_t i = 0; i < TARGET_SIZE * TARGET_SIZE; i++) {
      const uint32_t t = texels[i];
      if ((t >> 24) != 0xff || t == 0xffff0000u)   /* not drawn: the target's blue */
         continue;
      const uint32_t a = t & 0xff, b = (t >> 8) & 0xff, key = a + b;
      if (key >= 256)
         continue;
      for (uint32_t o = 0; o < 2; o++) {
         if (!count[key][o]) {
            seen[key][o] = t & 0xffff, count[key][o] = 1;
            break;
         }
         if (seen[key][o] == (t & 0xffff)) {
            count[key][o]++;
            break;
         }
      }
   }
   for (uint32_t key = 0; key < 256; key++) {
      if (!count[key][0])
         continue;
      lines++;
      for (uint32_t o = 0; o < 2 && count[key][o]; o++) {
         const uint32_t a = seen[key][o] & 0xff, b = seen[key][o] >> 8;
         const uint32_t low = a < b ? a : b;
         /* The vertices as drawn are the lower id then the higher, in a list and in a strip. */
         const char *perm = a == low ? "01" : "10";
         if (a == low && o == 0 && !count[key][1])
            in_order++;
         report("survey %s: line (vertices %u %u): perm=%s in %u texels", name, low, low + 1, perm, count[key][o]);
      }
   }
   report("survey %s: %u lines seen, %u in order", name, lines, in_order);
   char description[160];
   snprintf(description, sizeof(description), "survey %s: every line's vertices read in its own order", name);
   check(lines >= 16 && in_order == lines, description);
}

static void
test_barycentric_survey(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "barycentric survey: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_bary_survey_vert, sizeof(radv_smoke_bary_survey_vert));
   VkShaderModule frag = shader(c, radv_smoke_bary_frag, sizeof(radv_smoke_bary_frag));
   if (vert && frag) {
      survey_case(c, "list, one order", 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 48, vert, frag, &readback);
      survey_case(c, "list, six orders", 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 48, vert, frag, &readback);
      survey_case(c, "strip", 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 18, vert, frag, &readback);
      survey_case(c, "fan", 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, 18, vert, frag, &readback);
   }
   VkShaderModule line_frag = shader(c, radv_smoke_bary_line_frag, sizeof(radv_smoke_bary_line_frag));
   if (vert && line_frag) {
      survey_lines(c, "line list, first", 4, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, 32, false, vert, line_frag, &readback);
      survey_lines(c, "line strip, first", 5, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, 17, false, vert, line_frag, &readback);
      if (c->provoking) {
         survey_lines(c, "line list, last", 4, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, 32, true, vert, line_frag, &readback);
         survey_lines(c, "line strip, last", 5, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, 17, true, vert, line_frag, &readback);
      }
   }
   vkDestroyShaderModule(c->device, line_frag, NULL);
   VkShaderModule coord = shader(c, radv_smoke_bary_coord_frag, sizeof(radv_smoke_bary_coord_frag));
   if (vert && coord) {
      survey_bary_coord(c, "gl_BaryCoord, list, one order", 0, vert, coord, &readback);
      survey_bary_coord(c, "gl_BaryCoord, list, six orders", 1, vert, coord, &readback);
   }
   vkDestroyShaderModule(c->device, coord, NULL);
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* A pipeline fragment shading rate over a triangle covering the target: at
 * 2x2 the fragment shader runs once for each 2x2 block of texels and reports
 * the rate (gl_ShadingRateEXT 5, 2 pixels each way), at 1x1 once for each
 * texel. Every texel is written either way. */
static void
test_shading_rate(struct context *c)
{
   struct buffer readback, counts;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "shading rate: readback buffer");
      return;
   }
   if (!buffer_create(c, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &counts)) {
      check(false, "shading rate: count buffer");
      buffer_destroy(c, &readback);
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_full_vert, sizeof(radv_smoke_full_vert));
   VkShaderModule frag = shader(c, radv_smoke_rate_frag, sizeof(radv_smoke_rate_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   static const struct {
      uint32_t size, rate;
   } rates[2] = {{2, 5}, {1, 0}};
   for (unsigned r = 0; r < 2; r++) {
      const uint32_t size = rates[r].size;
      const VkPipelineFragmentShadingRateStateCreateInfoKHR rate_state = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
         .fragmentSize = {size, size},
         .combinerOps = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR},
      };
      char what[64];
      snprintf(what, sizeof(what), "shading rate %ux%u", size, size);
      memset(counts.map, 0, 4096);
      const bool ok = vert && frag &&
                      render_readback_with(c, what, stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 3, &readback,
                                           counts.buffer, &rate_state);
      const uint32_t *const texels = readback.map;
      const uint32_t *const words = counts.map;
      unsigned wrong = 0;
      for (unsigned i = 0; ok && i < TARGET_SIZE * TARGET_SIZE; i++)
         wrong += texels[i] != 0xff0000ffu;
      const uint32_t expected = TARGET_SIZE * TARGET_SIZE / (size * size);
      report("%s: %u invocations (%u expected), rates seen 0x%x (0x%x expected), %u texels wrong", what,
             ok ? words[0] : 0, expected, ok ? words[1] : 0, 1u << rates[r].rate, wrong);
      char description[128];
      snprintf(description, sizeof(description),
               "%s: the fragment shader runs once for each %ux%u block and reports the rate", what, size, size);
      check(ok && wrong == 0 && words[0] == expected && words[1] == 1u << rates[r].rate, description);
   }
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &counts);
   buffer_destroy(c, &readback);
}

/* The primitive ID a fragment shader reads when no earlier stage writes it:
 * the vertex shader (NGG) exports it. Two triangles split the target, the
 * left one primitive 0 (red), the right one primitive 1 (green). */
static void
test_primitive_id(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "primitive ID: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_prim_id_vert, sizeof(radv_smoke_prim_id_vert));
   VkShaderModule frag = shader(c, radv_smoke_prim_id_frag, sizeof(radv_smoke_prim_id_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   const bool ok = vert && frag &&
                   render_readback(c, "primitive ID", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 6, &readback);
   const uint32_t *const texels = readback.map;
   unsigned left_red = 0, left_green = 0, right_red = 0, right_green = 0, other = 0;
   for (unsigned y = 0; ok && y < TARGET_SIZE; y++) {
      for (unsigned x = 0; x < TARGET_SIZE; x++) {
         const uint32_t t = texels[y * TARGET_SIZE + x];
         const bool left = x < TARGET_SIZE / 2;
         if (t == 0xff0000ffu)
            left ? left_red++ : right_red++;
         else if (t == 0xff00ff00u)
            left ? left_green++ : right_green++;
         else
            other++;
      }
   }
   report("primitive ID: left %u red %u green, right %u red %u green, %u other", left_red, left_green, right_red,
          right_green, other);
   if (getenv("RADV_SMOKE_PRIM_ID_RAW")) {
      /* Diagnostic: the raw IDs each side read. */
      VkShaderModule raw = shader(c, radv_smoke_prim_id_raw_frag, sizeof(radv_smoke_prim_id_raw_frag));
      const VkPipelineShaderStageCreateInfo raw_stages[2] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, raw),
      };
      if (raw && render_readback(c, "primitive ID raw", raw_stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 6,
                                 &readback)) {
         const unsigned y = TARGET_SIZE / 2;
         report("primitive ID raw: left 0x%08x, right 0x%08x, top-left 0x%08x, bottom-right 0x%08x",
                texels[y * TARGET_SIZE + TARGET_SIZE / 4], texels[y * TARGET_SIZE + 3 * TARGET_SIZE / 4],
                texels[TARGET_SIZE / 8 * TARGET_SIZE + TARGET_SIZE / 8],
                texels[(TARGET_SIZE - 1) * TARGET_SIZE + TARGET_SIZE - 1]);
      }
      vkDestroyShaderModule(c->device, raw, NULL);
   }
   const unsigned half = TARGET_SIZE * TARGET_SIZE / 2;
   check(ok && left_red == half && right_green == half,
         "primitive ID: the fragment shader reads each triangle's own ID without a stage writing it");
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

static void *
thread_mxcsr(void *out)
{
   unsigned mxcsr;
   __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
   *(unsigned *)out = mxcsr;
   return NULL;
}

/* The title's startup sets the IEEE state (the console starts a title with
 * flush-to-zero and denormals-are-zero), and a new thread starts with its
 * creator's: exception flags aside, MXCSR reads 0x1f80 in both. */
static void
test_fp_state(void)
{
   unsigned mxcsr = 0, thread_value = 0;
   __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
   pthread_t thread;
   const bool made = pthread_create(&thread, NULL, thread_mxcsr, &thread_value) == 0 &&
                     pthread_join(thread, NULL) == 0;
   report("fp state: main 0x%x, a new thread 0x%x", mxcsr, thread_value);
   check((mxcsr & ~0x3fu) == 0x1f80 && made && (thread_value & ~0x3fu) == 0x1f80,
         "fp state: main and a new thread run with IEEE denormals (MXCSR 0x1f80)");
}

#if defined(__PROSPERO__)
/* A title's sandbox refuses access() for every path; the platform's
 * ps5_access, bound in its place, answers from stat() and open(). RADV's
 * shader cache makes its folders in the title's own, open to FTP (0777). */
static void
test_title_files(void)
{
   const bool found = access("/app0", F_OK) == 0 && access(RESULTS_PATH, R_OK | W_OK) == 0;
   errno = 0;
   const bool missing = access("/app0/no-such-file", F_OK) == -1 && errno == ENOENT;
   check(found && missing, "files: access answers in the title's folder");
   struct stat status;
   const bool cache = stat("/app0/radv-shader-cache/mesa_shader_cache_db", &status) == 0 &&
                      S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0777;
   check(cache, "files: RADV's shader cache is in /app0/radv-shader-cache, open to FTP");
}
#endif

/* One quad patch over the whole target, its evaluation positions taken from
 * the tessellation coordinates alone or from the control points the control
 * shader wrote (which travel through the off-chip ring). */
static void
test_tessellation(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "tessellation: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_tess_vert, sizeof(radv_smoke_tess_vert));
   VkShaderModule tesc = shader(c, radv_smoke_tess_tesc, sizeof(radv_smoke_tess_tesc));
   VkShaderModule from_coord = shader(c, radv_smoke_tess_coord_tese, sizeof(radv_smoke_tess_coord_tese));
   VkShaderModule from_patch = shader(c, radv_smoke_tess_patch_tese, sizeof(radv_smoke_tess_patch_tese));
   VkShaderModule frag = shader(c, radv_smoke_frag, sizeof(radv_smoke_frag));
   const struct {
      const char *what;
      VkShaderModule tese;
   } variants[2] = {
      {"tessellation from coordinates", from_coord},
      {"tessellation from control points", from_patch},
   };
   for (unsigned v = 0; v < 2; v++) {
      const VkPipelineShaderStageCreateInfo stages[4] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
         stage_info(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tesc),
         stage_info(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, variants[v].tese),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
      };
      const bool ok = vert && tesc && variants[v].tese && frag &&
                      render_readback(c, variants[v].what, stages, 4, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, 4, 4,
                                      &readback);
      uint32_t red = 0;
      const uint32_t *const texels = readback.map;
      for (uint32_t i = 0; ok && i < TARGET_SIZE * TARGET_SIZE; i++)
         red += texels[i] == 0xff0000ffu;
      char label[96];
      snprintf(label, sizeof(label), "%s: the patch covers the target (%u of %u texels)", variants[v].what, red,
               TARGET_SIZE * TARGET_SIZE);
      check(ok && red == TARGET_SIZE * TARGET_SIZE, label);
   }

   /* The CTS's shape: generic control points, nine-by-nine, and a colour from
    * the evaluation shader. */
   VkShaderModule varying_vert = shader(c, radv_smoke_tess_varying_vert, sizeof(radv_smoke_tess_varying_vert));
   VkShaderModule varying_tesc = shader(c, radv_smoke_tess_varying_tesc, sizeof(radv_smoke_tess_varying_tesc));
   VkShaderModule varying_tese = shader(c, radv_smoke_tess_varying_tese, sizeof(radv_smoke_tess_varying_tese));
   VkShaderModule colour_frag = shader(c, radv_smoke_colour_frag, sizeof(radv_smoke_colour_frag));
   const struct {
      const char *what;
      VkShaderModule tesc, frag;
   } shapes[1] = {
      {"tessellation with varyings", varying_tesc, colour_frag},
   };
   for (unsigned v = 0; v < 1; v++) {
      const VkPipelineShaderStageCreateInfo stages[4] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, varying_vert),
         stage_info(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, shapes[v].tesc),
         stage_info(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, varying_tese),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, shapes[v].frag),
      };
      const char *const what = shapes[v].what;
      const bool ok = varying_vert && shapes[v].tesc && varying_tese && shapes[v].frag &&
                      render_readback(c, what, stages, 4, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, 4, 4, &readback);
      uint32_t red = 0, blue = 0;
      const uint32_t *const texels = readback.map;
      for (uint32_t i = 0; ok && i < TARGET_SIZE * TARGET_SIZE; i++) {
         red += texels[i] == 0xff0000ffu;
         blue += texels[i] == 0xffff0000u;
      }
      if (ok && red != TARGET_SIZE * TARGET_SIZE)
         report("%s: %u red, %u blue, texel (128, 128) reads 0x%08x", what, red, blue,
                texels[128 * TARGET_SIZE + 128]);
      char label[96];
      snprintf(label, sizeof(label), "%s: the patch covers the target in its colour", what);
      check(ok && red == TARGET_SIZE * TARGET_SIZE, label);
   }
   /* Levels 2 to 9: NGG culling once dropped every patch above level 2. */
   VkShaderModule level_tesc = shader(c, radv_smoke_tess_level_tesc, sizeof(radv_smoke_tess_level_tesc));
   uint32_t levels_covered = 0;
   for (unsigned level = 2; level <= 9; level++) {
      const float value = (float)level;
      const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(float)};
      const VkSpecializationInfo special = {
         .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(float), .pData = &value};
      VkPipelineShaderStageCreateInfo stages[4] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, varying_vert),
         stage_info(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, level_tesc),
         stage_info(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, varying_tese),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
      };
      stages[1].pSpecializationInfo = &special;
      const bool ok =
         render_readback(c, "tessellation levels", stages, 4, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, 4, 4, &readback);
      uint32_t red = 0;
      const uint32_t *const texels = readback.map;
      for (uint32_t i = 0; ok && i < TARGET_SIZE * TARGET_SIZE; i++)
         red += texels[i] == 0xff0000ffu;
      if (red == TARGET_SIZE * TARGET_SIZE)
         levels_covered++;
      else
         report("tessellation level %u: %u of %u texels red", level, red, TARGET_SIZE * TARGET_SIZE);
   }
   check(levels_covered == 8, "tessellation levels 2 to 9: each patch covers the target");
   vkDestroyShaderModule(c->device, level_tesc, NULL);
   vkDestroyShaderModule(c->device, varying_vert, NULL);
   vkDestroyShaderModule(c->device, varying_tesc, NULL);
   vkDestroyShaderModule(c->device, varying_tese, NULL);
   vkDestroyShaderModule(c->device, colour_frag, NULL);
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, tesc, NULL);
   vkDestroyShaderModule(c->device, from_coord, NULL);
   vkDestroyShaderModule(c->device, from_patch, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* The rows of four a readback covers entirely in red, as a mask. */
static unsigned
rows_red(const struct buffer *readback)
{
   const uint32_t *const texels = readback->map;
   unsigned rows = 0;
   for (unsigned row = 0; row < 4; row++) {
      bool all = true;
      for (uint32_t i = row * TARGET_SIZE * TARGET_SIZE / 4; all && i < (row + 1) * TARGET_SIZE * TARGET_SIZE / 4; i++)
         all = texels[i] == 0xff0000ffu;
      rows |= (unsigned)all << row;
   }
   return rows;
}

/* Draws with a geometry shader and the recording c->draw names; the rows
 * covered, or ~0u when the draw failed. */
static unsigned
draw_rows(struct context *c, VkShaderModule vert, VkShaderModule geom, VkShaderModule frag,
          VkPrimitiveTopology topology, uint32_t vertex_count, struct buffer *readback)
{
   const VkPipelineShaderStageCreateInfo stages[3] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_GEOMETRY_BIT, geom),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   return render_readback(c, "geometry draws", stages, 3, topology, 0, vertex_count, readback) ? rows_red(readback)
                                                                                               : ~0u;
}

/* Geometry shader draws whose indices or counts live in memory, and
 * triangle strips cut by primitive restart: each covers the rows it names. */
static void
test_geometry_draws(struct context *c, struct buffer *readback, VkShaderModule points, VkShaderModule frag)
{
   struct buffer indices, args, count;
   if (!buffer_create(c, 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indices) ||
       !buffer_create(c, 256, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &args) ||
       !buffer_create(c, 16, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &count)) {
      check(false, "geometry draws: buffers");
      return;
   }
   VkShaderModule strip = shader(c, radv_smoke_gs_16_geom, sizeof(radv_smoke_gs_16_geom));
   VkShaderModule quads = shader(c, radv_smoke_gs_quads_vert, sizeof(radv_smoke_gs_quads_vert));
   VkShaderModule triangles = shader(c, radv_smoke_gs_triangles_geom, sizeof(radv_smoke_gs_triangles_geom));
   uint16_t *const index = indices.map;
   uint32_t *const word = args.map;
   uint32_t *const counted = count.map;
   unsigned rows;

   /* Points 1 and 3: indices 0 and 2 after the first, offset by one. */
   index[0] = 9;
   index[1] = 0;
   index[2] = 2;
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDEXED, .indices = indices.buffer, .first_index = 1, .vertex_offset = 1};
   rows = draw_rows(c, points, strip, frag, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 2, readback);
   if (rows != 0xa)
      report("geometry draws: an indexed draw covered rows 0x%x", rows);
   check(rows == 0xa, "geometry: an indexed draw with a vertex offset draws the points its indices name");

   const VkDrawIndexedIndirectCommand indexed_args = {2, 1, 1, 1, 0};
   memcpy(word, &indexed_args, sizeof(indexed_args));
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDEXED_INDIRECT, .indices = indices.buffer, .args = args.buffer,
                                   .max_draws = 1};
   rows = draw_rows(c, points, strip, frag, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, readback);
   if (rows != 0xa)
      report("geometry draws: an indexed indirect draw covered rows 0x%x", rows);
   check(rows == 0xa, "geometry: an indexed indirect draw draws the points its indices name");

   const VkDrawIndirectCommand indirect_args = {2, 1, 1, 0};
   memcpy(word, &indirect_args, sizeof(indirect_args));
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDIRECT, .args = args.buffer, .max_draws = 1};
   rows = draw_rows(c, points, strip, frag, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, readback);
   if (rows != 0x6)
      report("geometry draws: an indirect draw covered rows 0x%x", rows);
   check(rows == 0x6, "geometry: an indirect draw draws the points its arguments name");

   /* Two draws in memory, point 0 and point 3, and a count of one. */
   const VkDrawIndirectCommand two[2] = {{1, 1, 0, 0}, {1, 1, 3, 0}};
   memcpy(word, two, sizeof(two));
   counted[0] = 1;
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDIRECT_COUNT, .args = args.buffer, .count = count.buffer,
                                   .max_draws = 2};
   rows = draw_rows(c, points, strip, frag, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, readback);
   if (rows != 0x1)
      report("geometry draws: an indirect count draw covered rows 0x%x", rows);
   check(rows == 0x1, "geometry: an indirect count draw draws only the draws counted");

   /* Two strips of two triangles each, rows 0 and 2, cut by a restart. */
   const uint16_t strips[9] = {0, 1, 2, 3, 0xffff, 4, 5, 6, 7};
   memcpy(index, strips, sizeof(strips));
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDEXED, .restart = true, .indices = indices.buffer};
   rows = draw_rows(c, quads, triangles, frag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 9, readback);
   if (rows != 0x5)
      report("geometry draws: strips with a restart covered rows 0x%x", rows);
   check(rows == 0x5, "geometry: primitive restart cuts a triangle strip");

   const VkDrawIndexedIndirectCommand strip_args = {9, 1, 0, 0, 0};
   memcpy(word, &strip_args, sizeof(strip_args));
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDEXED_INDIRECT, .restart = true, .indices = indices.buffer,
                                   .args = args.buffer, .max_draws = 1};
   rows = draw_rows(c, quads, triangles, frag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0, readback);
   if (rows != 0x5)
      report("geometry draws: indirect strips with a restart covered rows 0x%x", rows);
   check(rows == 0x5, "geometry: primitive restart cuts a triangle strip drawn indirectly");

   c->draw = (__typeof__(c->draw)){.mode = DRAW_DIRECT};
   vkDestroyShaderModule(c->device, strip, NULL);
   vkDestroyShaderModule(c->device, quads, NULL);
   vkDestroyShaderModule(c->device, triangles, NULL);
   buffer_destroy(c, &indices);
   buffer_destroy(c, &args);
   buffer_destroy(c, &count);
}

/* Points drawn into strips by geometry shaders: every output size from 16 to
 * 128 vertices at 1, 2 and 4 points, a vertex count chosen from
 * gl_PrimitiveIDIn, a colour varying, and the IDs the invocations get.
 * Counts that are not constant go through NGG's workgroup repack. */
static void
test_geometry(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "geometry: readback buffer");
      return;
   }
   enum { ROW = TARGET_SIZE * TARGET_SIZE / 4 };
   VkShaderModule vert = shader(c, radv_smoke_gs_points_vert, sizeof(radv_smoke_gs_points_vert));
   VkShaderModule frag = shader(c, radv_smoke_frag, sizeof(radv_smoke_frag));

   /* Draws count points with the geometry shader and counts red texels. */
   #define DRAW_POINTS(vs, gs, fs, count, red)                                                                     \
      do {                                                                                                     \
         const VkPipelineShaderStageCreateInfo stages_[3] = {                                                   \
            stage_info(VK_SHADER_STAGE_VERTEX_BIT, vs),                                                         \
            stage_info(VK_SHADER_STAGE_GEOMETRY_BIT, gs),                                                       \
            stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, fs),                                                       \
         };                                                                                                     \
         const bool ok_ = render_readback(c, "geometry", stages_, 3, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, count, \
                                          &readback);                                                           \
         const uint32_t *const texels_ = readback.map;                                                          \
         red = 0;                                                                                               \
         for (uint32_t i_ = 0; ok_ && i_ < TARGET_SIZE * TARGET_SIZE; i_++)                                     \
            red += texels_[i_] == 0xff0000ffu;                                                                  \
      } while (0)

   const struct {
      unsigned vertices;
      const uint32_t *code;
      size_t bytes;
   } shapes[5] = {
      {16, radv_smoke_gs_16_geom, sizeof(radv_smoke_gs_16_geom)},
      {32, radv_smoke_gs_32_geom, sizeof(radv_smoke_gs_32_geom)},
      {64, radv_smoke_gs_64_geom, sizeof(radv_smoke_gs_64_geom)},
      {100, radv_smoke_gs_100_geom, sizeof(radv_smoke_gs_100_geom)},
      {128, radv_smoke_gs_128_geom, sizeof(radv_smoke_gs_128_geom)},
   };
   unsigned sizes_right = 0;
   for (unsigned s = 0; s < 5; s++) {
      VkShaderModule geom = shader(c, shapes[s].code, shapes[s].bytes);
      const unsigned counts[3] = {1, 2, 4};
      for (unsigned n = 0; n < 3; n++) {
         uint32_t red;
         DRAW_POINTS(vert, geom, frag, counts[n], red);
         if (red == counts[n] * ROW)
            sizes_right++;
         else
            report("geometry: %u vertices from %u points drew %u texels, not %u", shapes[s].vertices, counts[n], red,
                   counts[n] * ROW);
      }
      vkDestroyShaderModule(c->device, geom, NULL);
   }
   check(sizes_right == 15, "geometry: strips of 16 to 128 vertices from 1, 2 and 4 points cover their rows");

   VkShaderModule by_id = shader(c, radv_smoke_gs_primid_geom, sizeof(radv_smoke_gs_primid_geom));
   VkShaderModule reads_id = shader(c, radv_smoke_gs_primid_fixed_geom, sizeof(radv_smoke_gs_primid_fixed_geom));
   uint32_t one, two, three, four;
   DRAW_POINTS(vert, by_id, frag, 1, one);
   DRAW_POINTS(vert, by_id, frag, 2, two);
   DRAW_POINTS(vert, reads_id, frag, 1, three);
   DRAW_POINTS(vert, reads_id, frag, 2, four);
   if (one != ROW || two != 2 * ROW)
      report("geometry: a count from gl_PrimitiveIDIn drew %u and %u texels", one, two);
   check(one == ROW && two == 2 * ROW, "geometry: a vertex count chosen from gl_PrimitiveIDIn draws every strip");
   check(three == ROW && four == 2 * ROW, "geometry: a shader that may return early draws every strip");
   vkDestroyShaderModule(c->device, by_id, NULL);
   vkDestroyShaderModule(c->device, reads_id, NULL);

   VkShaderModule colour_vert = shader(c, radv_smoke_gs_colour_vert, sizeof(radv_smoke_gs_colour_vert));
   VkShaderModule colour_geom = shader(c, radv_smoke_gs_colour_geom, sizeof(radv_smoke_gs_colour_geom));
   VkShaderModule colour_frag = shader(c, radv_smoke_colour_frag, sizeof(radv_smoke_colour_frag));
   uint32_t coloured;
   DRAW_POINTS(colour_vert, colour_geom, colour_frag, 2, coloured);
   check(coloured == 2 * ROW, "geometry: a colour passes from the vertex shader to the fragment shader");
   vkDestroyShaderModule(c->device, colour_vert, NULL);
   vkDestroyShaderModule(c->device, colour_geom, NULL);
   vkDestroyShaderModule(c->device, colour_frag, NULL);
   #undef DRAW_POINTS

   test_geometry_draws(c, &readback, vert, frag);

   /* The IDs four points' invocations record. */
   struct buffer record;
   VkShaderModule record_geom = shader(c, radv_smoke_gs_record_geom, sizeof(radv_smoke_gs_record_geom));
   bool ids_right = false;
   if (record_geom && buffer_create(c, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &record)) {
      memset(record.map, 0, 4096);
      const VkPipelineShaderStageCreateInfo stages[3] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
         stage_info(VK_SHADER_STAGE_GEOMETRY_BIT, record_geom),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
      };
      const bool ok = render_readback_with(c, "geometry record", stages, 3, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, 4,
                                           &readback, record.buffer, NULL);
      const uint32_t *const words = record.map;
      unsigned seen = 0;
      for (unsigned i = 0; ok && words[0] == 4 && i < 4; i++) {
         if (words[1 + 2 * i] < 4 && words[2 + 2 * i] == 0)
            seen |= 1u << words[1 + 2 * i];
      }
      ids_right = seen == 0xf;
      if (!ids_right)
         report("geometry: %u invocations recorded (%u %u) (%u %u) (%u %u) (%u %u)", words[0], words[1], words[2],
                words[3], words[4], words[5], words[6], words[7], words[8]);
      buffer_destroy(c, &record);
   }
   check(ids_right, "geometry: four points' invocations get primitive IDs 0 to 3");
   vkDestroyShaderModule(c->device, record_geom, NULL);
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* ------------------------------------------------- acceleration structures */

static PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR;
static PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR;
static PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR;
static PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR;
static PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress;

/* A host-visible buffer with a device address. */
static bool
address_buffer_create(struct context *c, VkDeviceSize size, VkBufferUsageFlags usage, struct buffer *out,
                      VkDeviceAddress *address)
{
   *out = (struct buffer){.size = size};
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (vkCreateBuffer(c->device, &info, NULL, &out->buffer) != VK_SUCCESS)
      return false;
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->device, out->buffer, &req);
   const VkMemoryAllocateFlagsInfo flags = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
   };
   const uint32_t type = memory_type(c, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   const VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &flags,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   if (type == UINT32_MAX || vkAllocateMemory(c->device, &alloc, NULL, &out->memory) != VK_SUCCESS ||
       vkBindBufferMemory(c->device, out->buffer, out->memory, 0) != VK_SUCCESS ||
       vkMapMemory(c->device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->map) != VK_SUCCESS)
      return false;
   const VkBufferDeviceAddressInfo address_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = out->buffer,
   };
   *address = vkGetBufferDeviceAddress(c->device, &address_info);
   return true;
}

/* Builds a bottom level of `count` triangles on the GPU into host-visible
 * memory. Returns whether each triangle's first vertex is in the structure. */
static bool
build_bottom_level(struct context *c, const float (*vertices)[3], uint32_t count)
{
   struct buffer vertex_buffer, structure_buffer, scratch_buffer;
   VkDeviceAddress vertex_address, structure_address, scratch_address;
   if (!address_buffer_create(c, sizeof(float) * 9 * count,
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &vertex_buffer,
                              &vertex_address)) {
      report("acceleration structure: vertex buffer");
      return false;
   }
   memcpy(vertex_buffer.map, vertices, sizeof(float) * 9 * count);

   VkAccelerationStructureGeometryKHR geometry = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
      .geometry.triangles =
         {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData.deviceAddress = vertex_address,
            .vertexStride = sizeof(float) * 3,
            .maxVertex = 3 * count - 1,
            .indexType = VK_INDEX_TYPE_NONE_KHR,
         },
      .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
   };
   VkAccelerationStructureBuildGeometryInfoKHR build = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .geometryCount = 1,
      .pGeometries = &geometry,
   };
   VkAccelerationStructureBuildSizesInfoKHR sizes = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
   };
   vkGetAccelerationStructureBuildSizesKHR(c->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                           &count, &sizes);
   report("acceleration structure: %u triangles take %llu bytes, %llu of scratch", count,
          (unsigned long long)sizes.accelerationStructureSize, (unsigned long long)sizes.buildScratchSize);

   bool ok = address_buffer_create(c, sizes.accelerationStructureSize,
                                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, &structure_buffer,
                                   &structure_address) &&
             address_buffer_create(c, sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &scratch_buffer,
                                   &scratch_address);
   VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
   if (ok) {
      memset(structure_buffer.map, 0xcd, sizes.accelerationStructureSize);
      const VkAccelerationStructureCreateInfoKHR create = {
         .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
         .buffer = structure_buffer.buffer,
         .size = sizes.accelerationStructureSize,
         .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      };
      ok = vkCreateAccelerationStructureKHR(c->device, &create, NULL, &structure) == VK_SUCCESS;
   }
   if (ok) {
      build.dstAccelerationStructure = structure;
      build.scratchData.deviceAddress = scratch_address;
      const VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = count};
      const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
      ok = begin(c);
      vkCmdBuildAccelerationStructuresKHR(c->cmd, 1, &build, &ranges);
      ok = ok && submit_and_wait(c, "acceleration structure");
   }

   bool found = false;
   if (ok) {
      const uint32_t *const words = structure_buffer.map;
      const uint32_t total = (uint32_t)(sizes.accelerationStructureSize / 4);
      found = true;
      for (uint32_t t = 0; t < count; t++) {
         bool here = false;
         for (uint32_t i = 0; i + 3 <= total && !here; i++)
            here = memcmp(words + i, vertices[3 * t], sizeof(float) * 3) == 0;
         found = found && here;
      }
   } else {
      report("acceleration structure: the build of %u triangles did not complete", count);
   }
   if (structure)
      vkDestroyAccelerationStructureKHR(c->device, structure, NULL);
   buffer_destroy(c, &structure_buffer);
   buffer_destroy(c, &scratch_buffer);
   buffer_destroy(c, &vertex_buffer);
   return found;
}

/* GPU builds of bottom levels of one and two triangles: the structure holds
 * each triangle's vertices. Runs where acceleration structures are reported
 * (RADV_PS5_RAY_TRACING=1 until ray tracing is on). */
static void
test_acceleration_structure(struct context *c)
{
   vkCreateAccelerationStructureKHR =
      (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(c->device, "vkCreateAccelerationStructureKHR");
   vkDestroyAccelerationStructureKHR =
      (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(c->device, "vkDestroyAccelerationStructureKHR");
   vkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(
      c->device, "vkGetAccelerationStructureBuildSizesKHR");
   vkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(
      c->device, "vkCmdBuildAccelerationStructuresKHR");
   vkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(c->device, "vkGetBufferDeviceAddress");
   if (!vkCreateAccelerationStructureKHR || !vkDestroyAccelerationStructureKHR ||
       !vkGetAccelerationStructureBuildSizesKHR || !vkCmdBuildAccelerationStructuresKHR || !vkGetBufferDeviceAddress) {
      check(false, "acceleration structure: the extension's commands load");
      return;
   }

   static const float one[3][3] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
   check(build_bottom_level(c, one, 1), "acceleration structure: a GPU-built bottom level holds its one triangle");

   static const float two[6][3] = {
      {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f},
      {2.0f, 2.0f, 3.0f}, {3.0f, 2.0f, 3.0f}, {2.0f, 3.0f, 3.0f},
   };
   check(build_bottom_level(c, two, 2), "acceleration structure: a GPU-built bottom level holds its two triangles");
}

/* --------------------------------------------------------- sparse timing */

/* How the cost of sparse buffers grows with their number, which made the CTS's
 * 32-buffer sparse descriptor buffer cases outlast its watchdog (sparse-db-
 * scale-1 in PS5_Vulkan: 1, 8 and 16 buffers took about 1, 3 and 10 s). Makes
 * 48 sparse-binding buffers of 64 KiB and binds each to memory of its own,
 * as those cases do, timing the creation, the bind and its fence wait; one
 * line per eighth buffer. */
static bool
supported_sparse_binding(struct context *c)
{
   VkPhysicalDeviceFeatures supported;
   vkGetPhysicalDeviceFeatures(c->physical, &supported);
   return supported.sparseBinding;
}

static void
test_sparse_timing(struct context *c)
{
   PFN_vkQueueBindSparse bind_sparse = (PFN_vkQueueBindSparse)vkGetDeviceProcAddr(c->device, "vkQueueBindSparse");
   if (!bind_sparse) {
      check(false, "sparse timing: vkQueueBindSparse");
      return;
   }
   enum { COUNT = 48 };
   for (unsigned kind = 0; kind < (c->descriptor_buffer ? 2u : 1u); kind++) {
   const char *const kind_name = kind ? "descriptor buffers" : "storage buffers";
   VkBuffer buffers[COUNT] = {0};
   VkDeviceMemory memories[COUNT] = {0};
   uint64_t create_ns = 0, bind_ns = 0;
   bool ok = true;
   for (unsigned i = 0; i < COUNT && ok; i++) {
      const VkBufferCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT,
         .size = 64 * 1024,
         .usage = kind ? VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                       : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      uint64_t t0 = now_ns();
      ok &= vkCreateBuffer(c->device, &info, NULL, &buffers[i]) == VK_SUCCESS;
      const uint64_t t1 = now_ns();
      if (!ok)
         break;
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(c->device, buffers[i], &req);
      const VkMemoryAllocateInfo alloc = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = memory_type(c, req.memoryTypeBits, 0),
      };
      ok &= vkAllocateMemory(c->device, &alloc, NULL, &memories[i]) == VK_SUCCESS;
      if (!ok)
         break;
      const VkSparseMemoryBind bind = {.size = req.size, .memory = memories[i]};
      const VkSparseBufferMemoryBindInfo buffer_bind = {.buffer = buffers[i], .bindCount = 1, .pBinds = &bind};
      const VkBindSparseInfo bind_info = {
         .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
         .bufferBindCount = 1,
         .pBufferBinds = &buffer_bind,
      };
      const uint64_t t2 = now_ns();
      vkResetFences(c->device, 1, &c->fence);
      ok &= bind_sparse(c->queue, 1, &bind_info, c->fence) == VK_SUCCESS;
      ok &= vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
      const uint64_t t3 = now_ns();
      create_ns += t1 - t0;
      bind_ns += t3 - t2;
      if ((i + 1) % 8 == 0)
         report("sparse timing, %s: buffer %u: creation %.3f ms, bind %.3f ms; %u so far: %.3f ms, %.3f ms",
                kind_name, i + 1, (t1 - t0) / 1e6, (t3 - t2) / 1e6, i + 1, create_ns / 1e6, bind_ns / 1e6);
   }
   check(ok, kind ? "sparse timing: 48 sparse descriptor buffers created and bound"
                  : "sparse timing: 48 sparse buffers created and bound");
   const uint64_t t0 = now_ns();
   for (unsigned i = 0; i < COUNT; i++) {
      if (buffers[i])
         vkDestroyBuffer(c->device, buffers[i], NULL);
      if (memories[i])
         vkFreeMemory(c->device, memories[i], NULL);
   }
   report("sparse timing, %s: destroying them took %.3f ms", kind_name, (now_ns() - t0) / 1e6);
   }
}

/* ----------------------------------------------------------------- display */

/* VK_KHR_display on VideoOut (Mesa's wsi_common_videoout.c): the display and
 * its modes, a plane surface, and a swapchain presenting 60 frames, each
 * cleared to a gray of its own. FIFO paces presents to flips: the last 50 take
 * 50 refresh periods within 15%. The last frame is read back before its
 * present. A swapchain made with the first as oldSwapchain presents 5 more,
 * and the retired one's acquire reports it out of date. */
#define DISPLAY_INSTANCE_COMMANDS(X)                                                               \
   X(GetPhysicalDeviceDisplayPropertiesKHR)                                                        \
   X(GetDisplayModePropertiesKHR)                                                                  \
   X(CreateDisplayPlaneSurfaceKHR)                                                                 \
   X(DestroySurfaceKHR)                                                                            \
   X(GetPhysicalDeviceSurfaceSupportKHR)                                                           \
   X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                                      \
   X(GetPhysicalDeviceSurfaceFormatsKHR)

#define DISPLAY_DEVICE_COMMANDS(X)                                                                 \
   X(CreateSwapchainKHR)                                                                           \
   X(DestroySwapchainKHR)                                                                          \
   X(GetSwapchainImagesKHR)                                                                        \
   X(AcquireNextImageKHR)                                                                          \
   X(QueuePresentKHR)                                                                              \
   X(CreateSemaphore)                                                                              \
   X(DestroySemaphore)                                                                             \
   X(CmdClearColorImage)

DISPLAY_INSTANCE_COMMANDS(DECLARE)
DISPLAY_DEVICE_COMMANDS(DECLARE)

static void
display_image_barrier(struct context *c, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                      VkAccessFlags2 dst_access)
{
   const VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
   };
   vkCmdPipelineBarrier2(c->cmd, &dependency);
}

/* One frame: acquire, clear to gray, read back when asked, present. */
static VkResult
display_frame(struct context *c, VkSwapchainKHR swapchain, const VkImage *images, VkSemaphore acquired,
              VkSemaphore rendered, float gray, struct buffer *readback)
{
   uint32_t index;
   VkResult result = vkAcquireNextImageKHR(c->device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
   if (result != VK_SUCCESS)
      return result;
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   result = vkResetCommandBuffer(c->cmd, 0);
   if (result != VK_SUCCESS)
      return result;
   result = vkBeginCommandBuffer(c->cmd, &begin_info);
   if (result != VK_SUCCESS)
      return result;
   display_image_barrier(c, images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT);
   const VkClearColorValue color = {.float32 = {gray, gray, gray, 1.0f}};
   const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   vkCmdClearColorImage(c->cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
   VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   if (readback) {
      display_image_barrier(c, images[index], layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      /* Two pixels: the first tile's and the last one's. */
      const VkBufferImageCopy copies[2] = {
         {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}},
         {.bufferOffset = 4,
          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .imageOffset = {3839, 2159, 0},
          .imageExtent = {1, 1, 1}},
      };
      vkCmdCopyImageToBuffer(c->cmd, images[index], layout, readback->buffer, 2, copies);
   }
   display_image_barrier(c, images[index], layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
   result = vkEndCommandBuffer(c->cmd);
   if (result != VK_SUCCESS)
      return result;

   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &acquired,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &c->cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &rendered,
   };
   result = vkResetFences(c->device, 1, &c->fence);
   if (result != VK_SUCCESS)
      return result;
   result = vkQueueSubmit(c->queue, 1, &submit, c->fence);
   if (result != VK_SUCCESS)
      return result;
   const VkPresentInfoKHR present = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &rendered,
      .swapchainCount = 1,
      .pSwapchains = &swapchain,
      .pImageIndices = &index,
   };
   result = vkQueuePresentKHR(c->queue, &present);
   /* The one command buffer is recorded again next frame. */
   VkResult waited = vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX);
   return result == VK_SUCCESS ? waited : result;
}

static VkSwapchainKHR
display_swapchain(struct context *c, VkSurfaceKHR surface, VkSwapchainKHR old, VkImage *images, uint32_t *count)
{
   const VkSwapchainCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = 3,
      .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = {3840, 2160},
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
   };
   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   VkResult result = vkCreateSwapchainKHR(c->device, &info, NULL, &swapchain);
   check(result == VK_SUCCESS, old ? "display: a swapchain replacing the first" : "display: vkCreateSwapchainKHR");
   if (result != VK_SUCCESS)
      return VK_NULL_HANDLE;
   *count = 8;
   result = vkGetSwapchainImagesKHR(c->device, swapchain, count, images);
   check(result == VK_SUCCESS && *count >= 3 && *count <= 5, "display: 3 to 5 swapchain images");
   return swapchain;
}

static void
test_display(struct context *c)
{
   bool ok = true;
   uint32_t attribute3;
   if (present_read_attribute3(&attribute3))
      report("display timing: title attribute3 0x%08x", attribute3);
   else
      report("display timing: title attribute3 unavailable");
#define LOAD_I(name)                                                                               \
   vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(c->instance, "vk" #name);                    \
   ok &= vk##name != NULL;
#define LOAD_D(name)                                                                               \
   vk##name = (PFN_vk##name)vkGetDeviceProcAddr(c->device, "vk" #name);                            \
   ok &= vk##name != NULL;
   DISPLAY_INSTANCE_COMMANDS(LOAD_I)
   DISPLAY_DEVICE_COMMANDS(LOAD_D)
#undef LOAD_I
#undef LOAD_D
   check(ok, "display: the VK_KHR_display and VK_KHR_swapchain commands");
   if (!ok)
      return;

   VkDisplayPropertiesKHR display;
   uint32_t count = 1;
   VkResult result = vkGetPhysicalDeviceDisplayPropertiesKHR(c->physical, &count, &display);
   check(result == VK_SUCCESS && count == 1 && display.physicalResolution.width == 3840 &&
            display.physicalResolution.height == 2160,
         "display: one 3840x2160 display");
   if (result != VK_SUCCESS || count != 1)
      return;
   VkDisplayModePropertiesKHR modes[2];
   count = 2;
   result = vkGetDisplayModePropertiesKHR(c->physical, display.display, &count, modes);
   check((result == VK_SUCCESS || result == VK_INCOMPLETE) && count >= 1, "display: its modes");
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0)
      return;
   report("display: %s, %u mode(s), the first at %.3f Hz", display.displayName, count,
          modes[0].parameters.refreshRate / 1000.0);

   const VkDisplaySurfaceCreateInfoKHR surface_info = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = modes[0].displayMode,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = {3840, 2160},
   };
   VkSurfaceKHR surface;
   result = vkCreateDisplayPlaneSurfaceKHR(c->instance, &surface_info, NULL, &surface);
   check(result == VK_SUCCESS, "display: vkCreateDisplayPlaneSurfaceKHR");
   if (result != VK_SUCCESS)
      return;
   VkBool32 supported = VK_FALSE;
   vkGetPhysicalDeviceSurfaceSupportKHR(c->physical, c->family, surface, &supported);
   VkSurfaceCapabilitiesKHR caps;
   vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->physical, surface, &caps);
   check(supported && caps.currentExtent.width == 3840 && caps.currentExtent.height == 2160 &&
            caps.minImageCount <= 3 && caps.maxImageCount >= 3,
         "display: the surface's support and capabilities");

   struct buffer readback;
   VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
   const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   if (!buffer_create(c, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback) ||
       vkCreateSemaphore(c->device, &semaphore_info, NULL, &acquired) != VK_SUCCESS ||
       vkCreateSemaphore(c->device, &semaphore_info, NULL, &rendered) != VK_SUCCESS) {
      check(false, "display: its semaphores and readback buffer");
      vkDestroySurfaceKHR(c->instance, surface, NULL);
      return;
   }

   VkImage images[8];
   VkSwapchainKHR swapchain = display_swapchain(c, surface, VK_NULL_HANDLE, images, &count);
   if (swapchain) {
      const unsigned warmup = 30, paced = 360, frames = warmup + paced;
      uint64_t start = 0, previous = 0, end = 0, minimum = UINT64_MAX, maximum = 0;
      unsigned presented = 0, measured = 0;
      for (unsigned frame = 0; frame < frames; frame++) {
         if (frame == warmup)
            start = previous = now_ns();
         const float gray = 0.10f + 0.10f * (float)(frame % 16) / 16.0f;
         result = display_frame(c, swapchain, images, acquired, rendered, gray,
                                frame == frames - 1 ? &readback : NULL);
         if (result != VK_SUCCESS)
            break;
         presented++;
         if (frame >= warmup) {
            end = now_ns();
            if (end <= previous)
               break;
            uint64_t interval = end - previous;
            if (interval < minimum)
               minimum = interval;
            if (interval > maximum)
               maximum = interval;
            previous = end;
            measured++;
         }
      }
      const double seconds = measured ? (end - start) / 1e9 : 0.0;
      check(presented == frames && measured == paced && seconds > 0.0,
            "display: 30 warmup and 360 FIFO frames acquired, cleared and presented");
      report("display timing: %u frames, %.6f s, %.3f paced presents/s, interval min %.3f ms max %.3f ms",
             measured, seconds, seconds > 0.0 ? measured / seconds : 0.0,
             measured ? minimum / 1e6 : 0.0, maximum / 1e6);
      report("display timing: FIFO throughput, not independently measured scanout timestamps");
      /* B8G8R8A8: the last frame's gray, rounded to a byte. */
      const uint8_t *pixels = readback.map;
      const uint8_t want = (uint8_t)((0.10f + 0.10f * ((frames - 1) % 16) / 16.0f) * 255.0f + 0.5f);
      check(presented == frames && pixels[0] == want && pixels[1] == want && pixels[2] == want && pixels[3] == 255 &&
               pixels[4] == want && pixels[7] == 255,
            "display: the last frame's first and last pixels read back");

      uint32_t new_count = 0;
      VkImage new_images[8];
      VkSwapchainKHR replacement = presented == frames && measured == paced
                                     ? display_swapchain(c, surface, swapchain, new_images, &new_count)
                                     : VK_NULL_HANDLE;
      if (replacement) {
         uint32_t index;
         result = vkAcquireNextImageKHR(c->device, swapchain, 0, VK_NULL_HANDLE, c->fence, &index);
         check(result == VK_ERROR_OUT_OF_DATE_KHR, "display: the retired swapchain's acquire is out of date");
         vkDestroySwapchainKHR(c->device, swapchain, NULL);
         swapchain = VK_NULL_HANDLE;
         unsigned more = 0;
         for (unsigned frame = 0; frame < 5; frame++) {
            if (display_frame(c, replacement, new_images, acquired, rendered, 0.0f, NULL) != VK_SUCCESS)
               break;
            more++;
         }
         check(more == 5, "display: 5 frames through the replacement");
         vkDeviceWaitIdle(c->device);
         vkDestroySwapchainKHR(c->device, replacement, NULL);
      }
      if (swapchain)
         vkDestroySwapchainKHR(c->device, swapchain, NULL);
   }
   vkDestroySemaphore(c->device, acquired, NULL);
   vkDestroySemaphore(c->device, rendered, NULL);
   buffer_destroy(c, &readback);
   vkDestroySurfaceKHR(c->instance, surface, NULL);
}

/* -------------------------------------------------------------------- main */

static bool
context_create(struct context *c)
{
   vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
   if (!vkCreateInstance) {
      report("vk_icdGetInstanceProcAddr has no vkCreateInstance");
      return false;
   }
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "radv-smoke",
      .apiVersion = VK_API_VERSION_1_4,
   };
   /* VK_KHR_display where it is reported, for test_display. */
   const PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions =
      (PFN_vkEnumerateInstanceExtensionProperties)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE,
                                                                            "vkEnumerateInstanceExtensionProperties");
   VkExtensionProperties instance_extensions[64];
   uint32_t instance_extension_count = sizeof(instance_extensions) / sizeof(instance_extensions[0]);
   c->display = false;
   if (enumerate_instance_extensions &&
       enumerate_instance_extensions(NULL, &instance_extension_count, instance_extensions) >= VK_SUCCESS) {
      for (uint32_t i = 0; i < instance_extension_count; i++)
         c->display |= strcmp(instance_extensions[i].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME) == 0;
   }
   const char *const display_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = c->display ? 2 : 0,
      .ppEnabledExtensionNames = display_extensions,
   };
   VkResult result = vkCreateInstance(&instance_info, NULL, &c->instance);
   check(result == VK_SUCCESS, "vkCreateInstance");
   if (result != VK_SUCCESS || !load_instance(c->instance))
      return false;
   uint32_t count = 1;
   result = vkEnumeratePhysicalDevices(c->instance, &count, &c->physical);
   check((result == VK_SUCCESS || result == VK_INCOMPLETE) && count == 1, "one physical device");
   if (count == 0)
      return false;

   VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
   VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
   vkGetPhysicalDeviceProperties2(c->physical, &props);
   report("device: %s, Vulkan %u.%u.%u, %s %s", props.properties.deviceName,
          VK_API_VERSION_MAJOR(props.properties.apiVersion), VK_API_VERSION_MINOR(props.properties.apiVersion),
          VK_API_VERSION_PATCH(props.properties.apiVersion), driver.driverName, driver.driverInfo);

   VkQueueFamilyProperties families[8];
   count = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(c->physical, &count, families);
   c->family = UINT32_MAX;
   for (uint32_t i = 0; i < count; i++) {
      if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
          (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
         c->family = i;
         break;
      }
   }
   if (c->family == UINT32_MAX) {
      check(false, "a graphics and compute queue family");
      return false;
   }
   vkGetPhysicalDeviceMemoryProperties(c->physical, &c->memory);

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = c->family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
      .fragmentShaderBarycentric = VK_TRUE,
   };
   VkPhysicalDeviceFragmentShadingRateFeaturesKHR shading_rate = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
      .pipelineFragmentShadingRate = VK_TRUE,
   };
   VkPhysicalDeviceVulkan12Features features12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .drawIndirectCount = VK_TRUE,
   };
   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features12,
      .synchronization2 = VK_TRUE,
      .dynamicRendering = VK_TRUE,
   };
   const char *device_extensions[12];
   uint32_t enabled_extensions = 0;
   VkExtensionProperties extensions[512];
   uint32_t extension_count = sizeof(extensions) / sizeof(extensions[0]);
   c->barycentric = false;
   c->provoking = false;
   c->shading_rate = false;
   c->acceleration_structure = false;
   if (vkEnumerateDeviceExtensionProperties(c->physical, NULL, &extension_count, extensions) >= VK_SUCCESS) {
      for (uint32_t i = 0; i < extension_count; i++) {
         c->barycentric |= strcmp(extensions[i].extensionName, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0;
         c->provoking |= strcmp(extensions[i].extensionName, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME) == 0;
         c->shading_rate |= strcmp(extensions[i].extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0;
         c->acceleration_structure |=
            strcmp(extensions[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0;
         c->descriptor_buffer |= strcmp(extensions[i].extensionName, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) == 0;
         c->mesh |= strcmp(extensions[i].extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0;
      }
   }
   VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
   };
   if (c->mesh) {
      VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &mesh};
      vkGetPhysicalDeviceFeatures2(c->physical, &query);
      c->mesh = mesh.meshShader;
      c->task = mesh.taskShader;
      mesh.pNext = NULL;
      mesh.multiviewMeshShader = VK_FALSE;
      mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
      mesh.meshShaderQueries = VK_FALSE;
   }
   if (c->mesh) {
      device_extensions[enabled_extensions++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
      mesh.pNext = features13.pNext;
      features13.pNext = &mesh;
   }
   VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
      .accelerationStructure = VK_TRUE,
   };
   if (c->acceleration_structure) {
      device_extensions[enabled_extensions++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
      device_extensions[enabled_extensions++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
      features12.bufferDeviceAddress = VK_TRUE;
      acceleration_structure.pNext = features13.pNext;
      features13.pNext = &acceleration_structure;
   }
   VkPhysicalDeviceProvokingVertexFeaturesEXT provoking = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
      .provokingVertexLast = VK_TRUE,
   };
   if (c->provoking) {
      device_extensions[enabled_extensions++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
      provoking.pNext = features13.pNext;
      features13.pNext = &provoking;
   }
   if (c->barycentric) {
      device_extensions[enabled_extensions++] = VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
      barycentric.pNext = features13.pNext;
      features13.pNext = &barycentric;
   }
   if (c->display)
      device_extensions[enabled_extensions++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
   VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
      .descriptorBuffer = VK_TRUE,
   };
   c->descriptor_buffer &= getenv("RADV_SMOKE_SPARSE_TIMING") != NULL;
   if (c->descriptor_buffer) {
      device_extensions[enabled_extensions++] = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME;
      features12.bufferDeviceAddress = VK_TRUE;
      descriptor_buffer.pNext = features13.pNext;
      features13.pNext = &descriptor_buffer;
   }
   if (c->shading_rate) {
      device_extensions[enabled_extensions++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
      shading_rate.pNext = features13.pNext;
      features13.pNext = &shading_rate;
   }
   VkPhysicalDeviceFeatures supported;
   vkGetPhysicalDeviceFeatures(c->physical, &supported);
   const VkPhysicalDeviceFeatures features = {
      .sparseBinding = supported.sparseBinding,
      .geometryShader = VK_TRUE,
      .tessellationShader = VK_TRUE,
      .vertexPipelineStoresAndAtomics = VK_TRUE,
      .fragmentStoresAndAtomics = VK_TRUE,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features13,
      .pEnabledFeatures = &features,
      .enabledExtensionCount = enabled_extensions,
      .ppEnabledExtensionNames = device_extensions,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   result = vkCreateDevice(c->physical, &device_info, NULL, &c->device);
   check(result == VK_SUCCESS, "vkCreateDevice");
   if (result != VK_SUCCESS || !load_device(c->device))
      return false;
   if (c->mesh) {
#define LOAD(name) c->mesh &= (vk##name = (PFN_vk##name)vkGetDeviceProcAddr(c->device, "vk" #name)) != NULL;
      MESH_COMMANDS(LOAD)
#undef LOAD
   }
   vkGetDeviceQueue(c->device, c->family, 0, &c->queue);

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = c->family,
   };
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (vkCreateCommandPool(c->device, &pool_info, NULL, &c->pool) != VK_SUCCESS ||
       vkCreateFence(c->device, &fence_info, NULL, &c->fence) != VK_SUCCESS)
      return false;
   const VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   return vkAllocateCommandBuffers(c->device, &cmd_info, &c->cmd) == VK_SUCCESS;
}

/* A mesh shader whose outputs follow an atomic (radv/shaders/mesh_ticket.mesh):
 * each of 32 x 32 workgroups takes a ticket and paints its 8-pixel cell in the
 * ticket's colour with 128 triangles, which this GPU gets in two parts. Every
 * cell must be one colour, every ticket taken once, and the counter must end
 * at the number of workgroups: the workgroup ran once, and its second part
 * showed what the first computed. 1,024 workgroups also go round the port's
 * publish ring more than once. */
static bool
mesh_tickets_check(struct context *c, const char *what, const struct buffer *readback, const struct buffer *tickets)
{
   enum { CELLS = TARGET_SIZE / 8, WORKGROUPS = CELLS * CELLS };
   static bool seen[WORKGROUPS];
   memset(seen, 0, sizeof(seen));
   const uint8_t *pixels = readback->map;
   uint32_t mixed = 0, uncovered = 0, repeated = 0;
   for (uint32_t cy = 0; cy < CELLS; cy++) {
      for (uint32_t cx = 0; cx < CELLS; cx++) {
         const uint8_t *first = pixels + ((cy * 8) * TARGET_SIZE + cx * 8) * 4;
         bool uniform = true;
         for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++)
               uniform &= memcmp(pixels + ((cy * 8 + y) * TARGET_SIZE + cx * 8 + x) * 4, first, 4) == 0;
         mixed += !uniform;
         if (first[2] != 0 || first[3] != 255) {
            uncovered++;
            continue;
         }
         const uint32_t ticket = first[0] | (uint32_t)first[1] << 8;
         if (ticket >= WORKGROUPS || seen[ticket])
            repeated++;
         else
            seen[ticket] = true;
      }
   }
   const uint32_t counter = *(const uint32_t *)tickets->map;
   char label[160];
   snprintf(label, sizeof(label), "%s: %u workgroups each ran once and every cell shows one ticket", what, WORKGROUPS);
   const bool ok = mixed == 0 && uncovered == 0 && repeated == 0 && counter == WORKGROUPS;
   if (!ok)
      report("%s: %u cells of two colours, %u uncovered, %u tickets out of range or twice, counter %u", what, mixed,
             uncovered, repeated, counter);
   check(ok, label);
   (void)c;
   return ok;
}

static void
test_mesh(struct context *c)
{
   struct buffer readback = {0}, tickets = {0}, args = {0};
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback) ||
       !buffer_create(c, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &tickets) ||
       !buffer_create(c, 2 * sizeof(VkDrawMeshTasksIndirectCommandEXT), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &args)) {
      check(false, "mesh: buffers");
      return;
   }
   VkShaderModule mesh = shader(c, radv_smoke_mesh_ticket_mesh, sizeof(radv_smoke_mesh_ticket_mesh));
   VkShaderModule frag = shader(c, radv_smoke_mesh_colour_frag, sizeof(radv_smoke_mesh_colour_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_MESH_BIT_EXT, mesh),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };

   /* One draw of 32 x 32 workgroups. */
   memset(tickets.map, 0, 64);
   c->draw.mode = DRAW_MESH;
   c->draw.groups[0] = TARGET_SIZE / 8;
   c->draw.groups[1] = TARGET_SIZE / 8;
   c->draw.groups[2] = 1;
   if (render_readback_with(c, "mesh tickets, one draw", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0,
                            &readback, tickets.buffer, NULL))
      mesh_tickets_check(c, "mesh tickets, one draw", &readback, &tickets);

   /* The same cells from two indirect draws of 32 x 16: the second's
    * workgroups come after the first's. */
   memset(tickets.map, 0, 64);
   VkDrawMeshTasksIndirectCommandEXT *const commands = args.map;
   commands[0] = (VkDrawMeshTasksIndirectCommandEXT){TARGET_SIZE / 8, TARGET_SIZE / 16, 1};
   commands[1] = commands[0];
   c->draw.mode = DRAW_MESH_INDIRECT;
   c->draw.args = args.buffer;
   c->draw.max_draws = 2;
   if (render_readback_with(c, "mesh tickets, two indirect draws", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                            0, 0, &readback, tickets.buffer, NULL))
      mesh_tickets_check(c, "mesh tickets, two indirect draws", &readback, &tickets);

   memset(&c->draw, 0, sizeof(c->draw));
   vkDestroyShaderModule(c->device, mesh, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &args);
   buffer_destroy(c, &tickets);
   buffer_destroy(c, &readback);
}

/* Task shaders (radv/shaders/task_payload.task): 8,192 task workgroups, each
 * filling its 16 KiB payload with its number and launching two mesh workgroups
 * that paint their 2-pixel cells from its first and last words. The port runs
 * task shaders in chunks of as many workgroups as its payload ring holds
 * (4,096 of these), so the draw crosses a chunk; every cell must name its task
 * and mesh workgroup, and no payload may be torn. */
static bool
task_payload_check(const char *what, const struct buffer *readback)
{
   const uint8_t *pixels = readback->map;
   uint32_t wrong = 0, torn = 0;
   for (uint32_t cell = 0; cell < 128 * 128; cell++) {
      const uint32_t task = cell / 2, workgroup = cell % 2;
      const uint32_t x = (cell % 128) * 2, y = (cell / 128) * 2;
      const uint8_t *p = pixels + (y * TARGET_SIZE + x) * 4;
      const uint8_t expected[4] = {task & 255, (task >> 8) | (workgroup << 7), 0, 255};
      torn += p[2] == 255;
      wrong += memcmp(p, expected, 4) != 0;
   }
   char label[160];
   snprintf(label, sizeof(label), "%s: 8192 task workgroups' payloads reach their 16384 mesh workgroups", what);
   const bool ok = wrong == 0;
   if (!ok)
      report("%s: %u cells wrong, %u from torn payloads", what, wrong, torn);
   check(ok, label);
   return ok;
}

static void
test_task(struct context *c)
{
   struct buffer readback = {0}, args = {0};
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback) ||
       !buffer_create(c, 2 * sizeof(VkDrawMeshTasksIndirectCommandEXT), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &args)) {
      check(false, "task: buffers");
      return;
   }
   VkShaderModule task = shader(c, radv_smoke_task_payload_task, sizeof(radv_smoke_task_payload_task));
   VkShaderModule mesh = shader(c, radv_smoke_mesh_payload_mesh, sizeof(radv_smoke_mesh_payload_mesh));
   VkShaderModule frag = shader(c, radv_smoke_mesh_colour_frag, sizeof(radv_smoke_mesh_colour_frag));
   const VkPipelineShaderStageCreateInfo stages[3] = {
      stage_info(VK_SHADER_STAGE_TASK_BIT_EXT, task),
      stage_info(VK_SHADER_STAGE_MESH_BIT_EXT, mesh),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };

   c->draw.mode = DRAW_MESH;
   c->draw.groups[0] = 128;
   c->draw.groups[1] = 64;
   c->draw.groups[2] = 1;
   if (render_readback_with(c, "task payloads, one draw", stages, 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0,
                            &readback, VK_NULL_HANDLE, NULL))
      task_payload_check("task payloads, one draw", &readback);

   VkDrawMeshTasksIndirectCommandEXT *const commands = args.map;
   commands[0] = (VkDrawMeshTasksIndirectCommandEXT){128, 32, 1};
   commands[1] = commands[0];
   c->draw.mode = DRAW_MESH_INDIRECT;
   c->draw.args = args.buffer;
   c->draw.max_draws = 2;
   if (render_readback_with(c, "task payloads, two indirect draws", stages, 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                            0, 0, &readback, VK_NULL_HANDLE, NULL))
      task_payload_check("task payloads, two indirect draws", &readback);

   memset(&c->draw, 0, sizeof(c->draw));
   vkDestroyShaderModule(c->device, task, NULL);
   vkDestroyShaderModule(c->device, mesh, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &args);
   buffer_destroy(c, &readback);
}

/* The external host memory probe (docs/CTS_GAPS.md, VK_EXT_external_memory_host):
 * whether the GPU reaches a title's own anonymous (flexible) memory once
 * sceKernelMprotect grants it GPU access, at the address the CPU uses, as the
 * driver's direct memory is. A shader writes 16 KiB through a raw device
 * address. A GPU fault ends the run, so this check comes last. */
#if defined(__PROSPERO__)
int32_t sceKernelMprotect(const void *address, size_t length, int protection);
#include <sys/mman.h>

static void
test_host_pointer(struct context *c)
{
   enum { WORDS = 4096 };
   void *const memory = mmap(NULL, WORDS * 4, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
   if (memory == MAP_FAILED) {
      check(false, "host pointer: mmap");
      return;
   }
   memset(memory, 0, WORDS * 4);
   /* CPU read and write, GPU read and write. */
   const int32_t result = sceKernelMprotect(memory, WORDS * 4, 0x1 | 0x2 | 0x10 | 0x20);
   report("host pointer: sceKernelMprotect with GPU access returned 0x%08x", (unsigned)result);
   if (result != 0) {
      check(false, "host pointer: anonymous memory takes GPU access");
      munmap(memory, WORDS * 4);
      return;
   }

   const VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 12};
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &range,
   };
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkShaderModule module = shader(c, radv_smoke_ptr_write_comp, sizeof(radv_smoke_ptr_write_comp));
   bool ok = module && vkCreatePipelineLayout(c->device, &layout_info, NULL, &layout) == VK_SUCCESS;
   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
         },
      .layout = layout,
   };
   ok = ok && vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS;
   if (ok) {
      struct {
         uint64_t address;
         uint32_t value;
      } __attribute__((packed)) push = {(uint64_t)(uintptr_t)memory, 0x5a000000u};
      ok = begin(c);
      vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdPushConstants(c->cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
      vkCmdDispatch(c->cmd, WORDS / 64, 1, 1);
      ok = ok && submit_and_wait(c, "host pointer");
   }
   unsigned wrong = 0;
   const volatile uint32_t *const words = memory;
   for (uint32_t i = 0; ok && i < WORDS; i++)
      wrong += words[i] != 0x5a000000u + i;
   if (ok && wrong)
      report("host pointer: %u of %u words wrong, the first reads 0x%08x", wrong, WORDS, words[0]);
   check(ok && wrong == 0, "host pointer: a shader writes a title's anonymous memory through its address");
   vkDestroyPipeline(c->device, pipeline, NULL);
   vkDestroyPipelineLayout(c->device, layout, NULL);
   vkDestroyShaderModule(c->device, module, NULL);
   munmap(memory, WORDS * 4);
}
#endif

int
main(void)
{
   const int captured = ps5_klog_capture_stderr("[radv-smoke:stderr] ");
   /* Optional NAME=VALUE lines for the driver's environment (RADV_DEBUG,
    * ACO_DEBUG), read before the driver is. */
   FILE *const env = fopen("/app0/radv-smoke-env.txt", "r");
   if (env) {
      char line[256];
      while (fgets(line, sizeof(line), env)) {
         line[strcspn(line, "\r\n")] = '\0';
         char *const equals = strchr(line, '=');
         if (equals && line[0] != '#') {
            *equals = '\0';
            setenv(line, equals + 1, 1);
         }
      }
      fclose(env);
   }
   /* RADV_SMOKE_STDERR names a file for standard error instead of klog, for
    * output too large for it (RADV_DEBUG=shaders). */
   if (getenv("RADV_SMOKE_STDERR") && freopen(getenv("RADV_SMOKE_STDERR"), "w", stderr))
      setvbuf(stderr, NULL, _IOLBF, 0);
   results = fopen(RESULTS_PATH, "w");
   report("RADV smoke test starts");
   /* The line below arrives in klog prefixed "[radv-smoke:stderr]" when the
    * capture works; the driver's own messages take the same way. */
   report("standard error to klog: %s", captured == 0 ? "captured" : "not captured");
   fprintf(stderr, "standard error reaches klog\n");
  test_fp_state();
  struct context c = {0};
   /* RADV_SMOKE_ONLY=task runs the task shader checks alone, for a driver
    * debugging them (RADV_DEBUG=shaders dumps every shader compiled). */
   const bool ready = context_create(&c);
#if defined(__PROSPERO__)
   if (ready)
      test_title_files();
#endif
   const char *const only = getenv("RADV_SMOKE_ONLY");
   if (ready && only && strcmp(only, "display") == 0) {
      if (c.display)
         test_display(&c);
      else
         check(false, "display: VK_KHR_display is not reported");
   } else if (ready && only && strcmp(only, "task") == 0) {
      if (c.task)
         test_task(&c);
   } else if (ready && only && strcmp(only, "barycentric") == 0) {
      /* The barycentric survey alone (docs/BARYCENTRIC_GFX101.md): runs where the rest of the checks do not, such as
       * with RADV_DEBUG=nongg, which a legacy geometry shader hangs. */
      if (c.barycentric) {
         test_barycentric(&c);
         test_barycentric_pair(&c);
         test_barycentric_survey(&c);
      } else {
         report("barycentric: VK_KHR_fragment_shader_barycentric is not reported");
      }
   } else if (ready) {
      test_fill(&c);
      test_copy(&c);
      test_compute(&c);
      test_triangle(&c);
      test_tessellation(&c);
      /* Diagnostic runs without NGG, where a legacy GS hangs, skip the GS checks. */
      if (!getenv("RADV_SMOKE_SKIP_GEOMETRY"))
         test_geometry(&c);
      test_scratch(&c);
      test_coherence(&c);
      test_primitive_id(&c);
      if (c.barycentric) {
         test_barycentric(&c);
         test_barycentric_pair(&c);
         test_barycentric_survey(&c);
      } else {
         report("barycentric: VK_KHR_fragment_shader_barycentric is not reported; its checks are skipped");
      }
      if (c.shading_rate)
         test_shading_rate(&c);
      else
         report("shading rate: VK_KHR_fragment_shading_rate is not reported; its checks are skipped");
      if (c.acceleration_structure)
         test_acceleration_structure(&c);
      if (c.mesh)
         test_mesh(&c);
      else
         report("mesh: VK_EXT_mesh_shader is not reported; its checks are skipped");
      if (c.task)
         test_task(&c);
      else
         report("task: task shaders are not reported; their checks are skipped");
      if (getenv("RADV_SMOKE_SPARSE_TIMING") && supported_sparse_binding(&c))
         test_sparse_timing(&c);
      if (c.display)
         test_display(&c);
      else
         report("display: VK_KHR_display is not reported; its checks are skipped");
#if defined(__PROSPERO__)
      /* Last: a GPU fault ends the run. It needs buffer device addresses. */
      if (c.acceleration_structure)
         test_host_pointer(&c);
#endif
   }
   if (c.device) {
      vkDeviceWaitIdle(c.device);
      vkDestroyFence(c.device, c.fence, NULL);
      vkDestroyCommandPool(c.device, c.pool, NULL);
      vkDestroyDevice(c.device, NULL);
   }
   if (c.instance)
      vkDestroyInstance(c.instance, NULL);
   report("RADV smoke test ends: %u passed, %u failed", passed, failed);
   if (results)
      fclose(results);
   return failed == 0 && passed > 0 ? 0 : 1;
}
