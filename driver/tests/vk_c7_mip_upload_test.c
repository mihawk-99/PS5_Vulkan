/*
 * PS5 Vulkan driver - Phase C7: the tiled mip chain the driver uploads itself.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C7 (docs/M5_REFERENCE.md). The driver used to refuse "a
 * copy into an image stored in 64 KiB tiles": the console's c7-mip-tiled case
 * proved the chain's layout with the probe writing the tiles itself, and the
 * runner's c7-mip-upload case has the driver carry them in, one
 * vkCmdCopyBufferToImage a level out of the harness's mapped staging buffer,
 * placed at the level bases the pinned AddrLib's rule measured (pid 147,
 * Klog_Logs/c7-mip-upload-run1.log). This test is the PC arm, built and run
 * through the loader and directly by tools/check-driver.sh (see ps5vk_test.h):
 * nothing renders on the PC, so it checks that the upload copies and the six
 * frames record, submit and signal their fences, and tools/check-driver.sh
 * compares the submission the host layer records (PS5_HOST_SUBMISSION_DUMP)
 * with the console's own run of the runner case.
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

/* Phase C7's chain, as the runner builds it: five levels of 256, 128, 64, 32
 * and 16 texels, one solid grey each (src/diagnostics.cpp, kMipLevelGreys). */
#define C7_LEVELS 5
#define C7_EXTENT 256
static const uint8_t kLevelGreys[C7_LEVELS] = {10, 100, 200, 40, 60};

static const float kBandVertices[C7_LEVELS * 4 * 4] = {
   /* band 0 */ -1.0f, 0.6f, 0.0f, 0.0f, 1.0f, 0.6f, 1.0f, 0.0f,
   1.0f, 1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
   /* band 1 */ -1.0f, 0.2f, 0.0f, 0.0f, 1.0f, 0.2f, 1.0f, 0.0f,
   1.0f, 0.6f, 1.0f, 1.0f, -1.0f, 0.6f, 0.0f, 1.0f,
   /* band 2 */ -1.0f, -0.2f, 0.0f, 0.0f, 1.0f, -0.2f, 1.0f, 0.0f,
   1.0f, 0.2f, 1.0f, 1.0f, -1.0f, 0.2f, 0.0f, 1.0f,
   /* band 3 */ -1.0f, -0.6f, 0.0f, 0.0f, 1.0f, -0.6f, 1.0f, 0.0f,
   1.0f, -0.2f, 1.0f, 1.0f, -1.0f, -0.2f, 0.0f, 1.0f,
   /* band 4 */ -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
   1.0f, -0.6f, 1.0f, 1.0f, -1.0f, -0.6f, 0.0f, 1.0f,
};

static const uint16_t kBandIndices[C7_LEVELS * 6] = {
   0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10,
   10, 11, 8,  12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16,
};

int
main(void)
{
   test_begin("C7 tiled upload");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "c7-mip", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "c7-mip", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the c7-mip SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
      };
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_CLEAR, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         kBandVertices, C7_LEVELS * 4, 16, kBandIndices, C7_LEVELS * 6,
         2, {attributes[0], attributes[1]}, false, NULL, 0, 0,
      };
      input.texture_data = kLevelGreys;
      input.texture_width = C7_EXTENT;
      input.texture_height = C7_EXTENT;
      input.texture_bilinear = false;
      input.texture_levels = C7_LEVELS;
      input.texture_level_colours = kLevelGreys;
      input.texture_mip_linear = false;
      input.texture_max_lod = (float)(C7_LEVELS - 1);
      /* The chain the driver fills: TRANSFER_DST on the image and one
       * vkCmdCopyBufferToImage a level in the frame's upload command buffer. */
      input.texture_tiled = true;
      input.texture_upload = true;
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      for (uint32_t frame = 0; frame < C7_LEVELS + 1 && status == PS5VK_TRIANGLE_OK; frame++) {
         if (frame > 0 && !ps5vk_triangle_set_texture_lod(&triangle, (float)(frame - 1),
                                                          (float)(frame - 1)))
            status = PS5VK_TRIANGLE_FAILED;
         else
            status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      }
      check(status == PS5VK_TRIANGLE_OK,
            "the upload copies and the six frames record, submit and signal their fences");
      check(steps.failed == NULL, "no step of the upload or a frame failed before its fence");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
