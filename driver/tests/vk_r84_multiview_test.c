/*
 * PS5 Vulkan driver - R84 test: a multiview render pass, one draw per view.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Round 84 (docs/M5_PHASE_C.md); built and run through the loader and directly
 * by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Vulkan 1.1 requires multiview: a subpass with a view mask renders every draw
 * once per view the mask names, each into its own layer of the attachments, and
 * a stage reads the view it is drawing as gl_ViewIndex. The driver replays each
 * draw per view (driver/ps5vk_draw.c, ps5vk_cmd_draw and ps5vk_select_view),
 * pointing the attachment registers at the view's layer and writing the view
 * into the user-data dword the compiler named for it
 * (tooling/psbc/patch-view-index.py). This program draws the r84-multiview
 * frame -- views 0 and 1 of a three-layer target, cleared by the render pass,
 * the quad squeezed into the view's half and coloured by the view -- and reads
 * the recorded stream back:
 *
 *   - the frame records, submits and signals, and the target reports a layer
 *     stride (the slice every view's attachment moves by);
 *   - the pass's clear is one DRAW_INDEX_AUTO per view and the frame's draw one
 *     DRAW_INDEX_2 per view, in view order;
 *   - each of those draws programs CB_COLOR0_BASE at its own view's layer: layer
 *     0, then layer 1, and no draw names layer 2, which no view renders;
 *   - in both stages' user data, exactly one dword goes from 0 in the first
 *     view's draw to 1 in the second: the view index reached the vertex stage
 *     and the pixel stage. (Other dwords may differ too: each copy of a draw
 *     builds its own vertex-buffer table, whose pointer the vertex stage
 *     reads.)
 *
 * The console's r84-multiview case draws the same frame and reads the three
 * layers back, the one no view renders included.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

#if defined(__linux__)
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

#if defined(__linux__) && defined(PS5VK_TEST_DIRECT)
/* The user data the driver writes (driver/ps5vk_draw.c): the vertex stage's
 * from SH 0x8c, the pixel stage's from SH 0x0c. */
#define VERTEX_USER_DATA 0x8cu
#define PIXEL_USER_DATA 0x0cu
#define MAX_USER_DATA 32u
#define MAX_DRAWS 8u

/* One draw of the submission, with the state the packets before it left: the
 * CB_COLOR0_BASE word of the last context table that named it, and each
 * stage's last user data. */
struct draw {
   uint32_t opcode;
   uint32_t base;
   uint32_t vertex_words[MAX_USER_DATA];
   uint32_t vertex_count;
   uint32_t pixel_words[MAX_USER_DATA];
   uint32_t pixel_count;
};

struct stream {
   struct draw draws[MAX_DRAWS];
   uint32_t count;
   bool overflow;
};

/* Walks one step's words. A context table load (PM4 0x9f) carries the host
 * address of its records, which this direct build can read; SET_SH_REG (0x76)
 * carries its registers in the packet. */
static void
walk(const uint32_t *words, uint32_t dwords, uint32_t base_offset, struct draw *state,
     struct stream *stream)
{
   for (uint32_t at = 0; words != NULL && at < dwords;) {
      const uint32_t header = words[at];
      if ((header >> 30) != 3u) {
         at++;
         continue;
      }
      const uint32_t count = ((header >> 16) & 0x3fffu) + 1u;
      const uint32_t opcode = (header >> 8) & 0xffu;
      if (at + count >= dwords)
         break;
      const uint32_t *const body = words + at + 1;
      if (opcode == 0x9fu && count == 4u) {
         const uint32_t *const records =
            (const uint32_t *)(uintptr_t)(((uint64_t)body[1] << 32) | body[0]);
         for (uint32_t record = 0; records != NULL && record < body[3]; record++) {
            if ((records[2 * record] & 0xffffu) == base_offset)
               state->base = records[2 * record + 1];
         }
      } else if (opcode == 0x76u && count >= 2u) {
         const uint32_t values = count - 1u < MAX_USER_DATA ? count - 1u : MAX_USER_DATA;
         if (body[0] == VERTEX_USER_DATA) {
            memcpy(state->vertex_words, body + 1, values * sizeof(uint32_t));
            state->vertex_count = values;
         } else if (body[0] == PIXEL_USER_DATA) {
            memcpy(state->pixel_words, body + 1, values * sizeof(uint32_t));
            state->pixel_count = values;
         }
      } else if (opcode == 0x27u || opcode == 0x2du) {
         if (stream->count < MAX_DRAWS) {
            stream->draws[stream->count] = *state;
            stream->draws[stream->count].opcode = opcode;
            stream->count++;
         } else {
            stream->overflow = true;
         }
      }
      at += 1u + count;
   }
}

/* The dword that goes from 0 in the first user-data block to 1 in the second,
 * if exactly one does, and how many dwords differ in all. */
static bool
view_dword(const uint32_t *first, const uint32_t *second, uint32_t count, uint32_t *dword,
           uint32_t *differences)
{
   uint32_t views = 0;
   *differences = 0;
   for (uint32_t at = 0; at < count; at++) {
      if (first[at] == second[at])
         continue;
      (*differences)++;
      if (first[at] == 0 && second[at] == 1) {
         *dword = at;
         views++;
      }
   }
   return views == 1;
}
#endif

