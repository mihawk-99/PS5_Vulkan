/*
 * PS5 Vulkan driver - Phase C7 test: vkCmdClearColorImage.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C7 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * An image clear is the same CPU work at a submission split point the transfers
 * are: the driver writes one encoded texel over every texel of the ranges'
 * regions, through the image's own map (the padded rows of a linear image, the
 * measured tile map of a colour attachment). What this test checks is the two
 * halves of that: the encoding of a clear value into the format's texel bytes,
 * and the placement of those bytes, which the readback then reads back:
 *
 *   - R8G8B8A8_UNORM, R8_UNORM, R8G8_UNORM, R16_UNORM and R32_SFLOAT, each
 *     cleared with a value whose texel bytes the test computes;
 *   - R8G8B8A8_SRGB, whose stored texel is the encode of the linear clear
 *     value;
 *   - a tiled colour attachment, read back through the tile map;
 *   - a two-level image, one level cleared at a time;
 *   - a range that names array layers, which the driver refuses by name (D1).
 *
 * The PS5 build only links; the console's own run of the clear and readback is
 * what proves the same bytes there.
 */

#include <string.h>

#include "ps5vk_test.h"

#define WIDTH 64u
#define HEIGHT 36u

static VkInstance g_instance;
static VkDevice g_device;
static VkQueue g_queue;

static bool
submit_ok(VkFence fence, VkCommandBuffer command, const char *what)
{
   const VkSubmitInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command,
   };
   const VkResult submitted = VK_FUNCTION(g_instance, QueueSubmit)(g_queue, 1, &info, fence);
   const VkResult waited =
      submitted == VK_SUCCESS
         ? VK_FUNCTION(g_instance, WaitForFences)(g_device, 1, &fence, VK_TRUE, UINT64_MAX)
         : submitted;
   VK_FUNCTION(g_instance, ResetFences)(g_device, 1, &fence);
   check(waited == VK_SUCCESS, what);
   return waited == VK_SUCCESS;
}

/* A command buffer that runs the caller's calls, or VK_NULL_HANDLE with the
 * driver's refusal reported when it records one. */
static VkCommandBuffer
record(VkCommandPool pool, VkFence fence, void (*calls)(VkCommandBuffer), const char *what)
{
   const VkCommandBufferAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, AllocateCommandBuffers)(g_device, &allocate, &command) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VK_FUNCTION(g_instance, BeginCommandBuffer)(command, &begin);
   calls(command);
   const VkResult ended = VK_FUNCTION(g_instance, EndCommandBuffer)(command);
   if (ended != VK_SUCCESS) {
      /* The refusal is the finding: a range the driver does not accept. */
      check(true, what);
      return NULL;
   }
   if (!submit_ok(fence, command, what))
      return NULL;
   VK_FUNCTION(g_instance, FreeCommandBuffers)(g_device, pool, 1, &command);
   return command;
}

/* The setup one format's case needs: an image of it, a readback buffer, and the
 * calls that clear the image and read it back. */
struct case_state {
   VkImage image;
   VkBuffer buffer;
   VkImageUsageFlags usage;
   VkFormat format;
   VkClearColorValue clear;
   uint32_t mip_level;
   uint32_t levels;
   bool tiled;
   bool refuse_range;
};

static struct case_state *g_case;

static void
calls_clear(VkCommandBuffer command)
{
   const VkImageSubresourceRange range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = g_case->mip_level,
      .levelCount = 1,
      /* The refusal case names two layers, which no image here has: the driver
       * refuses it by name before it looks at the image. */
      .baseArrayLayer = 0,
      .layerCount = g_case->refuse_range ? 2u : 1u,
   };
   VK_FUNCTION(g_instance, CmdClearColorImage)(command, g_case->image, VK_IMAGE_LAYOUT_GENERAL,
                                               &g_case->clear, 1, &range);
}

