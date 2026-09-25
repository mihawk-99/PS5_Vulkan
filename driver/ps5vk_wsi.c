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
 * A swapchain presents through VideoOut and its two framebuffers, opened and registered
 * as the test runner does (M2): two 32 MiB tiled B8G8R8A8 images in one
 * 64 MiB direct-memory allocation. Presentation follows run pid 134
 * (c1-present): a submission that renders into a swapchain image starts with
 * that image's wait packet (ps5vk_queue.c), and vkQueuePresentKHR submits the
 * image's flip in a stream of its own. FIFO is the only present mode.
 *
 * Three images make it a queue: a present returns once its flip is queued, and
 * VideoOut shows it at a later vblank while the application renders the next
 * frame into a third image. An acquire takes a buffer that is neither on
 * screen nor queued, and waits a vblank at a time only when none is. Waiting
 * for each flip to be shown instead left an application that presents every
 * frame twice at 120 Hz (RetroArch's swap interval of 2) one vblank, 8.3 ms,
 * for its whole frame (2026-09-24). An application holds one image at a time:
 * minImageCount is 2 of the 3, so an application holding an image may not wait
 * on another (VUID-vkAcquireNextImageKHR-swapchain-01802); such an acquire
 * returns VK_NOT_READY or VK_TIMEOUT.
 *
 * A swapchain created with oldSwapchain takes VideoOut over; the old one is
 * retired, and acquiring or presenting its images returns
 * VK_ERROR_OUT_OF_DATE_KHR. A second swapchain without oldSwapchain is
 * refused with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR.
 */

#include "ps5vk_private.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"
#include "util/os_time.h"
#include "vk_alloc.h"
#include "vk_fence.h"
#include "vk_semaphore.h"
#include "vk_util.h"
#include "vulkan/vk_icd.h"

#define PS5VK_DISPLAY_NAME "PS5 VideoOut"
#define PS5VK_DISPLAY_WIDTH 3840
#define PS5VK_DISPLAY_HEIGHT 2160
/* The refresh each mode reports: what the console measured, not a nominal rate.
 * Sixty bare vblanks averaged 16.6831 ms (59.941 Hz) in the 60 Hz output and
 * 8.3416 ms (119.881 Hz) in the high-frame-rate one (port evidence
 * m6-output-mode-120hz). */
#define PS5VK_DISPLAY_REFRESH_MILLIHERTZ 59940
#define PS5VK_DISPLAY_HIGH_REFRESH_MILLIHERTZ 119880
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
ps5vk_display_mode_handle(struct ps5vk_display *mode)
{
   return (VkDisplayModeKHR)(uintptr_t)mode;
}

static bool ps5vk_title_declares_high_frame_rate(void);

/* The output-mode selector, and the mode that restores the default output. 15
 * came from the publicly released ps5-opengl runtime and was proved on the
 * console by measurement: configuring it halves the measured vblank period to
 * 8.3416 ms, and 1 puts it back to 16.6834 ms (port evidence
 * m6-output-mode-120hz). */
#define PS5VK_VIDEO_OUT_MODE_HIGH_FRAME_RATE 15
#define PS5VK_VIDEO_OUT_MODE_RESTORE 1

/* The process's VideoOut (struct ps5vk_video_out): opened when the modes are
 * settled or a swapchain first needs it, and closed with its swapchain unless
 * the application retains it (ps5vk_display_retain). A VideoOut handle and its
 * framebuffers are the process's, not a device's, so a retained output serves
 * the next device's swapchain as well. */
static pthread_mutex_t ps5vk_output_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ps5vk_video_out *ps5vk_output;
static bool ps5vk_output_retained;
/* When the process last presented, on any output: the start of the gap a
 * handover report measures. */
static uint64_t ps5vk_output_last_present_ns;

static struct ps5vk_video_out *ps5vk_output_open_handle(bool high_frame_rate);
static void ps5vk_output_close(void);

/* Which modes the display offers, settled once per physical device: the
 * 59.94 Hz mode always, and the 119.88 Hz mode first when the title's own
 * metadata declares high-frame-rate support -- without it the console refuses
 * the mode (0x80290016), which is what was measured before the port declared
 * it -- VideoOut reports the output supported, and VideoOut accepts it. The
 * output is configured here rather than at swapchain creation so that the
 * refresh a mode reports is the one the panel runs at: an application that
 * sizes its timing from the mode (RetroArch's video_refresh_rate) would
 * otherwise believe 119.88 Hz where the console refused it. The switch happens
 * once, before the first frame, and the swapchain presents through the same
 * handle (ps5vk_video_out_open). */
static void
ps5vk_display_settle_modes(struct ps5vk_physical_device *device)
{
   if (device->modes_settled)
      return;
   device->modes_settled = true;
   device->mode = (struct ps5vk_display){.refresh_millihertz = PS5VK_DISPLAY_REFRESH_MILLIHERTZ};
   device->high_mode = (struct ps5vk_display){
      .refresh_millihertz = PS5VK_DISPLAY_HIGH_REFRESH_MILLIHERTZ,
      .high_frame_rate = true,
   };
   if (!ps5vk_title_declares_high_frame_rate())
      return;
   pthread_mutex_lock(&ps5vk_output_lock);
   /* An output a swapchain holds keeps its mode; one retained without a
    * swapchain that is not in the high-frame-rate mode gets another try. */
   if (ps5vk_output != NULL && !ps5vk_output->owned && !ps5vk_output->high_frame_rate)
      ps5vk_output_close();
   if (ps5vk_output == NULL)
      ps5vk_output = ps5vk_output_open_handle(true);
   device->high_mode_offered = ps5vk_output != NULL && ps5vk_output->high_frame_rate;
   /* A refused output is not kept open: the swapchain opens its own. */
   if (ps5vk_output != NULL && !ps5vk_output->owned && !ps5vk_output->registered &&
       !ps5vk_output->high_frame_rate)
      ps5vk_output_close();
   pthread_mutex_unlock(&ps5vk_output_lock);
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
   ps5vk_display_settle_modes(device);
   /* The fastest first: an application that takes the first mode gets 120 Hz
    * where it is offered, and the 60 Hz mode where it is not. */
   struct ps5vk_display *const modes[2] = {device->high_mode_offered ? &device->high_mode : NULL,
                                           &device->mode};
   VK_OUTARRAY_MAKE_TYPED(VkDisplayModePropertiesKHR, out, pProperties, pPropertyCount);
   for (unsigned i = 0; i < 2; i++) {
      if (modes[i] == NULL)
         continue;
      vk_outarray_append_typed(VkDisplayModePropertiesKHR, &out, properties) {
         *properties = (VkDisplayModePropertiesKHR){
            .displayMode = ps5vk_display_mode_handle(modes[i]),
            .parameters = {
               .visibleRegion = ps5vk_display_extent,
               .refreshRate = modes[i]->refresh_millihertz,
            },
         };
      }
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
                    "VideoOut's modes are the ones vkGetDisplayModePropertiesKHR reports "
                    "(3840x2160 at 59.94 Hz, and at 119.88 Hz where offered); it makes no others");
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
   /* Valid usage: the plane capabilities allow only the full display. Refused by
    * field rather than asserted, as ps5vk_CreateSwapchainKHR refuses its own. */
   if (pCreateInfo->planeIndex != 0)
      return vk_errorf(instance, VK_ERROR_UNKNOWN,
                       "planeIndex %u is not a plane the display reports: it has plane 0 only",
                       pCreateInfo->planeIndex);
   if (pCreateInfo->imageExtent.width != PS5VK_DISPLAY_WIDTH ||
       pCreateInfo->imageExtent.height != PS5VK_DISPLAY_HEIGHT)
      return vk_errorf(instance, VK_ERROR_UNKNOWN,
                       "imageExtent %ux%u is not the plane's %ux%u, its only extent",
                       pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
                       PS5VK_DISPLAY_WIDTH, PS5VK_DISPLAY_HEIGHT);
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
      .minImageCount = PS5VK_SWAPCHAIN_IMAGES - 1,
      .maxImageCount = PS5VK_SWAPCHAIN_IMAGES,
      .currentExtent = ps5vk_display_extent,
      .minImageExtent = ps5vk_display_extent,
      .maxImageExtent = ps5vk_display_extent,
      .maxImageArrayLayers = 1,
      .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      /* R23 also reads the acquired image back before presenting it. */
      .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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

/* R31: what the display's refresh actually is, measured rather than declared.
 * The mode the driver reports is a constant; a configured refresh value is not
 * evidence of scanout timing, and a throttled game's frame rate cannot give it
 * either. A run of bare sceVideoOutWaitVblank calls with no flip pending
 * returns once per refresh, so the interval between two returns is one refresh
 * period and the spread says whether intervals are being skipped. The first
 * call returns at the next vblank whatever the phase, so its interval is
 * discarded. Opt-in through a file the fixture stages or an environment
 * variable, and off by default: it delays startup by the refresh times the
 * count below and measures nothing the rest of the driver uses.
 *
 * One line per stage, one write each: this driver's consumer reopens stderr
 * unbuffered onto a file in the title's folder, so a message written as one
 * call per field costs the frame budget it is measuring (ps5vk_queue.c). */
#define PS5VK_VBLANK_PROBE_INTERVALS 60

/* A stage's measured refresh period, one line. */
static void
ps5vk_video_out_report_cadence(int handle, const char *stage)
{
   if (sceVideoOutWaitVblank(handle) != 0) {
      fputs("[ps5vk] vblank probe: the first wait failed\n", stderr);
      return;
   }
   uint64_t previous = os_time_get_nano();
   uint64_t sum = 0, low = UINT64_MAX, high = 0;
   unsigned counted = 0;
   for (unsigned interval = 0; interval < PS5VK_VBLANK_PROBE_INTERVALS; interval++) {
      if (sceVideoOutWaitVblank(handle) != 0)
         break;
      const uint64_t now = os_time_get_nano();
      const uint64_t delta = now - previous;
      previous = now;
      sum += delta;
      low = MIN2(low, delta);
      high = MAX2(high, delta);
      counted++;
   }
   if (counted == 0) {
      fprintf(stderr, "[ps5vk] vblank probe stage=%s: no interval measured\n", stage);
      return;
   }
   const double mean = (double)sum / (double)counted;
   char line[256];
   snprintf(line, sizeof(line),
            "[ps5vk] vblank probe stage=%s intervals=%u mean_ms=%.4f min_ms=%.4f max_ms=%.4f "
            "spread_ms=%.4f equivalent_hz=%.3f\n",
            stage, counted, mean / 1000000.0, (double)low / 1000000.0, (double)high / 1000000.0,
            (double)(high - low) / 1000000.0, mean > 0.0 ? 1e9 / mean : 0.0);
   fputs(line, stderr);
}

static void
ps5vk_video_out_probe_vblank(int handle)
{
   FILE *flag = fopen("/app0/ps5vk-vblank-probe.txt", "rb");
   const bool enabled = flag != NULL || getenv("PS5VK_VBLANK_PROBE") != NULL;
   if (flag)
      fclose(flag);
   if (enabled)
      ps5vk_video_out_report_cadence(handle, "as-opened");
}

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
   /* A high-frame-rate output outlives the process that asked for it (the
    * vendored runtime records this), so it is put back before the handle goes. */
   if (video->handle >= 0 && video->high_frame_rate) {
      const int restored =
         sceVideoOutConfigureOutput(video->handle, PS5VK_VIDEO_OUT_MODE_RESTORE, NULL, NULL, NULL);
      if (restored != 0)
         mesa_loge("sceVideoOutConfigureOutput(restore) failed: 0x%08x", (unsigned)restored);
      else
         fputs("[ps5vk] output: default mode restored\n", stderr);
      video->high_frame_rate = false;
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

/* Closes the process's output. Called with ps5vk_output_lock held. */
static void
ps5vk_output_close(void)
{
   if (ps5vk_output == NULL)
      return;
   ps5vk_video_out_close(ps5vk_output);
   free(ps5vk_output);
   ps5vk_output = NULL;
}

/* Opens VideoOut and, when asked, configures its high-frame-rate output on the
 * fresh handle, before the flip rate, which is where the console accepted it. A
 * refusal is not an error: the output presents at 59.94 Hz, and says so once.
 * NULL when VideoOut does not open. */
static struct ps5vk_video_out *
ps5vk_output_open_handle(bool high_frame_rate)
{
   struct ps5vk_video_out *const video = calloc(1, sizeof(*video));
   if (video == NULL)
      return NULL;
   *video = (struct ps5vk_video_out){.handle = -1, .buffers = {.start = -1}, .shown = UINT32_MAX};
   video->handle = sceVideoOutOpen(PS5VK_VIDEO_OUT_USER, 0, 0, NULL);
   if (video->handle < 0) {
      mesa_loge("sceVideoOutOpen failed: 0x%08x", (unsigned)video->handle);
      free(video);
      return NULL;
   }
   if (high_frame_rate) {
      const int supported = sceVideoOutIsOutputSupported(
         video->handle, PS5VK_VIDEO_OUT_MODE_HIGH_FRAME_RATE, NULL, NULL, NULL);
      const int configured =
         supported > 0 ? sceVideoOutConfigureOutput(
                            video->handle, PS5VK_VIDEO_OUT_MODE_HIGH_FRAME_RATE, NULL, NULL, NULL)
                       : supported;
      video->high_frame_rate = configured == 0;
      char line[160];
      snprintf(line, sizeof(line),
               configured == 0 ? "[ps5vk] output: 119.88 Hz selected\n"
               : supported > 0 ? "[ps5vk] output: 119.88 Hz refused (0x%08x); presenting at 59.94 Hz\n"
                               : "[ps5vk] output: 119.88 Hz not supported (%d); presenting at 59.94 Hz\n",
               configured);
      fputs(line, stderr);
   }
   return video;
}

/* Maps two cleared framebuffers and registers them as SDR buffers at flip rate
 * 0, as the test runner does (open_video_target). */
static VkResult
ps5vk_video_out_register(struct ps5vk_device *device, struct ps5vk_video_out *video)
{
   int result = sceVideoOutSetFlipRate(video->handle, 0);
   if (result != 0)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "sceVideoOutSetFlipRate failed: 0x%08x", (unsigned)result);
   const size_t bytes = (size_t)(PS5VK_SWAPCHAIN_IMAGES * PS5VK_SWAPCHAIN_IMAGE_BYTES);
   result = ps5vk_direct_mapping_create(&video->buffers, bytes, PS5VK_SWAPCHAIN_ALIGNMENT);
   if (result != 0)
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "the framebuffers could not be mapped in the address window: 0x%08x",
                       (unsigned)result);
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
   if (result != 0)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "sceVideoOutRegisterBuffers2 failed: 0x%08x", (unsigned)result);
   video->registered = true;
   ps5vk_video_out_probe_vblank(video->handle);
   return VK_SUCCESS;
}

