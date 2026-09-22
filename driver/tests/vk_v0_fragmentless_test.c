/*
 * PS5 Vulkan driver - the fragment-less pipeline.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * docs/M5_PHASE_C.md, the fragment-less pipeline; built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * A graphics pipeline whose only stage is the vertex stage: the driver links it
 * with an empty fragment shader of its own (ps5vk_nir_noop_fragment) and writes no
 * colour. The runner's v0-fragmentless case proves the frame on the console: the
 * pass stores the stencil reference and a depth of 0.5 where it rasterises with the
 * colour target left exactly the clear, and v0-stencil-test's green then passes its
 * stencil test only there. This program is that case's second frame on the host,
 * through the same harness and the same two packages -- v0-stencil-setup's vertex
 * stage alone for the first pipeline, v0-stencil-test whole for the second -- and
 * judged by the words the driver records for the fragment-less draw:
 *
 *   CB_TARGET_MASK 0x08e and CB_SHADER_MASK 0x08f = 0: no colour, whatever the
 *     pipeline's blend attachment asked for;
 *   DB_DEPTH_CONTROL 0x200 = the depth test and write with LESS and the stencil
 *     test ALWAYS on both faces, DB_STENCIL_CONTROL 0x10b = PASS REPLACE on both
 *     faces, and the two DB_STENCILREFMASK words = the reference 0x5a with both
 *     masks -- the words v0-stencil's setup pass records with a fragment stage.
 *
 * PS5VK_PROBES names the probes directory. Nothing renders on the PC.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

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
static uint32_t *
read_spirv(const char *directory, const char *set, const char *stage, size_t *bytes)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/%s/%s.spv", directory, set, stage);
   FILE *const file = fopen(path, "rb");
   if (file == NULL) {
      *bytes = 0;
      return NULL;
   }
   fseek(file, 0, SEEK_END);
   const long length = ftell(file);
   fseek(file, 0, SEEK_SET);
   uint32_t *const words = length > 0 ? malloc((size_t)length) : NULL;
   if (words != NULL && fread(words, 1, (size_t)length, file) != (size_t)length) {
      free(words);
      fclose(file);
      *bytes = 0;
      return NULL;
   }
   fclose(file);
   *bytes = words ? (size_t)length : 0;
   return words;
}
#endif

/* The case's geometry as 28-byte records of a position and a colour: the middle
 * quarter at depth 0.5 for the fragment-less pass's six indices, then the whole
 * target at 0.25 for the test pass's. */
#define FL_VERTEX_STRIDE 28
#define FL_VERTEX_COUNT 8
#define FL_INDEX_COUNT 12

static float kVertices[FL_VERTEX_COUNT * 7];
static uint16_t kIndices[FL_INDEX_COUNT];

static void
put_rect(uint32_t vertex, uint32_t index, float left, float top, float right, float bottom,
         float depth)
{
   const float corners[4][2] = {{left, bottom}, {right, bottom}, {right, top}, {left, top}};
   for (unsigned corner = 0; corner < 4; corner++) {
      float *const record = kVertices + (size_t)(vertex + corner) * 7;
      record[0] = corners[corner][0];
      record[1] = corners[corner][1];
      record[2] = depth;
      const float colour[4] = {1.0f, 0.0f, 0.0f, 1.0f};
      memcpy(record + 3, colour, sizeof(colour));
   }
   const uint16_t quad[6] = {(uint16_t)vertex,       (uint16_t)(vertex + 1),
                             (uint16_t)(vertex + 2), (uint16_t)(vertex + 2),
                             (uint16_t)(vertex + 3), (uint16_t)vertex};
   memcpy(kIndices + index, quad, sizeof(quad));
}

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
   test_begin("V0 fragment-less");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t setup_vertex_bytes = 0;
   size_t test_vertex_bytes = 0;
   size_t test_pixel_bytes = 0;
   uint32_t *const setup_vertex =
      probes ? read_spirv(probes, "v0-stencil-setup", "vertex", &setup_vertex_bytes) : NULL;
   uint32_t *const test_vertex =
      probes ? read_spirv(probes, "v0-stencil-test", "vertex", &test_vertex_bytes) : NULL;
   uint32_t *const test_pixel =
      probes ? read_spirv(probes, "v0-stencil-test", "pixel", &test_pixel_bytes) : NULL;
   check(setup_vertex && test_vertex && test_pixel,
         "PS5VK_PROBES holds the v0-stencil-setup vertex and v0-stencil-test SPIR-V");
#else
   uint32_t *const setup_vertex = NULL;
   uint32_t *const test_vertex = NULL;
   uint32_t *const test_pixel = NULL;
   size_t setup_vertex_bytes = 0;
   size_t test_vertex_bytes = 0;
   size_t test_pixel_bytes = 0;