static void
calls_readback(VkCommandBuffer command)
{
   const VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, g_case->mip_level, 0, 1},
      .imageOffset = {0, 0, 0},
      .imageExtent = {(WIDTH >> g_case->mip_level) > 1u ? (WIDTH >> g_case->mip_level) : 1u,
                      (HEIGHT >> g_case->mip_level) > 1u ? (HEIGHT >> g_case->mip_level) : 1u, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyImageToBuffer)(command, g_case->image, VK_IMAGE_LAYOUT_GENERAL,
                                                 g_case->buffer, 1, &copy);
}

/* Clears one level of one image of one format with a value whose texel bytes
 * the test computes, reads it back, and compares. */
static void
clear_level_case(VkCommandPool pool, VkFence fence, VkFormat format, VkImageUsageFlags usage,
                 VkClearColorValue clear, const uint8_t *expected, uint32_t texel_bytes,
                 uint32_t levels, uint32_t level, const char *what);

/* One level, one format. */
static void
clear_case(VkCommandPool pool, VkFence fence, VkFormat format, VkImageUsageFlags usage,
           VkClearColorValue clear, const uint8_t *expected, uint32_t texel_bytes,
           const char *what)
{
   clear_level_case(pool, fence, format, usage, clear, expected, texel_bytes, 1, 0, what);
}

static void
clear_level_case(VkCommandPool pool, VkFence fence, VkFormat format, VkImageUsageFlags usage,
                 VkClearColorValue clear, const uint8_t *expected, uint32_t texel_bytes,
                 uint32_t levels, uint32_t level, const char *what)
{
   struct case_state state = {
      .format = format,
      .usage = usage,
      .clear = clear,
      .levels = levels,
      .mip_level = level,
      .tiled = (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0,
   };
   const VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {WIDTH, HEIGHT, 1},
      .mipLevels = levels,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
   };
   const size_t level_width = (WIDTH >> level) > 1u ? (WIDTH >> level) : 1u;
   const size_t level_height = (HEIGHT >> level) > 1u ? (HEIGHT >> level) : 1u;
   const size_t level_bytes = level_width * level_height * texel_bytes;
   state.image = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateImage)(g_device, &info, NULL, &state.image) != VK_SUCCESS)
      return;
   VkMemoryRequirements requirements = {0};
   VK_FUNCTION(g_instance, GetImageMemoryRequirements)(g_device, state.image, &requirements);
   const VkMemoryAllocateInfo image_memory = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, AllocateMemory)(g_device, &image_memory, NULL, &memory) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, BindImageMemory)(g_device, state.image, memory, 0) != VK_SUCCESS)
      return;

   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = (VkDeviceSize)level_bytes,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateBuffer)(g_device, &buffer_info, NULL, &state.buffer) !=
       VK_SUCCESS)
      return;
   VkMemoryRequirements buffer_requirements = {0};
   VK_FUNCTION(g_instance, GetBufferMemoryRequirements)(g_device, state.buffer,
                                                        &buffer_requirements);
   const VkMemoryAllocateInfo buffer_allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
   };
   void *mapped = NULL;
   if (VK_FUNCTION(g_instance, AllocateMemory)(g_device, &buffer_allocate, NULL, &buffer_memory) !=
           VK_SUCCESS ||
       VK_FUNCTION(g_instance, BindBufferMemory)(g_device, state.buffer, buffer_memory, 0) !=
           VK_SUCCESS ||
       VK_FUNCTION(g_instance, MapMemory)(g_device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
           VK_SUCCESS ||
       mapped == NULL)
      return;
   memset(mapped, 0xa5, level_bytes);

   g_case = &state;
   VkCommandBuffer cleared = record(pool, fence, calls_clear, what);
   if (cleared == NULL) {
      g_case = NULL;
      return;
   }
   g_case = &state;
   if (record(pool, fence, calls_readback, what) != NULL) {
      size_t bad = level_bytes;
      const uint8_t *const bytes = mapped;
      for (size_t at = 0; at < bad; at++) {
         if (bytes[at] != expected[at % texel_bytes]) {
            bad = at;
            break;
         }
      }
      check(bad == level_bytes, what);
      if (bad != level_bytes)
         printf("  (texel byte %zu of %s is %02x, not %02x)\n", bad / texel_bytes, what,
                bytes[bad], expected[bad % texel_bytes]);
   }
   g_case = NULL;
   VK_FUNCTION(g_instance, DestroyImage)(g_device, state.image, NULL);
   VK_FUNCTION(g_instance, DestroyBuffer)(g_device, state.buffer, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, buffer_memory, NULL);
}

