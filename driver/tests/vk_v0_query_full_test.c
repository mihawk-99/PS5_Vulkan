/*
 * PS5 Vulkan driver - V0-query's occlusion query, on the PC.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase V0-query (docs/M5_PHASE_C.md). The console proved the query path three
 * times: regions of the 4K target covering none, half and all of it answered 0,
 * 4,147,200 and 8,294,400 samples through vkGetQueryPoolResults (pids 113-115).
 * This test runs the same full-target region on the PC, in both arms
 * tools/check-driver.sh builds -- directly and behind the Vulkan loader -- so
 * the driver's query code is exercised where a failure is cheap, and its
 * recorded stream is compared with golden/v0-query-driver's replay by
 * tools/golden.py compare-run.
 *
 * What the PC can check by itself differs from the console: the host layer
 * completes a submission without a GPU, so the counters keep the zero a pool is
 * created with and a result read here is 0 samples. The checks are therefore
 * that the path works -- the pool is created, the frame records inside it, the
 * device accepts the submission and the result is available -- plus, in the
 * direct arm alone, that the stream the driver queued carries the ZPASS_DONE
 * sample of this hardware (packet 0xc0024600, event 0x00000115), which is what
 * the console's golden holds. The counts themselves are the console's.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps5vk_test.h"

/* The driver's own debug API, which reads the queued stream (the direct arm
 * only: the loader's ICD exports no such symbol). */
#if defined(PS5VK_TEST_DIRECT)
#include "../ps5vk_debug.h"
#endif

#include "ps5vk_triangle.h"

#if defined(__linux__)
/* A SPIR-V file of a probe set, in words (as vk_c5_depth_test.c reads them). */
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

#if defined(PS5VK_TEST_DIRECT)
/* GFX10 ZPASS_DONE: EVENT_WRITE (PKT3 0x46, two body words), event 21 with
 * index 1, then the counter's address (driver/ps5vk_query.c). Only the direct
 * arm reads the queued stream, where the driver's debug API is exported. */
#define ZPASS_PACKET UINT32_C(0xc0024600)
#define ZPASS_EVENT UINT32_C(0x00000115)
#endif

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   const char **failed = context;
   if (!passed && *failed == NULL) {
      fprintf(stderr, "  step %s failed: %d %s\n", name, result, detail);
      *failed = name;
   }
}

#if defined(PS5VK_TEST_DIRECT)
/* Whether the queued stream carries a ZPASS_DONE sample. */
static bool
stream_has_zpass(const uint32_t *words, uint32_t dwords)
{
   for (uint32_t word = 0; word + 1 < dwords; word++) {
      if (words[word] == ZPASS_PACKET && words[word + 1] == ZPASS_EVENT)
         return true;
   }
   return false;
}
#endif

int
main(void)
{
   test_begin("V0 query full");
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
      const char *failed = NULL;
      const struct ps5vk_triangle_report report = {&failed, record_step};
      /* The console's full region: the whole target, with the M2 set's three
       * vertices and no vertex binding (the B7 triangle every battery draws). */
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
      };
      input.use_scissor = true;
      input.scissor = (VkRect2D){{0, 0}, {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT}};

      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      check(status == PS5VK_TRIANGLE_OK, "the program creates its instance, device and target");

      VkQueryPool pool = VK_NULL_HANDLE;
      uint64_t samples = 0;
      if (status == PS5VK_TRIANGLE_OK) {
         pool = ps5vk_triangle_create_query_pool(&triangle, 1);
         check(pool != VK_NULL_HANDLE, "the device creates an occlusion query pool");
      }
      if (pool != VK_NULL_HANDLE) {
         ps5vk_triangle_set_query(&triangle, pool, 0);
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         check(status == PS5VK_TRIANGLE_OK, "the frame records inside the query and submits");
         check(failed == NULL, "no step of the create or the draw failed");
         check(ps5vk_triangle_query_samples(&triangle, pool, 0, &samples),
               "vkGetQueryPoolResults answers the query");
         /* The host layer runs no GPU: the counters keep the zero the pool was
          * created with, and the console's own counts are in its golden. */
         check(samples == 0, "a replayed submission writes no counters, so the result is zero");
#if defined(PS5VK_TEST_DIRECT)
         /* Only the direct build calls the debug API: the loader build sees the
          * driver through its ICD, which exports no such symbol
          * (driver/ps5vk_icd.map). */
         uint32_t dwords = 0;
         const uint32_t *const words = ps5vk_debug_last_submission(triangle.device, &dwords);
         check(words != NULL && dwords != 0, "the device queued a command stream");
         check(words != NULL && stream_has_zpass(words, dwords),
               "the stream carries this hardware's ZPASS_DONE sample");
#endif
         ps5vk_triangle_destroy_query_pool(&triangle, pool);
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
