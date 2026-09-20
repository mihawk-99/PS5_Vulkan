/*
 * PS5 Vulkan driver - display, surfaces, swapchains and presentation.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C1 (docs/M5_PHASE_C.md).
 *
 * The console has one display, VideoOut, exposed through VK_KHR_display, the
 * standard extension for a screen without a windowing system: one display,
 * one 3840x2160 mode at VideoOut's flip rate 0 (60 Hz), and one plane. Mesa's
 * VK_KHR_display drives Linux DRM, so these objects are the driver's own;
 * surfaces use the loader's VkIcdSurfaceDisplay layout.
 *
 * A swapchain owns VideoOut and its two framebuffers, opened and registered
 * as the test runner does (M2): two 32 MiB tiled B8G8R8A8 images in one
 * 64 MiB direct-memory allocation. Presentation follows run pid 134
 * (c1-present): a submission that renders into a swapchain image starts with
 * that image's wait packet (ps5vk_queue.c), and vkQueuePresentKHR submits the
 * image's flip in a stream of its own and waits for VideoOut's flip status.
 * FIFO is the only present mode.
 *
 * Images alternate, and the image acquired next is never the one on screen.
 * An application holds one image at a time, since the other stays on screen
 * until the held one is presented. minImageCount is therefore 1, so an
 * application holding an image may not wait on another
 * (VUID-vkAcquireNextImageKHR-swapchain-01802); such an acquire returns
 * VK_NOT_READY or VK_TIMEOUT.
 *
 * A swapchain created with oldSwapchain takes VideoOut over; the old one is
 * retired, and acquiring or presenting its images returns
 * VK_ERROR_OUT_OF_DATE_KHR. A second swapchain without oldSwapchain is
 * refused with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR.
 */

#include "ps5vk_private.h"

#include <assert.h>
#include <string.h>

#include "util/log.h"
#include "vk_alloc.h"
#include "vk_fence.h"
#include "vk_semaphore.h"
#include "vk_util.h"
#include "vulkan/vk_icd.h"

#define PS5VK_DISPLAY_NAME "PS5 VideoOut"
#define PS5VK_DISPLAY_WIDTH 3840
#define PS5VK_DISPLAY_HEIGHT 2160
#define PS5VK_DISPLAY_REFRESH_MILLIHERTZ 60000
#define PS5VK_SWAPCHAIN_IMAGE_BYTES UINT64_C(0x2000000)
#define PS5VK_SWAPCHAIN_ALIGNMENT UINT64_C(0x200000)
/* sceVideoOutOpen's user, bus and index, and the SDR pixel format the runner
 * registers its framebuffers with. */
#define PS5VK_VIDEO_OUT_USER 0xff
#define PS5VK_VIDEO_OUT_PIXEL_FORMAT_SDR UINT64_C(0x8000000000000000)
#define PS5VK_VIDEO_OUT_ATTRIBUTE_BYTES 80
/* 0x80290009: busy, which ps5-opengl and ProsperoLight tolerate on unregister. */
#define PS5VK_VIDEO_OUT_BUSY 0x80290009u
#define PS5VK_FLIP_DRAIN_VBLANKS 120

static const VkExtent2D ps5vk_display_extent = {PS5VK_DISPLAY_WIDTH, PS5VK_DISPLAY_HEIGHT};

static VkDisplayKHR
ps5vk_display_handle(struct ps5vk_physical_device *device)
{
   return (VkDisplayKHR)(uintptr_t)&device->display;
}

static VkDisplayModeKHR
ps5vk_display_mode_handle(struct ps5vk_physical_device *device)
{
   return (VkDisplayModeKHR)(uintptr_t)&device->mode;
}

static VkIcdSurfaceBase *
ps5vk_surface(VkSurfaceKHR surface)
{
   return (VkIcdSurfaceBase *)(uintptr_t)surface;
}

