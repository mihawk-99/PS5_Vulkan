/*
 * PS5 Vulkan driver - Phase C7 test: reading an image back into a buffer.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C7 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * vkCmdCopyImageToBuffer is a readback: the image's texels, placed by the map
 * its storage has (padded rows for a linear image, the measured 64 KiB tile map
 * for a colour attachment), land in a buffer whose rows the application may pad
 * with bufferRowLength. It is the same CPU work at a submission split point
 * every transfer here is (driver/ps5vk_queue.c), and this test reads the bytes
 * back from the mapped buffer:
 *
 *   - a round trip: a buffer uploaded into an image, then read back into a
 *     second buffer, byte for byte, with tight rows on both sides;
 *   - a padded readback: bufferRowLength wider than the region, whose padding
 *     the copy must leave alone;
 *   - a region: an imageOffset and an extent smaller than the image;
 *   - a tiled source: a colour attachment's storage, which the copy reads
 *     through the tile map (and which a bufferRowLength cannot describe for the
 *     image, only for the buffer).
 *
 * The PS5 build only links; the console's own run of the upload and readback is
 * what proves the same bytes there.
 */

#include <string.h>

#include "ps5vk_test.h"

#define WIDTH 64u
#define HEIGHT 36u
#define ROW_BYTES (WIDTH * 4u)
#define TEXELS_BYTES (WIDTH * HEIGHT * 4u)
#define TILE_TEXELS 128u
#define TILE_BYTES 0x10000u

/* Where texel (x, y) of a single-level four-byte tiled image lives: the map the
 * driver's tiled storage uses and the console's readbacks decode
 * (driver/ps5vk_image.c, ps5vk_tiled_texel_offset; src/diagnostics.cpp,
 * tiled_rgba8_offset). */
static size_t
tiled_offset(uint32_t x, uint32_t y)
{
   const size_t sx = x;
   const size_t sy = y;
   const size_t local = ((sy << 4) & 0x70u) ^ ((sy << 5) & 0xf00u) ^ ((sy << 9) & 0x1000u) ^
                        ((sy << 8) & 0x4000u) ^ ((sx << 2) & 0x0cu) ^ ((sx << 5) & 0x380u) ^
                        ((sx << 4) & 0x400u) ^ ((sx << 6) & 0x800u) ^ ((sx << 9) & 0xa000u);
   const size_t blocks_per_row = (WIDTH + TILE_TEXELS - 1u) / TILE_TEXELS;
   const size_t block = (sy / TILE_TEXELS) * blocks_per_row + sx / TILE_TEXELS;
   return block * TILE_BYTES + local;
}

/* The pattern: red the column, green the row, blue a checker, so a displaced,
 * mis-strided or flipped readback changes a byte. */
static void
fill_pattern(unsigned char *bytes)
{
   for (unsigned row = 0; row < HEIGHT; row++) {
      for (unsigned column = 0; column < WIDTH; column++) {
         unsigned char *const texel = bytes + ((size_t)row * WIDTH + column) * 4u;
         texel[0] = (unsigned char)(column * 4u);
         texel[1] = (unsigned char)(row * 7u);
         texel[2] = (unsigned char)(((column ^ row) & 1u) != 0u ? 0xe0 : 0x20);
         texel[3] = 0xff;
      }
   }
}

static VkInstance g_instance;
static VkDevice g_device;
static VkQueue g_queue;

static unsigned char g_pattern[TEXELS_BYTES];
static unsigned char g_readback[TEXELS_BYTES + 4096];

static void
submit_and_wait(VkFence fence, VkCommandBuffer command)
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
   check(waited == VK_SUCCESS, "the readback submits and signals its fence");
}

