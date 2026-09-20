/*
 * PS5 Vulkan driver - Phase B8 test: a frame through a secondary command buffer.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B8 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * vkCmdExecuteCommands copies the secondary's recorded words into the primary
 * at the call, which is what the queue submits -- not the PM4 INDIRECT_BUFFER
 * route B8 measured as faulting the GPU (docs/M5_PHASE_B.md). The secondary's
 * draws are the same ones a direct frame records, with the targets its
 * inheritance info's framebuffer names, so the stream the driver builds has to
 * be the stream a direct frame builds: this test draws the M2 triangle through
 * a secondary the primary executes, and tools/check-driver.sh compares that
 * submission with the console's own b4-headless frame, exactly as it compares
 * the b7_draw and c2_indirect frames.
 *
 * The PS5 build only links; the console's run of the same frame is what proves
 * the pixels there.
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

int
main(void)
{
   test_begin("B8 secondary");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "m2", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m2", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the M2 SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         NULL, 0, 0, NULL, 0, 0, {{0}}, false, NULL, 0, 0,
      };
      /* The M2 triangle, through a secondary the primary executes. */
      input.secondary = true;
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the secondary's draw records, the primary executes it and the fence signals");
      check(steps.failed == NULL, "no step of the secondary frame failed");
      check(status == PS5VK_TRIANGLE_OK && triangle.target != NULL &&
               triangle.target_bytes == UINT64_C(0x2000000),
            "the frame is readable through the mapped 32 MiB image memory");
      /* The two refusals the implementation keeps: a primary is not a secondary
       * to execute, and a secondary does not execute another -- a nested
       * execution would have to be copied twice. Each ends the recording with
       * the driver's message (VK_ERROR_UNKNOWN). */
      if (status != PS5VK_TRIANGLE_IN_FLIGHT && triangle.pool != VK_NULL_HANDLE) {
         const __typeof__(&vkResetCommandBuffer) reset = VK_FUNCTION(triangle.instance, ResetCommandBuffer);
         const __typeof__(&vkBeginCommandBuffer) begin = VK_FUNCTION(triangle.instance, BeginCommandBuffer);
         const __typeof__(&vkEndCommandBuffer) end = VK_FUNCTION(triangle.instance, EndCommandBuffer);
         const __typeof__(&vkCmdExecuteCommands) execute =
            VK_FUNCTION(triangle.instance, CmdExecuteCommands);
         const VkCommandBufferBeginInfo plain = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
         };
         const VkCommandBufferInheritanceInfo inheritance = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
            .renderPass = triangle.first_pass,
            .subpass = 0,
            .framebuffer = triangle.framebuffers[0],
         };
         const VkCommandBufferBeginInfo continuing = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
                     VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = &inheritance,
         };
         /* A primary named as the buffer to execute. */
         reset(triangle.commands[0], 0);
         begin(triangle.commands[0], &plain);
         execute(triangle.commands[0], 1, &triangle.commands[1]);
         check(end(triangle.commands[0]) == VK_ERROR_UNKNOWN,
               "a primary named as the executed buffer is refused by name");
         /* A secondary that executes another. */
         reset(triangle.secondary_command, 0);
         begin(triangle.secondary_command, &continuing);
         execute(triangle.secondary_command, 1, &triangle.commands[1]);
         check(end(triangle.secondary_command) == VK_ERROR_UNKNOWN,
               "a secondary that executes another is refused by name");
      }
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

#if defined(__linux__)
   free(vertex);
   free(pixel);
#endif
   return test_finish();
}
