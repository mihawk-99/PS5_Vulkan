/*
 * PS5 Vulkan driver - Phase C3 test: a uniform buffer bound through the
 * Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C3 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the m3 probe set -- a full-target triangle whose colour comes from a
 * uniform buffer at set 0, binding 0 -- from a buffer the application
 * supplies: the harness creates the buffer, its memory and mapping, a set
 * layout with that one binding, a pool and a set, updates the set with the
 * buffer, and records vkCmdBindDescriptorSets before the draw. The m3 pixel
 * shader was compiled for exactly that binding (tools/build-probe-shaders.sh
 * m3-uniform: --descriptor-binding 0:0:uniform_buffer:1:0:16, a 16-byte
 * uniform buffer holding one RGBA colour, probes/m3/bindings.txt), and
 * src/diagnostics.cpp's run_uniform_buffer_frames is the console's own
 * hardware run of the same set.
 *
 * Nothing renders on the PC, so the test checks that the draw records,
 * submits and signals its fence; tools/check-driver.sh compares the
 * submission the host layer records (PS5_HOST_SUBMISSION_DUMP) with the
 * console's own driver run. PS5VK_PROBES names the probes directory. The PS5
 * build only links; it is not run.
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

/* The colour the m3 uniform buffer holds: one RGBA colour of 16 bytes, the
 * range the pixel shader's binding was compiled with. The channels are exact
 * multiples of 1/255, as the runner's write_uniform_colour writes them, so a
 * readback of the frame stores them without rounding ambiguity (the console,
 * not this test, is what reads the frame back). */
struct c3_colour {
   float red;
   float green;
   float blue;
   float alpha;
};

static const struct c3_colour kColours[2] = {
   {0xff / 255.0f, 0x40 / 255.0f, 0x80 / 255.0f, 1.0f},
   {0x30 / 255.0f, 0xd0 / 255.0f, 0x60 / 255.0f, 1.0f},
};

int
main(void)
{
   test_begin("C3 uniform");
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
      const struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         /* The m3 set draws its triangle without vertex bindings, and its
          * colour comes from the uniform buffer this test passes, the only
          * descriptor set the harness creates for it. */
         NULL, 0, 0, NULL, 0, 0, {{0}}, false,
         &kColours[0], (uint32_t)sizeof(kColours[0]), VK_SHADER_STAGE_FRAGMENT_BIT,
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
            "the uniform-coloured frame records, submits and signals its fence");
      check(steps.failed == NULL, "no step of the draw failed before the fence");
      /* The buffer and the set a caller asked for, and the way to change them
       * between frames, are the objects this phase adds; neither exists for a
       * caller that passes no uniform data. The console test draws twice with
       * different colours, and the driver records one submission per frame, so
       * this program does the same. */
      if (status == PS5VK_TRIANGLE_OK) {
         check(triangle.descriptor_set != VK_NULL_HANDLE &&
                  triangle.uniform_buffer != VK_NULL_HANDLE,
               "the draw bound a uniform buffer at set 0, binding 0");
         check(ps5vk_triangle_set_uniform(&triangle, &kColours[1],
                                         (uint32_t)sizeof(kColours[1])),
               "the uniform buffer takes the caller's bytes after the frame");
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         if (status == PS5VK_TRIANGLE_OK)
            frames++;
      }
      check(frames == 2, "two frames submitted the same draw with different uniform bytes");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