/* --- VK_KHR_display ------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice physicalDevice,
                                            uint32_t *pPropertyCount,
                                            VkDisplayPropertiesKHR *pProperties)
{
   VK_FROM_HANDLE(ps5vk_physical_device, device, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkDisplayPropertiesKHR, out, pProperties, pPropertyCount);
   vk_outarray_append_typed(VkDisplayPropertiesKHR, &out, properties) {
      *properties = (VkDisplayPropertiesKHR){
         .display = ps5vk_display_handle(device),
         .displayName = PS5VK_DISPLAY_NAME,
         /* The size of the screen VideoOut drives is not known. */
         .physicalDimensions = {0, 0},
         .physicalResolution = ps5vk_display_extent,
         .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
         .planeReorderPossible = VK_FALSE,
         .persistentContent = VK_FALSE,
      };
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice physicalDevice,
                                                 uint32_t *pPropertyCount,
                                                 VkDisplayPlanePropertiesKHR *pProperties)
{
   VK_FROM_HANDLE(ps5vk_physical_device, device, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkDisplayPlanePropertiesKHR, out, pProperties, pPropertyCount);
   vk_outarray_append_typed(VkDisplayPlanePropertiesKHR, &out, properties) {
      *properties = (VkDisplayPlanePropertiesKHR){
         .currentDisplay = ps5vk_display_handle(device),
         .currentStackIndex = 0,
      };
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice physicalDevice, uint32_t planeIndex,
                                          uint32_t *pDisplayCount, VkDisplayKHR *pDisplays)
{
   VK_FROM_HANDLE(ps5vk_physical_device, device, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkDisplayKHR, out, pDisplays, pDisplayCount);
   if (planeIndex == 0) {
      vk_outarray_append_typed(VkDisplayKHR, &out, display)
         *display = ps5vk_display_handle(device);
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetDisplayModePropertiesKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display,
                                  uint32_t *pPropertyCount,
                                  VkDisplayModePropertiesKHR *pProperties)
{
   VK_FROM_HANDLE(ps5vk_physical_device, device, physicalDevice);
   /* Valid usage: the display belongs to this physical device. */
   assert(display == ps5vk_display_handle(device));
   (void)display;
   VK_OUTARRAY_MAKE_TYPED(VkDisplayModePropertiesKHR, out, pProperties, pPropertyCount);
   vk_outarray_append_typed(VkDisplayModePropertiesKHR, &out, properties) {
      *properties = (VkDisplayModePropertiesKHR){
         .displayMode = ps5vk_display_mode_handle(device),
         .parameters = {
            .visibleRegion = ps5vk_display_extent,
            .refreshRate = PS5VK_DISPLAY_REFRESH_MILLIHERTZ,
         },
      };
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateDisplayModeKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display,
                           const VkDisplayModeCreateInfoKHR *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator, VkDisplayModeKHR *pMode)
{
   VK_FROM_HANDLE(ps5vk_physical_device, device, physicalDevice);
   (void)display;
   (void)pCreateInfo;
   (void)pAllocator;
   (void)pMode;
   return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                    "VideoOut has one mode, 3840x2160 at 60 Hz, and no others");
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkDisplayModeKHR mode,
                                     uint32_t planeIndex,
                                     VkDisplayPlaneCapabilitiesKHR *pCapabilities)
{
   (void)physicalDevice;
   (void)mode;
   (void)planeIndex;
   *pCapabilities = (VkDisplayPlaneCapabilitiesKHR){
      .supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .minSrcPosition = {0, 0},
      .maxSrcPosition = {0, 0},
      .minSrcExtent = ps5vk_display_extent,
      .maxSrcExtent = ps5vk_display_extent,
      .minDstPosition = {0, 0},
      .maxDstPosition = {0, 0},
      .minDstExtent = ps5vk_display_extent,
      .maxDstExtent = ps5vk_display_extent,
   };
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateDisplayPlaneSurfaceKHR(VkInstance _instance,
                                   const VkDisplaySurfaceCreateInfoKHR *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(ps5vk_instance, instance, _instance);
   /* Valid usage: the plane capabilities allow only the full display. */
   assert(pCreateInfo->planeIndex == 0 &&
          pCreateInfo->imageExtent.width == PS5VK_DISPLAY_WIDTH &&
          pCreateInfo->imageExtent.height == PS5VK_DISPLAY_HEIGHT);
   VkIcdSurfaceDisplay *const surface = vk_alloc2(&instance->vk.alloc, pAllocator,
                                                  sizeof(*surface), 8,
                                                  VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!surface)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   *surface = (VkIcdSurfaceDisplay){
      .base = {.platform = VK_ICD_WSI_PLATFORM_DISPLAY},
      .displayMode = pCreateInfo->displayMode,
      .planeIndex = pCreateInfo->planeIndex,
      .planeStackIndex = pCreateInfo->planeStackIndex,
      .transform = pCreateInfo->transform,
      .globalAlpha = pCreateInfo->globalAlpha,
      .alphaMode = pCreateInfo->alphaMode,
      .imageExtent = pCreateInfo->imageExtent,
   };
   *pSurface = (VkSurfaceKHR)(uintptr_t)surface;
   return VK_SUCCESS;
}

/* --- VK_KHR_surface ------------------------------------------------------ */

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroySurfaceKHR(VkInstance _instance, VkSurfaceKHR surface,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_instance, instance, _instance);
   vk_free2(&instance->vk.alloc, pAllocator, ps5vk_surface(surface));
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                         uint32_t queueFamilyIndex, VkSurfaceKHR surface,
                                         VkBool32 *pSupported)
{
   (void)physicalDevice;
   *pSupported = queueFamilyIndex == 0 &&
                 ps5vk_surface(surface)->platform == VK_ICD_WSI_PLATFORM_DISPLAY;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                              VkSurfaceKHR surface,
                                              VkSurfaceCapabilitiesKHR *pCapabilities)
{
   (void)physicalDevice;
   (void)surface;
   *pCapabilities = (VkSurfaceCapabilitiesKHR){
      .minImageCount = 1,
      .maxImageCount = PS5VK_SWAPCHAIN_IMAGES,
      .currentExtent = ps5vk_display_extent,
      .minImageExtent = ps5vk_display_extent,
      .maxImageExtent = ps5vk_display_extent,
      .maxImageArrayLayers = 1,
      .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      /* Rendering is the only use swapchain images have been proven for. */
      .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
   };
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                         uint32_t *pSurfaceFormatCount,
                                         VkSurfaceFormatKHR *pSurfaceFormats)
{
   (void)physicalDevice;
   (void)surface;
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   vk_outarray_append_typed(VkSurfaceFormatKHR, &out, format) {
      *format = (VkSurfaceFormatKHR){
         .format = VK_FORMAT_B8G8R8A8_UNORM,
         .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      };
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
                                              VkSurfaceKHR surface, uint32_t *pPresentModeCount,
                                              VkPresentModeKHR *pPresentModes)
{
   (void)physicalDevice;
   (void)surface;
   VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, out, pPresentModes, pPresentModeCount);
   vk_outarray_append_typed(VkPresentModeKHR, &out, mode)
      *mode = VK_PRESENT_MODE_FIFO_KHR;
   return vk_outarray_status(&out);
}

/* --- VideoOut -------------------------------------------------------------- */

/* The framebuffer descriptors sceVideoOutRegisterBuffers2 takes. */
struct ps5vk_video_out_buffer {
   void *data;
   void *metadata;
   void *reserved0;
   void *reserved1;
};

static void
ps5vk_video_out_close(struct ps5vk_video_out *video)
{
   if (video->handle >= 0) {
      /* Pending flips drain before the buffers are unregistered, as
       * ProsperoLight does. */
      for (unsigned wait = 0; wait < PS5VK_FLIP_DRAIN_VBLANKS &&
                              sceVideoOutIsFlipPending(video->handle) > 0;
           wait++)
         sceVideoOutWaitVblank(video->handle);
   }
   if (video->registered) {
      const int result = sceVideoOutUnregisterBuffers(video->handle, 0);
      if (result != 0 && (uint32_t)result != PS5VK_VIDEO_OUT_BUSY)
         mesa_loge("sceVideoOutUnregisterBuffers failed: 0x%08x", (unsigned)result);
   }
   if (video->handle >= 0)
      sceVideoOutClose(video->handle);
   ps5vk_direct_mapping_destroy(&video->buffers);
   video->handle = -1;
   video->registered = false;
}

/* Opens VideoOut at flip rate 0, maps two cleared framebuffers and registers
 * them as SDR buffers, as the test runner does (open_video_target). */
static VkResult
ps5vk_video_out_open(struct ps5vk_device *device, struct ps5vk_video_out *video)
{
   *video = (struct ps5vk_video_out){.handle = -1, .buffers = {.start = -1}, .shown = UINT32_MAX};
   video->handle = sceVideoOutOpen(PS5VK_VIDEO_OUT_USER, 0, 0, NULL);
   if (video->handle < 0)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED, "sceVideoOutOpen failed: 0x%08x",
                       (unsigned)video->handle);
   int result = sceVideoOutSetFlipRate(video->handle, 0);
   if (result != 0) {
      ps5vk_video_out_close(video);
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "sceVideoOutSetFlipRate failed: 0x%08x", (unsigned)result);
   }
   const size_t bytes = (size_t)(PS5VK_SWAPCHAIN_IMAGES * PS5VK_SWAPCHAIN_IMAGE_BYTES);
   result = ps5vk_direct_mapping_create(&video->buffers, bytes, PS5VK_SWAPCHAIN_ALIGNMENT);
   if (result != 0) {
      ps5vk_video_out_close(video);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "the framebuffers could not be mapped in the address window: 0x%08x",
                       (unsigned)result);
   }
   memset(video->buffers.address, 0, bytes);
   ps5vk_flush_cpu_cache(video->buffers.address, bytes);

   struct ps5vk_video_out_buffer buffers[PS5VK_SWAPCHAIN_IMAGES];
   for (uint32_t index = 0; index < PS5VK_SWAPCHAIN_IMAGES; index++)
      buffers[index] = (struct ps5vk_video_out_buffer){
         .data = (uint8_t *)video->buffers.address + index * PS5VK_SWAPCHAIN_IMAGE_BYTES,
      };
   uint8_t attribute[PS5VK_VIDEO_OUT_ATTRIBUTE_BYTES] = {0};
   sceVideoOutSetBufferAttribute2(attribute, PS5VK_VIDEO_OUT_PIXEL_FORMAT_SDR, 0,
                                  PS5VK_DISPLAY_WIDTH, PS5VK_DISPLAY_HEIGHT, 0, 0, 0);
   result = sceVideoOutRegisterBuffers2(video->handle, 0, 0, buffers, PS5VK_SWAPCHAIN_IMAGES,
                                        attribute, 0, NULL);
   if (result != 0) {
      ps5vk_video_out_close(video);
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "sceVideoOutRegisterBuffers2 failed: 0x%08x", (unsigned)result);
   }
   video->registered = true;
   return VK_SUCCESS;
}