int
main(void)
{
   test_begin("R84 multiview");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
   uint32_t *const vertex = probes ? read_spirv(probes, "r84-multiview", "vertex", &vertex_bytes)
                                   : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "r84-multiview", "pixel", &pixel_bytes)
                                  : NULL;
   check(vertex != NULL && pixel != NULL, "PS5VK_PROBES holds the r84-multiview SPIR-V");
   if (vertex != NULL && pixel != NULL) {
      static const float vertices[] = {-1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, 1, 0, 1};
      static const uint16_t indices[] = {0, 1, 2, 2, 3, 0};
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      struct ps5vk_triangle_input input = {0};
      input.get_instance_proc_addr = GET_PROC;
      input.pipeline_count = 1;
      input.shaders[0] = (struct ps5vk_triangle_shaders){vertex, vertex_bytes, pixel, pixel_bytes};
      input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
      input.report = &report;
      input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
      input.vertex_data = vertices;
      input.vertex_count = 4;
      input.vertex_stride = 16;
      input.index_data = indices;
      input.index_count = 6;
      input.attribute_count = 2;
      input.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
      input.attributes[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32_SFLOAT, 8};
      input.multiview_mask = 0x3u;
      input.multiview_layers = 3;
      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK && steps.failed == NULL,
            "the two-view frame records, submits and signals its fence");
      if (steps.failed != NULL)
         printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
                steps.detail != NULL ? steps.detail : "");
#if defined(PS5VK_TEST_DIRECT)
      /* The layers follow one another in the image's storage, a whole slice
       * apart (driver/ps5vk_image.c, ps5vk_image_layer_bytes). */
      size_t storage_bytes = 0;
      const bool stored = triangle.images[0] != VK_NULL_HANDLE &&
                          ps5vk_debug_image_storage(triangle.images[0], &storage_bytes) != NULL;
      const size_t layer_bytes = stored && triangle.target_layers != 0
                                    ? storage_bytes / triangle.target_layers
                                    : 0;
      check(triangle.target_layers == 3 && layer_bytes != 0 && layer_bytes % 0x10000u == 0,
            "the target has three layers a whole number of 64 KiB tiles apart");
      printf("  (layer stride %zu bytes)\n", layer_bytes);
      if (status == PS5VK_TRIANGLE_OK) {
         ps5vk_debug_target targets[1] = {{0}};
         const uint32_t target_count = ps5vk_debug_colour_targets(triangle.device, targets, 1);
         check(target_count == 1, "the rendering programs one colour attachment");
         struct stream stream = {0};
         struct draw state = {0};
         ps5vk_debug_stage parts[8] = {{0}};
         const uint32_t part_count = ps5vk_debug_submission_steps(triangle.device, parts, 8);
         for (uint32_t part = 0; part < part_count && part < 8; part++)
            walk(parts[part].address, (uint32_t)(parts[part].bytes / sizeof(uint32_t)),
                 targets[0].base_offset, &state, &stream);
         const uint32_t layer_words = (uint32_t)(layer_bytes >> 8);
         const uint32_t base = targets[0].base_value;
         printf("  (%u draws; CB_COLOR0_BASE 0x%x, layer stride 0x%x in its units)\n",
                stream.count, base, layer_words);
         check(!stream.overflow && stream.count == 4 && stream.draws[0].opcode == 0x2du &&
                  stream.draws[1].opcode == 0x2du && stream.draws[2].opcode == 0x27u &&
                  stream.draws[3].opcode == 0x27u,
               "the clear is one DRAW_INDEX_AUTO per view and the frame's draw one DRAW_INDEX_2 "
               "per view");
         bool layers = stream.count == 4;
         bool spare = true;
         for (uint32_t at = 0; at < stream.count; at++) {
            const uint32_t view = at % 2u;
            printf("  (draw %u: opcode 0x%x, base 0x%x)\n", at, stream.draws[at].opcode,
                   stream.draws[at].base);
            layers = layers && stream.draws[at].base == base + view * layer_words;
            spare = spare && stream.draws[at].base != base + 2u * layer_words;
         }
         check(layers, "each draw programs CB_COLOR0_BASE at its own view's layer, 0 then 1");
         check(spare, "no draw names layer 2, which no view renders");
         if (stream.count == 4) {
            const struct draw *const first = &stream.draws[2];
            const struct draw *const second = &stream.draws[3];
            uint32_t dword = 0;
            uint32_t differences = 0;
            const bool vertex_view =
               first->vertex_count != 0 && first->vertex_count == second->vertex_count &&
               view_dword(first->vertex_words, second->vertex_words, first->vertex_count, &dword,
                          &differences);
            printf("  (vertex stage: %u of %u user-data dwords differ; the view index is dword %u)\n",
                   differences, first->vertex_count, dword);
            check(vertex_view, "one vertex-stage user-data dword goes from view 0 to view 1");
            const bool pixel_view =
               first->pixel_count != 0 && first->pixel_count == second->pixel_count &&
               view_dword(first->pixel_words, second->pixel_words, first->pixel_count, &dword,
                          &differences);
            printf("  (pixel stage: %u of %u user-data dwords differ; the view index is dword %u)\n",
                   differences, first->pixel_count, dword);
            check(pixel_view, "one pixel-stage user-data dword goes from view 0 to view 1");
         }
      }
#endif
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }
   free(vertex);
   free(pixel);
#else
   check(true, "the PS5 build only links this program");
#endif
   return test_finish();
}