static VkBuffer
create_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **mapped)
{
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = bytes,
      .usage = usage,
   };
   VkBuffer buffer = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateBuffer)(g_device, &info, NULL, &buffer) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   VkMemoryRequirements requirements = {0};
   VK_FUNCTION(g_instance, GetBufferMemoryRequirements)(g_device, buffer, &requirements);
   const VkMemoryAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = 0,
   };
   if (VK_FUNCTION(g_instance, AllocateMemory)(g_device, &allocate, NULL, memory) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, BindBufferMemory)(g_device, buffer, *memory, 0) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   if (mapped != NULL &&
       (VK_FUNCTION(g_instance, MapMemory)(g_device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) !=
           VK_SUCCESS ||
        *mapped == NULL))
      return VK_NULL_HANDLE;
   return buffer;
}

/* Records one command buffer of the caller's calls and runs it. */
static VkResult
record(VkCommandPool pool, VkFence fence, void (*calls)(VkCommandBuffer), VkCommandBuffer *out)
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
   calls(command);
   const VkResult ended = VK_FUNCTION(g_instance, EndCommandBuffer)(command);
   if (ended != VK_SUCCESS) {
      check(false, "(the driver refused a recorded command)");
      return ended;
   }
   submit_and_wait(fence, command);
   *out = command;
   return VK_SUCCESS;
}

/* The setup the cases share: a linear image, a tiled one, and the three buffers
 * (the upload, the readback and a padded readback). */
static VkImage g_linear_image;
static VkImage g_tiled_image;
static VkBuffer g_upload;
static VkBuffer g_readback_buffer;
static VkBuffer g_padded_buffer;
static void *g_upload_mapped;
static void *g_readback_mapped;
static void *g_padded_mapped;
static void *g_tiled_mapped;

static VkImage
create_image(VkImageUsageFlags usage, unsigned samples, VkDeviceMemory *memory_out, void **mapped)
{
   const VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {WIDTH, HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
   };
   VkImage image = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateImage)(g_device, &info, NULL, &image) != VK_SUCCESS ||
       image == VK_NULL_HANDLE)
      return VK_NULL_HANDLE;
   VkMemoryRequirements requirements = {0};
   VK_FUNCTION(g_instance, GetImageMemoryRequirements)(g_device, image, &requirements);
   const VkMemoryAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, AllocateMemory)(g_device, &allocate, NULL, &memory) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, BindImageMemory)(g_device, image, memory, 0) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   *memory_out = memory;
   if (mapped != NULL &&
       (VK_FUNCTION(g_instance, MapMemory)(g_device, memory, 0, VK_WHOLE_SIZE, 0, mapped) !=
           VK_SUCCESS ||
        *mapped == NULL))
      return VK_NULL_HANDLE;
   return image;
}

