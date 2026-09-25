/*
 * PS5 Vulkan driver - Phase B3 test: formats and images.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B3 (docs/M5_PHASE_B.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h). Checks the
 * reported formats and image format properties, the storage size and
 * alignment of images against layouts the runner used on the hardware, and
 * binding.
 */

#include "ps5vk_test.h"

#define MIB (UINT64_C(1) << 20)

static VkInstance g_instance;
static VkPhysicalDevice g_physical;
static VkDevice g_device;

static const VkFormatFeatureFlags kRgba8Features =
   VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
   VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
   VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;

static VkFormatProperties
format_properties(VkFormat format)
{
   VkFormatProperties p;
   memset(&p, 0xff, sizeof(p));
   VK_FUNCTION(g_instance, GetPhysicalDeviceFormatProperties)(g_physical, format, &p);
   return p;
}

static VkResult
image_format_properties(VkFormat format, VkImageType type, VkImageTiling tiling,
                        VkImageUsageFlags usage, VkImageCreateFlags flags,
                        VkImageFormatProperties *p)
{
   memset(p, 0xff, sizeof(*p));
   return VK_FUNCTION(g_instance, GetPhysicalDeviceImageFormatProperties)(
      g_physical, format, type, tiling, usage, flags, p);
}

static bool
all_zero(const VkImageFormatProperties *p)
{
   static const VkImageFormatProperties zero;
   return memcmp(p, &zero, sizeof(zero)) == 0;
}

