/*
 * PS5 Vulkan driver - compute dispatch through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase D2 (docs/M5_REFERENCE.md). See ps5vk_compute.h for what the
 * program is; this file is the program. The dispatch reaches the queue as the
 * packets the V0-compute probe proved on the console (driver/ps5vk_compute.c),
 * and what the console proves here is that the same packets come out of an
 * ordinary Vulkan program: one storage buffer bound to a compute pipeline, one
 * dispatch, and the shader's word in the readback.
 */

#include "ps5vk_compute.h"

#include <stdio.h>
#include <string.h>

/* The harness's calling convention: every Vulkan function comes from the
 * caller's GetInstanceProcAddr, through the instance. */
#define CALL(compute, name)                                                                        \
   ((__typeof__(&vk##name))(compute)->get_instance_proc_addr((compute)->instance, "vk" #name))

/* One reported step. */
static bool step(struct ps5vk_compute *compute, const struct ps5vk_compute_input *input,
                 const char *name, VkResult result, const char *detail)
{
   (void)compute;
   const bool passed = result == VK_SUCCESS;
   if (input->report.step)
      input->report.step(input->report.context, name, passed, (int)result, detail ? detail : "");
   return passed;
}

bool ps5vk_compute_run(const struct ps5vk_compute_input *input, struct ps5vk_compute *compute)
{
   memset(compute, 0, sizeof(*compute));
   compute->get_instance_proc_addr = input->get_instance_proc_addr;
   if (compute->get_instance_proc_addr == NULL || input->spirv == NULL ||
      input->spirv_bytes == 0 || input->entry_point == NULL)
   {
      if (input->report.step)
         input->report.step(input->report.context, "compute_input", false,
                               (int)VK_ERROR_INITIALIZATION_FAILED,
                               "a dispatch needs SPIR-V, an entry point and a GetInstanceProcAddr");
      return false;
   }

   const VkApplicationInfo application = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "ps5vk compute",
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
   };
   if (!step(compute, input, "create_instance",
              CALL(compute, CreateInstance)(&instance_info, NULL, &compute->instance), NULL))
      return false;

   uint32_t physical_count = 1;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   VkResult result =
      CALL(compute, EnumeratePhysicalDevices)(compute->instance, &physical_count, &physical);
   if (result == VK_INCOMPLETE)
      result = VK_SUCCESS;
   if (!step(compute, input, "enumerate_physical_devices", result, NULL) ||
      physical == VK_NULL_HANDLE)
      return false;
   compute->physical = physical;

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   if (!step(compute, input, "create_device",
              CALL(compute, CreateDevice)(physical, &device_info, NULL, &compute->device), NULL))
      return false;
   CALL(compute, GetDeviceQueue)(compute->device, 0, 0, &compute->queue);

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = input->spirv_bytes,
      .pCode = input->spirv,
   };
   if (!step(compute, input, "create_shader_module",
              CALL(compute, CreateShaderModule)(compute->device, &module_info, NULL,
                                    &compute->module),
              "the caller's compute SPIR-V"))
      return false;

   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   if (!step(compute, input, "create_descriptor_set_layout",
              CALL(compute, CreateDescriptorSetLayout)(compute->device, &set_layout_info, NULL,
                                                       &compute->set_layout),
              "one storage buffer at set 0 binding 0"))
      return false;

   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &compute->set_layout,
   };
   if (!step(compute, input, "create_pipeline_layout",
              CALL(compute, CreatePipelineLayout)(compute->device, &layout_info, NULL,
                                                  &compute->pipeline_layout),
              NULL))
      return false;

   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = compute->module,
                  .pName = input->entry_point},
      .layout = compute->pipeline_layout,
   };
   if (!step(compute, input, "create_compute_pipeline",
              CALL(compute, CreateComputePipelines)(compute->device, VK_NULL_HANDLE, 1,
                                       &pipeline_info, NULL, &compute->pipeline),
              "the caller's entry point"))
      return false;

   /* One host-visible storage buffer, mapped and filled with the caller's
     * initial word: what the shader writes over it is the readback. */
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = PS5VK_COMPUTE_BUFFER_BYTES,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (!step(compute, input, "create_buffer",
              CALL(compute, CreateBuffer)(compute->device, &buffer_info, NULL, &compute->buffer),
              "one storage buffer"))
      return false;
   VkMemoryRequirements requirements;
   CALL(compute, GetBufferMemoryRequirements)(compute->device, compute->buffer, &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(compute, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
         index++)
   {
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
         (memory_properties.memoryTypes[index].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
         memory_type = index;
   }
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   if (!step(compute, input, "allocate_memory",
              memory_type == UINT32_MAX
                  ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                  : CALL(compute, AllocateMemory)(compute->device, &allocate_info, NULL,
                                                  &compute->memory),
              "for the storage buffer") ||
      !step(compute, input, "bind_buffer_memory",
              CALL(compute, BindBufferMemory)(compute->device, compute->buffer, compute->memory, 0),
              NULL) ||
      !step(compute, input, "map_memory",
              CALL(compute, MapMemory)(compute->device, compute->memory, 0, VK_WHOLE_SIZE, 0,
                                       &compute->mapped),
              NULL))
      return false;
   uint32_t *const words = compute->mapped;
   for (size_t index = 0; index < PS5VK_COMPUTE_BUFFER_BYTES / sizeof(uint32_t); index++)
      words[index] = input->initial_word;
   if (input->indirect)
      /* VkDispatchIndirectCommand: one workgroup in each dimension, so a dispatch
       * that read a zero would run nothing. */
      for (unsigned axis = 0; axis < 3; axis++)
         words[PS5VK_COMPUTE_INDIRECT_OFFSET / sizeof(uint32_t) + axis] =
            PS5VK_COMPUTE_GROUP_COUNT;

   const VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   if (!step(compute, input, "create_descriptor_pool",
              CALL(compute, CreateDescriptorPool)(compute->device, &pool_info, NULL,
                                                  &compute->descriptor_pool),
              NULL))
      return false;
   const VkDescriptorSetAllocateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = compute->descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &compute->set_layout,
   };
   if (!step(compute, input, "allocate_descriptor_sets",
              CALL(compute, AllocateDescriptorSets)(compute->device, &set_info,
                                       &compute->descriptor_set),
              NULL))
      return false;
   const VkDescriptorBufferInfo buffer_binding = {
      .buffer = compute->buffer,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = compute->descriptor_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &buffer_binding,
   };
   CALL(compute, UpdateDescriptorSets)(compute->device, 1, &write, 0, NULL);

   const VkCommandPoolCreateInfo command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   if (!step(compute, input, "create_command_pool",
              CALL(compute, CreateCommandPool)(compute->device, &command_pool_info, NULL,
                                               &compute->command_pool),
              NULL))
      return false;
   const VkCommandBufferAllocateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = compute->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   if (!step(compute, input, "allocate_command_buffer",
              CALL(compute, AllocateCommandBuffers)(compute->device, &command_info,
                                       &compute->command),
              NULL))
      return false;

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   const VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
   };
   if (!step(compute, input, "begin_command_buffer",
              CALL(compute, BeginCommandBuffer)(compute->command, &begin_info), NULL))
      return false;
   CALL(compute, CmdPipelineBarrier)(compute->command, VK_PIPELINE_STAGE_HOST_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL,
                                      0, NULL);
   CALL(compute, CmdBindPipeline)(compute->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                   compute->pipeline);
   CALL(compute, CmdBindDescriptorSets)(compute->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                         compute->pipeline_layout, 0, 1, &compute->descriptor_set,
                                         0, NULL);
   if (input->indirect)
      CALL(compute, CmdDispatchIndirect)(compute->command, compute->buffer,
                                        PS5VK_COMPUTE_INDIRECT_OFFSET);
   else
      CALL(compute, CmdDispatch)(compute->command, PS5VK_COMPUTE_GROUP_COUNT,
                                PS5VK_COMPUTE_GROUP_COUNT, PS5VK_COMPUTE_GROUP_COUNT);
   if (!step(compute, input, "end_command_buffer",
              CALL(compute, EndCommandBuffer)(compute->command),
              input->indirect ? "one indirect dispatch" : "one dispatch"))
      return false;

   const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &compute->command,
   };
   if (!step(compute, input, "create_fence",
              CALL(compute, CreateFence)(compute->device, &fence_info, NULL, &compute->fence),
              NULL) ||
      !step(compute, input, "queue_submit",
              CALL(compute, QueueSubmit)(compute->queue, 1, &submit_info, compute->fence),
              "one dispatch"))
      return false;
   if (!step(
         compute, input, "wait_for_fences",
         CALL(compute, WaitForFences)(compute->device, 1, &compute->fence, VK_TRUE, UINT64_MAX),
         NULL))
      return false;
   /* The host wrote the buffer before the dispatch and reads what the shader
     * left after its fence: coherent memory is the one mapping both see. */
   compute->result_word = words[0];
   return true;
}

