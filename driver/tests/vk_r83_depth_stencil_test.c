/*
 * PS5 Vulkan driver - R83 test: the two aspects of D32_SFLOAT_S8_UINT.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Round 83 (docs/M5_PHASE_C.md); built and run through the loader and directly
 * by tools/check-driver.sh (see ps5vk_test.h).
 *
 * LRPS2 creates its depth target as D32_SFLOAT_S8_UINT with sampled and
 * transfer usage, and refuses to start unless the format reports both. The
 * format's two aspects are two planes of one allocation: the depth surface at
 * the image's address, laid out as D32_SFLOAT's is (the four-byte Z map), and
 * the one-byte stencil plane at the next 64 KiB boundary after it, in the
 * one-byte Z_X map AddrLib gives (driver/ps5vk_image.c, ps5vk_image_plane and
 * ps5vk_tiled_stencil_terms; tools/check-mip-layout.sh holds the map against
 * AddrLib). This program checks, on a 300x200 image that spans six depth tiles
 * and two stencil tiles:
 *
 *   - the format reports sampling and both transfers beside its attachment bit;
 *   - an upload of each aspect lands in its own plane, every texel where the
 *     plane's map puts it (read straight from the mapped image memory), and a
 *     readback of each aspect returns exactly what was uploaded;
 *   - vkCmdClearDepthStencilImage clears each aspect it names and leaves the
 *     other plane alone;
 *   - vkCmdCopyImage copies one aspect into the same aspect of another image,
 *     and a copy between different aspects is refused.
 *
 * The console's r83-depth-stencil case is the other half: the depth block
 * writes both planes, and the texture unit and the readback read them back.
 */

#include <string.h>

#include "ps5vk_test.h"

#define WIDTH 300u
#define HEIGHT 200u
#define TEXELS (WIDTH * HEIGHT)
#define TILE_BYTES 0x10000u
/* Six 128x128 depth tiles (3x2), so the plane after them starts at 6 tiles. */
#define STENCIL_OFFSET (6u * TILE_BYTES)

static VkInstance g_instance;
static VkDevice g_device;
static VkQueue g_queue;

/* The four-byte depth map (driver/ps5vk_image.c, ps5vk_tiled_depth_offset;
 * the console's C5 readbacks proved it). */
static size_t
depth_offset(uint32_t x, uint32_t y)
{
   static const uint16_t x_masks[7] = {0x0004, 0x0010, 0x0040, 0x0100, 0x2200, 0x0800, 0x8400};
   static const uint16_t y_masks[7] = {0x0008, 0x0020, 0x0080, 0x1100, 0x0200, 0x0400, 0x4800};
   size_t local = 0;
   for (unsigned bit = 0; bit < 7; bit++) {
      if ((x >> bit) & 1u)
         local ^= x_masks[bit];
      if ((y >> bit) & 1u)
         local ^= y_masks[bit];
   }
   const size_t blocks_per_row = (WIDTH + 127u) / 128u;
   return ((size_t)(y >> 7) * blocks_per_row + (x >> 7)) * TILE_BYTES + local;
}

/* The one-byte Z_X map, as `mip-layout-oracle swizzle 1 1 64kb_z_x` prints it:
 * for each term, the coordinate shifted left and masked. */
static size_t
stencil_offset(uint32_t x, uint32_t y)
{
   static const struct {
      unsigned y;
      unsigned shift;
      unsigned mask;
   } terms[] = {
      {0, 0, 0x1},   {0, 1, 0x4},   {0, 2, 0x10},   {0, 3, 0x40},   {0, 5, 0x300},
      {0, 6, 0x800}, {0, 4, 0x400}, {0, 7, 0x2000}, {0, 8, 0x8000}, {1, 1, 0x2},
      {1, 2, 0x8},   {1, 3, 0xa0},  {1, 5, 0xf00},  {1, 6, 0x1000}, {1, 7, 0x4000},
   };
   const size_t in_x = x & 255u;
   const size_t in_y = y & 255u;
   size_t local = 0;
   for (unsigned index = 0; index < sizeof(terms) / sizeof(terms[0]); index++)
      local ^= ((terms[index].y ? in_y : in_x) << terms[index].shift) & terms[index].mask;
   const size_t blocks_per_row = (WIDTH + 255u) / 256u;
   return STENCIL_OFFSET + ((size_t)(y >> 8) * blocks_per_row + (x >> 8)) * TILE_BYTES + local;
}

static float g_depths[TEXELS];
static uint8_t g_stencils[TEXELS];

