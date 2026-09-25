/*
 * PS5 Vulkan driver - the physical device.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B2 (docs/M5_PHASE_B.md). One physical device: the
 * console's AMD GPU, driven through AGC.
 *
 * Features: every optional Vulkan 1.0 feature is off until probes prove it.
 *
 * Limits: the Vulkan specification's required values, taken from its Required
 * Limits table (Vulkan-Docs v1.4.354, chapters/limits.adoc, SHA-256
 * 0749aab1b5ea35913d2026d94a35d51ade24bb8bf0a08527034a9457b60e7043, lines
 * 6760-6971 and footnotes 2 and 8 at 7475 and 7516). Limits of unsupported
 * features take the table's "Unsupported Limit" column; the others its
 * {core} "Supported Limit". Hardware has not yet proven every value; probes
 * raise them where it has. Rendering has reached 3840x2160 targets (M2-M4),
 * within the 4096 image and framebuffer minimums.
 */

#include "ps5vk_private.h"

#include <string.h>

#include "vk_alloc.h"
#include "vk_util.h"

/* AMD's PCI vendor ID; the GPU is a GFX10.3 (RDNA2) part, whose register
 * layout (Mesa's gfx103) the probes program. Its PCI device ID is not known
 * to this driver, so deviceID stays 0. */
#define PS5VK_VENDOR_ID 0x1002
#define PS5VK_DEVICE_NAME "PS5 AGC GPU (ps5vk)"
/* No pipeline cache exists yet; the UUID changes when one does (Phase B6). */
#define PS5VK_PIPELINE_CACHE_UUID "ps5vk-no-cache-1"

/* Vertex, fragment and compute; footnote 8 multiplies the per-stage
 * descriptor minimums by this count for the per-set limits. */
#define PS5VK_SHADER_STAGES 3

/* Fences and semaphores: the CPU-signalled binary sync of ps5vk_sync.c. With
 * no timeline sync type Mesa submits immediately (ps5vk_queue.c). */
static const struct vk_sync_type *const ps5vk_sync_types[] = {&ps5vk_sync_type, NULL};

/* The features this device supports. Vulkan 1.0 requires exactly one of them
 * (docs/M5_REFERENCE.md, "1.0: make the current claim true"): robustBufferAccess,
 * which V0-robust proved on the console -- a draw asking for twice the indices
 * its index buffer holds draws the same square, because the driver clamps the
 * count to what the binding covers (driver/ps5vk_draw.c; console run pid 140,
 * golden/v0-robust). Everything else stays off, and the device's own
 * assertions in driver/tests/vk_b2_device_test.c hold both halves of that. */
static const struct vk_features ps5vk_features = {
   .robustBufferAccess = true,
   /* Anisotropic filtering up to 16x: the sampler's MAX_ANISO_RATIO,
    * ANISO_THRESHOLD and ANISO_BIAS and the anisotropic XY filters, as RADV
    * encodes them (ps5vk_image.c, ps5vk_CreateSampler). */
   .samplerAnisotropy = true,
   /* R71: dual-source blending. A pixel shader that writes a second colour
    * (location 0, index 1) exports it to MRT1 with MRT0's format, and the
    * compiler's SPI_SHADER_COL_FORMAT and CB_SHADER_MASK carry both
    * (psbc_compile.c, RADV's mrt0_is_dual_src); the SRC1 blend factors read it
    * (ps5vk_pipeline.c). Dolphin needs it for the destination-alpha and
    * blending modes its fallback cannot reproduce: without it Resident Evil 4's
    * haze covered the whole frame, on the console and in desktop Dolphin with
    * the feature switched off alike. */
   .dualSrcBlend = true,
};

/* Presentation to VideoOut (ps5vk_wsi.c, Phase C1). */
static const struct vk_device_extension_table ps5vk_device_extensions = {
   .KHR_swapchain = true,
};

