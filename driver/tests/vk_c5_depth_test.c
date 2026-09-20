/*
 * PS5 Vulkan driver - Phase C5 test: a depth attachment, depth test and write.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C5 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * The tutorial's depth program: a D32_SFLOAT attachment the frame renders
 * through, two overlapping quads, and the depth test that decides which one
 * wins where they overlap. The m4-depth shaders draw them (28-byte records of
 * x, y, z, r, g, b, a), the same set the M4 step 1 canary drew its shapes with,
 * so every register this test is judged by has a recorded value on the console:
 * the driver programs DB_Z_INFO 0x80000183 (D32F in 64 KiB tiles),
 * DB_DEPTH_SIZE_XY from the target, the Z read and write bases from the
 * depth image's address, and DB_DEPTH_CONTROL 0x16 for depth test and write
 * with LESS -- exactly the words the canary's clear triangle and rectangles ran
 * (docs/HARDWARE_FINDINGS.md, M4 step 1).
 *
 * The near quad draws first and the far one second, both with LESS, so the far
 * quad's fragments over the overlap have to be rejected: the overlap holds the
 * near colour, which is what the console reads back, together with the depth
 * the near quad wrote there and the clear value outside both quads. Nothing
 * renders on the PC, so this program's job is the stream and the state: that
 * the frame records, submits and signals its fence, that the depth attachment
 * was created and mapped, and that the draw's table carries the canary's depth
 * registers. PS5VK_PROBES names the probes directory.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

/* The driver's own debug API: the register tables a draw records, which is
 * where the depth registers are (src/diagnostics.cpp reads the same tables). */
#include "../ps5vk_debug.h"
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

/* The m4-depth geometry: 28-byte records of a position (x, y, z) and a colour
 * (r, g, b, a), and two triangles per axis-aligned rectangle. src/diagnostics.cpp
 * holds the same records as its put_rect and scene vertices, and its readback
 * check is what accepts the frame on the console. */
#define C5_VERTEX_STRIDE 28
#define C5_VERTEX_COUNT 8
#define C5_INDEX_COUNT 12

static float kVertices[C5_VERTEX_COUNT * 7];
static uint16_t kIndices[C5_INDEX_COUNT];

/* One rectangle of target pixels (right and bottom exclusive) as four vertex
 * records starting at vertex and six indices starting at index, at depth, in an
 * 8-bit colour. Edges fall on pixel boundaries, and the depths are exact binary
 * fractions, so the readback compares exactly. */
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

/* The task pixels of the frame: the near quad, the far quad, and where they
 * overlap (the near one wins there). */
#define C5_NEAR_LEFT 960
#define C5_NEAR_TOP 540
#define C5_NEAR_RIGHT 2880
#define C5_NEAR_BOTTOM 1620
#define C5_FAR_LEFT 480
#define C5_FAR_TOP 270
#define C5_FAR_RIGHT 1920
#define C5_FAR_BOTTOM 1080

static void
fill_geometry(void)
{
   /* The near quad first at z = 0.25, the far one second at z = 0.75: the far
    * quad fails the depth test over their overlap. */
   put_rect(0, 0, C5_NEAR_LEFT, C5_NEAR_TOP, C5_NEAR_RIGHT, C5_NEAR_BOTTOM, 0.25f, 0xff, 0x00,
            0x00);
   put_rect(4, 6, C5_FAR_LEFT, C5_FAR_TOP, C5_FAR_RIGHT, C5_FAR_BOTTOM, 0.75f, 0x00, 0xff, 0x00);
}

/* Whether a register table a draw recorded holds offset = value, which is how
 * the console's capture sees the depth registers (the tables live in the
 * driver's chunks, ps5vk_debug_table_chunks). Only the direct build reads them:
 * the loader build sees the driver through its ICD, which exports no debug
 * symbol (driver/ps5vk_icd.map). */
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

/* Whether the recorded tables name a register at all, whatever its value: the
 * check round 12's depth-only frame needs, where a stencil-testing pipeline must
 * leave the three stencil state registers unrecorded because the attachment it
 * renders through carries no stencil plane. */
static bool
table_names(const ps5vk_debug_stage *chunks, uint32_t count, uint16_t offset)
{
   for (uint32_t chunk = 0; chunk < count; chunk++) {
      const uint32_t *const words = chunks[chunk].address;
      const size_t records = chunks[chunk].bytes / (2 * sizeof(uint32_t));
      for (size_t record = 0; record + 1 < records * 2; record += 2) {
         if ((words[record] & 0xffffu) == offset)
            return true;
      }
   }
   return false;
}
#endif