static void
check_formats(void)
{
   VkFormatProperties p = format_properties(VK_FORMAT_R8G8B8A8_UNORM);
   /* The claim: the sampled, transfer, attachment and blit bits above, plus the
    * three buffer features rung round 3 and blocker rounds 5 and 6 gave it
    * (v0-vertex-formats, v0-formats-texel-buffer, v0-formats-texel-buffer-store)
    * and the storage image round 7 proved (v0-formats-storage-image). */
   check(p.optimalTilingFeatures ==
               (kRgba8Features | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            p.linearTilingFeatures == 0 &&
            p.bufferFeatures ==
               (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
                VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT),
         "R8G8B8A8_UNORM: sampled, linear filter, transfer, colour attachment, blending, "
         "blit, storage image; vertex buffer; uniform and storage texel buffer");
   p = format_properties(VK_FORMAT_D32_SFLOAT);
   /* Every feature the two depth rows carry, all of them proved on the console:
    * the attachment bit (M4/C5), the transfer pair (the four-sample depth copy
    * and round 21's D16 round trip), the fetch (v0-formats-sampled-depth) and
    * the blit source (c7-blit-depth, a one-to-one depth-to-depth blit). */
   const VkFormatFeatureFlags kDepthFeatures =
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
   check(p.optimalTilingFeatures == kDepthFeatures && p.linearTilingFeatures == 0 &&
            p.bufferFeatures == 0,
         "D32_SFLOAT: depth attachment, sampled, blit source and both transfers");
   p = format_properties(VK_FORMAT_D16_UNORM);
   check(p.optimalTilingFeatures == kDepthFeatures && p.linearTilingFeatures == 0 &&
            p.bufferFeatures == 0,
         "D16_UNORM: its own depth word and the same fetch, blit source and transfers");
   /* The two stencil formats report the attachment bit round 12's console probe
    * proved (Klog_Logs/v0-stencil-run5.log), and nothing else: no transfer,
    * fetch or blit of a stencil plane has been measured, so the rest of their
    * features must stay zero. What the audit's clause requires of one of them is
    * exactly this bit. */
   for (int stencil_format = 0; stencil_format < 2; stencil_format++) {
      const VkFormat format = stencil_format == 0 ? VK_FORMAT_D24_UNORM_S8_UINT
                                                   : VK_FORMAT_D32_SFLOAT_S8_UINT;
      p = format_properties(format);
      check(p.optimalTilingFeatures == VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT &&
               p.linearTilingFeatures == 0 && p.bufferFeatures == 0,
            "a stencil format reports the attachment bit the probe proved and nothing else");
   }

   VkImageFormatProperties ip;
   VkResult result = image_format_properties(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                             VK_IMAGE_TILING_OPTIMAL,
                                             VK_IMAGE_USAGE_SAMPLED_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                             0, &ip);
   check(result == VK_SUCCESS && ip.maxExtent.width == 16384 && ip.maxExtent.height == 16384 &&
            ip.maxExtent.depth == 1 && ip.maxMipLevels == 13 && ip.maxArrayLayers == 256 &&
            ip.sampleCounts == (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT |
                                VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT) &&
            ip.maxResourceSize >= UINT64_C(1) << 31,
         "a sampled 2D RGBA8 image: 16384 extent, 13 levels, 256 layers, 1, 2, 4 and 8 samples");
   result = image_format_properties(VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D,
                                    VK_IMAGE_TILING_OPTIMAL,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &ip);
   check(result == VK_SUCCESS && ip.sampleCounts == (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT |
                                                     VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT),
         "a D32 depth attachment is supported with 1, 2, 4 and 8 samples");
   /* The specification's Format Feature Dependent Image Usage Flags table (the
    * copy in .deps/native/vulkan-docs) requires VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
    * of any format carrying COLOR_ATTACHMENT or DEPTH_STENCIL_ATTACHMENT, so the
    * query has to answer for the combination an offscreen colour buffer names
    * (R5 of the port's requests). */
   result = image_format_properties(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                    VK_IMAGE_TILING_OPTIMAL,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                    0, &ip);
   check(result == VK_SUCCESS,
         "an RGBA8 colour buffer may also name the input-attachment usage");

   const struct {
      VkFormat format;
      VkImageType type;
      VkImageTiling tiling;
      VkImageUsageFlags usage;
      VkImageCreateFlags flags;
      const char *what;
   } refused[] = {
      {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR,
       VK_IMAGE_USAGE_SAMPLED_BIT, 0, "linear tiling"},
      {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
       VK_IMAGE_USAGE_SAMPLED_BIT, 0, "a 3D image"},
      /* R8G8B8A8_UNORM reports the storage image bit now (round 7), so the
       * storage usage a format without it asks for is the refusal: R16_UNORM is
       * sampled and transferred but no storage image. */
      {VK_FORMAT_R16_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
       VK_IMAGE_USAGE_STORAGE_BIT, 0, "storage usage on a format without the bit"},
      /* Mutable format is supported for same-size views (PPSSPP's presentation
       * images); a sparse image is still refused. */
      {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
       VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_CREATE_SPARSE_BINDING_BIT, "a create flag"},
      /* The stencil formats are attachments and nothing else: the sampled usage
       * they do not claim is the refusal (round 12). */
      {VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
       VK_IMAGE_USAGE_SAMPLED_BIT, 0, "a stencil format asked for a fetch"},
   };
   bool all_refused = true;
   for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
      result = image_format_properties(refused[i].format, refused[i].type, refused[i].tiling,
                                       refused[i].usage, refused[i].flags, &ip);
      const bool ok = result == VK_ERROR_FORMAT_NOT_SUPPORTED && all_zero(&ip);
      if (!ok)
         printf("  (not refused with zeros: %s)\n", refused[i].what);
      all_refused = all_refused && ok;
   }
   check(all_refused, "linear, 3D, storage, create flags and unproven formats are refused with "
                      "zeroed properties");

   uint32_t count = 7;
   VK_FUNCTION(g_instance, GetPhysicalDeviceSparseImageFormatProperties)(
      g_physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, NULL);
   check(count == 0, "no sparse image format properties");
}