static void
ps5vk_get_properties(struct vk_properties *p)
{
   *p = (struct vk_properties){
      .apiVersion = PS5VK_DEVICE_API_VERSION,
      .driverVersion = PS5VK_DRIVER_VERSION,
      .vendorID = PS5VK_VENDOR_ID,
      .deviceID = 0,
      /* The GPU shares memory with the CPU: the probes read frames back from
       * the direct memory the GPU drew into. */
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,

      /* 16384, the GFX10 texture and colour/depth surface limit RADV reports:
       * PPSSPP's 10x internal resolution renders 4800x2720 targets. */
      .maxImageDimension1D = 16384,
      .maxImageDimension2D = 16384,
      .maxImageDimension3D = 256,
      .maxImageDimensionCube = 16384,
      .maxImageArrayLayers = 256,
      /* R72: a texel buffer's descriptor holds its element count in the 32-bit
       * NUM_RECORDS word (range / texel bytes, ps5vk_draw.c), as RADV's does,
       * and RADV reports UINT32_MAX. The 1.0 minimum this said before, 65536,
       * sized Dolphin's texel stream buffer at 64 KiB (it takes the smaller of
       * 16 MiB and this), and GPU texture decoding then failed to fit a 64 KiB
       * texture in it. */
      .maxTexelBufferElements = UINT32_MAX,
      .maxUniformBufferRange = 16384,
      .maxStorageBufferRange = UINT32_C(1) << 27,
      .maxPushConstantsSize = 128,
      .maxMemoryAllocationCount = PS5VK_MAX_MEMORY_ALLOCATIONS,
      .maxSamplerAllocationCount = 4000,
      .bufferImageGranularity = 131072,
      .sparseAddressSpaceSize = 0, /* no sparse binding */
      .maxBoundDescriptorSets = 4,
      .maxPerStageDescriptorSamplers = 16,
      .maxPerStageDescriptorUniformBuffers = 12,
      .maxPerStageDescriptorStorageBuffers = 4,
      .maxPerStageDescriptorSampledImages = 16,
      .maxPerStageDescriptorStorageImages = 4,
      .maxPerStageDescriptorInputAttachments = 4,
      /* Footnote 2: the smaller of 128 and the sum of the uniform-buffer,
       * storage-buffer, sampled-image, storage-image and input-attachment
       * per-stage limits and maxColorAttachments: 12 + 4 + 16 + 4 + 4 + 4. */
      .maxPerStageResources = 44,
      .maxDescriptorSetSamplers = PS5VK_SHADER_STAGES * 16,
      .maxDescriptorSetUniformBuffers = PS5VK_SHADER_STAGES * 12,
      .maxDescriptorSetUniformBuffersDynamic = PS5VK_DYNAMIC_UNIFORM_COUNT,
      .maxDescriptorSetStorageBuffers = PS5VK_SHADER_STAGES * 4,
      .maxDescriptorSetStorageBuffersDynamic = 4,
      .maxDescriptorSetSampledImages = PS5VK_SHADER_STAGES * 16,
      .maxDescriptorSetStorageImages = PS5VK_SHADER_STAGES * 4,
      .maxDescriptorSetInputAttachments = 4,
      .maxVertexInputAttributes = 16,
      .maxVertexInputBindings = 16,
      .maxVertexInputAttributeOffset = 2047,
      .maxVertexInputBindingStride = 2048,
      .maxVertexOutputComponents = 64,
      /* No tessellation or geometry shaders: all their limits are 0. */
      .maxFragmentInputComponents = 64,
      .maxFragmentOutputAttachments = 4,
      .maxFragmentDualSrcAttachments = 1, /* dualSrcBlend: MRT0's second source (R71) */
      .maxFragmentCombinedOutputResources = 4,
      .maxComputeSharedMemorySize = 16384,
      .maxComputeWorkGroupCount = {65535, 65535, 65535},
      .maxComputeWorkGroupInvocations = 128,
      .maxComputeWorkGroupSize = {128, 128, 64},
      .subPixelPrecisionBits = 4,
      .subTexelPrecisionBits = 4,
      .mipmapPrecisionBits = 4,
      .maxDrawIndexedIndexValue = (UINT32_C(1) << 24) - 1, /* no fullDrawIndexUint32 */
      .maxDrawIndirectCount = 1,                           /* no multiDrawIndirect */
      .maxSamplerLodBias = 2.0f,
      .maxSamplerAnisotropy = 16.0f, /* samplerAnisotropy, 2^MAX_ANISO_RATIO 4 */
      .maxViewports = 1,            /* no multiViewport */
      .maxViewportDimensions = {16384, 16384},
      .viewportBoundsRange = {-32768.0f, 32767.0f},
      .viewportSubPixelBits = 0,
      /* Mapped direct memory is page aligned, above the required 64. */
      .minMemoryMapAlignment = 4096,
      .minTexelBufferOffsetAlignment = 256,
      .minUniformBufferOffsetAlignment = 256,
      .minStorageBufferOffsetAlignment = 256,
      .minTexelOffset = -8,
      .maxTexelOffset = 7,
      .minTexelGatherOffset = 0, /* no shaderImageGatherExtended */
      .maxTexelGatherOffset = 0,
      .minInterpolationOffset = 0.0f, /* no sampleRateShading */
      .maxInterpolationOffset = 0.0f,
      .subPixelInterpolationOffsetBits = 0,
      .maxFramebufferWidth = 16384,
      .maxFramebufferHeight = 16384,
      .maxFramebufferLayers = 256,
      /* Required for every implementation; multisampling is not yet probed
       * on the hardware (Phase C8). */
      .framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .maxColorAttachments = PS5VK_MAX_COLOR_TARGETS,
      .sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT, /* no storageImageMultisample */
      .maxSampleMaskWords = 1,
      /* Timestamps are supported on the one queue family, which has both the
       * graphics and the compute bit, and V0-query measured the clock (pid
       * 143): 99,786,056 ticks a second against CLOCK_MONOTONIC, which is the
       * 100 MHz reference clock, so 10 ns a tick. */
      .timestampComputeAndGraphics = VK_TRUE,
      .timestampPeriod = 10.0f,
      .maxClipDistances = 0, /* no shaderClipDistance */
      .maxCullDistances = 0, /* no shaderCullDistance */
      .maxCombinedClipAndCullDistances = 0,
      .discreteQueuePriorities = 2,
      .pointSizeRange = {1.0f, 1.0f}, /* no largePoints */
      .lineWidthRange = {1.0f, 1.0f}, /* no wideLines */
      .pointSizeGranularity = 0.0f,
      .lineWidthGranularity = 0.0f,
      .strictLines = VK_FALSE,
      .standardSampleLocations = VK_FALSE,
      .optimalBufferCopyOffsetAlignment = 1,
      .optimalBufferCopyRowPitchAlignment = 1,
      .nonCoherentAtomSize = 256,
   };
   STATIC_ASSERT(sizeof(PS5VK_DEVICE_NAME) <= VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
   memcpy(p->deviceName, PS5VK_DEVICE_NAME, sizeof(PS5VK_DEVICE_NAME));
   STATIC_ASSERT(sizeof(PS5VK_PIPELINE_CACHE_UUID) - 1 == VK_UUID_SIZE);
   memcpy(p->pipelineCacheUUID, PS5VK_PIPELINE_CACHE_UUID, VK_UUID_SIZE);
}

VkResult
ps5vk_physical_device_create(struct ps5vk_instance *instance,
                             struct ps5vk_physical_device **out_device)
{
   struct ps5vk_physical_device *const device =
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return vk_error(&instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &ps5vk_physical_device_entrypoints, true);
   /* VK_KHR_swapchain, no optional features, and robustBufferAccess. */
   const VkResult result = vk_physical_device_init(&device->vk, &instance->vk,
                                                   &ps5vk_device_extensions, &ps5vk_features, NULL,
                                                   &dispatch_table);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, device);
      return result;
   }
   ps5vk_get_properties(&device->vk.properties);
   device->vk.supported_sync_types = ps5vk_sync_types;

   *out_device = device;
   return VK_SUCCESS;
}

