/*
 * PS5 Vulkan driver - R7 test: the console case's own frame.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R7 round 3's shape (PS5_VULKAN_REQUESTSv2.md, docs/M5_PHASE_C.md): vkQuake's
 * collapsed texture sets -- three combined image samplers at set 0's bindings 0,
 * 1 and 2 and the frame's uniform block at set 1's binding 0 -- built by the
 * harness's textures_in_first_set mode from probes/v0-multiset-quake.
 *
 * This program is the host half of the runner case v0-multiset-quake
 * (src/diagnostics.cpp, jobs/v0-multiset-quake/queue.txt), and it draws exactly
 * one frame because that case's golden holds exactly one submission: the gate
 * compares this recording with the console's own stream word for word
 * (tools/check-driver.sh, tools/golden.py compare-run). The console run is what
 * proves the frame's *values* -- it reads 255/64/255 back, one channel from each
 * of set 0's three bindings and the value set 1's block carries -- while this
 * half asserts what a readback could only infer: the two tables the draw built,
 * where each set's pointer went, and the shape of each table's entries (R7's
 * per-set tables, driver/ps5vk_draw.c).
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
#define V0Q_VERTEX_STRIDE 16
#define V0Q_TEXTURE_WIDTH 64
#define V0Q_TEXTURE_HEIGHT 64
/* Room for two stages' worth of sets in the debug API's report. */
#define V0Q_MAX_TABLES 4

static float kVertices[4 * 4];
static uint16_t kIndices[6];

/* One solid texel value per image, and the block set 1 names: its green is
 * 64/255, the value the console's readback has to find (the case asserts
 * 255/64/255, which no single wrong binding produces). */
static const uint32_t kTexelColours[PS5VK_TRIANGLE_MULTISET_TEXTURES] = {0xff0000ffu, 0xff00ff00u,
                                                                       0xffff0000u};
static const float kTint[4] = {1.0f, 64.0f / 255.0f, 1.0f, 1.0f};

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
}

int
main(void)
{
   test_begin("V0 vkQuake-shaped two sets");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
   uint32_t *const vertex =
      probes ? read_spirv(probes, "v0-multiset-quake", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel =
      probes ? read_spirv(probes, "v0-multiset-quake", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the v0-multiset-quake SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#endif

   if (vertex && pixel) {
      static uint32_t texels[PS5VK_TRIANGLE_MULTISET_TEXTURES]
                            [V0Q_TEXTURE_WIDTH * V0Q_TEXTURE_HEIGHT];
      for (unsigned index = 0; index < PS5VK_TRIANGLE_MULTISET_TEXTURES; index++) {
         for (unsigned at = 0; at < V0Q_TEXTURE_WIDTH * V0Q_TEXTURE_HEIGHT; at++)
            texels[index][at] = kTexelColours[index];
      }
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
      };
      fill_geometry();
      struct ps5vk_triangle_input input = {0};
      input.get_instance_proc_addr = GET_PROC;
      input.pipeline_count = 1;
      input.shaders[0] = (struct ps5vk_triangle_shaders){vertex, vertex_bytes, pixel, pixel_bytes};
      input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
      input.report = &report;
      input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
      input.vertex_data = kVertices;
      input.vertex_count = 4;
      input.vertex_stride = V0Q_VERTEX_STRIDE;
      input.index_data = kIndices;
      input.index_count = 6;
      input.attribute_count = 2;
      input.attributes[0] = attributes[0];
      input.attributes[1] = attributes[1];
      input.texture_data = texels[0];
      input.texture_data_second = texels[1];
      input.texture_data_third = texels[2];
      input.texture_width = V0Q_TEXTURE_WIDTH;
      input.texture_height = V0Q_TEXTURE_HEIGHT;
      input.texture_format = VK_FORMAT_R8G8B8A8_UNORM;
      input.textures_in_first_set = true;
      input.uniform_data = kTint;
      input.uniform_bytes = (uint32_t)sizeof(kTint);
      input.uniform_stages = VK_SHADER_STAGE_FRAGMENT_BIT;
      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the vkQuake-shaped frame records, submits and signals its fence");
      check(steps.failed == NULL, "no step of the create or the draw failed before the fence");
      if (steps.failed != NULL)
         printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
                steps.detail != NULL ? steps.detail : "");
#if defined(PS5VK_TEST_DIRECT)
      if (status == PS5VK_TRIANGLE_OK) {
         ps5vk_debug_table tables[4] = {{0}};
         const ps5vk_debug_table *set0 = NULL;
         const ps5vk_debug_table *set1 = NULL;
         check_two_tables(triangle.device, tables, V0Q_MAX_TABLES, &set0, &set1);
         if (set0 != NULL) {
            /* Set 0's table is three 48-byte image-sampler entries one after
             * another -- the bindings the metadata puts at offsets 0, 48 and 96
             * (probes/v0-multiset-quake/bindings.txt) -- and set 1's is the
             * frame's 16-byte uniform entry. A driver that sized one table for
             * the stage, or wrote one set's entry into the other's, fails here. */
            bool three_entries = true;
            for (unsigned at = 0; at < PS5VK_TRIANGLE_MULTISET_TEXTURES; at++)
               three_entries = three_entries &&
                               test_table_image_entry(set0->words + at * PS5VK_TEST_IMAGE_ENTRY_WORDS,
                                                    triangle.texture_extent);
            check(three_entries,
                  "set 0's table holds three image samplers, one entry after another");
            check(test_table_uniform_entry(set1->words, (uint32_t)sizeof(kTint)),
                  "set 1's table holds the 16-byte uniform descriptor of the frame's block");
         }
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