/* The upload case's calls: a buffer copy into the linear image. */
static void
calls_upload(VkCommandBuffer command)
{
   const VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {0, 0, 0},
      .imageExtent = {WIDTH, HEIGHT, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyBufferToImage)(command, g_upload, g_linear_image,
                                                 VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
}

static void
calls_readback_tight(VkCommandBuffer command)
{
   const VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {0, 0, 0},
      .imageExtent = {WIDTH, HEIGHT, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyImageToBuffer)(command, g_linear_image,
                                                 VK_IMAGE_LAYOUT_GENERAL, g_readback_buffer, 1,
                                                 &copy);
}

static void
calls_readback_padded(VkCommandBuffer command)
{
   /* Rows 80 texels wide: 64 of image and 16 the copy must not touch. */
   const VkBufferImageCopy copy = {
      .bufferOffset = 64,
      .bufferRowLength = WIDTH + 16u,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {0, 0, 0},
      .imageExtent = {WIDTH, HEIGHT, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyImageToBuffer)(command, g_linear_image,
                                                 VK_IMAGE_LAYOUT_GENERAL, g_padded_buffer, 1,
                                                 &copy);
}

static void
calls_readback_region(VkCommandBuffer command)
{
   /* The 16x8 block at (8, 4), which the test compares against the same block
    * of the pattern. */
   const VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {8, 4, 0},
      .imageExtent = {16, 8, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyImageToBuffer)(command, g_linear_image,
                                                 VK_IMAGE_LAYOUT_GENERAL, g_readback_buffer, 1,
                                                 &copy);
}

static void
calls_readback_tiled(VkCommandBuffer command)
{
   const VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {0, 0, 0},
      .imageExtent = {WIDTH, HEIGHT, 1},
   };
   VK_FUNCTION(g_instance, CmdCopyImageToBuffer)(command, g_tiled_image,
                                                 VK_IMAGE_LAYOUT_GENERAL, g_readback_buffer, 1,
                                                 &copy);
}

/* The first byte where the readback and the pattern differ, or the number of
 * bytes compared when they are the same. */
static size_t
first_mismatch(const void *readback, size_t bytes)
{
   const unsigned char *const bytes_at = readback;
   for (size_t at = 0; at < bytes; at++) {
      if (bytes_at[at] != g_pattern[at])
         return at;
   }
   return bytes;
}

int
main(void)
{
   test_begin("C7 image readback");
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   if (!test_create_device(&g_instance, &physical, &g_device)) {
      test_destroy_device(g_instance, g_device);
      return test_finish();
   }
   VK_FUNCTION(g_instance, GetDeviceQueue)(g_device, 0, 0, &g_queue);
   fill_pattern(g_pattern);
   memset(g_readback, 0xa5, sizeof(g_readback));

   VkDeviceMemory linear_memory = VK_NULL_HANDLE;
   VkDeviceMemory tiled_memory = VK_NULL_HANDLE;
   g_linear_image = create_image(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_SAMPLE_COUNT_1_BIT, &linear_memory, NULL);
   /* A colour attachment's storage is the tiled one, and the test fills it
    * through the map that storage uses. */
   g_tiled_image = create_image(VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                VK_SAMPLE_COUNT_1_BIT, &tiled_memory, &g_tiled_mapped);
   VkDeviceMemory upload_memory = VK_NULL_HANDLE;
   VkDeviceMemory readback_memory = VK_NULL_HANDLE;
   VkDeviceMemory padded_memory = VK_NULL_HANDLE;
   g_upload = create_buffer(TEXELS_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload_memory,
                            &g_upload_mapped);
   g_readback_buffer = create_buffer(TEXELS_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     &readback_memory, &g_readback_mapped);
   g_padded_buffer = create_buffer(TEXELS_BYTES + 4096u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   &padded_memory, &g_padded_mapped);
   check(g_linear_image != VK_NULL_HANDLE && g_tiled_image != VK_NULL_HANDLE &&
            g_upload != VK_NULL_HANDLE && g_readback_buffer != VK_NULL_HANDLE &&
            g_padded_buffer != VK_NULL_HANDLE && g_upload_mapped != NULL &&
            g_readback_mapped != NULL && g_padded_mapped != NULL,
         "a linear and a tiled image, and the transfer buffers");

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (g_upload_mapped == NULL ||
       VK_FUNCTION(g_instance, CreateCommandPool)(g_device, &pool_info, NULL, &pool) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, CreateFence)(g_device, &fence_info, NULL, &fence) != VK_SUCCESS) {
      check(false, "a command pool and a fence");
      test_destroy_device(g_instance, g_device);
      return test_finish();
   }

   /* The tiled image's texels, written where the tile map puts them: the
    * readback below has to decode the same map to match. */
   if (g_tiled_mapped != NULL) {
      for (unsigned row = 0; row < HEIGHT; row++)
         for (unsigned column = 0; column < WIDTH; column++)
            memcpy((unsigned char *)g_tiled_mapped + tiled_offset(column, row),
                   g_pattern + ((size_t)row * WIDTH + column) * 4u, 4u);
   }

   /* 1. Upload the pattern through a buffer copy, so the image holds it. */
   {
      memcpy(g_upload_mapped, g_pattern, TEXELS_BYTES);
      VkCommandBuffer command = VK_NULL_HANDLE;
      record(pool, fence, calls_upload, &command);
   }

   /* 2. Read it back with tight rows. */
   {
      memset(g_readback_mapped, 0xa5, TEXELS_BYTES);
      VkCommandBuffer command = VK_NULL_HANDLE;
      if (record(pool, fence, calls_readback_tight, &command) == VK_SUCCESS) {
         const size_t bad = first_mismatch(g_readback_mapped, TEXELS_BYTES);
         check(bad == TEXELS_BYTES,
               "the readback is the uploaded pattern, byte for byte");
         if (bad != TEXELS_BYTES)
            printf("  (byte %zu of the readback is %02x, not %02x)\n", bad,
                   ((const unsigned char *)g_readback_mapped)[bad], g_pattern[bad]);
      }
   }

   /* 3. Read it back into rows 80 texels wide: the image is the same, and the
    * padding between the rows stays whatever the buffer held. */
   {
      const size_t pitch = (size_t)(WIDTH + 16u) * 4u;
      memset(g_padded_mapped, 0xa5, TEXELS_BYTES + 4096u);
      VkCommandBuffer command = VK_NULL_HANDLE;
      if (record(pool, fence, calls_readback_padded, &command) == VK_SUCCESS) {
         const unsigned char *const buffer = g_padded_mapped;
         bool rows = true;
         bool padding = true;
         bool before = true;
         /* The copy's bufferOffset is where its first row starts. */
         for (size_t at = 0; at < 64u; at++)
            before = before && buffer[at] == 0xa5;
         for (unsigned row = 0; row < HEIGHT; row++) {
            const unsigned char *const at = buffer + 64u + row * pitch;
            rows = rows && memcmp(at, g_pattern + (size_t)row * ROW_BYTES, ROW_BYTES) == 0;
            for (size_t pad = ROW_BYTES; pad < pitch; pad++)
               padding = padding && at[pad] == 0xa5;
         }
         check(rows, "a padded readback writes the region's rows where its pitch names them");
         check(padding && before, "and leaves the padding and the bytes before it alone");
      }
   }

   /* 4. A region: the 16x8 block at (8, 4), tight. */
   {
      memset(g_readback_mapped, 0xa5, TEXELS_BYTES);
      VkCommandBuffer command = VK_NULL_HANDLE;
      if (record(pool, fence, calls_readback_region, &command) == VK_SUCCESS) {
         bool block = true;
         for (unsigned row = 0; row < 8; row++) {
            const unsigned char *const source =
               g_pattern + ((size_t)(row + 4u) * WIDTH + 8u) * 4u;
            const unsigned char *const got = (const unsigned char *)g_readback_mapped + row * 64u;
            block = block && memcmp(got, source, 16u * 4u) == 0;
         }
         check(block, "an imageOffset and extent read back exactly the region they name");
      }
   }

   /* 5. A tiled source: the same pattern, placed by the tile map, read back
    * through the copy's own decoding of it. */
   {
      memset(g_readback_mapped, 0xa5, TEXELS_BYTES);
      VkCommandBuffer command = VK_NULL_HANDLE;
      if (record(pool, fence, calls_readback_tiled, &command) == VK_SUCCESS) {
         const size_t bad = first_mismatch(g_readback_mapped, TEXELS_BYTES);
         check(bad == TEXELS_BYTES,
               "a tiled image reads back through the tile map, byte for byte");
         if (bad != TEXELS_BYTES)
            printf("  (byte %zu of the tiled readback is %02x, not %02x)\n", bad,
                   ((const unsigned char *)g_readback_mapped)[bad], g_pattern[bad]);
      }
   }

   VK_FUNCTION(g_instance, DestroyFence)(g_device, fence, NULL);
   VK_FUNCTION(g_instance, DestroyCommandPool)(g_device, pool, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, upload_memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, readback_memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, padded_memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, linear_memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, tiled_memory, NULL);
   VK_FUNCTION(g_instance, DestroyBuffer)(g_device, g_upload, NULL);
   VK_FUNCTION(g_instance, DestroyBuffer)(g_device, g_readback_buffer, NULL);
   VK_FUNCTION(g_instance, DestroyBuffer)(g_device, g_padded_buffer, NULL);
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
