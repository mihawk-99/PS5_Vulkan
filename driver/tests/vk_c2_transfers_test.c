/*
 * PS5 Vulkan driver - Phase C2 test: vkCmdFillBuffer and vkCmdUpdateBuffer.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C2 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * The console's memory is shared between the CPU and the GPU, and the driver's
 * transfers are CPU work at a submission split point: the queue runs the words
 * recorded before the transfer, waits for that step, does the work, and only
 * then runs the words after it, which is what keeps Vulkan's command order
 * (driver/ps5vk_queue.c, Phase C2). vkCmdFillBuffer and vkCmdUpdateBuffer are
 * that same work -- a fill and a memcpy -- so nothing here renders and no probe
 * was needed for them, and this test is what proves them: every write is read
 * back from the mapped buffer, which is the memory the GPU would read.
 *
 * The order a transfer falls in is the case worth testing: a fill followed by
 * an update of the same bytes has to leave the update's bytes, which only holds
 * if the queue really splits the submission between them. vkCmdUpdateBuffer also
 * snapshots the application's data -- Vulkan lets the application free it when
 * the call returns -- which the last case checks by overwriting the source after
 * recording and before submitting. The PS5 build only links.
 */

#include <string.h>

#include "ps5vk_test.h"

#define BUFFER_BYTES 256u
#define BUFFER_WORDS (BUFFER_BYTES / 4u)
#define FILL_WORD UINT32_C(0x11223344)
#define SECOND_WORD UINT32_C(0xaabbccdd)
#define WHOLE_WORD UINT32_C(0x55667788)
#define PATTERN 0x9e3779b9u

static VkInstance g_instance;
static VkDevice g_device;
static VkQueue g_queue;

/* Submits one recorded command buffer and waits for it, so every write has run
 * by the time it returns. */
static VkResult
submit(VkFence fence, const VkCommandBuffer *buffers, uint32_t count)
{
   const VkSubmitInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = count,
      .pCommandBuffers = buffers,
   };
   VkResult result = VK_FUNCTION(g_instance, QueueSubmit)(g_queue, 1, &info, fence);
   if (result != VK_SUCCESS)
      return result;
   result = VK_FUNCTION(g_instance, WaitForFences)(g_device, 1, &fence, VK_TRUE, UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;
   return VK_FUNCTION(g_instance, ResetFences)(g_device, 1, &fence);
}

/* Opens a recording the case then fills in. */
static void
begin_record(VkCommandBuffer command)
{
   const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VK_FUNCTION(g_instance, BeginCommandBuffer)(command, &begin);
}

