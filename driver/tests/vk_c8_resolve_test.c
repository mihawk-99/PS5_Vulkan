/*
 * PS5 Vulkan driver - Phase C8 test: the four-sample frame and its resolve.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C8 (docs/M5_REFERENCE.md); built and run through the loader
 * and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the m3-vertex canary's quad over the whole viewport four times, the
 * frames src/diagnostics.cpp's c8-resolve renders on the console: one sample
 * first (the pipeline alone), then four samples with no resolve (the sample
 * count alone), then the four-sample frame with vkCmdResolveImage into the
 * program's own one-sample image, then the one-sample reference the resolve must
 * reproduce. The sample count the target and its passes carry is what the
 * driver's colour-target and rasterizer registers report (CB_COLOR0_ATTRIB's
 * NUM_SAMPLES and the PA_SC_* MSAA registers, driver/ps5vk_draw.c); nothing
 * renders on the PC, so this program's job is the stream -- that the four-sample
 * pipeline is accepted, that each frame records, submits and signals its fence,
 * and that the target the driver allocated is the four-sample one it sized. The
 * storage itself is the console's to measure: its probe counts the words that
 * hold the frame's colour and dumps a spread of tiles' first chunks, which is
 * the four-sample map the resolve walks (docs/M5_PHASE_C.md, C8).
 * tools/check-driver.sh compares every frame with the console's own run of it
 * (golden/c8-resolve). PS5VK_PROBES names the probes directory. The PS5 build
 * only links; it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

/* The whole-viewport quad the runner's c8-resolve case passes: the canary's
 * colours over the viewport's corners, so every pixel is covered and the
 * frame's green and blue name the pixel (src/diagnostics.cpp). */
static const float kVertices[4 * 6] = {
   -1.0f, -1.0f, 128.0f / 255.0f, 0.0f, 1.0f, 1.0f, /* 0 bottom left */
   1.0f,  -1.0f, 128.0f / 255.0f, 1.0f, 1.0f, 1.0f, /* 1 bottom right */
   1.0f,  1.0f,  128.0f / 255.0f, 1.0f, 0.0f, 1.0f, /* 2 top right */
   -1.0f, 1.0f,  128.0f / 255.0f, 0.0f, 0.0f, 1.0f, /* 3 top left */
};
static const uint16_t kIndices[6] = {0, 1, 2, 2, 3, 0};
static const uint32_t kVertexStride = 24;

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
   test_begin("C8 resolve");
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
      /* Four frames, in the order the console's case draws them: the same quad
       * at one sample first (the pipeline alone), then at four samples with no
       * resolve (the sample count alone), then the resolved four-sample frame,
       * then the one-sample reference the resolve must equal. */
      unsigned drawn = 0;
      for (unsigned frame = 0; frame < 4; frame++) {
         const bool sample_control = frame == 1;
         const bool resolving = frame == 2;
         struct ps5vk_triangle_input input = {
            GET_PROC, 1,
            {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         };
         input.vertex_data = kVertices;
         input.vertex_count = 4;
         input.vertex_stride = kVertexStride;
         input.index_data = kIndices;
         input.index_count = 6;
         input.attribute_count = 2;
         input.attributes[0] = attributes[0];
         input.attributes[1] = attributes[1];
         input.samples = sample_control || resolving ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
         input.resolve_output = resolving;
         struct ps5vk_triangle triangle = {0};

         enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
         if (status == PS5VK_TRIANGLE_OK)
            status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         check(status == PS5VK_TRIANGLE_OK,
               resolving ? "the four-sample frame and its resolve record, submit and signal "
                           "their fence"
                         : "a control frame records, submits and signals its fence");
         check(steps.failed == NULL, "no step of the frame failed before its fence");
         if (status == PS5VK_TRIANGLE_OK) {
            ++drawn;
            if (resolving) {
               /* The target is the four-sample one the driver sized: its memory
                * is four times the one-sample image's, sixteen bytes a texel. */
               check(triangle.resolve_image != VK_NULL_HANDLE &&
                        triangle.resolve_view != VK_NULL_HANDLE,
                     "the resolving frame rendered into a four-sample image of its own");
               check(triangle.target_bytes >=
                        (size_t)PS5VK_TRIANGLE_WIDTH * PS5VK_TRIANGLE_HEIGHT * 4u,
                     "the image the resolve writes is the program's one-sample one");
               check(triangle.samples == VK_SAMPLE_COUNT_4_BIT,
                     "the resolving frame's target is a four-sample one");
            }
         }
         if (status == PS5VK_TRIANGLE_IN_FLIGHT) {
            check(false, "a frame's submission did not complete");
            break;
         }
         ps5vk_triangle_finish(&triangle);
         if (status == PS5VK_TRIANGLE_FAILED)
            break;
      }
      check(drawn == 4, "all four frames recorded, submitted and signalled their fences");
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
