/*
 * PS5 Vulkan driver - R7 test: two descriptor sets, one table each.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R7 (PS5_VULKAN_REQUESTSv2.md, docs/M5_PHASE_C.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * The frame is the probes/v0-multiset program, whose one fragment stage reads
 * both sets: the colour from a uniform block at set 0, binding 0 and the scale
 * from a combined image sampler at set 1, binding 0. The mechanism under test is
 * what the driver builds for it -- one table per set, each sized from that set's
 * own bindings, and one user-data pointer per set, written into the dword the
 * metadata names for that set (driver/ps5vk_draw.c). A driver that keeps one
 * table, or writes one pointer, cannot report two of either, and the two tables
 * have to disagree in the way their own bindings differ: set 0's entry is the
 * 16-byte uniform descriptor, set 1's is the 48-byte image sampler carrying the
 * texture's own extent.
 *
 * The frame's *values* are what the console case of the same program reads back
 * (src/diagnostics.cpp): this half asserts the pointers and the entries a
 * readback could only infer, which is what R9's push-constant test does for its
 * own block.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The driver's own debug API: the tables a draw builds. */
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

/* The m3-texture geometry the probe's vertex shader declares: a vec2 position
 * and a vec2 texture coordinate in 16-byte records, and the quad's two
 * triangles. */
#define V0M_VERTEX_STRIDE 16
#define V0M_TEXTURE_WIDTH 64
#define V0M_TEXTURE_HEIGHT 64
/* The uniform binding's element stride, which is what the driver's own layout
 * gives a uniform buffer (driver/ps5vk_descriptor_set_layout.c), and room for
 * two stages' worth of sets in the debug API's report. */
#define V0M_UNIFORM_STRIDE 16
#define V0M_MAX_TABLES 8

static float kVertices[4 * 4];
static uint16_t kIndices[6];
static uint8_t kTexels[V0M_TEXTURE_WIDTH * V0M_TEXTURE_HEIGHT * 4];

/* The colour block the fragment shader multiplies: one vec4, so set 0's table
 * holds exactly one 16-byte element. */
static const float kColour[4] = {1.0f, 0.0f, 0.0f, 1.0f};

static void
fill_geometry(void)
{
   const float quad[4][4] = {
      {-1.0f, -1.0f, 0.0f, 0.0f},
      {1.0f, -1.0f, 1.0f, 0.0f},
      {1.0f, 1.0f, 1.0f, 1.0f},
      {-1.0f, 1.0f, 0.0f, 1.0f},
   };
   memcpy(kVertices, quad, sizeof(quad));
   const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
   memcpy(kIndices, indices, sizeof(indices));
   /* One texel is red and the rest black, so the scale the console case reads
    * back is a value the texture's own content supplies (multiset.frag). */
   memset(kTexels, 0, sizeof(kTexels));
   kTexels[0] = 0xff;
}

#if defined(PS5VK_TEST_DIRECT)
/* The two entry shapes the checks below read out of the reported tables. A
 * table's address is a CPU address in this driver's 32-bit-pointer build, which
 * is how the debug API's own caller dereferences it (R9's test does the same
 * with the push-constant block). */

/* The uniform descriptor's four words: the address, the stride in the high half
 * of word 1, the elements the range covers and the flags -- the same shape
 * driver/ps5vk_draw.c writes for a bound uniform buffer. */
static bool
uniform_entry(const uint32_t *entry, uint32_t bytes)
{
   return entry[0] != 0 && (entry[1] >> 16) == V0M_UNIFORM_STRIDE &&
          entry[2] == bytes / V0M_UNIFORM_STRIDE && entry[3] != 0;
}

/* The combined image sampler's twelve words, as far as a caller can know them
 * without being the driver: the texture's own extent in word 2 -- width minus
 * one in the low bits and the height minus one from bit 14 -- the sampler's
 * address word in word 8 and the sampler's filter word in word 10. */
static bool
image_entry(const uint32_t *entry, VkExtent2D extent)
{
   const uint32_t width = (extent.width - 1u) >> 2;
   const uint32_t height = extent.height - 1u;
   return (entry[2] & 0x3fffu) == width && ((entry[2] >> 14) & 0xffffu) == height &&
          entry[8] != 0 && entry[10] != 0;
}
#endif