void
ps5vk_physical_device_destroy(struct vk_physical_device *vk_device)
{
   struct ps5vk_physical_device *const device =
      container_of(vk_device, struct ps5vk_physical_device, vk);
   const VkAllocationCallbacks *const alloc = &vk_device->instance->alloc;
   vk_physical_device_finish(&device->vk);
   vk_free(alloc, device);
}

/* One queue family with one queue. The specification requires a family with
 * both graphics and compute when graphics is exposed (Vulkan-Docs v1.4.354,
 * chapters/devsandqueues.adoc, SHA-256 724b7b1f..., lines 1468-1471); compute
 * submission itself is probed in Phase D2. Timestamps are supported: V0-query's
 * probe read the console's 64-bit GPU clock (pid 143), so all 64 bits of a
 * timestamp are valid. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                              uint32_t *pQueueFamilyPropertyCount,
                                              VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   (void)physicalDevice;
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pQueueFamilyProperties,
                          pQueueFamilyPropertyCount);
   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, properties) {
      properties->queueFamilyProperties = (VkQueueFamilyProperties){
         .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 1,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1},
      };
   }
}

/* One memory type on one heap: the process's direct memory, shared by CPU
 * and GPU. The specification requires a host-visible, host-coherent type and
 * a device-local one (Vulkan-Docs v1.4.354, chapters/memory.adoc, SHA-256
 * d53c53c4..., lines 675-681); this type is both, and every allocation stays
 * mapped for both processors (ps5vk_memory.c). GPU-visible memory must lie in
 * one 4 GiB address window, so the heap is at most that large. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                         VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   (void)physicalDevice;
   const int64_t direct_memory = sceKernelGetDirectMemorySize();
   const VkDeviceSize heap_size =
      direct_memory > 0 ? MIN2((VkDeviceSize)direct_memory, PS5VK_ADDRESS_WINDOW_BYTES) : 0;
   pMemoryProperties->memoryProperties = (VkPhysicalDeviceMemoryProperties){
      /* Two types on one heap. Type 0 is device-local only: the application
       * cannot map it, so the queue never has to evict it from the CPU's
       * caches around a submission (ps5vk_queue_flush_targets), which is
       * where images and other GPU-only resources belong -- allocators such
       * as VMA put them there. Type 1 is the mappable one. The spec orders a
       * type whose flags are a strict subset of another's first. The driver
       * maps both for its own CPU paths alike. */
      .memoryTypeCount = PS5VK_MEMORY_TYPE_COUNT,
      .memoryTypes = {
         [PS5VK_MEMORY_TYPE_DEVICE] = {
            .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            .heapIndex = 0,
         },
         [PS5VK_MEMORY_TYPE_HOST] = {
            .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            .heapIndex = 0,
         },
      },
      .memoryHeapCount = 1,
      .memoryHeaps = {
         {
            .size = heap_size,
            .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
         },
      },
   };
}
