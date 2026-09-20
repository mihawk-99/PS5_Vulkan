/*
 * PS5 Vulkan driver - Phase B7 test: drawing through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B7 (docs/M5_PHASE_B.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the M2 triangle with ps5vk_triangle.c. Nothing renders on the PC, so
 * the test checks that the draw records, submits and signals its fence.
 * tools/check-driver.sh runs it against a replay of the console's
 * b4-headless frame (PS5_HOST_REPLAY) and compares the submission the host
 * layer records (PS5_HOST_SUBMISSION_DUMP) with that frame; the console
 * reads the frame back (runner test b7-triangle).
 *
 * The replay hands its stage and target addresses to the first allocations
 * of their sizes, so the M2 draw runs first. A second draw loads the
 * attachment with VK_ATTACHMENT_LOAD_OP_CLEAR, which Mesa's vk_meta draws
 * through this driver's own entry points (Phase C1b), so the test checks that
 * the clear records and submits too. PS5VK_PROBES names the probes directory.
 * The PS5 build only links; it is not run.
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
   test_begin("B7 draw");
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
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         /* The M2 triangle has no vertex bindings: no geometry, and the empty
          * vertex input the pipeline is built with (Phase C2's c2_indexed test
          * is where indexed draws are recorded). */
         NULL, 0, 0, NULL, 0, 0, {{0}}, false, NULL, 0, 0,
      };
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the M2 triangle records, submits and signals its fence");
      check(status == PS5VK_TRIANGLE_OK && triangle.target != NULL &&
               triangle.target_bytes == UINT64_C(0x2000000),
            "the frame is readable through the mapped 32 MiB image memory");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);

      /* A clear is no longer refused: Phase C1b draws it through Mesa's
       * vk_meta, and the c1_present test is where a clearing render pass is
       * recorded and compared, so that this test keeps the one submission its
       * golden frame describes. */
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
