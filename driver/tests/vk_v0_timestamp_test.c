/*
 * PS5 Vulkan driver - V0-query's timestamps, on the PC.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase V0-query (docs/M5_PHASE_C.md). V0-query's timestamp probe measured the
 * console's GPU clock -- a RELEASE_MEM with the BOTTOM_OF_PIPE_TS event and an
 * EOP selector naming the timestamp, about 100 MHz (pid 143) -- so the driver
 * creates a timestamp pool and vkCmdWriteTimestamp records that packet. This
 * test runs the same path on the PC, in both arms tools/check-driver.sh builds
 * (directly and behind the Vulkan loader), and its recorded stream is compared
 * with golden/v0-timestamp-driver's replay by tools/golden.py compare-run.
 *
 * What the PC can check by itself differs from the console: the host layer
 * completes a submission without a GPU, so no clock is ever written and both
 * readings stay the zero the pool is created with. The checks are therefore
 * that the path works -- the pool is created, the frame records its two writes
 * and the copy, the device accepts the submission, and a result read back is
 * zero with availability 0, which is what tells a query no command wrote apart
 * from one that ran and read zero -- plus, in the direct arm alone, that the
 * queued stream carries this hardware's timestamp packet (0xc0064900 with the
 * event word 0x0030c528 and the selector 0x63000000), which is what the
 * console's golden holds. The clock itself is the console's.
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
/* GFX10 RELEASE_MEM: PKT3 opcode 0x49 with six body words, the
 * BOTTOM_OF_PIPE_TS event (40) at index 5 and the EOP selector that names the
 * timestamp (driver/ps5vk_query.c, docs/HARDWARE_FINDINGS.md). Only the direct
 * arm reads the queued stream, where the driver's debug API is exported. */
#define TIMESTAMP_PACKET UINT32_C(0xc0064900)
#define TIMESTAMP_EVENT UINT32_C(0x0030c528)
#define TIMESTAMP_SELECT UINT32_C(0x63000000)
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
/* Whether the queued stream carries a timestamp write, header and event word,
 * and the selector that names the clock. */
static bool
stream_has_timestamp(const uint32_t *words, uint32_t dwords)
{
   for (uint32_t word = 0; word + 2 < dwords; word++) {
      if (words[word] == TIMESTAMP_PACKET && words[word + 1] == TIMESTAMP_EVENT &&
          words[word + 2] == TIMESTAMP_SELECT)
         return true;
   }
   return false;
}
#endif

int
main(void)
{
   test_begin("V0 timestamp");
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
      /* The console's timestamp frame: the whole target, the M2 set's three
       * vertices, and the copy that carries the second write into the harness's
       * buffer. */
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
      };
      input.use_scissor = true;
      input.scissor = (VkRect2D){{0, 0}, {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT}};
      input.query_copy = true;

      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      check(status == PS5VK_TRIANGLE_OK, "the program creates its instance, device and target");

      VkQueryPool pool = VK_NULL_HANDLE;
      uint64_t before = 0;
      uint64_t after = 0;
      if (status == PS5VK_TRIANGLE_OK) {
         pool = ps5vk_triangle_create_timestamp_pool(&triangle, 2);
         check(pool != VK_NULL_HANDLE, "the device creates a timestamp query pool");
      }
      if (pool != VK_NULL_HANDLE) {
         ps5vk_triangle_set_timestamp_pool(&triangle, pool);
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         check(status == PS5VK_TRIANGLE_OK, "the frame records its two writes and submits");
         check(failed == NULL, "no step of the create or the draw failed");
         check(ps5vk_triangle_query_timestamp(&triangle, pool, 0, &before),
               "vkGetQueryPoolResults answers the first timestamp");
         check(ps5vk_triangle_query_timestamp(&triangle, pool, 1, &after),
               "vkGetQueryPoolResults answers the second timestamp");
         /* The host layer runs no GPU: no clock is written, so both readings are
          * the zero the pool was created with. The console's own ticks are in
          * its golden. */
         check(before == 0 && after == 0, "a replayed submission writes no clock, so both are zero");
         /* What a query no command wrote reads: zero with availability 0. The
          * pool holds two queries and the frame writes both, so the third slot
          * is what a never-written timestamp looks like -- the copy's own
          * availability word says the same for the second write. */
         uint64_t copied = 0;
         uint64_t available = 0;
         check(ps5vk_triangle_query_copy(&triangle, &copied, &available),
               "the frame's recorded vkCmdCopyQueryPoolResults left a result");
         check(copied == 0 && available == 0,
               "an unwritten timestamp copies as zero with availability 0");
#if defined(PS5VK_TEST_DIRECT)
         /* Only the direct build calls the debug API: the loader build sees the
          * driver through its ICD, which exports no such symbol
          * (driver/ps5vk_icd.map). */
         uint32_t dwords = 0;
         const uint32_t *const words = ps5vk_debug_last_submission(triangle.device, &dwords);
         check(words != NULL && dwords != 0, "the device queued a command stream");
         check(words != NULL && stream_has_timestamp(words, dwords),
               "the stream carries this hardware's timestamp packet");
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
