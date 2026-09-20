/*
 * PS5 Vulkan driver - C8's four-sample depth attachment, on the PC.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase C8 and V0-unknowns (docs/M5_REFERENCE.md). A four-sample depth target
 * needed three things: the attachment accepted (the pass's description and the
 * image's sample count have to agree -- the runtime asserts when they do not,
 * which is how the first console attempts died), DB_Z_INFO's NUM_SAMPLES set
 * for the count (bits 2-3, the count's log2: docs/HARDWARE_FINDINGS.md), and a
 * map for the sample planes the driver's readback and clear walk. The console's
 * runner case unknowns-depth4x-map measures that map -- it renders the m4-depth
 * canary's two rectangles with the test off and the write on, so every covered
 * sample holds its rectangle's depth bits, and reports what the storage says per
 * plane. This test is the PC arm: nothing renders here, so it checks that the
 * four-sample depth attachment, the frame and its fence record and complete,
 * and (once the console run exists) tools/check-driver.sh compares the
 * submission with the console's own frame.
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

/* The m4-depth canary's two rectangles: position (x, y, z) at offset 0 and the
 * colour at offset 12, the 28-byte records probes/m4-depth declares. The near
 * rectangle is drawn first, the far one second. */
#define D4_VERTEX_STRIDE 28
#define D4_VERTEX_COUNT 8
#define D4_INDEX_COUNT 12

static void
put_rect(float *vertices, uint16_t *indices, uint32_t vertex, uint32_t index, float left,
         float top, float right, float bottom, float depth)
{
   const float corners[4][2] = {{left, bottom}, {right, bottom}, {right, top}, {left, top}};
   for (uint32_t corner = 0; corner < 4; corner++) {
      float *const record = vertices + (size_t)(vertex + corner) * 7;
      record[0] = corners[corner][0];
      record[1] = corners[corner][1];
      record[2] = depth;
      record[3] = 1.0f;
      record[4] = 1.0f;
      record[5] = 1.0f;
      record[6] = 1.0f;
   }
   const uint16_t quad[6] = {(uint16_t)vertex,     (uint16_t)(vertex + 1),
                             (uint16_t)(vertex + 2), (uint16_t)(vertex + 2),
                             (uint16_t)(vertex + 3), (uint16_t)vertex};
   memcpy(indices + index, quad, sizeof(quad));
}

int
main(void)
{
   test_begin("C8 depth four samples");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "m4-depth", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m4-depth", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m4-depth SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
      };
      float vertices[D4_VERTEX_COUNT * 7] = {0};
      uint16_t indices[D4_INDEX_COUNT] = {0};
      put_rect(vertices, indices, 0, 0, -0.5f, 0.5f, 0.0f, 0.0f, 0.25f);
      put_rect(vertices, indices, 4, 6, 0.0f, 0.0f, 0.5f, -0.5f, 0.75f);
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         vertices, D4_VERTEX_COUNT, D4_VERTEX_STRIDE, indices, D4_INDEX_COUNT,
         2, {attributes[0], attributes[1]}, false, NULL, 0, 0,
      };
      /* The four-sample depth attachment: the test off and the write on, so
       * every covered sample takes its rectangle's depth. */
      input.samples = VK_SAMPLE_COUNT_4_BIT;
      input.depth = true;
      input.depth_test = false;
      input.depth_write = true;
      input.depth_compare_op = VK_COMPARE_OP_ALWAYS;
      input.depth_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
      input.depth_clear_value = 1.0f;
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "a four-sample depth attachment, its pass and its frame record, submit and signal "
            "their fence");
      check(steps.failed == NULL, "no step of the create or the draw failed before the fence");
      check(triangle.depth_target != NULL && triangle.depth_target_bytes != 0,
            "the frame left a depth target to read back");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
