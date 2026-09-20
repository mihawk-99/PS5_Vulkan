/*
 * PS5 Vulkan driver - Phase C8 test: a four-sample colour target.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C8 (docs/M5_REFERENCE.md); built and run through the loader
 * and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the M2 solid frame into a four-sample R8G8B8A8_UNORM target, the frame
 * src/diagnostics.cpp's c8-msaa renders on the console. The sample count the
 * target and its passes carry is what the driver's colour-target registers
 * report (CB_COLOR0_ATTRIB.NUM_SAMPLES, driver/ps5vk_draw.c): nothing renders on
 * the PC, so this program's job is the stream -- that the four-sample pipeline
 * is accepted, that the frame records, submits and signals its fence, and that
 * the target the driver allocated is the four-sample one it sized. The storage
 * itself is the console's to measure: its probe counts the words that hold the
 * frame's colour and dumps the first of them, which is the layout the resolve
 * has to walk.
 * tools/check-driver.sh compares the submission with the console's own run of it
 * (golden/c8-msaa). PS5VK_PROBES names the probes directory. The PS5 build only
 * links; it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

/* The first failed step of a draw, apart from driver messages. */
struct steps {
   const char *failed;
   int result;
};

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   struct steps *const steps = context;
   if (passed)
      return;
   if (strcmp(name, "vk_message") == 0) {
      printf("  (driver: %s)\n", detail);
      return;
   }
   printf("  (%s failed: VkResult %d%s%s)\n", name, result, detail[0] ? ", " : "", detail);
   if (!steps->failed) {
      steps->failed = name;
      steps->result = result;
   }
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

int
main(void)
{
   test_begin("C8 msaa");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "m2", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m2", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m2 SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
      };
      /* The four-sample target the console's frame draws into. */
      input.samples = VK_SAMPLE_COUNT_4_BIT;
      struct ps5vk_triangle triangle = {0};

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the four-sample frame records, submits and signals its fence");
      check(steps.failed == NULL, "no step of the four-sample draw failed before its fence");
      if (status == PS5VK_TRIANGLE_OK) {
         /* The target is the four-sample one the driver sized: its memory is
          * four times the one-sample image's, sixteen bytes a texel. */
         check(triangle.samples == VK_SAMPLE_COUNT_4_BIT, "the program kept the four-sample target");
         check(triangle.target_bytes >= (size_t)PS5VK_TRIANGLE_WIDTH * PS5VK_TRIANGLE_HEIGHT * 16u,
               "the target the driver allocated holds four samples a texel");
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
