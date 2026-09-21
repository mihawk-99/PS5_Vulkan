/*
 * PS5 Vulkan driver - R9 test: push constants.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R9 (PS5_VULKAN_REQUESTSv2.md, docs/M5_PHASE_C.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * The runner's v0-push-constant case proves the path on the console: one
 * pipeline whose fragment shader exports the colour in its layout(push_constant)
 * block, two draws that upload different values with vkCmdPushConstants, and a
 * readback of both halves. This program is that case's host half *and* its
 * mechanism too: it pushes two values, records the two draws, and asks the
 * driver what it copied -- the block the debug API hands back
 * (ps5vk_debug_push_constants) must hold the *second* draw's bytes, which is
 * what the stage's reserved set-0 binding points at. A readback alone cannot
 * say whether the upload, the descriptor or the shader's read broke; this can.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The driver's own debug API: the register tables a draw records. */
#include "../ps5vk_debug.h"
#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

struct steps {
   const char *failed;
   int result;
   const char *detail;
};

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   struct steps *const steps = context;
   if (!passed && steps->failed == NULL) {
      steps->failed = name;
      steps->result = result;
      steps->detail = detail;
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

/* The vertex stage's layout, from probes/v0-push: a vec2 position and a vec4
 * colour in 24-byte records. */
#define V0P_VERTEX_STRIDE 24

static float kVertices[8 * 6];
static uint16_t kIndices[12];

/* One half of the target as four records, so the frame's two draws cover one
 * half each: the first six indices are the left half's, the rest the right's. */
static void
put_half(unsigned at, uint32_t left, uint32_t right)
{
   const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
   const float corners[4][2] = {{(float)left / (float)(PS5VK_TRIANGLE_WIDTH / 2) - 1.0f,
                                 1.0f - (float)PS5VK_TRIANGLE_HEIGHT /
                                           (float)(PS5VK_TRIANGLE_HEIGHT / 2)},
                                {(float)right / (float)(PS5VK_TRIANGLE_WIDTH / 2) - 1.0f,
                                 1.0f - (float)PS5VK_TRIANGLE_HEIGHT /
                                           (float)(PS5VK_TRIANGLE_HEIGHT / 2)},
                                {(float)right / (float)(PS5VK_TRIANGLE_WIDTH / 2) - 1.0f,
                                 1.0f - 0.0f / (float)(PS5VK_TRIANGLE_HEIGHT / 2)},
                                {(float)left / (float)(PS5VK_TRIANGLE_WIDTH / 2) - 1.0f,
                                 1.0f - 0.0f / (float)(PS5VK_TRIANGLE_HEIGHT / 2)}};
   for (unsigned corner = 0; corner < 4; corner++) {
      float *const record = kVertices + (size_t)(at + corner) * 6;
      memcpy(record, corners[corner], sizeof(corners[corner]));
      memcpy(record + 2, white, sizeof(white));
   }
   const uint16_t quad[6] = {(uint16_t)at, (uint16_t)(at + 1), (uint16_t)(at + 2),
                            (uint16_t)(at + 2), (uint16_t)(at + 3), (uint16_t)at};
   memcpy(kIndices + (size_t)(at / 4) * 6, quad, sizeof(quad));
}

int
main(void)
{
   test_begin("V0 push constants");
   const float first[4] = {1.0f, 0.0f, 0.0f, 1.0f};
   const float second[4] = {0.0f, 0.0f, 1.0f, 1.0f};
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
   uint32_t *const vertex = probes ? read_spirv(probes, "v0-push", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "v0-push", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the v0-push SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8},
      };
      put_half(0, 0, PS5VK_TRIANGLE_WIDTH / 2);
      put_half(4, PS5VK_TRIANGLE_WIDTH / 2, PS5VK_TRIANGLE_WIDTH);
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_CLEAR, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         kVertices, 8, V0P_VERTEX_STRIDE, kIndices, 12,
         2, {attributes[0], attributes[1]},
         false,
         NULL, 0, 0,
         NULL, 0, 0, false,
         false,
      };
      /* R9: a 16-byte push-constant range for the fragment stage, and the two
       * draws' values -- the pipeline is created once, so the second draw can
       * only differ through vkCmdPushConstants. */
      input.push_constant_bytes = sizeof(first);
      input.push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT;
      memcpy(input.push_constant_first, first, sizeof(first));
      memcpy(input.push_constant_second, second, sizeof(second));
      input.first_draw_indices = 6;
      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_COMMAND_BUFFER);
      check(status == PS5VK_TRIANGLE_OK,
            "the two draws record and submit through a push-constant pipeline layout");
      check(steps.failed == NULL, "no step of the create or the draws failed before the fence");
      if (steps.failed != NULL)
         printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
                steps.detail != NULL ? steps.detail : "");
#if defined(PS5VK_TEST_DIRECT)
      if (status == PS5VK_TRIANGLE_OK) {
         /* R9's mechanism: the bytes the last draw copied from
          * vkCmdPushConstants into the block its stage's reserved set-0 binding
          * points at (ps5vk_debug.h). The frame's second draw uploaded blue, so
          * the block must hold blue and not the first draw's red -- which is
          * what a readback cannot say by itself. */
         const void *block = NULL;
         uint32_t bytes = 0;
         const uint32_t *descriptor = NULL;
         ps5vk_debug_push_constants(triangle.device, &block, &bytes, &descriptor);
         check(block != NULL && bytes == sizeof(second),
               "the draw copied the push constants into a 16-byte block");
         float pushed[4] = {0};
         if (block != NULL && bytes >= sizeof(pushed))
            memcpy(pushed, block, sizeof(pushed));
         check(block != NULL && memcmp(pushed, second, sizeof(pushed)) == 0,
               "the block holds the second draw's push-constant bytes");
         printf("  (the block holds %.1f %.1f %.1f %.1f, expected %.1f %.1f %.1f %.1f; "
                "descriptor %08x %08x %08x %08x)\n",
                (double)pushed[0], (double)pushed[1], (double)pushed[2], (double)pushed[3],
                (double)second[0], (double)second[1], (double)second[2], (double)second[3],
                descriptor != NULL ? descriptor[0] : 0, descriptor != NULL ? descriptor[1] : 0,
                descriptor != NULL ? descriptor[2] : 0, descriptor != NULL ? descriptor[3] : 0);
      }
#endif
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