/* The output a new swapchain presents through, in the mode its surface asked
 * for: the retained or settled one when its mode matches, a fresh one
 * otherwise. A retained output keeps its framebuffers and the image on screen,
 * which stays there until the new swapchain's first present. */
static VkResult
ps5vk_video_out_open(struct ps5vk_device *device, bool high_frame_rate,
                     struct ps5vk_video_out **opened)
{
   pthread_mutex_lock(&ps5vk_output_lock);
   if (ps5vk_output != NULL && ps5vk_output->owned) {
      pthread_mutex_unlock(&ps5vk_output_lock);
      return vk_errorf(device, VK_ERROR_NATIVE_WINDOW_IN_USE_KHR,
                       "VideoOut belongs to another swapchain");
   }
   /* A mode other than the output's is a fresh output: switching a registered
    * output is not what the console was measured doing. The high-frame-rate
    * mode is offered only where the output took it, so a refused one stays. */
   if (ps5vk_output != NULL && high_frame_rate && !ps5vk_output->high_frame_rate)
      high_frame_rate = false;
   if (ps5vk_output != NULL && ps5vk_output->high_frame_rate != high_frame_rate)
      ps5vk_output_close();
   if (ps5vk_output == NULL)
      ps5vk_output = ps5vk_output_open_handle(high_frame_rate);
   if (ps5vk_output == NULL) {
      pthread_mutex_unlock(&ps5vk_output_lock);
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED, "sceVideoOutOpen failed");
   }
   if (!ps5vk_output->registered) {
      const VkResult result = ps5vk_video_out_register(device, ps5vk_output);
      if (result != VK_SUCCESS) {
         ps5vk_output_close();
         pthread_mutex_unlock(&ps5vk_output_lock);
         return result;
      }
   }
   ps5vk_output->owned = true;
   ps5vk_output->kept = ps5vk_output->shown != UINT32_MAX;
   ps5vk_output->previous_present_ns = ps5vk_output_last_present_ns;
   ps5vk_output->taken_ns = ps5vk_profile_now();
   ps5vk_output->watched = 0;
   ps5vk_output->black = 0;
   *opened = ps5vk_output;
   pthread_mutex_unlock(&ps5vk_output_lock);
   return VK_SUCCESS;
}

