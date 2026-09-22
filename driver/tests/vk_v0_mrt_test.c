/*
 * PS5 Vulkan driver - R7 step 1b test: one register row per colour attachment.
 *
 * PARKED, not registered in tools/check-driver.sh's list: the refusal half of
 * this program passes (it prints the driver's sentence for a rendering into two
 * colour attachments), and then the direct build dies inside Mesa's
 * vk_object_base_assert_valid, which is a third failure this round did not
 * chase. The console case v0-mrt is what measures the interim state; this file
 * is kept because its assertions are the ones the round that lands the
 * per-attachment writes wants, and docs/M5_PHASE_C.md records the crash.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The driver advertises four colour attachments (VkPhysicalDeviceLimits.
 * maxColorAttachments), so a rendering may declare up to four and the draw has
 * to program each one's CB_COLORi_* registers. This program is the host half of
 * the console case v0-mrt (src/diagnostics.cpp) and asserts the *mechanism* the
 * console's readback can only infer: for a rendering of 1, 2 and the advertised
 * maximum attachments, the rows the draw built carry
 *
 *   - the attachment's own CB_COLORi_BASE offset (0x318, then +15 a target, the
 *     stride amdgfxregs.h's gfx103 rows give -- two families, two strides, so a
 *     table built as "target 0 plus one stride" would be visibly wrong here),
 *   - that attachment's own address in the word there, which is what says the
 *     rows were filled from the attachments rather than reused from one, and
 *   - one row per attachment and no more.
 *
 * The console run is what proves the frame; this proves the registers, so a
 * wrong picture and a wrong table cannot hide behind each other.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The driver's own debug API: the rows a begin-rendering built. */
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

/* The m3-texture geometry the probe's vertex shader declares. */
#define V0R_VERTEX_STRIDE 16
/* CB_COLOR0_BASE's dword offset and the main family's per-target stride: what
 * the table in driver/ps5vk_draw.c derives from amdgfxregs.h ((address -
 * 0x28000) / 4, the gfx103 rows). */
#define V0R_BASE_OFFSET 0x318
#define V0R_TARGET_STRIDE 15

static float kVertices[4 * 4];
static uint16_t kIndices[6];

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
   test_begin("V0 multiple render targets");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
   uint32_t *const vertex = probes ? read_spirv(probes, "v0-mrt", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "v0-mrt", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the v0-mrt SPIR-V");
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
      /* The counts this half proves: more than one, which the driver refuses by
       * name until the per-attachment writes land. The count of **one** is the
       * console's -- the probe's shader writes four outputs and the host model's
       * stage workspace is sized by the replay, which refuses this shader's
       * register tables ("a created shader's register tables are out of bounds")
       * where the console compiled it. The drawn frame is v0-mrt's console run;
       * this half is the refusal that keeps it honest in the meantime. */
      unsigned counts[2] = {2, 0};
      unsigned advertised = 0;
      for (unsigned at = 0; at < 2; at++) {
         if (counts[at] == 0)
            counts[at] = advertised;
         if (counts[at] < 2 || counts[at] > PS5VK_TRIANGLE_MAX_TARGETS) {
            check(false, "the device advertises more than one colour attachment");
            break;
         }
         struct ps5vk_triangle_input input = {0};
         input.get_instance_proc_addr = GET_PROC;
         input.pipeline_count = 1;
         input.shaders[0] =
            (struct ps5vk_triangle_shaders){vertex, vertex_bytes, pixel, pixel_bytes};
         input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
         input.report = &report;
         input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
         input.vertex_data = kVertices;
         input.vertex_count = 4;
         input.vertex_stride = V0R_VERTEX_STRIDE;
         input.index_data = kIndices;
         input.index_count = 6;
         input.attribute_count = 2;
         input.attributes[0] = attributes[0];
         input.attributes[1] = attributes[1];
         input.color_attachment_count = counts[at];
         struct ps5vk_triangle triangle = {0};
         enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
         if (advertised == 0)
            advertised = triangle.max_color_attachments;
         if (status == PS5VK_TRIANGLE_OK)
            status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         /* The interim state this round lands: one attachment records and
          * submits, and a rendering into more is refused by name until the
          * per-attachment writes land (driver/ps5vk_draw.c). The round that makes
          * them land flips this expectation. */
         check(status != PS5VK_TRIANGLE_OK && steps.failed != NULL,
               "a rendering into more than one colour attachment is refused by name");
         if (steps.failed != NULL && counts[at] > 1)
            printf("  (the refusal: %s: %s)\n", steps.failed,
                   steps.detail != NULL ? steps.detail : "");
#if defined(PS5VK_TEST_DIRECT)
         if (status == PS5VK_TRIANGLE_OK) {
            ps5vk_debug_target targets[PS5VK_TRIANGLE_MAX_TARGETS] = {{0}};
            const uint32_t count =
               ps5vk_debug_colour_targets(triangle.device, targets, PS5VK_TRIANGLE_MAX_TARGETS);
            check(count == counts[at],
                  "the rendering programmed one target row per colour attachment");
            bool own_offset = count == counts[at];
            bool own_address = own_offset;
            for (unsigned index = 0; index < count; index++) {
               own_offset = own_offset &&
                            targets[index].base_offset == V0R_BASE_OFFSET + index * V0R_TARGET_STRIDE;
               const uint32_t address = (uint32_t)((uintptr_t)triangle.target_mappings[index] >> 8);
               own_address = own_address && targets[index].base_value == address;
               printf("  (attachment %u: CB_COLOR_BASE offset %#x value %#x, its mapping %#x)\n",
                      index, targets[index].base_offset, targets[index].base_value, address);
            }
            check(own_offset, "each attachment's row names its own CB_COLORi_BASE register");
            check(own_address, "each attachment's row carries that attachment's own address");
         }
#endif
         if (status != PS5VK_TRIANGLE_IN_FLIGHT)
            ps5vk_triangle_finish(&triangle);
      }
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
