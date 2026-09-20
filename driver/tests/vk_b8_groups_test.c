/*
 * PS5 Vulkan driver - Phase B8 test: several draws, command buffers and
 * submissions.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B8 (docs/M5_PHASE_B.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws two M2 triangles per frame with ps5vk_triangle.c, in each grouping a
 * Vulkan application can use: both in one command buffer, one command buffer
 * each in one VkSubmitInfo, in two VkSubmitInfos of one vkQueueSubmit, and in
 * two vkQueueSubmit calls. tools/check-driver.sh runs it against the
 * b4-headless replay, whose one stage workspace serves one pipeline, so both
 * draws use the M2 pipeline; it compares the five submissions the host layer
 * records with that frame, the draw repeated: two, two, two, one, one. Two
 * VkSubmitInfos of one vkQueueSubmit arrive as one submission, because Mesa's
 * queue merges submits without a signal between them (vk_queue_submits_merge).
 * The console draws the second triangle in a second colour and reads every
 * frame back (runner test b8-groups). PS5VK_PROBES names the probes directory.
 * The PS5 build only links; it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

static void
print_failure(void *context, const char *name, bool passed, int result, const char *detail)
{
   (void)context;
   if (!passed)
      printf("  (%s: VkResult %d%s%s)\n", name, result, detail[0] ? ", " : "", detail);
}

#if defined(__linux__)
/* A SPIR-V file of a probe set, in words (vk_b7_draw_test.c has the same). */
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

int
main(void)
{
   test_begin("B8 groups");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "m2", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m2", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the M2 SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      const struct ps5vk_triangle_report report = {NULL, print_failure};
      const struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         /* No vertex bindings: the corner sets draw without geometry (Phase
          * C2's c2_indexed test is where indexed draws are recorded). */
         NULL, 0, 0, NULL, 0, 0, {{0}}, false, NULL, 0, 0,
      };
      static const enum ps5vk_triangle_grouping groupings[] = {
         PS5VK_TRIANGLE_ONE_COMMAND_BUFFER,
         PS5VK_TRIANGLE_TWO_COMMAND_BUFFERS,
         PS5VK_TRIANGLE_TWO_SUBMIT_INFOS,
         PS5VK_TRIANGLE_TWO_SUBMISSIONS,
      };
      struct ps5vk_triangle triangle;
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      check(status == PS5VK_TRIANGLE_OK, "the device, target, pipeline and command buffers exist");
      for (size_t index = 0;
           index < sizeof(groupings) / sizeof(groupings[0]) && status == PS5VK_TRIANGLE_OK;
           index++) {
         status = ps5vk_triangle_draw(&triangle, groupings[index]);
         char what[128];
         snprintf(what, sizeof(what), "%s: recorded, submitted and signalled",
                  ps5vk_triangle_grouping_name(groupings[index]));
         check(status == PS5VK_TRIANGLE_OK, what);
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