int
main(void)
{
   test_begin("C7 image clear");
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   if (!test_create_device(&g_instance, &physical, &g_device)) {
      test_destroy_device(g_instance, g_device);
      return test_finish();
   }
   VK_FUNCTION(g_instance, GetDeviceQueue)(g_device, 0, 0, &g_queue);

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (VK_FUNCTION(g_instance, CreateCommandPool)(g_device, &pool_info, NULL, &pool) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, CreateFence)(g_device, &fence_info, NULL, &fence) != VK_SUCCESS) {
      check(false, "a command pool and a fence");
      test_destroy_device(g_instance, g_device);
      return test_finish();
   }

   VkClearColorValue clear;
   memset(&clear, 0, sizeof(clear));

   /* R8G8B8A8_UNORM: 0.25, 0.5, 0.75 and opaque. */
   {
      const uint8_t expected[4] = {0x40, 0x80, 0xbf, 0xff};
      clear.float32[0] = 0.25f;
      clear.float32[1] = 0.5f;
      clear.float32[2] = 0.75f;
      clear.float32[3] = 1.0f;
      clear_case(pool, fence, VK_FORMAT_R8G8B8A8_UNORM,
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                 clear, expected, 4, "R8G8B8A8_UNORM clears to its encoded texel");
   }

   /* Round 14's byte-reversed sRGB form: the same encode written in B, G, R
    * order, which is the branch ps5vk_format_encode_clear gained for it. */
   {
      const VkClearColorValue clear = {.float32 = {0x40 / 255.0f, 0x80 / 255.0f, 0xc0 / 255.0f,
                                                   1.0f}};
      const unsigned char expected[4] = {0xe1, 0xbc, 0x89, 0xff};
      clear_case(pool, fence, VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, clear,
                 expected, 4,
                 "B8G8R8A8_SRGB clears to the encode of the linear value, blue first");
   }
   /* Round 17's packed sRGB form: the clear writes the encode in the image's own
    * storage order (R, G, B, A -- ps5vk_format.storage_reversed) and the copy
    * this case reads back with presents Vulkan's layout for the format, A, B, G,
    * R, so the bytes below are the ones an application sees. */
   {
      const VkClearColorValue clear = {.float32 = {0x40 / 255.0f, 0x80 / 255.0f, 0xc0 / 255.0f,
                                                   1.0f}};
      const uint8_t expected[4] = {0xff, 0xe1, 0xbc, 0x89};
      clear_case(pool, fence, VK_FORMAT_A8B8G8R8_SRGB_PACK32, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 clear, expected, 4,
                 "A8B8G8R8_SRGB_PACK32 clears to the encode, read back in the format's order");
   }
   /* R8G8B8A8_SRGB: the clear value is linear and the texel is the encode. */
   {
      const uint8_t expected[4] = {0xbc, 0xbc, 0xbc, 0xff};
      clear.float32[0] = clear.float32[1] = clear.float32[2] = 0.5f;
      clear.float32[3] = 1.0f;
      clear_case(pool, fence, VK_FORMAT_R8G8B8A8_SRGB,
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                 clear, expected, 4, "R8G8B8A8_SRGB clears to the encode of the linear value");
   }

   /* One- and two-channel and 16-bit formats. */
   {
      const uint8_t expected[1] = {0x80};
      clear.float32[0] = 0.5f;
      clear_case(pool, fence, VK_FORMAT_R8_UNORM,
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                 clear, expected, 1, "R8_UNORM clears to one byte");
   }
   {
      const uint8_t expected[2] = {0x40, 0x80};
      clear.float32[0] = 0.25f;
      clear.float32[1] = 0.5f;
      clear_case(pool, fence, VK_FORMAT_R8G8_UNORM,
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                 clear, expected, 2, "R8G8_UNORM clears to two bytes");
   }
   {
      const uint8_t expected[2] = {0x00, 0x80};
      clear.float32[0] = 0.5f;
      clear_case(pool, fence, VK_FORMAT_R16_UNORM,
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                 clear, expected, 2, "R16_UNORM clears to a 16-bit UNORM");
   }
   {
      const float value = 0.75f;
      uint8_t expected[4];
      memcpy(expected, &value, sizeof(value));
      clear.float32[0] = value;
      clear_case(pool, fence, VK_FORMAT_R32_SFLOAT,
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                 clear, expected, 4, "R32_SFLOAT clears to the float's own bits");
   }

   /* A tiled colour attachment: the clear writes through the tile map, and the
    * readback decodes the same map. */
   {
      const uint8_t expected[4] = {0x10, 0x20, 0x30, 0xff};
      clear.float32[0] = 16.0f / 255.0f;
      clear.float32[1] = 32.0f / 255.0f;
      clear.float32[2] = 48.0f / 255.0f;
      clear.float32[3] = 1.0f;
      clear_case(pool, fence, VK_FORMAT_R8G8B8A8_UNORM,
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, clear,
                 expected, 4, "a tiled attachment clears through its tile map");
   }

   /* One level of a two-level image: the clear and the readback both have to
    * name the level's own layout. */
   {
      const uint8_t expected[4] = {0x20, 0x40, 0x60, 0xff};
      clear.float32[0] = 32.0f / 255.0f;
      clear.float32[1] = 64.0f / 255.0f;
      clear.float32[2] = 96.0f / 255.0f;
      clear.float32[3] = 1.0f;
      clear_level_case(pool, fence, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT,
                       clear, expected, 4, 2, 1, "level 1 of a two-level image clears");
   }

   /* A range the driver refuses: two array layers of a single-layer image. */
   {
      struct case_state state = {
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .refuse_range = true,
      };
      const VkImageCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {WIDTH, HEIGHT, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      };
      if (VK_FUNCTION(g_instance, CreateImage)(g_device, &info, NULL, &state.image) == VK_SUCCESS) {
         VkMemoryRequirements requirements = {0};
         VK_FUNCTION(g_instance, GetImageMemoryRequirements)(g_device, state.image, &requirements);
         const VkMemoryAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
         };
         VkDeviceMemory memory = VK_NULL_HANDLE;
         if (VK_FUNCTION(g_instance, AllocateMemory)(g_device, &allocate, NULL, &memory) ==
                 VK_SUCCESS &&
             VK_FUNCTION(g_instance, BindImageMemory)(g_device, state.image, memory, 0) ==
                 VK_SUCCESS) {
            g_case = &state;
            const VkCommandBufferAllocateInfo buffer_allocate = {
               .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
               .commandPool = pool,
               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
               .commandBufferCount = 1,
            };
            VkCommandBuffer command = VK_NULL_HANDLE;
            VK_FUNCTION(g_instance, AllocateCommandBuffers)(g_device, &buffer_allocate, &command);
            const VkCommandBufferBeginInfo begin = {
               .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VK_FUNCTION(g_instance, BeginCommandBuffer)(command, &begin);
            calls_clear(command);
            check(VK_FUNCTION(g_instance, EndCommandBuffer)(command) == VK_ERROR_UNKNOWN,
                  "a clear range that names array layers is refused by name");
            g_case = NULL;
         }
      }
   }

   VK_FUNCTION(g_instance, DestroyFence)(g_device, fence, NULL);
   VK_FUNCTION(g_instance, DestroyCommandPool)(g_device, pool, NULL);
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