int
main(void)
{
   test_begin("V0 two descriptor sets");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
   uint32_t *const vertex = probes ? read_spirv(probes, "v0-multiset", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "v0-multiset", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the v0-multiset SPIR-V");
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
         {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
      };
      fill_geometry();
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         kVertices, 4, V0M_VERTEX_STRIDE, kIndices, 6,
         2, {attributes[0], attributes[1]},
         false,
         /* R7: both sets, so the harness declares two set layouts and binds two
          * sets -- the uniform block at set 0 and the texture's combined image
          * sampler at set 1, which is where the probe's fragment shader reads
          * them from. */
         kColour, (uint32_t)sizeof(kColour), VK_SHADER_STAGE_FRAGMENT_BIT,
         kTexels, V0M_TEXTURE_WIDTH, V0M_TEXTURE_HEIGHT, false,
         false,
      };
      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the two-set frame records, submits and signals its fence");
      check(steps.failed == NULL, "no step of the create or the draw failed before the fence");
      if (steps.failed != NULL)
         printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
                steps.detail != NULL ? steps.detail : "");
      if (status == PS5VK_TRIANGLE_OK) {
         check(triangle.set_count == 2,
               "the pipeline layout declares two set layouts and the frame binds both");
#if defined(PS5VK_TEST_DIRECT)
         /* R7's mechanism, one table per set: the draw reports what it built,
          * the dword each pointer went into and the pointer itself. */
         ps5vk_debug_table tables[V0M_MAX_TABLES] = {{0}};
         const uint32_t count = ps5vk_debug_descriptor_tables(
            triangle.device, tables, V0M_MAX_TABLES);
         check(count == 2, "the draw built one table per set the stage reads");
         if (count == 2) {
            const ps5vk_debug_table *set0 = tables[0].set == 0 ? &tables[0] : &tables[1];
            const ps5vk_debug_table *set1 = tables[0].set == 1 ? &tables[0] : &tables[1];
            check(set0->set == 0 && set1->set == 1, "the two tables are set 0's and set 1's");
            for (unsigned at = 0; at < count; at++)
               printf("  (table %u: stage %u set %u dword %u address %08x words %p bytes %zu)\n", at,
                      (unsigned)tables[at].stage, (unsigned)tables[at].set,
                      (unsigned)tables[at].user_data_dword, tables[at].address_low,
                      (const void *)tables[at].words, tables[at].bytes);
            check(set0->user_data_dword != set1->user_data_dword,
                  "each set's pointer has a user-data dword of its own");
            check(set0->words != NULL && set1->words != NULL && set0->words != set1->words &&
                     set0->address_low != set1->address_low,
                  "the two sets' tables are two different tables");
            /* The pointer in the ABI is the table the draw allocated: one dword
             * in the 32-bit-pointer build these programs compile for, as R9's
             * push-constant test reads its own (ps5vk_debug.h). */
            check((uint32_t)(uintptr_t)set0->words == set0->address_low &&
                     (uint32_t)(uintptr_t)set1->words == set1->address_low,
                  "each set's table is the pointer its user-data dword carries");
            check(set0->stage == set1->stage,
                  "both tables belong to the one stage that reads descriptors");
            /* The entries, each the shape of its own set's binding: a driver
             * that sized one table for both sets, or wrote one set's entry into
             * the other's, fails here. */
            const uint32_t *const entry0 = set0->words;
            const uint32_t *const entry1 = set1->words;
            check(uniform_entry(entry0, (uint32_t)sizeof(kColour)),
                  "set 0's table holds the 16-byte uniform descriptor of the colour block");
            check(image_entry(entry1, triangle.texture_extent),
                  "set 1's table holds the image sampler of the 64x64 scale texture");
            printf("  (stage %u: set 0 dword %u -> %08x %08x %08x %08x, set 1 dword %u -> "
                   "%08x %08x %08x %08x)\n",
                   (unsigned)set0->stage, (unsigned)set0->user_data_dword, entry0[0], entry0[1],
                   entry0[2], entry0[3], (unsigned)set1->user_data_dword, entry1[0], entry1[1],
                   entry1[2], entry1[3]);
         }
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