static void
fill_patterns(void)
{
   for (uint32_t y = 0; y < HEIGHT; y++)
      for (uint32_t x = 0; x < WIDTH; x++) {
         g_depths[y * WIDTH + x] = (float)(y * WIDTH + x) / (float)TEXELS;
         g_stencils[y * WIDTH + x] = (uint8_t)(x * 7u + y * 13u + 1u);
      }
}

static VkBuffer
create_buffer(VkDeviceSize bytes, VkDeviceMemory *memory, void **mapped)
{
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = bytes,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer buffer = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateBuffer)(g_device, &info, NULL, &buffer) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   VkMemoryRequirements requirements = {0};
   VK_FUNCTION(g_instance, GetBufferMemoryRequirements)(g_device, buffer, &requirements);
   const VkMemoryAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
   };
   if (VK_FUNCTION(g_instance, AllocateMemory)(g_device, &allocate, NULL, memory) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, BindBufferMemory)(g_device, buffer, *memory, 0) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, MapMemory)(g_device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) !=
          VK_SUCCESS)
      return VK_NULL_HANDLE;
   return buffer;
}

static VkImage
create_image(VkDeviceMemory *memory, void **mapped)
{
   const VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
      .extent = {WIDTH, HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   };
   VkImage image = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateImage)(g_device, &info, NULL, &image) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   VkMemoryRequirements requirements = {0};
   VK_FUNCTION(g_instance, GetImageMemoryRequirements)(g_device, image, &requirements);
   const VkMemoryAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
   };
   if (VK_FUNCTION(g_instance, AllocateMemory)(g_device, &allocate, NULL, memory) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, BindImageMemory)(g_device, image, *memory, 0) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, MapMemory)(g_device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) !=
          VK_SUCCESS)
      return VK_NULL_HANDLE;
   check(requirements.size >= STENCIL_OFFSET + 2u * TILE_BYTES,
         "the allocation holds the six depth tiles and the two stencil tiles after them");
   return image;
}

/* One command buffer: begin, the caller's recording, end, submit and wait. The
 * end's result is returned, so a refused command is seen there. */
static VkResult
run(VkCommandPool pool, VkFence fence, void (*calls)(VkCommandBuffer, void *), void *context)
{
   const VkCommandBufferAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, AllocateCommandBuffers)(g_device, &allocate, &command) != VK_SUCCESS)
      return VK_ERROR_INITIALIZATION_FAILED;
   const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VK_FUNCTION(g_instance, BeginCommandBuffer)(command, &begin);
   calls(command, context);
   const VkResult ended = VK_FUNCTION(g_instance, EndCommandBuffer)(command);
   if (ended != VK_SUCCESS) {
      VK_FUNCTION(g_instance, FreeCommandBuffers)(g_device, pool, 1, &command);
      return ended;
   }
   const VkSubmitInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command,
   };
   VkResult result = VK_FUNCTION(g_instance, QueueSubmit)(g_queue, 1, &info, fence);
   if (result == VK_SUCCESS)
      result = VK_FUNCTION(g_instance, WaitForFences)(g_device, 1, &fence, VK_TRUE, UINT64_MAX);
   VK_FUNCTION(g_instance, ResetFences)(g_device, 1, &fence);
   VK_FUNCTION(g_instance, FreeCommandBuffers)(g_device, pool, 1, &command);
   return result;
}

struct transfer {
   VkImage image;
   VkImage other;
   VkBuffer buffer;
   VkImageAspectFlags aspect;
   VkImageAspectFlags other_aspect;
   VkClearDepthStencilValue clear;
};

