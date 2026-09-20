/*
 * PS5 Vulkan driver - Phase B5 test: command buffers, submission, fences and
 * semaphores.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B5 (docs/M5_PHASE_B.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h). A
 * submission with command buffers signals its fence only once the completion
 * marker holds its value; on the PC the host layer writes it when it finds a
 * well-formed marker packet aimed at mapped direct memory in the submitted
 * stream, so every passing submission also proves that packet.
 */

#include "ps5vk_test.h"

#define SECOND_NS UINT64_C(1000000000)
#define REPEATED_SUBMISSIONS 100u

static VkInstance g_instance;
static VkDevice g_device;
static VkQueue g_queue;

static VkResult
submit(uint32_t buffer_count, const VkCommandBuffer *buffers, VkSemaphore wait, VkSemaphore signal,
       VkFence fence)
{
   const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   const VkSubmitInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = wait != VK_NULL_HANDLE,
      .pWaitSemaphores = &wait,
      .pWaitDstStageMask = &stage,
      .commandBufferCount = buffer_count,
      .pCommandBuffers = buffers,
      .signalSemaphoreCount = signal != VK_NULL_HANDLE,
      .pSignalSemaphores = &signal,
   };
   return VK_FUNCTION(g_instance, QueueSubmit)(g_queue, 1, &info, fence);
}

static bool
record(VkCommandBuffer buffer)
{
   const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   return VK_FUNCTION(g_instance, BeginCommandBuffer)(buffer, &begin) == VK_SUCCESS &&
          VK_FUNCTION(g_instance, EndCommandBuffer)(buffer) == VK_SUCCESS;
}

static VkResult
fence_status(VkFence fence)
{
   return VK_FUNCTION(g_instance, GetFenceStatus)(g_device, fence);
}

static VkResult
wait_fence(VkFence fence, uint64_t timeout_ns)
{
   return VK_FUNCTION(g_instance, WaitForFences)(g_device, 1, &fence, VK_TRUE, timeout_ns);
}

static bool
reset_fence(VkFence fence)
{
   return VK_FUNCTION(g_instance, ResetFences)(g_device, 1, &fence) == VK_SUCCESS;
}

int
main(void)
{
   test_begin("B5 submit");
   VkPhysicalDevice physical;
   if (!test_create_device(&g_instance, &physical, &g_device)) {
      test_destroy_device(g_instance, g_device);
      return test_finish();
   }
   VK_FUNCTION(g_instance, GetDeviceQueue)(g_device, 0, 0, &g_queue);

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   const VkCommandBufferAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 2,
   };
   VkFence fence = VK_NULL_HANDLE;
   VkFence signaled = VK_NULL_HANDLE;
   VkSemaphore semaphore = VK_NULL_HANDLE;
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   const VkFenceCreateInfo signaled_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
   };
   const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

   bool created =
      VK_FUNCTION(g_instance, CreateCommandPool)(g_device, &pool_info, NULL, &pool) == VK_SUCCESS;
   VkCommandBufferAllocateInfo buffers_info = allocate_info;
   buffers_info.commandPool = pool;
   created = created && VK_FUNCTION(g_instance, AllocateCommandBuffers)(g_device, &buffers_info,
                                                                        buffers) == VK_SUCCESS;
   created = created &&
             VK_FUNCTION(g_instance, CreateFence)(g_device, &fence_info, NULL, &fence) == VK_SUCCESS &&
             VK_FUNCTION(g_instance, CreateFence)(g_device, &signaled_info, NULL, &signaled) ==
                VK_SUCCESS &&
             VK_FUNCTION(g_instance, CreateSemaphore)(g_device, &semaphore_info, NULL, &semaphore) ==
                VK_SUCCESS;
   check(created, "a command pool, two command buffers, two fences and a semaphore");
   if (created) {
      check(record(buffers[0]) && record(buffers[1]), "two empty command buffers record");
      check(fence_status(fence) == VK_NOT_READY && fence_status(signaled) == VK_SUCCESS,
            "a new fence is unsignalled unless created signalled");

      check(submit(1, buffers, VK_NULL_HANDLE, VK_NULL_HANDLE, fence) == VK_SUCCESS &&
               wait_fence(fence, SECOND_NS) == VK_SUCCESS && fence_status(fence) == VK_SUCCESS,
            "a submitted command buffer signals its fence once its completion marker arrives");
      check(reset_fence(fence) && fence_status(fence) == VK_NOT_READY &&
               wait_fence(fence, 0) == VK_TIMEOUT && wait_fence(fence, SECOND_NS / 100) == VK_TIMEOUT,
            "a reset fence is unsignalled, and waits on it time out");
      check(submit(0, NULL, VK_NULL_HANDLE, VK_NULL_HANDLE, fence) == VK_SUCCESS &&
               fence_status(fence) == VK_SUCCESS,
            "a submission without command buffers signals its fence");

      check(reset_fence(fence) &&
               submit(1, &buffers[0], VK_NULL_HANDLE, semaphore, VK_NULL_HANDLE) == VK_SUCCESS &&
               submit(1, &buffers[1], semaphore, VK_NULL_HANDLE, fence) == VK_SUCCESS &&
               fence_status(fence) == VK_SUCCESS,
            "a submission waits on the semaphore another submission signalled");

      bool repeated = true;
      for (unsigned i = 0; i < REPEATED_SUBMISSIONS && repeated; i++)
         repeated = reset_fence(fence) &&
                    submit(2, buffers, VK_NULL_HANDLE, VK_NULL_HANDLE, fence) == VK_SUCCESS &&
                    fence_status(fence) == VK_SUCCESS;
      check(repeated, "100 submissions of two command buffers each complete in turn");

      check(VK_FUNCTION(g_instance, ResetCommandBuffer)(buffers[0], 0) == VK_SUCCESS &&
               record(buffers[0]) && reset_fence(fence) &&
               submit(1, buffers, VK_NULL_HANDLE, VK_NULL_HANDLE, fence) == VK_SUCCESS &&
               fence_status(fence) == VK_SUCCESS,
            "a reset command buffer records and submits again");
      check(VK_FUNCTION(g_instance, QueueWaitIdle)(g_queue) == VK_SUCCESS &&
               VK_FUNCTION(g_instance, DeviceWaitIdle)(g_device) == VK_SUCCESS,
            "the queue and the device wait idle");
   }

   VK_FUNCTION(g_instance, DestroySemaphore)(g_device, semaphore, NULL);
   VK_FUNCTION(g_instance, DestroyFence)(g_device, signaled, NULL);
   VK_FUNCTION(g_instance, DestroyFence)(g_device, fence, NULL);
   if (pool != VK_NULL_HANDLE) {
      VK_FUNCTION(g_instance, FreeCommandBuffers)(g_device, pool, 2, buffers);
      VK_FUNCTION(g_instance, DestroyCommandPool)(g_device, pool, NULL);
   }
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
