/*
 * PS5 Vulkan driver - Phase B5 negative test: a completion that never arrives.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B5 (docs/M5_PHASE_B.md). tools/check-driver.sh runs the
 * direct PC build with PS5_HOST_DROP_COMPLETION_MARKERS=1, so no submitted
 * completion marker is ever written, as when the GPU never runs a stream.
 * The driver must not report success: the submission loses the device, the
 * fence reports it, and the lost queue refuses further submissions.
 */

#include "ps5vk_test.h"

int
main(void)
{
   test_begin("B5 lost completion");
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
   if (!test_create_device(&instance, &physical, &device)) {
      test_destroy_device(instance, device);
      return test_finish();
   }
   VkQueue queue = VK_NULL_HANDLE;
   VK_FUNCTION(instance, GetDeviceQueue)(device, 0, 0, &queue);

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer buffer = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

   bool ready =
      VK_FUNCTION(instance, CreateCommandPool)(device, &pool_info, NULL, &pool) == VK_SUCCESS;
   const VkCommandBufferAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   ready = ready &&
           VK_FUNCTION(instance, AllocateCommandBuffers)(device, &allocate_info, &buffer) == VK_SUCCESS &&
           VK_FUNCTION(instance, BeginCommandBuffer)(buffer, &begin) == VK_SUCCESS &&
           VK_FUNCTION(instance, EndCommandBuffer)(buffer) == VK_SUCCESS &&
           VK_FUNCTION(instance, CreateFence)(device, &fence_info, NULL, &fence) == VK_SUCCESS;
   check(ready, "a recorded command buffer and a fence");
   if (ready) {
      const VkSubmitInfo with_buffer = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &buffer,
      };
      const VkSubmitInfo without_buffers = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
      const __typeof__(&vkQueueSubmit) queue_submit = VK_FUNCTION(instance, QueueSubmit);
      check(queue_submit(queue, 1, &with_buffer, fence) == VK_ERROR_DEVICE_LOST,
            "a submission whose completion marker never arrives loses the device");
      check(VK_FUNCTION(instance, GetFenceStatus)(device, fence) == VK_ERROR_DEVICE_LOST,
            "its fence reports the lost device");
      check(queue_submit(queue, 1, &without_buffers, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST,
            "the lost queue refuses further submissions");
   }

   VK_FUNCTION(instance, DestroyFence)(device, fence, NULL);
   if (pool != VK_NULL_HANDLE)
      VK_FUNCTION(instance, DestroyCommandPool)(device, pool, NULL);
   test_destroy_device(instance, device);
   return test_finish();
}
