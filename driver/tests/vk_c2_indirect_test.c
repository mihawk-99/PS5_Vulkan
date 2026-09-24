/*
 * PS5 Vulkan driver - Phase C2 test: the indirect draws.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C2 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * vkCmdDrawIndirect and vkCmdDrawIndexedIndirect read their parameters from a
 * buffer. A buffer is host memory here, so the driver reads them when the draw
 * is recorded and records the packets a direct draw of the same parameters
 * records -- which is what the console's own b4-headless frame holds, and what
 * tools/check-driver.sh compares this test's submission against. The harness
 * fills an indirect buffer with the parameters it would otherwise pass, so the
 * frames differ only in how the counts reached the driver.
 *
 * The one case the driver refuses is a command buffer that writes the indirect
 * buffer itself: the parameters would be read before that write ran, and Vulkan
 * reads them at execution time, so the test records a fill of the buffer
 * followed by an indirect draw and checks the refusal. PS5VK_PROBES names the
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

int
main(void)
{
   test_begin("C2 indirect draws");
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
      /* The M2 triangle's three vertices, through an indirect buffer. */
      input.indirect = true;
      struct ps5vk_triangle triangle = {0};

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      check(status == PS5VK_TRIANGLE_OK, "an indirect frame is created");
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      check(status == PS5VK_TRIANGLE_OK,
            "the indirect draw records, submits and signals its fence");
      check(steps.failed == NULL, "no step of the indirect draw failed");

      /* A command buffer that writes the buffer the draw reads: refused by
       * name, because the parameters are read at record time. */
      if (status == PS5VK_TRIANGLE_OK) {
         const VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 64,
            .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         };
         const __typeof__(&vkCreateBuffer) create_buffer =
            VK_FUNCTION(triangle.instance, CreateBuffer);
         VkBuffer indirect = VK_NULL_HANDLE;
         VkDeviceMemory memory = VK_NULL_HANDLE;
         VkMemoryRequirements requirements = {0};
         const __typeof__(&vkGetBufferMemoryRequirements) get_requirements =
            VK_FUNCTION(triangle.instance, GetBufferMemoryRequirements);
         const __typeof__(&vkAllocateMemory) allocate_memory =
            VK_FUNCTION(triangle.instance, AllocateMemory);
         const __typeof__(&vkBindBufferMemory) bind_memory =
            VK_FUNCTION(triangle.instance, BindBufferMemory);
         if (create_buffer(triangle.device, &buffer_info, NULL, &indirect) == VK_SUCCESS) {
            get_requirements(triangle.device, indirect, &requirements);
            const VkMemoryAllocateInfo allocate = {
               .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
               .allocationSize = requirements.size,
               .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
            };
            if (allocate_memory(triangle.device, &allocate, NULL, &memory) == VK_SUCCESS &&
                bind_memory(triangle.device, indirect, memory, 0) == VK_SUCCESS) {
               const VkCommandBufferAllocateInfo command_info = {
                  .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                  .commandPool = triangle.pool,
                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                  .commandBufferCount = 1,
               };
               for (unsigned indexed = 0; indexed < 2; indexed++) {
                  VkCommandBuffer command = VK_NULL_HANDLE;
                  VK_FUNCTION(triangle.instance, AllocateCommandBuffers)(triangle.device, &command_info,
                                                                         &command);
                  const VkCommandBufferBeginInfo begin = {
                     .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                  VK_FUNCTION(triangle.instance, BeginCommandBuffer)(command, &begin);
                  VK_FUNCTION(triangle.instance, CmdFillBuffer)(command, indirect, 0, 20, 0);
                  if (indexed)
                     VK_FUNCTION(triangle.instance, CmdDrawIndexedIndirect)(command, indirect, 0, 1, 0);
                  else
                     VK_FUNCTION(triangle.instance, CmdDrawIndirect)(command, indirect, 0, 1, 0);
                  check(VK_FUNCTION(triangle.instance, EndCommandBuffer)(command) == VK_ERROR_UNKNOWN,
                        "zero stride reaches the written-parameters refusal for both indirect forms");
                  VK_FUNCTION(triangle.instance, FreeCommandBuffers)(triangle.device, triangle.pool,
                                                                      1, &command);
               }
            }
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
