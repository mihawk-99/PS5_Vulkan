/*
 * PS5 Vulkan driver - D1 test: a dynamic uniform buffer through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Rung 1.0, Phase D1 (docs/M5_REFERENCE.md); built and run through the loader
 * and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the m3 uniform program twice through the driver, with one thing between
 * the frames: the dynamic offset the descriptor set is bound with. The buffer
 * holds two 16-byte colours, the descriptor's range is the shader's one colour,
 * and the offsets 0 and 16 choose which half each frame reads -- which is what
 * VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC is for and what the driver's
 * reported maxDescriptorSetUniformBuffersDynamic = 8 promises. The recorded
 * stream shows the address moving by 16 with the record count unchanged, and
 * tools/check-driver.sh compares both frames with the console's own run of the
 * runner's `d1-dynamic-ubo` test (golden/d1-dynamic-ubo).
 *
 * The console, not this test, is what proves the colours reach the screen:
 * src/diagnostics.cpp's `d1-dynamic-ubo` reads each frame back and every pixel
 * must hold the colour its offset chose. PS5VK_PROBES names the probes
 * directory. The PS5 build only links; it is not run.
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

/* The two colours the buffer holds, as floats of exact 1/255 multiples, which
 * is what src/diagnostics.cpp's dynamic uniform probe writes too
 * (kUniformFrameColours). */
#define D1_COLOURS 2
#define D1_COLOUR_BYTES 16

int
main(void)
{
   test_begin("D1 dynamic uniform buffer");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "m3", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m3", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m3 SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const uint8_t colours[D1_COLOURS][3] = {{0xff, 0x40, 0x80}, {0x30, 0xd0, 0x60}};
      uint8_t uniform[D1_COLOURS * D1_COLOUR_BYTES] = {0};
      for (unsigned colour = 0; colour < D1_COLOURS; colour++) {
         const float value[4] = {colours[colour][0] / 255.0f, colours[colour][1] / 255.0f,
                                 colours[colour][2] / 255.0f, 1.0f};
         memcpy(&uniform[colour * D1_COLOUR_BYTES], value, sizeof(value));
      }
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         NULL, 0, 0, NULL, 0,
         0, {{0, 0, VK_FORMAT_UNDEFINED, 0}, {0, 0, VK_FORMAT_UNDEFINED, 0}}, false,
         uniform, sizeof(uniform), VK_SHADER_STAGE_FRAGMENT_BIT,
      };
      /* The range is the shader's one colour, and a nonzero initial offset
       * declares the binding dynamic; the frames then move it. */
      input.uniform_range_bytes = D1_COLOUR_BYTES;
      input.uniform_dynamic_offset = D1_COLOUR_BYTES;
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      for (unsigned frame = 0; frame < D1_COLOURS && status == PS5VK_TRIANGLE_OK; frame++) {
         if (!ps5vk_triangle_set_uniform_offset(&triangle, frame * D1_COLOUR_BYTES))
            status = PS5VK_TRIANGLE_FAILED;
         else
            status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      }
      check(status == PS5VK_TRIANGLE_OK,
            "both frames record, submit and signal their fences");
      check(steps.failed == NULL, "no step of either draw failed before its fence");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
