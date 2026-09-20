/*
 * PS5 Vulkan driver - Phase C1 test: presenting to the display.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C1 (docs/M5_PHASE_C.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the b8-corner triangle with ps5vk_triangle.c to the display, clearing
 * the attachment first (Phase C1b), as the Vulkan Tutorial draws to a window:
 * display, mode and plane-0 surface, a FIFO swapchain, four frames each
 * acquired, drawn and presented. The images must alternate, since the one on
 * screen is never acquired. tools/check-driver.sh runs the test against a
 * replay of the console's own driver run and compares the eight submissions
 * the host layer records, a frame and its flip each, with that run's streams
 * (tools/golden.py compare-run, golden/c1-triangle); the console reads each
 * frame back before its flip (runner test c1-triangle).
 *
 * Afterwards: a second swapchain on the surface is refused while the first
 * holds VideoOut, and a replacement passing it as oldSwapchain retires it, so
 * acquiring from the old one reports VK_ERROR_OUT_OF_DATE_KHR. Neither
 * submits. PS5VK_PROBES names the probes directory. The PS5 build only links.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

#define FRAMES 4

static void
report_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   (void)context;
   if (!passed)
      printf("  (%s failed: VkResult %d%s%s)\n", name, result, detail[0] ? ", " : "", detail);
}

#if defined(__linux__)
/* A SPIR-V file of a probe set, in words. */
static uint32_t *
read_spirv(const char *probes, const char *set, const char *stage, size_t *bytes)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s/%s.spv", probes, set, stage);
   FILE *const file = fopen(path, "rb");
   if (!file)
      return NULL;
   fseek(file, 0, SEEK_END);
   const long length = ftell(file);
   fseek(file, 0, SEEK_SET);
   uint32_t *words = length > 0 && length % 4 == 0 ? malloc((size_t)length) : NULL;
   if (words && fread(words, 1, (size_t)length, file) != (size_t)length) {
      free(words);
      words = NULL;
   }
   fclose(file);
   *bytes = words ? (size_t)length : 0;
   return words;
}
#endif

/* Swapchain replacement on the program's device, after its frames. An image
 * acquired from the first swapchain before it is replaced can still be
 * presented, which reports VK_ERROR_OUT_OF_DATE_KHR and flips nothing; a
 * retired swapchain may not be acquired from again. */
static void
check_replacement(struct ps5vk_triangle *triangle)
{
   VkSwapchainCreateInfoKHR info = triangle->swapchain_info;
   VkSwapchainKHR second = VK_NULL_HANDLE;
   VkResult result = VK_FUNCTION(triangle->instance, CreateSwapchainKHR)(triangle->device, &info,
                                                                         NULL, &second);
   check(result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR,
         "a second swapchain is refused while the first holds VideoOut");
   if (result == VK_SUCCESS)
      VK_FUNCTION(triangle->instance, DestroySwapchainKHR)(triangle->device, second, NULL);

   uint32_t index = UINT32_MAX;
   result = VK_FUNCTION(triangle->instance, ResetFences)(triangle->device, 1, &triangle->fence);
   if (result == VK_SUCCESS)
      result = VK_FUNCTION(triangle->instance, AcquireNextImageKHR)(
         triangle->device, triangle->swapchain, UINT64_MAX, VK_NULL_HANDLE, triangle->fence,
         &index);
   if (result == VK_SUCCESS)
      result = VK_FUNCTION(triangle->instance, WaitForFences)(triangle->device, 1,
                                                             &triangle->fence, VK_TRUE, 0);
   check(result == VK_SUCCESS, "an image is acquired, its fence signalled at once, before the "
                               "swapchain is replaced");
   if (result != VK_SUCCESS)
      return;

   info.oldSwapchain = triangle->swapchain;
   VkSwapchainKHR replacement = VK_NULL_HANDLE;
   result = VK_FUNCTION(triangle->instance, CreateSwapchainKHR)(triangle->device, &info, NULL,
                                                                &replacement);
   check(result == VK_SUCCESS, "a swapchain passing the first as oldSwapchain replaces it");
   const VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .swapchainCount = 1,
      .pSwapchains = &triangle->swapchain,
      .pImageIndices = &index,
   };
   const VkResult presented =
      VK_FUNCTION(triangle->instance, QueuePresentKHR)(triangle->queue, &present_info);
   check(result != VK_SUCCESS || presented == VK_ERROR_OUT_OF_DATE_KHR,
         "presenting the image acquired from the replaced swapchain is out of date");
   if (result != VK_SUCCESS)
      return;
   uint32_t count = 0;
   result = VK_FUNCTION(triangle->instance, GetSwapchainImagesKHR)(triangle->device, replacement,
                                                                   &count, NULL);
   check(result == VK_SUCCESS && count == 2, "the replacement has two images");
   VK_FUNCTION(triangle->instance, DestroySwapchainKHR)(triangle->device, replacement, NULL);
}

int
main(void)
{
   test_begin("C1 present");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex =
      probes ? read_spirv(probes, "b8-corner", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "b8-corner", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the b8-corner SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      const struct ps5vk_triangle_report report = {NULL, report_step};
      const struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_CLEAR, &report, PS5VK_TRIANGLE_OUTPUT_DISPLAY,
         /* No vertex bindings: the corner sets draw without geometry (Phase
          * C2's c2_indexed test is where indexed draws are recorded). */
         NULL, 0, 0, NULL, 0, 0, {{0}}, false, NULL, 0, 0,
      };
      struct ps5vk_triangle triangle;
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      check(status == PS5VK_TRIANGLE_OK && triangle.image_count == 2 &&
               triangle.format == VK_FORMAT_B8G8R8A8_UNORM,
            "a display surface and a FIFO swapchain of two B8G8R8A8_UNORM images");

      unsigned presented = 0;
      bool alternating = true;
      for (unsigned frame = 0; frame < FRAMES && status == PS5VK_TRIANGLE_OK; frame++) {
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         alternating = alternating && triangle.image_index == frame % 2;
         if (status == PS5VK_TRIANGLE_OK)
            status = ps5vk_triangle_present(&triangle);
         presented += status == PS5VK_TRIANGLE_OK ? 1u : 0u;
      }
      check(presented == FRAMES, "four frames acquired, drawn and presented");
      check(presented == FRAMES && alternating, "the frames alternate between the two images");
      check(status != PS5VK_TRIANGLE_OK || triangle.target == NULL,
            "swapchain images are not mapped for the application");
      if (status == PS5VK_TRIANGLE_OK) {
         check(ps5vk_triangle_present(&triangle) == PS5VK_TRIANGLE_FAILED,
               "the program presents only a drawn image");
         check_replacement(&triangle);
      }
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