/* The swapchain lets its output go: kept, with its image on screen, while the
 * application retains it, closed otherwise. */
static void
ps5vk_video_out_release(struct ps5vk_video_out *video)
{
   pthread_mutex_lock(&ps5vk_output_lock);
   assert(video == ps5vk_output);
   video->owned = false;
   if (!ps5vk_output_retained)
      ps5vk_output_close();
   pthread_mutex_unlock(&ps5vk_output_lock);
}

void
ps5vk_display_retain(bool retain)
{
   pthread_mutex_lock(&ps5vk_output_lock);
   ps5vk_output_retained = retain;
   if (!retain && ps5vk_output != NULL && !ps5vk_output->owned)
      ps5vk_output_close();
   pthread_mutex_unlock(&ps5vk_output_lock);
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
   /* The GPU may still render into the images or have their flips queued
    * (R69). */
   if (device->queue_initialized)
      ps5vk_queue_wait_idle(&device->queue);
   for (uint32_t index = 0; index < PS5VK_SWAPCHAIN_IMAGES; index++) {
      if (swapchain->images[index])
         vk_image_destroy(&device->vk, NULL, &swapchain->images[index]->vk);
   }
   if (swapchain->video) {
      ps5vk_video_out_release(swapchain->video);
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
    * these values, and oldSwapchain is not retired. The rule is the driver's to
    * enforce, but an application that breaks it gets a refusal naming the field
    * rather than an aborted title: an assertion gives it no VkResult and nothing
    * to act on (R4 of the vkQuake port's requests, measured when vkQuake asked
    * for TRANSFER_SRC usage the surface does not report). One sentence for the
    * first field that disagrees, in the order the structure declares them. */
   if (info->minImageCount > PS5VK_SWAPCHAIN_IMAGES)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "minImageCount %u is past the surface's maxImageCount %u",
                       info->minImageCount, PS5VK_SWAPCHAIN_IMAGES);
   if (info->imageFormat != VK_FORMAT_B8G8R8A8_UNORM)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "imageFormat %d is not a format the surface reports: it reports "
                       "VK_FORMAT_B8G8R8A8_UNORM only",
                       (int)info->imageFormat);
   if (info->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "imageColorSpace %d is not the surface's: it reports "
                       "VK_COLOR_SPACE_SRGB_NONLINEAR_KHR only",
                       (int)info->imageColorSpace);
   if (info->imageExtent.width != PS5VK_DISPLAY_WIDTH ||
       info->imageExtent.height != PS5VK_DISPLAY_HEIGHT)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "imageExtent %ux%u is not the surface's %ux%u, its only extent",
                       info->imageExtent.width, info->imageExtent.height, PS5VK_DISPLAY_WIDTH,
                       PS5VK_DISPLAY_HEIGHT);
   if (info->imageArrayLayers != 1)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "imageArrayLayers %u is past the surface's maxImageArrayLayers 1",
                       info->imageArrayLayers);
   const VkImageUsageFlags supported_usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   if ((info->imageUsage & ~supported_usage) != 0)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "imageUsage 0x%x asks for 0x%x outside the surface's supportedUsageFlags "
                       "(COLOR_ATTACHMENT_BIT | TRANSFER_SRC_BIT)",
                       (unsigned)info->imageUsage,
                       (unsigned)(info->imageUsage & ~supported_usage));
   if (info->presentMode != VK_PRESENT_MODE_FIFO_KHR)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "presentMode %d is not a mode the surface reports: it reports "
                       "VK_PRESENT_MODE_FIFO_KHR only",
                       (int)info->presentMode);
   if (old && (old->retired || !old->video))
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "oldSwapchain was already retired by another swapchain's creation");
   if (!old && device->video_out)
      return vk_errorf(device, VK_ERROR_NATIVE_WINDOW_IN_USE_KHR,
                       "VideoOut belongs to another swapchain");
   /* The new swapchain takes over the old one's VideoOut buffers, which the
    * GPU may still be rendering into (R69). */
   if (old && device->queue_initialized)
      ps5vk_queue_wait_idle(&device->queue);

   struct ps5vk_swapchain *const swapchain =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*swapchain), VK_OBJECT_TYPE_SWAPCHAIN_KHR);
   if (!swapchain)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   swapchain->acquired = UINT32_MAX;

   struct ps5vk_video_out *video = old ? old->video : NULL;
   if (!video) {
      const VkIcdSurfaceDisplay *const surface =
         (const VkIcdSurfaceDisplay *)(uintptr_t)info->surface;
      const struct ps5vk_display *const mode =
         (const struct ps5vk_display *)(uintptr_t)surface->displayMode;
      const VkResult opened =
         ps5vk_video_out_open(device, mode != NULL && mode->high_frame_rate, &video);
      if (opened != VK_SUCCESS) {
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

/* A buffer the application may render into: neither on screen nor queued for a
 * flip. VideoOut's flip status names the latest flip shown; a buffer whose last
 * flip is past it is queued, the one whose flip it is stays on screen until a
 * later one shows, and before any flip of this output the buffer a retained
 * output left on screen (shown) stays there. Among the free ones the one
 * flipped longest ago is taken, so the buffers take turns. With none free the
 * wait is a vblank at a time, as the flip confirmation's is; a zero timeout
 * returns VK_NOT_READY instead. */
static VkResult
ps5vk_swapchain_free_image(struct ps5vk_video_out *video, uint64_t timeout, uint32_t *free_index)
{
   for (unsigned wait = 0; wait < PS5VK_FLIP_WAITS; wait++) {
      uint64_t status[PS5VK_FLIP_STATUS_WORDS] = {0};
      int64_t shown_marker = 0;
      if (video->flip_marker != 0 && sceVideoOutGetFlipStatus(video->handle, status) == 0)
         shown_marker = (int64_t)status[PS5VK_FLIP_STATUS_MARKER];
      /* The buffer on screen: the latest one flipped at or before the flip
       * VideoOut showed last. */
      uint32_t on_screen = video->flip_marker == 0 ? video->shown : UINT32_MAX;
      for (uint32_t index = 0; index < PS5VK_SWAPCHAIN_IMAGES; index++)
         if (video->image_marker[index] != 0 && video->image_marker[index] <= shown_marker &&
             (on_screen == UINT32_MAX || on_screen >= PS5VK_SWAPCHAIN_IMAGES ||
              video->image_marker[index] > video->image_marker[on_screen]))
            on_screen = index;
      uint32_t best = UINT32_MAX;
      for (uint32_t index = 0; index < PS5VK_SWAPCHAIN_IMAGES; index++) {
         if (index == on_screen || video->image_marker[index] > shown_marker)
            continue;
         if (best == UINT32_MAX || video->image_marker[index] < video->image_marker[best])
            best = index;
      }
      if (best != UINT32_MAX) {
         *free_index = best;
         return VK_SUCCESS;
      }
      if (timeout == 0)
         return VK_NOT_READY;
      sceVideoOutWaitVblank(video->handle);
   }
   return VK_TIMEOUT;
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
   uint32_t index = UINT32_MAX;
   const VkResult waited = ps5vk_swapchain_free_image(swapchain->video, timeout, &index);
   if (waited != VK_SUCCESS)
      return waited;
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
   /* The stretch attribution is the queue's (ps5vk_queue.c) and is a no-op
    * unless its opt-in profiling is on; a queue-less device has none. */
   struct ps5vk_queue *const queue = device->queue_initialized ? &device->queue : NULL;
   if (queue)
      ps5vk_profile_enter(queue, PS5VK_PROFILE_AFTER_ACQUIRE);
   const VkResult result =
      ps5vk_swapchain_acquire(device, swapchain, timeout, semaphore, fence, pImageIndex);
   if (queue)
      ps5vk_profile_leave(queue, PS5VK_PROFILE_AFTER_ACQUIRE);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_AcquireNextImage2KHR(VkDevice _device, const VkAcquireNextImageInfoKHR *pAcquireInfo,
                           uint32_t *pImageIndex)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_swapchain, swapchain, pAcquireInfo->swapchain);
   struct ps5vk_queue *const queue = device->queue_initialized ? &device->queue : NULL;
   if (queue)
      ps5vk_profile_enter(queue, PS5VK_PROFILE_AFTER_ACQUIRE);
   const VkResult result = ps5vk_swapchain_acquire(device, swapchain, pAcquireInfo->timeout,
                                                   pAcquireInfo->semaphore, pAcquireInfo->fence,
                                                   pImageIndex);
   if (queue)
      ps5vk_profile_leave(queue, PS5VK_PROFILE_AFTER_ACQUIRE);
   return result;
}

/* Presents a new swapchain's report watches: long enough for an application
 * that rebuilds its context to draw its first real frame. */
#define PS5VK_HANDOVER_WATCH_PRESENTS 600u
#define PS5VK_HANDOVER_SAMPLES 64u

/* Whether a presented image is black: every one of a spread of sampled texels
 * has zero colour. The submission that drew it has returned, so the GPU is done
 * with it; the sampled lines are flushed so the CPU reads what it wrote. */
static bool
ps5vk_video_out_image_black(const struct ps5vk_video_out *video, uint32_t index)
{
   const uint8_t *const image =
      (const uint8_t *)video->buffers.address + (size_t)index * PS5VK_SWAPCHAIN_IMAGE_BYTES;
   const size_t stride = PS5VK_SWAPCHAIN_IMAGE_BYTES / PS5VK_HANDOVER_SAMPLES;
   for (unsigned i = 0; i < PS5VK_HANDOVER_SAMPLES; i++) {
      const uint32_t *const texel =
         (const uint32_t *)(image + i * stride + (stride / 2 & ~(size_t)63));
      ps5vk_flush_cpu_cache(texel, 64);
      if ((*texel & 0x00ffffffu) != 0)
         return false;
   }
   return true;
}

/* One line per swapchain, after its first image that is not black (or its first
 * PS5VK_HANDOVER_WATCH_PRESENTS): whether VideoOut outlived the previous
 * swapchain, how long nothing was presented between the two, and how many black
 * presents followed. What the panel showed meanwhile follows from the first: a
 * kept output shows the previous image, a reopened one nothing. */
static void
ps5vk_video_out_watch(struct ps5vk_video_out *video, uint32_t index, uint64_t now)
{
   const uint64_t previous = video->previous_present_ns;
   ps5vk_output_last_present_ns = now;
   if (video->watched >= PS5VK_HANDOVER_WATCH_PRESENTS)
      return;
   video->watched++;
   const bool black = ps5vk_video_out_image_black(video, index);
   video->black += black ? 1u : 0u;
   if (black && video->watched < PS5VK_HANDOVER_WATCH_PRESENTS)
      return;
   char line[256];
   snprintf(line, sizeof(line),
            "[ps5vk] swapchain handover: output %s; %.1f ms from the previous present to "
            "this swapchain, first picture %.1f ms after it, %u black present%s before\n",
            video->kept ? "kept, previous image on screen" : "opened",
            previous != 0 && video->taken_ns > previous
               ? (double)(video->taken_ns - previous) / 1e6 : 0.0,
            (double)(now - video->taken_ns) / 1e6, video->black - (black ? 1u : 0u),
            video->black - (black ? 1u : 0u) == 1 ? "" : "s");
   fputs(line, stderr);
   video->watched = PS5VK_HANDOVER_WATCH_PRESENTS;
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
   ps5vk_profile_enter(queue, PS5VK_PROFILE_AFTER_PRESENT);

   /* The present consumes the semaphores it waits on. One a submission to this
    * queue signalled needs no CPU wait: the flip is submitted to the same
    * queue behind that submission's words, and the GPU runs them in order
    * (R69). Any other is waited for. */
   for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
      VK_FROM_HANDLE(vk_semaphore, semaphore, pPresentInfo->pWaitSemaphores[i]);
      struct vk_sync *const sync = vk_semaphore_get_active_sync(semaphore);
      VkResult result = ps5vk_sync_pending_on(sync, queue)
                           ? VK_SUCCESS
                           : vk_sync_wait(&device->vk, sync, 0, VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
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
         /* The two diagnostics below read the image the GPU renders, so they
          * wait for it: the rendering may still be running (R69). The watch
          * looks at a swapchain's first few presents only, and the content
          * check at one present in 1200 with the profile on. */
         if (video->watched < PS5VK_HANDOVER_WATCH_PRESENTS) {
            ps5vk_queue_wait_idle(queue);
            ps5vk_video_out_watch(video, index, ps5vk_profile_now());
         } else {
            ps5vk_output_last_present_ns = ps5vk_profile_now();
         }
         /* With profiling on, every 1200th presented image's content: how many
          * of the handover check's 64 sampled texels are not black. */
         if (queue->profile.enabled && ++video->content_checks % 1200u == 0) {
            ps5vk_queue_wait_idle(queue);
            const uint8_t *const image = (const uint8_t *)video->buffers.address +
                                         (size_t)index * PS5VK_SWAPCHAIN_IMAGE_BYTES;
            const size_t stride = PS5VK_SWAPCHAIN_IMAGE_BYTES / PS5VK_HANDOVER_SAMPLES;
            unsigned lit = 0;
            for (unsigned i = 0; i < PS5VK_HANDOVER_SAMPLES; i++) {
               const uint32_t *const texel =
                  (const uint32_t *)(image + i * stride + (stride / 2 & ~(size_t)63));
               ps5vk_flush_cpu_cache(texel, 64);
               lit += (*texel & 0x00ffffffu) != 0;
            }
            fprintf(stderr, "[ps5vk] presented content: image %u, %u of %u sampled texels lit\n",
                    index, lit, PS5VK_HANDOVER_SAMPLES);
         }
         const int64_t marker = ++video->flip_marker;
         result = ps5vk_queue_flip(queue, video->handle, index, marker,
                                   (ps5vk_ab_flags & PS5VK_AB_SYNC_PRESENT) != 0);
         if (result == VK_SUCCESS) {
            video->image_marker[index] = marker;
            video->shown = index;
         }
      }
      swapchain->acquired = UINT32_MAX;
      if (pPresentInfo->pResults)
         pPresentInfo->pResults[i] = result;
      overall = ps5vk_present_result(overall, result);
   }
   /* The frame ends here: the next submission's application stretch starts when
    * this returns, not when the submission before the present did. */
   if (queue->profile.enabled)
      queue->profile.last_return_ns = ps5vk_profile_now();
   ps5vk_profile_leave(queue, PS5VK_PROFILE_AFTER_PRESENT);
   return overall;
}

