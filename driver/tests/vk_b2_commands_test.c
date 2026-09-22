/*
 * PS5 Vulkan driver - Phase B2 test: the core commands with no path.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B2 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Vulkan 1.0's core commands are answered by the runtime's shared dispatch
 * table, and for most of them it has an implementation or a trampoline that
 * reaches one. For thirteen it has neither, so a call lands on an empty slot:
 * with an empty handle it crashes (the round's first, wrong reading, which
 * refused the render pass family and broke Mesa's meta clears), and with the
 * handles a valid call needs it crashes only where the command really has no
 * path. Two sweeps over the core commands, one call per fork
 * (docs/M5_PHASE_B.md, 2026-09-18), found these thirteen:
 *
 *   - vkCmdClearDepthStencilImage and vkCreateComputePipelines (NULL entry points),
 *   - vkCreateEvent, vkCreatePipelineCache and (then) vkCreateBufferView,
 *   - vkCmdExecuteCommands and vkCmdCopyQueryPoolResults,
 *
 * and driver/ps5vk_refusals.c refuses each by name with the phase that would
 * implement it. Six were in that list and are implemented instead:
 * vkGetDeviceMemoryCommitment is the allocation's size (ps5vk_AllocateMemory
 * commits all of it), vkCmdFillBuffer and vkCmdUpdateBuffer are CPU work at a
 * submission split point (driver/tests/vk_c2_transfers_test.c reads the mapped
 * buffer back, including that a fill followed by an update leaves the update's
 * bytes and that the update snapshots the application's data), and
 * vkCmdCopyImageToBuffer is the same work over an image's own texel map
 * (driver/tests/vk_c7_readback_test.c reads a linear image back tight, padded
 * and by region, and a tiled one through the tile map), and vkCmdClearColorImage
 * is that work writing one encoded texel per texel (driver/tests/
 * vk_c7_clear_image_test.c), and the indirect draws read their parameters from
 * the bound buffer when the draw is recorded, refusing only a command buffer
 * that writes that buffer itself (driver/tests/vk_c2_indirect_test.c compares
 * the resulting submission with the console's b4-headless frame). Two more of
 * the thirteen are now objects rather than refusals: an event is the flag a
 * binary sync object already was (driver/tests/vk_b5_events_test.c) and a
 * pipeline cache is the empty cache this driver is allowed to have
 * (driver/tests/vk_b6_pipeline_cache_test.c). The depth
 * depth clear is implemented too now
 * (ps5vk_CmdClearDepthStencilImage, driver/ps5vk_image.c): the depth image's
 * tiled map was proved by C5's readbacks, so the clear is the colour clear's
 * shape through it, and its own case (c5-depth-clear) holds the value in every
 * texel on the console.
 *
 * This program is the check for that: it calls every one of them the way an
 * application would and asserts the refusal. Nothing renders or dispatches, so
 * there is no frame and no golden: the PC runs it and the PS5 build only links.
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#if defined(__linux__)
#include <unistd.h>
#endif

#include "ps5vk_test.h"

/* Records one command on a command buffer of its own and returns what
 * vkEndCommandBuffer said: the driver's refusal is that result. */
