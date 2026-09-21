/*
 * PS5 Vulkan driver - R1's depth bias test: the six PA_SU_POLY_OFFSET_* words.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R1 (docs/M5_PHASE_C.md, PS5_VULKAN_REQUESTS.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * The runner's v0-depth-bias case proves the state on the console: every frame
 * clears its D32_SFLOAT depth attachment to 0.5 and draws geometry that starts at
 * that same depth, so the unbiased frame fails LESS against the equal clear and
 * keeps no pixel, while each sign of the constant factor and the slope factor
 * move fragments through the test and the depth plane carries what they wrote.
 * This program is that case's host half: one frame through the same harness and
 * the same m4-depth package, judged by the register words the driver records --
 *
 *   PA_SU_POLY_OFFSET_DB_FMT_CNTL 0x2de = 0x000001e9, ps5-opengl's GFX10 D32F
 *     word: -23 in the signed low byte's POLY_OFFSET_NEG_NUM_DB_BITS and
 *     POLY_OFFSET_DB_IS_FLOAT_FMT set,
 *   PA_SU_POLY_OFFSET_CLAMP 0x2df = depthBiasClamp's float bits,
 *   PA_SU_POLY_OFFSET_FRONT_SCALE 0x2e0 and BACK_SCALE 0x2e2 = the slope
 *     factor times sixteen, as ps5-opengl scales it, and
 *   PA_SU_POLY_OFFSET_FRONT_OFFSET 0x2e1 and BACK_OFFSET 0x2e3 = the constant
 *     factor's float bits, the back pair mirroring the front's.
 *
 * The frame's depth words are the ones the C5 frame records -- DB_DEPTH_CONTROL
 * 0x200 = LESS with the test and the write, DB_Z_INFO D32F's word -- so the
 * block is the only difference this state makes, and a frame without the bias
 * records none of the six (the case's own golden, golden/v0-depth-bias, is where
 * the console run shows that: its first frame is the unbiased one).
 * PS5VK_PROBES names the probes directory, which holds the m4-depth package.
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

/* The m4-depth geometry: 28-byte records of a position (x, y, z) and a colour
 * (r, g, b, a), and two triangles per axis-aligned rectangle. The C5 depth test
 * holds the same records, and src/diagnostics.cpp's v0-depth-bias case draws
 * them at the depth its attachment clears to. */
#define C5_VERTEX_STRIDE 28
#define C5_VERTEX_COUNT 8
#define C5_INDEX_COUNT 12

static float kVertices[C5_VERTEX_COUNT * 7];
static uint16_t kIndices[C5_INDEX_COUNT];

/* One rectangle of target pixels (right and bottom exclusive) as four vertex
 * records starting at vertex and six indices starting at index, at depth, in an
 * 8-bit colour. */
static void
put_rect(uint32_t vertex, uint32_t index, uint32_t left, uint32_t top, uint32_t right,
         uint32_t bottom, float depth, uint32_t red, uint32_t green, uint32_t blue)
{
   const float ndc_x_left = (float)left / (float)(PS5VK_TRIANGLE_WIDTH / 2) - 1.0f;
   const float ndc_x_right = (float)right / (float)(PS5VK_TRIANGLE_WIDTH / 2) - 1.0f;
   const float ndc_y_top = 1.0f - (float)top / (float)(PS5VK_TRIANGLE_HEIGHT / 2);
   const float ndc_y_bottom = 1.0f - (float)bottom / (float)(PS5VK_TRIANGLE_HEIGHT / 2);
   const float colour[4] = {red / 255.0f, green / 255.0f, blue / 255.0f, 1.0f};
   const float corners[4][2] = {{ndc_x_left, ndc_y_bottom},
                                {ndc_x_right, ndc_y_bottom},
                                {ndc_x_right, ndc_y_top},
                                {ndc_x_left, ndc_y_top}};
   for (unsigned corner = 0; corner < 4; corner++) {
      float *const record = kVertices + (size_t)(vertex + corner) * 7;
      record[0] = corners[corner][0];
      record[1] = corners[corner][1];
      record[2] = depth;
      memcpy(record + 3, colour, sizeof(colour));
   }
   const uint16_t quad[C5_INDEX_COUNT / 2] = {(uint16_t)vertex, (uint16_t)(vertex + 1),
                                             (uint16_t)(vertex + 2), (uint16_t)(vertex + 2),
                                             (uint16_t)(vertex + 3), (uint16_t)vertex};
   memcpy(kIndices + index, quad, sizeof(quad));
}

/* The two quads the C5 depth frame draws, at the depths its golden reads back:
 * the near one at 0.25 and the far one at 0.75 over a clear of 1.0. The bias
 * this test measures does not depend on them -- the recorded words are pipeline
 * and attachment state -- so the frame stays the C5 one and the words below are
 * the only difference. */
static void
fill_geometry(void)
{
   put_rect(0, 0, 960, 540, 2880, 1620, 0.25f, 0xff, 0x00, 0x00);
   put_rect(4, 6, 480, 270, 1920, 1080, 0.75f, 0x00, 0xff, 0x00);
}

