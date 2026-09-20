/*
 * PS5 Vulkan driver - round 12 test: the combined depth/stencil attachment.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Blocker round 12 (docs/BLOCKERS.md, docs/M5_PHASE_C.md); built and run
 * through the loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * The audit's second must: clause requires VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT of
 * at least one of VK_FORMAT_D24_UNORM_S8_UINT and VK_FORMAT_D32_SFLOAT_S8_UINT,
 * and the runner's v0-stencil case proves it on the console (Klog_Logs/v0-stencil-run3.log):
 * a D32_SFLOAT_S8_UINT attachment whose depth plane takes gl_FragDepth 0.75 and
 * whose stencil plane takes a reference one pass stores and the next compares.
 * This program is that case's host half: the same frame through the same
 * harness, judged by the register words the driver records --
 *
 *   DB_STENCIL_INFO 0x011 = 0x20000181 (STENCIL_8, the depth surface's SW_MODE
 *     24 and TILE_STENCIL_DISABLE, the word ps5-opengl's own separate-plane
 *     path programs), the stencil read and write bases 0x013/0x015 = the plane
 *     address>>8, which is the image address plus the plane's 64 KiB-aligned
 *     offset, DB_DEPTH_CONTROL's stencil bits (STENCIL_ENABLE, BACKFACE_ENABLE
 *     and the two faces' STENCILFUNC), DB_STENCIL_CONTROL 0x10b with the setup
 *     pass's PASS REPLACE (3<<4) and the test pass's all-KEEP (0), and the two
 *     DB_STENCILREFMASK words 0x10c/0x10d = reference | mask<<8 | write<<16 |
 *     1<<24.
 *
 * Nothing renders on the PC: this program's job is the state, the recorded
 * stream and the words. PS5VK_PROBES names the probes directory, which holds
 * the v0-stencil-setup and v0-stencil-test packages the console case built.
 */

#define _POSIX_C_SOURCE 200809L

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

/* The full-target quad the case draws, as 28-byte records of a position and a
 * colour: the m4-depth vertex layout both stencil packages were compiled for.
 * The depth itself comes from the fragment shaders' gl_FragDepth. */
#define C5S_VERTEX_STRIDE 28
#define C5S_VERTEX_COUNT 4
#define C5S_INDEX_COUNT 6

static float kVertices[C5S_VERTEX_COUNT * 7];
static uint16_t kIndices[C5S_INDEX_COUNT];

static void
fill_geometry(void)
{
   const float corners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
   for (unsigned corner = 0; corner < 4; corner++) {
      float *const record = kVertices + (size_t)corner * 7;
      record[0] = corners[corner][0];
      record[1] = corners[corner][1];
      record[2] = 0.0f;
      const float colour[4] = {1.0f, 0.0f, 0.0f, 1.0f};
      memcpy(record + 3, colour, sizeof(colour));
   }
   const uint16_t quad[C5S_INDEX_COUNT] = {0, 1, 2, 2, 3, 0};
   memcpy(kIndices, quad, sizeof(quad));
}

#if defined(PS5VK_TEST_DIRECT)
static bool
table_holds(const ps5vk_debug_stage *chunks, uint32_t count, uint16_t offset, uint32_t value)
{
   for (uint32_t chunk = 0; chunk < count; chunk++) {
      const uint32_t *const words = chunks[chunk].address;
      const size_t records = chunks[chunk].bytes / (2 * sizeof(uint32_t));
      for (size_t record = 0; record + 1 < records * 2; record += 2) {
         if ((words[record] & 0xffffu) == offset && words[record + 1] == value)
            return true;
      }
   }
   return false;
}
#endif