int
main(void)
{
   test_begin("C2 transfers");
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   if (!test_create_device(&g_instance, &physical, &g_device)) {
      test_destroy_device(g_instance, g_device);
      return test_finish();
   }
   VK_FUNCTION(g_instance, GetDeviceQueue)(g_device, 0, 0, &g_queue);

   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BUFFER_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
   };
   VkBuffer buffer = VK_NULL_HANDLE;
   check(VK_FUNCTION(g_instance, CreateBuffer)(g_device, &buffer_info, NULL, &buffer) == VK_SUCCESS &&
            buffer != VK_NULL_HANDLE,
         "a 256-byte transfer buffer");
   VkMemoryRequirements requirements = {0};
   VK_FUNCTION(g_instance, GetBufferMemoryRequirements)(g_device, buffer, &requirements);
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   check(VK_FUNCTION(g_instance, AllocateMemory)(g_device, &memory_info, NULL, &memory) == VK_SUCCESS,
         "memory for the buffer");
   check(VK_FUNCTION(g_instance, BindBufferMemory)(g_device, buffer, memory, 0) == VK_SUCCESS,
         "the buffer is bound");
   void *mapped = NULL;
   check(VK_FUNCTION(g_instance, MapMemory)(g_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) ==
            VK_SUCCESS && mapped != NULL,
         "the buffer's memory is mapped, which is where a readback reads it");
   if (!mapped)
      goto finish;

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateCommandPool)(g_device, &pool_info, NULL, &pool) != VK_SUCCESS) {
      check(false, "a command pool");
      goto finish;
   }
   const VkCommandBufferAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command = VK_NULL_HANDLE;
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, AllocateCommandBuffers)(g_device, &allocate_info, &command) !=
          VK_SUCCESS ||
       VK_FUNCTION(g_instance, CreateFence)(g_device, &fence_info, NULL, &fence) != VK_SUCCESS) {
      check(false, "a command buffer and a fence");
      goto finish;
   }

   /* Every word starts as the pattern, so a fill that writes past its range
    * changes a word this test then sees. */
   for (uint32_t word = 0; word < BUFFER_WORDS; word++)
      ((uint32_t *)mapped)[word] = PATTERN;

   /* vkCmdFillBuffer over the first quarter. */
   {
      begin_record(command);
      VK_FUNCTION(g_instance, CmdFillBuffer)(command, buffer, 0, 64, FILL_WORD);
      VK_FUNCTION(g_instance, EndCommandBuffer)(command);
      check(submit(fence, &command, 1) == VK_SUCCESS, "a fill submits and signals its fence");
      bool filled = true;
      for (uint32_t word = 0; word < 16; word++)
         filled = filled && ((uint32_t *)mapped)[word] == FILL_WORD;
      bool untouched = true;
      for (uint32_t word = 16; word < BUFFER_WORDS; word++)
         untouched = untouched && ((uint32_t *)mapped)[word] == PATTERN;
      check(filled, "the fill's range holds its word");
      check(untouched, "and nothing outside the fill's range changed");
   }

   /* A fill and then an update of bytes the fill wrote: the queue splits the
    * submission between them, so the update's bytes are the ones left. */
   {
      const uint32_t updated[2] = {UINT32_C(0xdeadbeef), UINT32_C(0xcafebabe)};
      uint32_t source[2] = {updated[0], updated[1]};
      begin_record(command);
      VK_FUNCTION(g_instance, CmdFillBuffer)(command, buffer, 64, 8, SECOND_WORD);
      VK_FUNCTION(g_instance, CmdUpdateBuffer)(command, buffer, 72, sizeof(source), source);
      /* The application may free its data when the call returns: overwriting it
       * here is what proves the driver snapshotted it. */
      source[0] = 0;
      source[1] = 0;
      VK_FUNCTION(g_instance, EndCommandBuffer)(command);
      check(submit(fence, &command, 1) == VK_SUCCESS, "a fill and an update submit together");
      check(((uint32_t *)mapped)[16] == SECOND_WORD && ((uint32_t *)mapped)[17] == SECOND_WORD,
            "the fill's two words are there");
      check(((uint32_t *)mapped)[18] == updated[0] && ((uint32_t *)mapped)[19] == updated[1],
            "the update after it wins the bytes it covers");
      check(((uint32_t *)mapped)[20] == PATTERN, "and stops where its range does");
   }

   /* VK_WHOLE_SIZE fills from the offset to the end of the buffer. */
   {
      begin_record(command);
      VK_FUNCTION(g_instance, CmdFillBuffer)(command, buffer, 128, VK_WHOLE_SIZE, WHOLE_WORD);
      VK_FUNCTION(g_instance, EndCommandBuffer)(command);
      check(submit(fence, &command, 1) == VK_SUCCESS, "a whole-buffer fill submits");
      bool whole = true;
      for (uint32_t word = 32; word < BUFFER_WORDS; word++)
         whole = whole && ((uint32_t *)mapped)[word] == WHOLE_WORD;
      check(whole, "VK_WHOLE_SIZE fills from the offset to the end");
      check(((uint32_t *)mapped)[31] == PATTERN, "and leaves the bytes before it alone");
   }

   VK_FUNCTION(g_instance, DestroyFence)(g_device, fence, NULL);
   VK_FUNCTION(g_instance, DestroyCommandPool)(g_device, pool, NULL);

finish:
   VK_FUNCTION(g_instance, UnmapMemory)(g_device, memory);
   VK_FUNCTION(g_instance, FreeMemory)(g_device, memory, NULL);
   VK_FUNCTION(g_instance, DestroyBuffer)(g_device, buffer, NULL);
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
