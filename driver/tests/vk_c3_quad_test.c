/*
 * PS5 Vulkan driver - Phase C3 test: a transformed quad drawn through the
 * Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C3 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the c3-quad probe set: a unit quad whose position comes from a vertex
 * buffer (four R32G32_SFLOAT records, stride 8) and whose transform comes from
 * a 16-byte uniform buffer at set 0, binding 0, so its vertex shader computes
 * position * scale_offset.xy + scale_offset.zw and its pixel shader exports
 * one constant colour. The harness creates the vertex buffer, the index buffer,
 * the uniform buffer, the set layout, the pool and the set, updates the set
 * with the buffer, and records vkCmdBindVertexBuffers, vkCmdBindIndexBuffer
 * and vkCmdBindDescriptorSets before the draw (driver/tests/ps5vk_triangle.c).
 * The c3-quad shaders were compiled for exactly that layout
 * (tools/build-probe-shaders.sh c3-quad, probes/c3-quad/compile.txt:
 * --vertex-attribute 0:r32g32_float:0:0:8:4 and
 * --descriptor-binding 0:0:uniform_buffer:1:0:16).
 *
 * The program draws two frames with the two halves of the tutorial's
 * transform -- a 0.5 scale offset by (-0.5, -0.5), then by (+0.5, +0.5) --
 * replacing the uniform bytes with ps5vk_triangle_set_uniform before the
 * second, as src/diagnostics.cpp's c3-quad test does. Nothing renders on the
 * PC, so this program's job is the stream: that both frames record, submit and
 * signal their fence, and that the driver built the descriptor set, the buffer
 * and the drawing buffers the application asked for. The frame itself is the
 * console's to prove: src/diagnostics.cpp's c3-quad draws the same two frames
 * through the same driver and reads each one back where its transform put the
 * quad (the top-left quadrant, then the bottom-right). PS5VK_PROBES names the
 * probes directory. The PS5 build only links; it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

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

/* The c3-quad set's geometry: one R32G32_SFLOAT position per 8-byte record,
 * and two triangles over six 16-bit indices (the same order the other quad
 * sets use). Its vertex shader was compiled for exactly this layout
 * (tools/build-probe-shaders.sh c3-quad: location 0 R32G32_SFLOAT at offset 0,
 * stride 8). src/diagnostics.cpp holds the same quad as kQuadVertices and
 * kQuadIndices, and its readback check is what accepts the frames on the
 * console. */
#define C3_QUAD_VERTEX_COUNT 4
#define C3_QUAD_VERTEX_STRIDE 8
#define C3_QUAD_INDEX_COUNT 6

static const float kVertices[C3_QUAD_VERTEX_COUNT * 2] = {
   -1.0f, -1.0f, /* 0 */
   1.0f,  -1.0f, /* 1 */
   1.0f,  1.0f,  /* 2 */
   -1.0f, 1.0f,  /* 3 */
};

static const uint16_t kIndices[C3_QUAD_INDEX_COUNT] = {0, 1, 2, 2, 3, 0};

/* The two transforms the program draws with, as the uniform buffer's four
 * floats: {scale x, scale y, offset x, offset y}. The first puts the
 * half-size quad in the target's top-left quadrant, the second -- what
 * ps5vk_triangle_set_uniform writes before the second frame -- in the
 * bottom-right, because the driver's viewport has y pointing down in
 * framebuffer space. src/diagnostics.cpp holds the same table as
 * kQuadFrameTransforms, and its readback check is what proves where each frame
 * landed. */
static const float kTransforms[2][4] = {
   {0.5f, 0.5f, -0.5f, -0.5f},
   {0.5f, 0.5f, 0.5f, 0.5f},
};

int
main(void)
{
   test_begin("C3 quad");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex =
      probes ? read_spirv(probes, "c3-quad", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "c3-quad", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the c3-quad SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attribute = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
      const struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         /* The unit quad and its six indices, drawn with the one position
          * attribute the c3-quad vertex shader declares, plus the transform it
          * reads from the one descriptor set the harness creates. */
         kVertices, C3_QUAD_VERTEX_COUNT, C3_QUAD_VERTEX_STRIDE, kIndices, C3_QUAD_INDEX_COUNT,
         1, {attribute}, false, &kTransforms[0], (uint32_t)sizeof(kTransforms[0]),
         /* The c3-quad vertex shader is what reads the uniform buffer, so the
          * set layout has to declare it for the vertex stage. */
         VK_SHADER_STAGE_VERTEX_BIT,
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
            "the transformed quad records, submits and signals its fence");
      check(steps.failed == NULL, "no step of the draw failed before the fence");
      if (status == PS5VK_TRIANGLE_OK) {
         /* The buffers this phase adds: the geometry and the indices the draw
          * binds, a uniform buffer, and the set that names it. The console
          * test draws twice with different transforms, and the driver records
          * one submission per frame, so this program does the same. */
         check(triangle.vertex_buffer != VK_NULL_HANDLE &&
                  triangle.index_buffer != VK_NULL_HANDLE,
               "the quad draws from a vertex buffer and an index buffer");
         check(triangle.descriptor_set != VK_NULL_HANDLE &&
                  triangle.uniform_buffer != VK_NULL_HANDLE,
               "the draw bound a uniform buffer at set 0, binding 0");
         check(ps5vk_triangle_set_uniform(&triangle, &kTransforms[1],
                                          (uint32_t)sizeof(kTransforms[1])),
               "the uniform buffer takes the second transform after the frame");
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         if (status == PS5VK_TRIANGLE_OK)
            frames++;
      }
      check(frames == 2, "two frames submitted the transformed quad with different transforms");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