static void
calls_upload(VkCommandBuffer command, void *context)
{
   const struct transfer *const t = context;
   const VkBufferImageCopy copy = {
      .imageSubresource = {t->aspect, 0, 0, 1},
      .imageExtent = {WIDTH, HEIGHT, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyBufferToImage)(command, t->buffer, t->image,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

static void
calls_readback(VkCommandBuffer command, void *context)
{
   const struct transfer *const t = context;
   const VkBufferImageCopy copy = {
      .imageSubresource = {t->aspect, 0, 0, 1},
      .imageExtent = {WIDTH, HEIGHT, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyImageToBuffer)(command, t->image,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->buffer, 1,
                                                 &copy);
}

static void
calls_clear(VkCommandBuffer command, void *context)
{
   const struct transfer *const t = context;
   const VkImageSubresourceRange range = {t->aspect, 0, 1, 0, 1};
   VK_FUNCTION(g_instance, CmdClearDepthStencilImage)(command, t->image,
                                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                      &t->clear, 1, &range);
}

static void
calls_copy(VkCommandBuffer command, void *context)
{
   const struct transfer *const t = context;
   const VkImageCopy copy = {
      .srcSubresource = {t->aspect, 0, 0, 1},
      .dstSubresource = {t->other_aspect, 0, 0, 1},
      .extent = {WIDTH, HEIGHT, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyImage)(command, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         t->other, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

/* How many texels of each plane of the mapped image hold the expected value. */
static unsigned
depth_plane_matches(const unsigned char *image, const float *want, float constant)
{
   unsigned matching = 0;
   for (uint32_t y = 0; y < HEIGHT; y++)
      for (uint32_t x = 0; x < WIDTH; x++) {
         float got;
         memcpy(&got, image + depth_offset(x, y), sizeof(got));
         matching += got == (want ? want[y * WIDTH + x] : constant);
      }
   return matching;
}

static unsigned
stencil_plane_matches(const unsigned char *image, const uint8_t *want, uint8_t constant)
{
   unsigned matching = 0;
   for (uint32_t y = 0; y < HEIGHT; y++)
      for (uint32_t x = 0; x < WIDTH; x++)
         matching += image[stencil_offset(x, y)] == (want ? want[y * WIDTH + x] : constant);
   return matching;
}

int
main(void)
{
   test_begin("R83 D32_SFLOAT_S8_UINT aspects");
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   if (!test_create_device(&g_instance, &physical, &g_device)) {
      test_destroy_device(g_instance, g_device);
      return test_finish();
   }
   VK_FUNCTION(g_instance, GetDeviceQueue)(g_device, 0, 0, &g_queue);

   VkFormatProperties properties = {0};
   VK_FUNCTION(g_instance, GetPhysicalDeviceFormatProperties)(
      physical, VK_FORMAT_D32_SFLOAT_S8_UINT, &properties);
   const VkFormatFeatureFlags wanted =
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
   check((properties.optimalTilingFeatures & wanted) == wanted,
         "D32_SFLOAT_S8_UINT reports sampling and both transfers beside its attachment bit");

   fill_patterns();
   VkDeviceMemory image_memory = VK_NULL_HANDLE, other_memory = VK_NULL_HANDLE;
   VkDeviceMemory depth_memory = VK_NULL_HANDLE, stencil_memory = VK_NULL_HANDLE;
   void *image_mapped = NULL, *other_mapped = NULL, *depth_mapped = NULL, *stencil_mapped = NULL;
   const VkImage image = create_image(&image_memory, &image_mapped);
   const VkImage other = create_image(&other_memory, &other_mapped);
   const VkBuffer depth_buffer = create_buffer(sizeof(g_depths), &depth_memory, &depth_mapped);
   const VkBuffer stencil_buffer =
      create_buffer(sizeof(g_stencils), &stencil_memory, &stencil_mapped);
   const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool = VK_NULL_HANDLE;
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence = VK_NULL_HANDLE;
   const bool ready =
      image != VK_NULL_HANDLE && other != VK_NULL_HANDLE && depth_buffer != VK_NULL_HANDLE &&
      stencil_buffer != VK_NULL_HANDLE &&
      VK_FUNCTION(g_instance, CreateCommandPool)(g_device, &pool_info, NULL, &pool) == VK_SUCCESS &&
      VK_FUNCTION(g_instance, CreateFence)(g_device, &fence_info, NULL, &fence) == VK_SUCCESS;
   check(ready, "two D32_SFLOAT_S8_UINT images with transfer and sampled usage and two buffers");
   if (ready) {
      memset(image_mapped, 0, (size_t)STENCIL_OFFSET + 2u * TILE_BYTES);
      memset(other_mapped, 0, (size_t)STENCIL_OFFSET + 2u * TILE_BYTES);

      /* Uploads: each aspect into its own plane, every texel at its map's place. */
      memcpy(depth_mapped, g_depths, sizeof(g_depths));
      memcpy(stencil_mapped, g_stencils, sizeof(g_stencils));
      struct transfer depth = {.image = image, .buffer = depth_buffer,
                               .aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
      struct transfer stencil = {.image = image, .buffer = stencil_buffer,
                                 .aspect = VK_IMAGE_ASPECT_STENCIL_BIT};
      check(run(pool, fence, calls_upload, &depth) == VK_SUCCESS &&
               run(pool, fence, calls_upload, &stencil) == VK_SUCCESS,
            "an upload of the depth aspect and one of the stencil aspect record and run");
      check(depth_plane_matches(image_mapped, g_depths, 0) == TEXELS,
            "every uploaded depth lands at the four-byte depth map's place");
      check(stencil_plane_matches(image_mapped, g_stencils, 0) == TEXELS,
            "every uploaded stencil byte lands in the plane at the one-byte Z_X map's place");

      /* Readbacks: four bytes a depth texel and one a stencil texel. */
      memset(depth_mapped, 0, sizeof(g_depths));
      memset(stencil_mapped, 0, sizeof(g_stencils));
      check(run(pool, fence, calls_readback, &depth) == VK_SUCCESS &&
               run(pool, fence, calls_readback, &stencil) == VK_SUCCESS,
            "a readback of each aspect records and runs");
      check(memcmp(depth_mapped, g_depths, sizeof(g_depths)) == 0,
            "the depth aspect reads back as the floats it was uploaded with");
      check(memcmp(stencil_mapped, g_stencils, sizeof(g_stencils)) == 0,
            "the stencil aspect reads back as the bytes it was uploaded with");

      /* Clears: both aspects, then depth alone. */
      struct transfer both = {.image = image,
                              .aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                              .clear = {0.25f, 0x7eu}};
      check(run(pool, fence, calls_clear, &both) == VK_SUCCESS,
            "a clear of both aspects records and runs");
      check(depth_plane_matches(image_mapped, NULL, 0.25f) == TEXELS &&
               stencil_plane_matches(image_mapped, NULL, 0x7e) == TEXELS,
            "the clear writes 0.25 into every depth texel and 0x7e into every stencil one");
      struct transfer depth_only = {.image = image, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                                    .clear = {0.75f, 0x11u}};
      check(run(pool, fence, calls_clear, &depth_only) == VK_SUCCESS,
            "a clear of the depth aspect alone records and runs");
      check(depth_plane_matches(image_mapped, NULL, 0.75f) == TEXELS &&
               stencil_plane_matches(image_mapped, NULL, 0x7e) == TEXELS,
            "a depth clear leaves the stencil plane as it was");

      /* Copies: the uploaded patterns again, then each aspect into the other
       * image's same aspect. */
      memcpy(depth_mapped, g_depths, sizeof(g_depths));
      memcpy(stencil_mapped, g_stencils, sizeof(g_stencils));
      check(run(pool, fence, calls_upload, &depth) == VK_SUCCESS &&
               run(pool, fence, calls_upload, &stencil) == VK_SUCCESS,
            "the patterns are uploaded again for the copies");
      struct transfer copy_depth = {.image = image, .other = other,
                                    .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                                    .other_aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
      check(run(pool, fence, calls_copy, &copy_depth) == VK_SUCCESS,
            "a copy of the depth aspect records and runs");
      check(depth_plane_matches(other_mapped, g_depths, 0) == TEXELS &&
               stencil_plane_matches(other_mapped, NULL, 0) == TEXELS,
            "the depth copy fills the other image's depth plane and nothing else");
      struct transfer copy_stencil = {.image = image, .other = other,
                                      .aspect = VK_IMAGE_ASPECT_STENCIL_BIT,
                                      .other_aspect = VK_IMAGE_ASPECT_STENCIL_BIT};
      check(run(pool, fence, calls_copy, &copy_stencil) == VK_SUCCESS,
            "a copy of the stencil aspect records and runs");
      check(stencil_plane_matches(other_mapped, g_stencils, 0) == TEXELS &&
               depth_plane_matches(other_mapped, g_depths, 0) == TEXELS,
            "the stencil copy fills the other image's stencil plane and keeps its depth");
      struct transfer crossed = {.image = image, .other = other,
                                 .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                                 .other_aspect = VK_IMAGE_ASPECT_STENCIL_BIT};
      check(run(pool, fence, calls_copy, &crossed) != VK_SUCCESS,
            "a copy from the depth aspect into the stencil aspect is refused");
   }
   if (fence != VK_NULL_HANDLE)
      VK_FUNCTION(g_instance, DestroyFence)(g_device, fence, NULL);
   if (pool != VK_NULL_HANDLE)
      VK_FUNCTION(g_instance, DestroyCommandPool)(g_device, pool, NULL);
   VK_FUNCTION(g_instance, DestroyBuffer)(g_device, depth_buffer, NULL);
   VK_FUNCTION(g_instance, DestroyBuffer)(g_device, stencil_buffer, NULL);
   VK_FUNCTION(g_instance, DestroyImage)(g_device, image, NULL);
   VK_FUNCTION(g_instance, DestroyImage)(g_device, other, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, depth_memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, stencil_memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, image_memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, other_memory, NULL);
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