/* --- VK_KHR_swapchain ------------------------------------------------------ */

static VkResult
ps5vk_swapchain_create_images(struct ps5vk_device *device, struct ps5vk_swapchain *swapchain,
                              const VkSwapchainCreateInfoKHR *info,
                              const struct ps5vk_video_out *video)
{
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = info->imageFormat,
      .extent = {PS5VK_DISPLAY_WIDTH, PS5VK_DISPLAY_HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = info->imageUsage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   for (uint32_t index = 0; index < PS5VK_SWAPCHAIN_IMAGES; index++) {
      struct ps5vk_image *const image =
         vk_image_create(&device->vk, &image_info, NULL, sizeof(*image));
      if (!image)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      image->storage = PS5VK_IMAGE_STORAGE_TILES;
      image->size = PS5VK_SWAPCHAIN_IMAGE_BYTES;
      image->alignment = PS5VK_SWAPCHAIN_ALIGNMENT;
      image->address =
         (uint64_t)(uintptr_t)video->buffers.address + index * PS5VK_SWAPCHAIN_IMAGE_BYTES;
      image->video = video->handle;
      image->buffer_index = index;
      swapchain->images[index] = image;
   }
   return VK_SUCCESS;
}

static void
ps5vk_swapchain_free(struct ps5vk_device *device, struct ps5vk_swapchain *swapchain,
                     const VkAllocationCallbacks *allocator)
{
   for (uint32_t index = 0; index < PS5VK_SWAPCHAIN_IMAGES; index++) {
      if (swapchain->images[index])
         vk_image_destroy(&device->vk, NULL, &swapchain->images[index]->vk);
   }
   if (swapchain->video) {
      ps5vk_video_out_close(swapchain->video);
      vk_free(&device->vk.alloc, swapchain->video);
      device->video_out = NULL;
   }
   vk_object_free(&device->vk, allocator, swapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateSwapchainKHR(VkDevice _device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_swapchain, old, pCreateInfo->oldSwapchain);
   const VkSwapchainCreateInfoKHR *const info = pCreateInfo;
   /* Valid usage: the surface's capabilities, formats and present modes allow
    * these values, and oldSwapchain is not retired. */
   assert(info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM &&
          info->imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
          info->imageExtent.width == PS5VK_DISPLAY_WIDTH &&
          info->imageExtent.height == PS5VK_DISPLAY_HEIGHT && info->imageArrayLayers == 1 &&
          (info->imageUsage & ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 &&
          info->presentMode == VK_PRESENT_MODE_FIFO_KHR &&
          info->minImageCount <= PS5VK_SWAPCHAIN_IMAGES);
   assert(!old || (!old->retired && old->video));
   if (!old && device->video_out)
      return vk_errorf(device, VK_ERROR_NATIVE_WINDOW_IN_USE_KHR,
                       "VideoOut belongs to another swapchain");

   struct ps5vk_swapchain *const swapchain =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*swapchain), VK_OBJECT_TYPE_SWAPCHAIN_KHR);
   if (!swapchain)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   swapchain->acquired = UINT32_MAX;

   struct ps5vk_video_out *video = old ? old->video : NULL;
   if (!video) {
      video = vk_zalloc(&device->vk.alloc, sizeof(*video), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!video) {
         vk_object_free(&device->vk, pAllocator, swapchain);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      const VkResult opened = ps5vk_video_out_open(device, video);
      if (opened != VK_SUCCESS) {
         vk_free(&device->vk.alloc, video);
         vk_object_free(&device->vk, pAllocator, swapchain);
         return opened;
      }
      swapchain->video = video;
      device->video_out = video;
   }
   const VkResult result = ps5vk_swapchain_create_images(device, swapchain, info, video);
   if (result != VK_SUCCESS) {
      ps5vk_swapchain_free(device, swapchain, pAllocator);
      return result;
   }
   if (old) {
      /* The new swapchain takes VideoOut over; the old one's images keep
       * their storage until it is destroyed, but can no longer be presented. */
      swapchain->video = video;
      old->video = NULL;
      old->retired = true;
   }

   *pSwapchain = ps5vk_swapchain_to_handle(swapchain);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroySwapchainKHR(VkDevice _device, VkSwapchainKHR _swapchain,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_swapchain, swapchain, _swapchain);
   if (swapchain)
      ps5vk_swapchain_free(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetSwapchainImagesKHR(VkDevice _device, VkSwapchainKHR _swapchain,
                            uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
   (void)_device;
   VK_FROM_HANDLE(ps5vk_swapchain, swapchain, _swapchain);
   VK_OUTARRAY_MAKE_TYPED(VkImage, out, pSwapchainImages, pSwapchainImageCount);
   for (uint32_t index = 0; index < PS5VK_SWAPCHAIN_IMAGES; index++) {
      vk_outarray_append_typed(VkImage, &out, image)
         *image = ps5vk_image_to_handle(swapchain->images[index]);
   }
   return vk_outarray_status(&out);
}

static VkResult
ps5vk_swapchain_acquire(struct ps5vk_device *device, struct ps5vk_swapchain *swapchain,
                        uint64_t timeout, VkSemaphore _semaphore, VkFence _fence,
                        uint32_t *pImageIndex)
{
   VK_FROM_HANDLE(vk_semaphore, semaphore, _semaphore);
   VK_FROM_HANDLE(vk_fence, fence, _fence);
   if (swapchain->retired || !swapchain->video)
      return VK_ERROR_OUT_OF_DATE_KHR;
   if (swapchain->acquired != UINT32_MAX)
      return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
   const uint32_t shown = swapchain->video->shown;
   const uint32_t index = shown == UINT32_MAX ? 0 : (shown + 1) % PS5VK_SWAPCHAIN_IMAGES;
   /* The image is ready at once: the fence and semaphore signal now. */
   if (semaphore) {
      const VkResult result =
         vk_sync_signal(&device->vk, vk_semaphore_get_active_sync(semaphore), 0);
      if (result != VK_SUCCESS)
         return result;
   }
   if (fence) {
      const VkResult result = vk_sync_signal(&device->vk, vk_fence_get_active_sync(fence), 0);
      if (result != VK_SUCCESS)
         return result;
   }
   swapchain->acquired = index;
   *pImageIndex = index;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_AcquireNextImageKHR(VkDevice _device, VkSwapchainKHR _swapchain, uint64_t timeout,
                          VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_swapchain, swapchain, _swapchain);
   return ps5vk_swapchain_acquire(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_AcquireNextImage2KHR(VkDevice _device, const VkAcquireNextImageInfoKHR *pAcquireInfo,
                           uint32_t *pImageIndex)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_swapchain, swapchain, pAcquireInfo->swapchain);
   return ps5vk_swapchain_acquire(device, swapchain, pAcquireInfo->timeout,
                                  pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
}

/* The more severe of two presentation results. */
static VkResult
ps5vk_present_result(VkResult current, VkResult next)
{
   if (current == VK_ERROR_DEVICE_LOST || next == VK_SUCCESS)
      return current;
   if (next < 0 || current == VK_SUCCESS)
      return next;
   return current;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_QueuePresentKHR(VkQueue _queue, const VkPresentInfoKHR *pPresentInfo)
{
   VK_FROM_HANDLE(ps5vk_queue, queue, _queue);
   struct ps5vk_device *const device =
      container_of(queue->vk.base.device, struct ps5vk_device, vk);

   /* Submission is synchronous, so the semaphores a present waits on were
    * signalled when their submissions returned; waiting confirms it, and the
    * present consumes them. */
   for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
      VK_FROM_HANDLE(vk_semaphore, semaphore, pPresentInfo->pWaitSemaphores[i]);
      struct vk_sync *const sync = vk_semaphore_get_active_sync(semaphore);
      VkResult result = vk_sync_wait(&device->vk, sync, 0, VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (result == VK_SUCCESS)
         result = vk_sync_reset(&device->vk, sync);
      if (result != VK_SUCCESS)
         return result;
   }

   VkResult overall = VK_SUCCESS;
   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      VK_FROM_HANDLE(ps5vk_swapchain, swapchain, pPresentInfo->pSwapchains[i]);
      const uint32_t index = pPresentInfo->pImageIndices[i];
      VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
      if (!swapchain->retired && swapchain->video) {
         /* Valid usage: the image was acquired and not yet presented. */
         assert(swapchain->acquired == index);
         struct ps5vk_video_out *const video = swapchain->video;
         result = ps5vk_queue_flip(queue, video->handle, index, ++video->flip_marker);
         if (result == VK_SUCCESS)
            video->shown = index;
      }
      swapchain->acquired = UINT32_MAX;
      if (pPresentInfo->pResults)
         pPresentInfo->pResults[i] = result;
      overall = ps5vk_present_result(overall, result);
   }
   return overall;
}

int
ps5vk_debug_video_handle(VkDevice _device)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   return device != NULL && device->video_out != NULL ? device->video_out->handle : -1;
}