int
main(void)
{
   test_begin("C5 depth");
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
      fill_geometry();
      /* Round 12's groundwork: the pipeline enables the stencil test. The
       * attachment is D32_SFLOAT, which carries no stencil plane, so Vulkan
       * ignores the test and the driver must record exactly the stream the
       * console's own depth frame did -- which is what the golden comparison
       * below is. The three stencil state registers must stay unrecorded, and
       * the check after the frame says so. */
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         /* The two quads and their twelve indices, in the layout the m4-depth
          * vertex shader declares: location 0 R32G32B32_SFLOAT at offset 0 and
          * location 1 R32G32B32A32_SFLOAT at offset 12, stride 28. */
         kVertices, C5_VERTEX_COUNT, C5_VERTEX_STRIDE, kIndices, C5_INDEX_COUNT,
         2, {attributes[0], attributes[1]},
         /* Directly, not staged. */
         false,
         /* No uniform buffer and no texture: the canary's own attributes. */
         NULL, 0, 0,
         NULL, 0, 0, false,
         false,
         /* Phase C5: the depth attachment the frame renders through, with the
          * test and the write the M4 canary ran, and a clear of 1.0 that
          * vk_meta draws (the clear value the console reads back outside both
          * quads). */
         true, true, true, VK_COMPARE_OP_LESS, VK_ATTACHMENT_LOAD_OP_CLEAR, 1.0f,
      };
      input.stencil[0] = (struct ps5vk_stencil_state){
         .test = true,
         .fail_op = VK_STENCIL_OP_KEEP,
         .pass_op = VK_STENCIL_OP_REPLACE,
         .depth_fail_op = VK_STENCIL_OP_KEEP,
         .compare_op = VK_COMPARE_OP_EQUAL,
         .reference = 0x5au,
         .compare_mask = 0xffu,
         .write_mask = 0xffu,
      };
      /* Zeroed first: a create that fails before it fills the program in
       * leaves the checks below and ps5vk_triangle_finish with nothing to
       * read or release. */
      struct ps5vk_triangle triangle = {0};

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      unsigned frames = 0;
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      if (status == PS5VK_TRIANGLE_OK)
         frames++;
      check(status == PS5VK_TRIANGLE_OK,
            "the frame renders through its depth attachment and signals its fence");
      check(steps.failed == NULL, "no step of the create or the draw failed before the fence");
      if (status == PS5VK_TRIANGLE_OK) {
         check(triangle.depth && triangle.depth_image != VK_NULL_HANDLE &&
                  triangle.depth_view != VK_NULL_HANDLE && triangle.depth_target != NULL &&
                  triangle.depth_target_bytes != 0,
               "the program created, mapped and reported a depth attachment");
#if defined(PS5VK_TEST_DIRECT)
         /* The registers the M4 canary recorded, read back out of the tables the
          * driver recorded for the draw. Only the direct build calls the debug
          * API: the loader build sees the driver through its ICD, which exports
          * no such symbol (driver/ps5vk_icd.map). */
         size_t depth_bytes = 0;
         void *const depth_address =
            ps5vk_debug_image_storage(triangle.depth_image, &depth_bytes);
         const uint64_t address = (uint64_t)(uintptr_t)depth_address;
         ps5vk_debug_stage chunks[8] = {{0}};
         const uint32_t chunk_count = ps5vk_debug_table_chunks(triangle.device, chunks, 8);
         const uint32_t size_xy = (PS5VK_TRIANGLE_WIDTH - 1u) |
                                  ((PS5VK_TRIANGLE_HEIGHT - 1u) << 16);
         check(address != 0 && chunk_count != 0 && depth_bytes != 0,
               "the depth image has an address and the draw recorded register tables");
         check(table_holds(chunks, chunk_count, 0x010, 0x80000183u),
               "DB_Z_INFO is the canary's: D32F in 64 KiB tiles");
         check(table_holds(chunks, chunk_count, 0x011, 0x20000180u),
               "DB_STENCIL_INFO disables stencil, as the canary's does");
         check(table_holds(chunks, chunk_count, 0x012, (uint32_t)(address >> 8)) &&
                  table_holds(chunks, chunk_count, 0x014, (uint32_t)(address >> 8)) &&
                  table_holds(chunks, chunk_count, 0x01a, (uint32_t)(address >> 40)) &&
                  table_holds(chunks, chunk_count, 0x01c, (uint32_t)(address >> 40)),
               "the Z read and write bases are the depth image's address, low and high");
         check(table_holds(chunks, chunk_count, 0x007, size_xy),
               "DB_DEPTH_SIZE_XY is the target's size");
         /* Depth test and write with LESS: 0x16 is the word the M4 canary's
          * rectangles ran (its clear triangle ran 0x76, ALWAYS). */
         check(table_holds(chunks, chunk_count, 0x200, 0x16u),
               "DB_DEPTH_CONTROL is the canary's LESS with test and write enabled");
         check(!table_names(chunks, chunk_count, 0x10b) &&
                  !table_names(chunks, chunk_count, 0x10c) &&
                  !table_names(chunks, chunk_count, 0x10d),
               "a stencil test with no stencil plane bound records no stencil state register");
#endif
      }
      check(frames == 1, "one frame recorded the depth-tested draws and submitted");
      /* A diagnostic: PS5VK_TARGET_DUMP names a file the mapped colour target
       * is written to, so the same frame rendered by another Vulkan
       * implementation (the PC's lavapipe, PS5VK_TARGET_DUMP with no
       * VK_DRIVER_FILES) can be compared with what the console's driver drew.
       * Nothing about the checks depends on it. */
      const char *const dump = getenv("PS5VK_TARGET_DUMP");
      if (dump != NULL && triangle.target != NULL && triangle.target_bytes != 0) {
         /* Another implementation's image memory may be non-coherent, so the
          * range is invalidated before it is read (the PS5 driver's memory is
          * shared and coherent, where the call is a no-op). */
         const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = triangle.memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
         };
         const PFN_vkInvalidateMappedMemoryRanges invalidate =
            (PFN_vkInvalidateMappedMemoryRanges)triangle.get_instance_proc_addr(
               triangle.instance, "vkInvalidateMappedMemoryRanges");
         if (invalidate != NULL)
            invalidate(triangle.device, 1, &range);
         FILE *const file = fopen(dump, "wb");
         if (file != NULL) {
            fwrite(triangle.target, 1, triangle.target_bytes, file);
            fclose(file);
         }
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