/* Whether a register table a draw recorded holds offset = value, and whether it
 * names the offset at all, which is how a capture sees the words: the tables
 * live in the driver's chunks (ps5vk_debug_table_chunks). Only the direct build
 * reads them: the loader build sees the driver through its ICD, which exports no
 * debug symbol (driver/ps5vk_icd.map). */
#if defined(PS5VK_TEST_DIRECT)
static bool
table_holds(const ps5vk_debug_stage *chunks, uint32_t count, uint16_t offset, uint32_t value)
{
   for (uint32_t chunk = 0; chunk < count; chunk++) {
      const uint32_t *const words = chunks[chunk].address;
      const size_t records = chunks[chunk].bytes / (2 * sizeof(uint32_t));
      for (size_t record = 0; record < records; record++) {
         if ((words[2 * record] & 0xffffu) == offset && words[2 * record + 1] == value)
            return true;
      }
   }
   return false;
}

#endif

int
main(void)
{
   test_begin("C5 depth bias");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
   uint32_t *const vertex = probes ? read_spirv(probes, "m4-depth", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m4-depth", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m4-depth SPIR-V");
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
         {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
      };
      fill_geometry();
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         kVertices, C5_VERTEX_COUNT, C5_VERTEX_STRIDE, kIndices, C5_INDEX_COUNT,
         2, {attributes[0], attributes[1]},
         false,
         NULL, 0, 0,
         NULL, 0, 0, false,
         false,
         /* The depth attachment the C5 frame renders through: the test and the
          * write with LESS, and a clear of 1.0 vk_meta draws. */
         true, true, true, VK_COMPARE_OP_LESS, VK_ATTACHMENT_LOAD_OP_CLEAR, 1.0f,
      };
      /* R1's depth bias, the frames the console case runs: a constant factor
       * toward the viewer, a slope factor, and a clamp small enough that the
       * driver has to cap the constant term itself -- the hardware's own clamp
       * word is inert on this path, which the console run measured
       * (Klog_Logs/r-depth-bias5.log). 4096 units is 4096 * 2^-23 = 4.88e-4 of
       * depth, so a 1e-4 clamp caps it to 1e-4 * 2^23 = 838.8608 units. */
      input.depth_bias_enable = true;
      input.depth_bias_constant = 4096.0f;
      input.depth_bias_slope = 2.0f;
      input.depth_bias_clamp = 1e-4f;
      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the depth-biased frame renders through its depth attachment and signals its fence");
      check(steps.failed == NULL, "no step of the create or the draw failed before the fence");
      if (steps.failed != NULL)
         printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
                steps.detail != NULL ? steps.detail : "");
      if (status == PS5VK_TRIANGLE_OK) {
         check(triangle.depth && triangle.depth_image != VK_NULL_HANDLE &&
                  triangle.depth_target != NULL && triangle.depth_target_bytes != 0,
               "the program created, mapped and reported a depth attachment");
#if defined(PS5VK_TEST_DIRECT)
         size_t depth_bytes = 0;
         void *const depth_address = ps5vk_debug_image_storage(triangle.depth_image, &depth_bytes);
         const uint64_t address = (uint64_t)(uintptr_t)depth_address;
         ps5vk_debug_stage chunks[8] = {{0}};
         const uint32_t chunk_count = ps5vk_debug_table_chunks(triangle.device, chunks, 8);
         check(address != 0 && chunk_count != 0 && depth_bytes != 0,
               "the depth image has an address and the draw recorded register tables");
         /* The C5 frame's own words first: the bias adds the block below and
          * changes nothing else about the depth state. */
         check(table_holds(chunks, chunk_count, 0x200, 0x16u),
               "DB_DEPTH_CONTROL is still the C5 frame's LESS with test and write enabled");
         check(table_holds(chunks, chunk_count, 0x010, 0x80000183u),
               "DB_Z_INFO is still D32F's measured word");
         /* The block's own enables in the rasterizer word: cull none with
          * Vulkan's counter-clockwise front face leaves it at
          * POLY_OFFSET_FRONT_ENABLE (bit 11) and POLY_OFFSET_BACK_ENABLE
          * (bit 12), which the six words below need before the hardware applies
          * them at all. */
         check(table_holds(chunks, chunk_count, 0x205, 0x1800u),
               "PA_SU_SC_MODE_CNTL carries the polygon offset enables and nothing else");
         check(table_holds(chunks, chunk_count, 0x2de, 0x000001e9u),
               "PA_SU_POLY_OFFSET_DB_FMT_CNTL is the D32 float word ps5-opengl writes");
         check(table_holds(chunks, chunk_count, 0x2e1, 0x4451b717u) &&
                  table_holds(chunks, chunk_count, 0x2e3, 0x4451b717u),
               "the front and back offsets hold the constant factor's bits, capped by the clamp");
         /* The clamp itself, and that capping the constant left the scale alone:
          * the slope term's gradient is the polygon's, so the driver's cap can
          * only cover this half (ps5vk_pipeline.c). */
         check(table_holds(chunks, chunk_count, 0x2df, 0x38d1b717u),
               "PA_SU_POLY_OFFSET_CLAMP holds the clamp's float bits");
         check(table_holds(chunks, chunk_count, 0x2e0, 0x42000000u) &&
                  table_holds(chunks, chunk_count, 0x2e2, 0x42000000u),
               "the scales are still the slope factor's bits, times sixteen");
#endif
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