void ps5vk_compute_finish(struct ps5vk_compute *compute)
{
   if (compute->device == VK_NULL_HANDLE)
      return;
   CALL(compute, DeviceWaitIdle)(compute->device);
   if (compute->fence != VK_NULL_HANDLE)
      CALL(compute, DestroyFence)(compute->device, compute->fence, NULL);
   if (compute->command_pool != VK_NULL_HANDLE)
      CALL(compute, DestroyCommandPool)(compute->device, compute->command_pool, NULL);
   if (compute->descriptor_pool != VK_NULL_HANDLE)
      CALL(compute, DestroyDescriptorPool)(compute->device, compute->descriptor_pool, NULL);
   if (compute->pipeline != VK_NULL_HANDLE)
      CALL(compute, DestroyPipeline)(compute->device, compute->pipeline, NULL);
   if (compute->pipeline_layout != VK_NULL_HANDLE)
      CALL(compute, DestroyPipelineLayout)(compute->device, compute->pipeline_layout, NULL);
   if (compute->set_layout != VK_NULL_HANDLE)
      CALL(compute, DestroyDescriptorSetLayout)(compute->device, compute->set_layout, NULL);
   if (compute->module != VK_NULL_HANDLE)
      CALL(compute, DestroyShaderModule)(compute->device, compute->module, NULL);
   if (compute->mapped != NULL)
      CALL(compute, UnmapMemory)(compute->device, compute->memory);
   if (compute->memory != VK_NULL_HANDLE)
      CALL(compute, FreeMemory)(compute->device, compute->memory, NULL);
   if (compute->buffer != VK_NULL_HANDLE)
      CALL(compute, DestroyBuffer)(compute->device, compute->buffer, NULL);
   CALL(compute, DestroyDevice)(compute->device, NULL);
   if (compute->instance != VK_NULL_HANDLE)
      CALL(compute, DestroyInstance)(compute->instance, NULL);
   compute->device = VK_NULL_HANDLE;
}
