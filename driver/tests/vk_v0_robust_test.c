/*
 * PS5 Vulkan driver - V0-robust test: an out-of-range index count.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Rung 1.0, V0-robust (docs/M5_REFERENCE.md); built and run through the loader
 * and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws C2's indexed square twice through the driver, with one thing between
 * the frames: the count the draw asks for. The first frame asks for the six
 * indices the buffer holds; the second asks for twelve, twice the bound, which
 * Vulkan 1.0's robustBufferAccess makes safe. The driver clamps the count to
 * what the index binding covers, so the recorded stream of the second frame
 * carries the six indices the buffer has -- which is what
 * tools/check-driver.sh compares with the console's own run of the runner's
 * `v0-robust` test (golden/v0-robust).
 *
 * The console, not this test, is what proves the geometry reaches the screen:
 * src/diagnostics.cpp's `v0-robust` reads both frames back and the square
 * checker finds the square, and nothing outside it, in each.
 * PS5VK_PROBES names the probes directory. The PS5 build only links; it is not
 * run.
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

/* The m3-vertex canary's square: position (x, y) then colour (r, g, b, a) per
 * 24-byte record, and six 16-bit indices (src/diagnostics.cpp, kVertices and
 * kIndices). */
#define V0_VERTEX_STRIDE 24
#define V0_VERTEX_COUNT 4
#define V0_INDEX_COUNT 6

static const float kVertices[V0_VERTEX_COUNT * 6] = {
   -0.5f, -0.5f, 0x80 / 255.0f, 0.0f, 1.0f, 1.0f, /* 0 bottom left */
   0.5f,  -0.5f, 0x80 / 255.0f, 1.0f, 1.0f, 1.0f, /* 1 bottom right */
   0.5f,  0.5f,  0x80 / 255.0f, 1.0f, 0.0f, 1.0f, /* 2 top right */
   -0.5f, 0.5f,  0x80 / 255.0f, 0.0f, 0.0f, 1.0f, /* 3 top left */
};

static const uint16_t kIndices[V0_INDEX_COUNT] = {0, 1, 2, 2, 3, 0};

int
main(void)
{
   test_begin("V0 robust");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "m3-vertex", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m3-vertex", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m3-vertex SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8},
      };
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         kVertices, V0_VERTEX_COUNT, V0_VERTEX_STRIDE, kIndices, V0_INDEX_COUNT,
         2, {attributes[0], attributes[1]}, false, NULL, 0, 0,
      };
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      for (unsigned frame = 0; frame < 2 && status == PS5VK_TRIANGLE_OK; frame++) {
         if (frame != 0 && !ps5vk_triangle_set_draw_index_count(&triangle, 2 * V0_INDEX_COUNT))
            status = PS5VK_TRIANGLE_FAILED;
         else
            status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      }
      check(status == PS5VK_TRIANGLE_OK,
            "both frames record, submit and signal their fences");
      check(steps.failed == NULL, "no step of either draw failed before its fence");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