static VkResult
record_one(VkInstance instance, VkDevice device, VkCommandPool pool, const char *name)
{
   const __typeof__(&vkAllocateCommandBuffers) allocate =
      VK_FUNCTION(instance, AllocateCommandBuffers);
   const __typeof__(&vkBeginCommandBuffer) begin = VK_FUNCTION(instance, BeginCommandBuffer);
   const __typeof__(&vkEndCommandBuffer) end = VK_FUNCTION(instance, EndCommandBuffer);
   VkCommandBuffer command = VK_NULL_HANDLE;
   const VkCommandBufferAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   if (allocate(device, &allocate_info, &command) != VK_SUCCESS)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if (begin(command, &begin_info) != VK_SUCCESS)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   if (strcmp(name, "vkCmdClearColorImage") == 0) {
      const VkClearColorValue value = {{0.0f, 0.0f, 0.0f, 1.0f}};
      VK_FUNCTION(instance, CmdClearColorImage)(command, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                                                &value, 0, NULL);
   } else if (strcmp(name, "vkCmdClearDepthStencilImage") == 0) {
      const VkClearDepthStencilValue value = {1.0f, 0};
      VK_FUNCTION(instance, CmdClearDepthStencilImage)(command, VK_NULL_HANDLE,
                                                       VK_IMAGE_LAYOUT_GENERAL, &value, 0, NULL);
   } else if (strcmp(name, "vkCmdWriteTimestamp") == 0) {
      VK_FUNCTION(instance, CmdWriteTimestamp)(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_NULL_HANDLE, 0);
   } else if (strcmp(name, "vkCmdCopyQueryPoolResults") == 0) {
      VK_FUNCTION(instance, CmdCopyQueryPoolResults)(command, VK_NULL_HANDLE, 0, 1, VK_NULL_HANDLE,
                                                     0, sizeof(uint32_t), VK_QUERY_RESULT_WAIT_BIT);
   } else {
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   return end(command);
}

int
main(void)
{
   test_begin("B2 unimplemented commands");
   VkInstance instance = VK_NULL_HANDLE;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   if (!test_create_device(&instance, &physical, &device)) {
      (void)physical;
      return test_finish();
   }

   /* The one creating command: a compute pipeline is Phase D2. */
   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
   };
   VkPipeline pipeline = VK_NULL_HANDLE;
   check(VK_FUNCTION(instance, CreateComputePipelines)(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                       NULL, &pipeline) == VK_ERROR_UNKNOWN &&
            pipeline == VK_NULL_HANDLE,
         "vkCreateComputePipelines refuses a compute pipeline instead of crashing");

   const VkBufferViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .range = 16,
   };
   /* Buffer views have a path now (ps5vk_CreateBufferView): this call names no
    * buffer, so it refuses for that, and a call naming one refuses because the
    * format carries no texel-buffer feature -- the rule the driver reports
    * through vkGetPhysicalDeviceFormatProperties, which
    * driver/tests/vk_b2_buffer_view_test.c checks against it. */
   VkBufferView buffer_view = VK_NULL_HANDLE;
   check(VK_FUNCTION(instance, CreateBufferView)(device, &view_info, NULL, &buffer_view) ==
            VK_ERROR_UNKNOWN,
         "vkCreateBufferView refuses a view of no buffer instead of crashing");

   /* Allocated memory is committed: ps5vk_AllocateMemory maps it in one piece,
    * and the device reports no lazily allocated memory. */
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = 4096,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   if (VK_FUNCTION(instance, AllocateMemory)(device, &memory_info, NULL, &memory) == VK_SUCCESS) {
      VkDeviceSize committed = 0;
      VK_FUNCTION(instance, GetDeviceMemoryCommitment)(device, memory, &committed);
      check(committed == 4096, "vkGetDeviceMemoryCommitment is the allocation's size");
      VK_FUNCTION(instance, FreeMemory)(device, memory, NULL);
   }

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   check(VK_FUNCTION(instance, CreateCommandPool)(device, &pool_info, NULL, &pool) == VK_SUCCESS,
         "a command pool for the refused commands");
   if (pool != VK_NULL_HANDLE) {
      /* Every recording command this driver has no path for leaves the command
       * buffer in error rather than jumping through an empty dispatch slot. The
       * depth clear (ps5vk_CmdClearDepthStencilImage) and the query-result copy
       * (ps5vk_CmdCopyQueryPoolResults) have paths now: the copy is here because
       * a call with no pool refuses rather than crashing, which is the same
       * promise for a caller that got its handles wrong. */
      static const char *const refused[] = {
         "vkCmdCopyQueryPoolResults",
      };
      for (size_t index = 0; index < sizeof(refused) / sizeof(refused[0]); index++) {
         char what[128];
         snprintf(what, sizeof(what), "%s refuses instead of crashing", refused[index]);
         check(record_one(instance, device, pool, refused[index]) == VK_ERROR_UNKNOWN, what);
      }
#if defined(__linux__)
      /* This instance has no debug messenger. Its recording refusal must still
       * reach stderr, which a console title redirects into its trace. */
      FILE *const capture = tmpfile();
      const int saved_stderr = dup(STDERR_FILENO);
      check(capture != NULL && saved_stderr >= 0, "capture refusal without a debug messenger");
      if (capture != NULL && saved_stderr >= 0) {
         fflush(stderr);
         const int redirected = dup2(fileno(capture), STDERR_FILENO);
         check(redirected >= 0, "redirect refusal trace");
         if (redirected >= 0) {
            const VkResult result = record_one(instance, device, pool, "vkCmdCopyQueryPoolResults");
            fflush(stderr);
            check(dup2(saved_stderr, STDERR_FILENO) >= 0, "restore refusal trace");
            rewind(capture);
            char text[2048] = {0};
            (void)fread(text, 1, sizeof(text) - 1, capture);
            check(result == VK_ERROR_UNKNOWN && strstr(text, "a query-result copy names a query pool"),
                  "recording refusal carries its sentence without a debug messenger");
         }
      }
      if (saved_stderr >= 0)
         close(saved_stderr);
      if (capture != NULL)
         fclose(capture);
#endif
      VK_FUNCTION(instance, DestroyCommandPool)(device, pool, NULL);
   }

   VK_FUNCTION(instance, DestroyDevice)(device, NULL);
   VK_FUNCTION(instance, DestroyInstance)(instance, NULL);
   return test_finish();
}