static VkResult
create_image(VkFormat format, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers,
             VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImage *image)
{
   const VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {width, height, 1},
      .mipLevels = levels,
      .arrayLayers = layers,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   *image = VK_NULL_HANDLE;
   return VK_FUNCTION(g_instance, CreateImage)(g_device, &info, NULL, image);
}

static void
destroy_image(VkImage image)
{
   VK_FUNCTION(g_instance, DestroyImage)(g_device, image, NULL);
}

static VkMemoryRequirements
requirements(VkImage image)
{
   VkMemoryRequirements r;
   memset(&r, 0, sizeof(r));
   VK_FUNCTION(g_instance, GetImageMemoryRequirements)(g_device, image, &r);
   return r;
}

/* Creates an image and checks its requirements; the image is destroyed. */
static bool
expect_storage(VkFormat format, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers,
               VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkDeviceSize size,
               VkDeviceSize alignment)
{
   VkImage image;
   if (create_image(format, width, height, levels, layers, samples, usage, &image) != VK_SUCCESS)
      return false;
   const VkMemoryRequirements r = requirements(image);
   destroy_image(image);
   if (r.size != size || r.alignment != alignment || r.memoryTypeBits != 3) {
      printf("  (size %llu, alignment %llu, types 0x%x)\n", (unsigned long long)r.size,
             (unsigned long long)r.alignment, r.memoryTypeBits);
      return false;
   }
   return true;
}

static void
check_storage(void)
{
   /* The runner's M3 texture: 64x36 RGBA8 rows of 256 bytes. */
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 64, 36, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 64 * 4 * 36,
                        256),
         "a 64x36 sampled RGBA8 image needs 9,216 bytes at 256-byte alignment (M3 texture)");
   /* Levels 64x36, 32x18, 16x9, 8x5, 4x3, 2x2, 1x1: every row pads to 256. */
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 64, 36, 7, 1, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_SAMPLED_BIT, 256 * (36 + 18 + 9 + 5 + 3 + 2 + 1), 256),
         "its complete mip chain adds rows of 256 bytes per level");
   /* Rows pad to 256: 100 texels of 4 bytes are 400 bytes, stored in 512. */
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 100, 10, 1, 6, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_SAMPLED_BIT, 512 * 10 * 6, 256),
         "rows of 400 bytes pad to 512, for each of 6 layers");
   /* The runner's 3840x2160 colour target and D32 depth buffer: 30x17 tiles of
    * 64 KiB, 33,423,360 bytes, in its 0x2000000-byte allocations. */
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 3840, 2160, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        0x2000000, 2 * MIB),
         "a 3840x2160 RGBA8 colour attachment needs the runner's 32 MiB at 2 MiB alignment");
   check(expect_storage(VK_FORMAT_D32_SFLOAT, 3840, 2160, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0x2000000, 2 * MIB),
         "a 3840x2160 D32 depth attachment needs the runner's 32 MiB at 2 MiB alignment");
   /* The same colour buffer with the input-attachment usage the table requires of
    * it: the usage is part of the image an application may ask for, and it does
    * not change what the image costs or how it is stored (R5). */
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 3840, 2160, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_STORAGE_BIT,
                        0x2000000, 2 * MIB),
         "the same target carrying the input-attachment usage is the same 32 MiB");
   /* One tile, rounded up to 2 MiB. */
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 2 * MIB, 2 * MIB),
         "a 16x16 colour attachment rounds one 64 KiB tile up to 2 MiB");
   /* ps5-opengl's 4x colour tiles of 64x64 texels: 64 tiles for 512x512. */
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 512, 512, 1, 1, VK_SAMPLE_COUNT_4_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 4 * MIB, 2 * MIB),
         "a 4x 512x512 colour attachment uses 64x64-texel tiles: 64 tiles in 4 MiB");
   /* R78: AddrLib's block rule at two and eight samples, 128x64 and 64x32
    * texels a 64 KiB tile. */
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 512, 512, 1, 1, VK_SAMPLE_COUNT_2_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 2 * MIB, 2 * MIB),
         "a 2x 512x512 colour attachment uses 128x64-texel tiles: 32 tiles in 2 MiB");
   check(expect_storage(VK_FORMAT_R8G8B8A8_UNORM, 512, 512, 1, 1, VK_SAMPLE_COUNT_8_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 8 * MIB, 2 * MIB),
         "an 8x 512x512 colour attachment uses 64x32-texel tiles: 128 tiles in 8 MiB");

   VkImage huge;
   const VkResult result =
      create_image(VK_FORMAT_R8G8B8A8_UNORM, 4096, 4096, 1, 256, VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_USAGE_SAMPLED_BIT, &huge);
   check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY,
         "a 16 GiB image (4096x4096, 256 layers) is refused");
   if (result == VK_SUCCESS)
      destroy_image(huge);

   /* A usage the format's entry does not claim is refused by name. This call
    * used to assert instead, and the abort's stale backtrace read as a compiler
    * fault for three rounds (docs/HARDWARE_FINDINGS.md, "An unsupported image is
    * refused, not asserted"): D16_UNORM is a depth attachment and a transfer
    * source, so a *colour* attachment of it must be refused. */
   VkImage unsupported;
   const VkResult unsupported_result =
      create_image(VK_FORMAT_D16_UNORM, 3840, 2160, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &unsupported);
   check(unsupported_result == VK_ERROR_FORMAT_NOT_SUPPORTED,
         "a colour attachment whose format has no COLOR_ATTACHMENT bit is refused by name");
   if (unsupported_result == VK_SUCCESS)
      destroy_image(unsupported);
}

