/*
 * PS5 Vulkan driver - V0-formats: the unsigned three-component integer vertex
 * position, through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase V0-formats (docs/V0_FORMATS_AUDIT.md). The audit lists
 * VK_FORMAT_R32G32B32_UINT as missing VERTEX_BUFFER alone, and the console's runner case
 * v0-vertex-uint drew the m3-vertex square with it: the integer position is
 * scaled to the float frames' square, its third component carried out as the
 * fragment's alpha and read back exact (pid 146,
 * Klog_Logs/v0-vertex-int-run2.log). That is where the bit is proved; this test
 * is the PC arm. Built and run through the loader and directly by
 * tools/check-driver.sh (see ps5vk_test.h): nothing renders on the PC, so it
 * checks that the draw records, submits and signals its fence, and
 * tools/check-driver.sh compares the submission the host layer records
 * (PS5_HOST_SUBMISSION_DUMP) with the console's own run of the runner case.
 *
 * PS5VK_PROBES names the probes directory. The PS5 build only links; it is not
 * run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* The m3-vertex square as unsigned integer records: position x, y and z at offset 0,
 * the canary's colour at offset 16, 32 bytes apart. z is 0 on the left vertices
 * and 1 on the right ones, which is the alpha gradient the console's readback
 * checks; an unsigned component holds no negative value, so x and y are 0 and 1 and
the set's shader subtracts a half.
 */
#define V0_VERTEX_STRIDE 32
#define V0_VERTEX_COUNT 4
#define V0_INDEX_COUNT 6

static const int32_t kPositions[V0_VERTEX_COUNT][3] = {
   {0, 0, 0}, /* 0 bottom left */
   {1, 0, 1}, /* 1 bottom right */
   {1, 1, 1}, /* 2 top right */
   {0, 1, 0}, /* 3 top left */
};

static const float kColours[V0_VERTEX_COUNT][4] = {
   {0x80 / 255.0f, 0.0f, 1.0f, 1.0f}, /* 0 bottom left */
   {0x80 / 255.0f, 1.0f, 1.0f, 1.0f}, /* 1 bottom right */
   {0x80 / 255.0f, 1.0f, 0.0f, 1.0f}, /* 2 top right */
   {0x80 / 255.0f, 0.0f, 0.0f, 1.0f}, /* 3 top left */
};

static const uint16_t kIndices[V0_INDEX_COUNT] = {0, 1, 2, 2, 3, 0};

static void
fill_vertices(uint8_t *records)
{
   for (int vertex = 0; vertex < V0_VERTEX_COUNT; vertex++) {
      memcpy(records + (size_t)vertex * V0_VERTEX_STRIDE, kPositions[vertex],
             sizeof(kPositions[vertex]));
      memcpy(records + (size_t)vertex * V0_VERTEX_STRIDE + 16, kColours[vertex],
             sizeof(kColours[vertex]));
   }
}

int
main(void)
{
   test_begin("V0 vertex uint");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "v0-vertex-uint", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "v0-vertex-uint", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the v0-vertex-uint SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32B32_UINT, 0},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
      };
      uint8_t vertices[V0_VERTEX_COUNT * V0_VERTEX_STRIDE] = {0};
      fill_vertices(vertices);
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         vertices, V0_VERTEX_COUNT, V0_VERTEX_STRIDE, kIndices, V0_INDEX_COUNT,
         2, {attributes[0], attributes[1]}, false, NULL, 0, 0,
      };
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the indexed square with a unsigned integer position records, submits and signals "
            "its fence");
      check(steps.failed == NULL, "no step of the draw failed before the fence");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
