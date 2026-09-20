/*
 * PS5 Vulkan driver - D1's cubemaps, through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase D1 (docs/M5_REFERENCE.md). Where layer i of a tiled image lives is the
 * oracle's answer -- a slice is a chain of its own and the layers are
 * consecutive (tools/check-mip-layout.sh) -- and which descriptor field makes a
 * view read it is the register database's: SQ_IMG_RSRC_WORD4.DEPTH and
 * BASE_ARRAY, with word 3's TYPE a 2D array. The console's runner case
 * v0-cube-faces confirmed both (pid 153, Klog_Logs/d1-array-run2.log): two
 * bands of one frame read the two layers' colours with 0 pixels outside them.
 * This test is the PC arm, built and run through the loader and directly by
 * tools/check-driver.sh (see ps5vk_test.h): nothing renders on the PC, so it
 * checks that the per-layer uploads and the frame record, submit and signal
 * their fences, and tools/check-driver.sh compares the submission the host
 * layer records (PS5_HOST_SUBMISSION_DUMP) with the console's own run of the
 * runner case.
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

/* The six bands: position (x, y) then that face's direction, the 20-byte
 * records probes/v0-cube declares. Band f samples the f-th face in Vulkan's
 * order, +X, -X, +Y, -Y, +Z, -Z. */
#define D1_VERTEX_STRIDE 20
#define D1_VERTEX_COUNT 24
#define D1_INDEX_COUNT 36
#define D1_EXTENT 256

static const float kBandVertices[D1_VERTEX_COUNT * 5] = {
   -1.0f, -1.0f,       1.0f,  0.0f,  0.0f,  1.0f, -1.0f,       1.0f,  0.0f,  0.0f,
   1.0f,  -0.3333333f, 1.0f,  0.0f,  0.0f,  -1.0f, -0.3333333f, 1.0f,  0.0f,  0.0f,
   -1.0f, -0.3333333f, -1.0f, 0.0f,  0.0f,  1.0f, -0.3333333f,  -1.0f, 0.0f,  0.0f,
   1.0f,  0.3333333f,  -1.0f, 0.0f,  0.0f,  -1.0f, 0.3333333f,  -1.0f, 0.0f,  0.0f,
   -1.0f, 0.3333333f,  0.0f,  1.0f,  0.0f,  1.0f, 0.3333333f,   0.0f,  1.0f,  0.0f,
   1.0f,  1.0f,        0.0f,  1.0f,  0.0f,  -1.0f, 1.0f,        0.0f,  1.0f,  0.0f,
   -1.0f, 1.0f,        0.0f,  -1.0f, 0.0f,  1.0f, 1.0f,        0.0f,  -1.0f, 0.0f,
   1.0f,  1.0f,        0.0f,  0.0f,  1.0f,  -1.0f, 1.0f,       0.0f,  0.0f,  1.0f,
   -1.0f, 1.0f,        0.0f,  0.0f,  1.0f,  1.0f, 1.0f,        0.0f,  0.0f,  1.0f,
   1.0f,  1.0f,        0.0f,  0.0f,  -1.0f, -1.0f, 1.0f,       0.0f,  0.0f,  -1.0f,
   -1.0f, 1.0f,        0.0f,  0.0f,  -1.0f, 1.0f, 1.0f,        0.0f,  0.0f,  -1.0f,
};

static const uint16_t kBandIndices[D1_INDEX_COUNT] = {
   0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
   12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20,
};

/* The two layers' colours: layer 0 then layer 1, four bytes each, which is the
 * layer-major order the harness's staging documents. */
static const uint8_t kFaceColours[24] = {
   0x80, 0x10, 0x10, 0xff, 0x10, 0x80, 0x10, 0xff, 0x10, 0x10, 0x80, 0xff,
   0x80, 0x80, 0x10, 0xff, 0x80, 0x10, 0x80, 0xff, 0x10, 0x80, 0x80, 0xff};

int
main(void)
{
   test_begin("V0 cube faces");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "v0-cube", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "v0-cube", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the v0-cube SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8},
      };
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         kBandVertices, D1_VERTEX_COUNT, D1_VERTEX_STRIDE, kBandIndices, D1_INDEX_COUNT,
         2, {attributes[0], attributes[1]}, false, NULL, 0, 0,
      };
      input.texture_data = kFaceColours;
      input.texture_width = D1_EXTENT;
      input.texture_height = D1_EXTENT;
      input.texture_bilinear = false;
      input.texture_levels = 1;
      input.texture_layers = 6;
      input.texture_cube = true;
      input.texture_level_colours = kFaceColours;
      input.texture_max_lod = 0.0f;
      /* Six faces of one cube-compatible image, each uploaded by the driver's
       * own per-layer copy at the slice the oracle measured. */
      input.texture_tiled = true;
      input.texture_upload = true;
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the six per-face uploads and the frame record, submit and signal their fences");
      check(steps.failed == NULL, "no step of the upload or the draw failed before its fence");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