static void
check_binding(void)
{
   VkImage first;
   VkImage second;
   VkImage target;
   if (create_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 36, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_SAMPLED_BIT, &first) != VK_SUCCESS ||
       create_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 36, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_SAMPLED_BIT, &second) != VK_SUCCESS ||
       create_image(VK_FORMAT_R8G8B8A8_UNORM, 256, 256, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &target) != VK_SUCCESS) {
      check(false, "vkCreateImage");
      return;
   }
   const VkMemoryRequirements r = requirements(first);
   const VkMemoryRequirements t = requirements(target);
   const VkDeviceSize second_offset = (r.size + r.alignment - 1) / r.alignment * r.alignment;
   const VkMemoryAllocateInfo textures_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = second_offset + r.size,
   };
   const VkMemoryAllocateInfo target_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = t.size,
   };
   VkDeviceMemory textures = VK_NULL_HANDLE;
   VkDeviceMemory target_memory = VK_NULL_HANDLE;
   const __typeof__(&vkBindImageMemory) bind = VK_FUNCTION(g_instance, BindImageMemory);
   check(VK_FUNCTION(g_instance, AllocateMemory)(g_device, &textures_info, NULL, &textures) ==
               VK_SUCCESS &&
            bind(g_device, first, textures, 0) == VK_SUCCESS &&
            bind(g_device, second, textures, second_offset) == VK_SUCCESS,
         "two sampled images bind next to each other in one allocation");
   check(VK_FUNCTION(g_instance, AllocateMemory)(g_device, &target_info, NULL, &target_memory) ==
               VK_SUCCESS &&
            bind(g_device, target, target_memory, 0) == VK_SUCCESS,
         "a colour attachment binds to its own 2 MiB allocation");

   uint32_t count = 5;
   VK_FUNCTION(g_instance, GetImageSparseMemoryRequirements)(g_device, first, &count, NULL);
   check(count == 0, "no sparse memory requirements");

   destroy_image(target);
   destroy_image(second);
   destroy_image(first);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, target_memory, NULL);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, textures, NULL);
}

int
main(void)
{
   test_begin("B3 image");
   if (test_create_device(&g_instance, &g_physical, &g_device)) {
      check_formats();
      check_storage();
      check_binding();
   }
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