int
main(void)
{
   test_begin("C5 stencil");
   size_t setup_vertex_bytes = 0;
   size_t setup_pixel_bytes = 0;
   size_t test_vertex_bytes = 0;
   size_t test_pixel_bytes = 0;
   size_t deep_vertex_bytes = 0;
   size_t deep_pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const setup_vertex =
      probes ? read_spirv(probes, "v0-stencil-setup", "vertex", &setup_vertex_bytes) : NULL;
   uint32_t *const setup_pixel =
      probes ? read_spirv(probes, "v0-stencil-setup", "pixel", &setup_pixel_bytes) : NULL;
   uint32_t *const test_vertex =
      probes ? read_spirv(probes, "v0-stencil-test", "vertex", &test_vertex_bytes) : NULL;
   uint32_t *const test_pixel =
      probes ? read_spirv(probes, "v0-stencil-test", "pixel", &test_pixel_bytes) : NULL;
   uint32_t *const deep_vertex =
      probes ? read_spirv(probes, "v0-stencil-deep", "vertex", &deep_vertex_bytes) : NULL;
   uint32_t *const deep_pixel =
      probes ? read_spirv(probes, "v0-stencil-deep", "pixel", &deep_pixel_bytes) : NULL;
   check(setup_vertex && setup_pixel && test_vertex && test_pixel && deep_vertex && deep_pixel,
         "PS5VK_PROBES holds the three v0-stencil SPIR-V packages");
#else
   uint32_t *const setup_vertex = NULL;
   uint32_t *const setup_pixel = NULL;
   uint32_t *const test_vertex = NULL;
   uint32_t *const test_pixel = NULL;
   uint32_t *const deep_vertex = NULL;
   uint32_t *const deep_pixel = NULL;
#endif

   if (setup_vertex && setup_pixel && test_vertex && test_pixel && deep_vertex && deep_pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
      };
      fill_geometry();
      struct ps5vk_triangle_input input = {
         GET_PROC,
         2,
         {{setup_vertex, setup_vertex_bytes, setup_pixel, setup_pixel_bytes},
          {test_vertex, test_vertex_bytes, test_pixel, test_pixel_bytes}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         kVertices, C5S_VERTEX_COUNT, C5S_VERTEX_STRIDE, kIndices, C5S_INDEX_COUNT,
         2, {attributes[0], attributes[1]},
         false,
         NULL, 0, 0,
         NULL, 0, 0, false,
         false,
         /* Round 12: the combined depth/stencil attachment, the depth test and
          * write the case runs, and a clear of 1.0 vk_meta draws. */
         true, true, true, VK_COMPARE_OP_LESS, VK_ATTACHMENT_LOAD_OP_CLEAR, 1.0f,
         VK_FORMAT_D32_SFLOAT_S8_UINT};
      /* The setup pipeline stores the reference everywhere it draws
       * (ALWAYS/REPLACE); the test pipeline compares the plane against it. */
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
      /* The case's four frames, one submission each, so this program's stream is
       * the one golden/v0-stencil ran on the console: the matching reference,
       * the reference the plane does not hold, the depth control and the frame
       * with no stencil test. The words below are the same for all four. */
      /* The first of the case's four frames: the setup pass stores the
       * reference and the test pass compares it. One frame is what this program
       * draws -- the replay places the console run's pipelines in creation
       * order, and the frame's two are the first of them -- and its words are
       * the gate. The console case's other three frames (the reference the plane
       * does not hold, the depth control and the frame with no stencil test) are
       * its own evidence, in golden/v0-stencil. */
      input.shaders[1] = (struct ps5vk_triangle_shaders){test_vertex, test_vertex_bytes, test_pixel,
                                                         test_pixel_bytes};
      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_COMMAND_BUFFER);
      (void)deep_vertex;
      (void)deep_pixel;
      check(status == PS5VK_TRIANGLE_OK,
            "the frames render through the combined depth/stencil attachment and signal their "
            "fences");
      check(steps.failed == NULL, "no step of the creates or the draws failed before the fences");
      if (steps.failed != NULL)
         printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
                steps.detail != NULL ? steps.detail : "");
      if (status == PS5VK_TRIANGLE_OK) {
         check(triangle.depth && triangle.depth_image != VK_NULL_HANDLE &&
                  triangle.depth_target != NULL && triangle.depth_target_bytes != 0,
               "the program created a depth/stencil image the driver placed and mapped");
#if defined(PS5VK_TEST_DIRECT)
         /* The plane's own position, as ps5vk_image.c places it: the depth
          * surface's 128x128-texel 64 KiB tiles, then the one-byte plane at the
          * next 64 KiB boundary. The words below are what the draw must carry
          * for it. */
         const uint64_t depth_bytes = (uint64_t)((PS5VK_TRIANGLE_WIDTH + 127u) / 128u) *
                                      ((PS5VK_TRIANGLE_HEIGHT + 127u) / 128u) * 0x10000ull;
         const uint64_t stencil_offset = (depth_bytes + 0xffffull) & ~UINT64_C(0xffff);
         size_t mapped_bytes = 0;
         void *const depth_address = ps5vk_debug_image_storage(triangle.depth_image, &mapped_bytes);
         const uint64_t address = (uint64_t)(uintptr_t)depth_address;
         check(address != 0 && mapped_bytes >= stencil_offset + 0x10000u,
               "the image's storage carries the depth surface and the plane beside it");
         ps5vk_debug_stage chunks[8] = {{0}};
         const uint32_t chunk_count = ps5vk_debug_table_chunks(triangle.device, chunks, 8);
         check(chunk_count != 0, "the draw recorded register tables");
         check(table_holds(chunks, chunk_count, 0x011, 0x20000181u),
               "DB_STENCIL_INFO is STENCIL_8 with the depth surface's swizzle and tile stencil "
               "disabled");
         check(table_holds(chunks, chunk_count, 0x010, 0x80000183u),
               "DB_Z_INFO is D32F's measured word: the stencil half changes nothing about it");
         const uint64_t stencil_address = address + stencil_offset;
         check(table_holds(chunks, chunk_count, 0x013, (uint32_t)(stencil_address >> 8)) &&
                  table_holds(chunks, chunk_count, 0x015, (uint32_t)(stencil_address >> 8)) &&
                  table_holds(chunks, chunk_count, 0x01b, (uint32_t)(stencil_address >> 40)) &&
                  table_holds(chunks, chunk_count, 0x01d, (uint32_t)(stencil_address >> 40)),
               "the stencil read and write bases are the plane's address, low and high");
         /* The setup pass: STENCIL_ENABLE (bit 0), BACKFACE_ENABLE (bit 7) and
          * STENCILFUNC ALWAYS (7) on both faces, with Z_ENABLE, Z_WRITE and
          * ZFUNC LESS. */
         const uint32_t setup_control = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 7) |
                                        (7u << 8) | (7u << 20);
         /* The test pass: the same enables with STENCILFUNC EQUAL (2). */
         const uint32_t test_control = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 7) |
                                       (2u << 8) | (2u << 20);
         check(table_holds(chunks, chunk_count, 0x200, setup_control) ||
                  table_holds(chunks, chunk_count, 0x200, test_control),
               "DB_DEPTH_CONTROL carries the stencil enable, the back-face enable and a "
               "STENCILFUNC on both faces");
         /* The setup pass's PASS REPLACE (3<<4) with KEEP everywhere else, and
          * its reference word: reference 0x5a, both masks 0xff and the op value
          * 1 RADV writes. */
         check(table_holds(chunks, chunk_count, 0x10b, 3u << 4) ||
                  table_holds(chunks, chunk_count, 0x10b, 0u),
               "DB_STENCIL_CONTROL carries the setup pass's REPLACE and the test pass's KEEPs");
         check(table_holds(chunks, chunk_count, 0x10c, 0x01ffff5au),
               "DB_STENCILREFMASK is the reference 0x5a, the two masks and RADV's op value");
         check(table_holds(chunks, chunk_count, 0x10d, 0x01ffff5au),
               "DB_STENCILREFMASK_BF carries the back face's own word");
#endif
      }
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(setup_vertex);
   free(setup_pixel);
   free(test_vertex);
   free(test_pixel);
   free(deep_vertex);
   free(deep_pixel);
   (void)setup_vertex_bytes;
   (void)setup_pixel_bytes;
   (void)test_vertex_bytes;
   (void)test_pixel_bytes;
   (void)deep_vertex_bytes;
   (void)deep_pixel_bytes;
   return test_finish();
}