#endif

   if (setup_vertex && test_vertex && test_pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      put_rect(0, 0, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f);
      put_rect(4, 6, -1.0f, 1.0f, 1.0f, -1.0f, 0.25f);
      struct ps5vk_triangle_input input = {0};
      input.get_instance_proc_addr = GET_PROC;
      input.pipeline_count = 2;
      /* The first pipeline has no fragment stage: no pixel SPIR-V. */
      input.shaders[0] =
         (struct ps5vk_triangle_shaders){setup_vertex, setup_vertex_bytes, NULL, 0};
      input.shaders[1] = (struct ps5vk_triangle_shaders){test_vertex, test_vertex_bytes,
                                                         test_pixel, test_pixel_bytes};
      input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
      input.report = &report;
      input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
      input.vertex_data = kVertices;
      input.vertex_count = FL_VERTEX_COUNT;
      input.vertex_stride = FL_VERTEX_STRIDE;
      input.index_data = kIndices;
      input.index_count = FL_INDEX_COUNT;
      input.first_draw_indices = 6;
      input.attribute_count = 2;
      input.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
      input.attributes[1] =
         (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12};
      input.depth = true;
      input.depth_test = true;
      input.depth_write = true;
      input.depth_compare_op = VK_COMPARE_OP_LESS;
      input.depth_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
      input.depth_clear_value = 1.0f;
      input.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
      input.stencil_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
      input.stencil[0] = (struct ps5vk_stencil_state){
         .test = true,
         .fail_op = VK_STENCIL_OP_KEEP,
         .pass_op = VK_STENCIL_OP_REPLACE,
         .depth_fail_op = VK_STENCIL_OP_KEEP,
         .compare_op = VK_COMPARE_OP_ALWAYS,
         .reference = 0x5au,
         .compare_mask = 0xffu,
         .write_mask = 0xffu,
      };
      input.stencil[1] = (struct ps5vk_stencil_state){
         .test = true,
         .fail_op = VK_STENCIL_OP_KEEP,
         .pass_op = VK_STENCIL_OP_KEEP,
         .depth_fail_op = VK_STENCIL_OP_KEEP,
         .compare_op = VK_COMPARE_OP_EQUAL,
         .reference = 0x5au,
         .compare_mask = 0xffu,
         .write_mask = 0xffu,
      };
      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      check(status == PS5VK_TRIANGLE_OK,
            "a pipeline with the vertex stage alone is created beside one with both stages");
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_COMMAND_BUFFER);
      check(status == PS5VK_TRIANGLE_OK,
            "the fragment-less draw and the stencil test's draw record, submit and signal");
      check(steps.failed == NULL, "no step of the creates or the draws failed before the fence");
      if (steps.failed != NULL)
         printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
                steps.detail != NULL ? steps.detail : "");
#if defined(PS5VK_TEST_DIRECT)
      if (status == PS5VK_TRIANGLE_OK) {
         ps5vk_debug_stage chunks[8] = {{0}};
         const uint32_t chunk_count = ps5vk_debug_table_chunks(triangle.device, chunks, 8);
         check(chunk_count != 0, "the draws recorded register tables");
         /* Only a pipeline whose mask is not RGBA records the two words, so the
          * zeros are the fragment-less draw's: the test pass writes RGBA. */
         check(table_holds(chunks, chunk_count, 0x08e, 0u) &&
                  table_holds(chunks, chunk_count, 0x08f, 0u),
               "CB_TARGET_MASK and CB_SHADER_MASK are 0: the fragment-less draw writes no "
               "colour");
         /* STENCIL_ENABLE (bit 0), Z_ENABLE, Z_WRITE_ENABLE, ZFUNC LESS (1 << 4),
          * BACKFACE_ENABLE (bit 7) and STENCILFUNC ALWAYS (7) on both faces --
          * v0-stencil's setup word, from a pipeline with no fragment stage. */
         const uint32_t mark_control = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) |
                                       (1u << 7) | (7u << 8) | (7u << 20);
         check(table_holds(chunks, chunk_count, 0x200, mark_control),
               "DB_DEPTH_CONTROL tests and writes depth with LESS and stencil ALWAYS on both "
               "faces");
         /* PASS REPLACE (3) on the front face (bits 4-7) and the back face's twin
          * (bits 16-19), every other operation KEEP. */
         check(table_holds(chunks, chunk_count, 0x10b, (3u << 4) | (3u << 16)),
               "DB_STENCIL_CONTROL stores the reference where the pass rasterises (PASS "
               "REPLACE)");
         check(table_holds(chunks, chunk_count, 0x10c, 0x01ffff5au) &&
                  table_holds(chunks, chunk_count, 0x10d, 0x01ffff5au),
               "both DB_STENCILREFMASK words carry the reference 0x5a and both masks");
      }
#endif
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(setup_vertex);
   free(test_vertex);
   free(test_pixel);
   (void)setup_vertex_bytes;
   (void)test_vertex_bytes;
   (void)test_pixel_bytes;
   return test_finish();
}