uint64_t
ps5vk_debug_now_ns(void)
{
   return ps5vk_profile_now();
}

uint64_t
ps5vk_debug_last_present_ns(void)
{
   return __atomic_load_n(&ps5vk_output_last_present_ns, __ATOMIC_RELAXED);
}

int
ps5vk_debug_video_handle(VkDevice _device)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   return device != NULL && device->video_out != NULL ? device->video_out->handle : -1;
}

/* Whether the title's own metadata declares high-frame-rate output: attribute3
 * holds 0x80040 (the value the publicly released ps5-opengl SDK's folder builder
 * sets above 60 Hz), in /app0/sce_sys/param.json. The console refused the mode
 * with 0x80290016 until the port declared it and accepted it after, so asking
 * for it without the declaration would only be refused. */
#define PS5VK_ATTRIBUTE3_HIGH_FRAME_RATE 0x80040u
static bool
ps5vk_title_declares_high_frame_rate(void)
{
   FILE *const file = fopen("/app0/sce_sys/param.json", "rb");
   if (file == NULL)
      return false;
   char text[16384];
   const size_t length = fread(text, 1, sizeof(text) - 1, file);
   fclose(file);
   text[length] = '\0';
   const char *at = strstr(text, "\"attribute3\"");
   if (at == NULL)
      return false;
   at = strchr(at, ':');
   if (at == NULL)
      return false;
   const unsigned long value = strtoul(at + 1, NULL, 0);
   return (value & PS5VK_ATTRIBUTE3_HIGH_FRAME_RATE) == PS5VK_ATTRIBUTE3_HIGH_FRAME_RATE;
}
