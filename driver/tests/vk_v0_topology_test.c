/*
 * PS5 Vulkan driver - the topologies a pipeline is linked as.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R6 of the vkQuake port's requests (docs/M5_PHASE_C.md); built and run through
 * the loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * A topology reaches the hardware through AGC's link, which takes AMD's DI_PT_*
 * value and writes it into VGT_PRIMITIVE_TYPE. The PC model replays the link's
 * outputs instead of computing them, so what the console's link is told is read
 * here through the driver's debug API (ps5vk_debug_pipeline_primitive_type):
 *
 *   triangle list    DI_PT_TRILIST   4
 *   triangle strip   DI_PT_TRISTRIP  6  (R6 first passed 5, which is the fan)
 *
 * and each frame is recorded and submitted through the same harness the runner
 * case v0-strip draws with, so the frames the console reads back are the ones
 * this program records: a strip of three vertices and a strip of four, drawn with
 * VkCmdDraw from a bound vertex buffer -- the DRAW_INDEX_AUTO in the submission
 * carries the frame's own vertex count -- and the quad as an indexed list. The
 * pixels are the console's to prove (runner case v0-strip). PS5VK_PROBES names
 * the probes directory, which holds the m3-vertex package the frames use.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The m3-vertex layout: a position and a colour, 24-byte records. Every vertex
 * has the same colour, as in the runner case, so a frame is its coverage alone. */
#define TOPOLOGY_STRIDE 24
static const float kVertices[4][6] = {
   {-1.0f, -1.0f, 0.5f, 1.0f, 0.25f, 1.0f},
   {-1.0f, 1.0f, 0.5f, 1.0f, 0.25f, 1.0f},
   {1.0f, -1.0f, 0.5f, 1.0f, 0.25f, 1.0f},
   {1.0f, 1.0f, 0.5f, 1.0f, 0.25f, 1.0f},
};
static const uint16_t kConsistent[6] = {0, 1, 2, 1, 3, 2};

/* The two draw packets as the PC's AGC model encodes them
 * (host/agc/agc_host.cpp): DRAW_INDEX_AUTO is PKT3(0x2d, 1) with the vertex count
 * first, DRAW_INDEX_2 PKT3(0x27, 4) with the index count first. */
#define DRAW_INDEX_AUTO_HEADER 0xc0012d00u
#define DRAW_INDEX_2_HEADER 0xc0042700u

#if defined(PS5VK_TEST_DIRECT)
/* The count of the submission's last packet with this header, or 0 for none:
 * the frame's own draw is its last one, after the pass's clear, which vk_meta
 * draws as a DRAW_INDEX_AUTO of its own. */
static uint32_t
last_draw_count(VkDevice device, uint32_t header)
{
   uint32_t dwords = 0;
   const uint32_t *const words = ps5vk_debug_last_submission(device, &dwords);
   uint32_t count = 0;
   for (uint32_t at = 0; words != NULL && at + 1 < dwords; at++) {
      if (words[at] == header)
         count = words[at + 1];
   }
   return count;
}
#endif

struct frame {
   const char *name;
   VkPrimitiveTopology topology;
   uint32_t vertex_count;
   const uint16_t *indices;
   uint32_t link;
   /* The count the frame's own draw packet carries: vertices for a
    * DRAW_INDEX_AUTO, indices for a DRAW_INDEX_2. */
   uint32_t draw_count;
};

static void
draw_frame(const struct frame *frame, const uint32_t *vertex, size_t vertex_bytes,
           const uint32_t *pixel, size_t pixel_bytes)
{
   struct steps steps = {0};
   const struct ps5vk_triangle_report report = {&steps, record_step};
   struct ps5vk_triangle_input input = {0};
   input.get_instance_proc_addr = GET_PROC;
   input.pipeline_count = 1;
   input.shaders[0] = (struct ps5vk_triangle_shaders){vertex, vertex_bytes, pixel, pixel_bytes};
   input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
   input.report = &report;
   input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
   input.vertex_data = kVertices;
   input.vertex_count = frame->vertex_count;
   input.vertex_stride = TOPOLOGY_STRIDE;
   input.index_data = frame->indices;
   input.index_count = frame->indices != NULL ? 6u : 0u;
   input.attribute_count = 2;
   input.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
   input.attributes[1] =
      (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8};
   input.primitive_topology = frame->topology;
   struct ps5vk_triangle triangle = {0};
   enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
   if (status == PS5VK_TRIANGLE_OK)
      status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
   char what[192];
   snprintf(what, sizeof(what), "%s: the frame records, submits and signals its fence",
            frame->name);
   check(status == PS5VK_TRIANGLE_OK && steps.failed == NULL, what);
   if (steps.failed != NULL)
      printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
             steps.detail != NULL ? steps.detail : "");
#if defined(PS5VK_TEST_DIRECT)
   if (status == PS5VK_TRIANGLE_OK) {
      const uint32_t link = ps5vk_debug_pipeline_primitive_type(triangle.pipelines[0]);
      snprintf(what, sizeof(what), "%s: the link is told DI_PT %u", frame->name, frame->link);
      check(link == frame->link, what);
      if (link != frame->link)
         printf("  (the pipeline links as %u)\n", link);
      const bool indexed = frame->indices != NULL;
      const uint32_t count =
         last_draw_count(triangle.device, indexed ? DRAW_INDEX_2_HEADER : DRAW_INDEX_AUTO_HEADER);
      snprintf(what, sizeof(what),
               indexed ? "%s: the submission's last draw is a DRAW_INDEX_2 of %u indices"
                       : "%s: the submission's last draw is a DRAW_INDEX_AUTO of %u vertices",
               frame->name, frame->draw_count);
      check(count == frame->draw_count, what);
      if (count != frame->draw_count)
         printf("  (its count is %u)\n", count);
   }
#endif
   if (status != PS5VK_TRIANGLE_IN_FLIGHT)
      ps5vk_triangle_finish(&triangle);
}

int
main(void)
{
   test_begin("V0 topology");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
   uint32_t *const vertex =
      probes ? read_spirv(probes, "m3-vertex", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m3-vertex", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m3-vertex SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#endif

   static const struct frame kFrames[] = {
      {"three vertices, strip", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 3, NULL, 6, 3},
      {"three vertices, list", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 3, NULL, 4, 3},
      {"quad, strip", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 4, NULL, 6, 4},
      {"quad, indexed list", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 4, kConsistent, 4, 6},
   };
   for (size_t at = 0; vertex && pixel && at < sizeof(kFrames) / sizeof(kFrames[0]); at++)
      draw_frame(&kFrames[at], vertex, vertex_bytes, pixel, pixel_bytes);

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
