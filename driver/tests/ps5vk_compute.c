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

   const uint32_t set_count = input->images ? 2 : 1;
   const uint32_t sources = input->descriptor_array ? 3 : 1;
   for (uint32_t i = 0; i < set_count; i++) {
   VkDescriptorSetLayoutBinding bindings[2] = {{
      .binding = 0,
      .descriptorType = input->images ? (i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   }};
   if (input->descriptor_array && i == 0) {
      bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      bindings[1] = (VkDescriptorSetLayoutBinding){
         .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .descriptorCount = 3, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      };
   }
   const VkDescriptorSetLayoutCreateInfo set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = input->descriptor_array && i == 0 ? 2 : 1,
      .pBindings = bindings,
   };
   if (!step(compute, input, "create_descriptor_set_layout",
              CALL(compute, CreateDescriptorSetLayout)(compute->device, &set_layout_info, NULL,
                                                       &compute->set_layout[i]),
              "binding 0 of the compute set"))
      return false;
   }

   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = set_count,
      .pSetLayouts = compute->set_layout,
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
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
               (input->images ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0),
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

   if (input->images) {
      /* 64x4 RGBA texels: full 256-byte rows in the driver's linear storage. */
      uint8_t *pixels = compute->mapped;
      for (uint32_t source = 0; source < sources; source++)
      for (uint32_t y = 0; y < 4; y++)
         for (uint32_t x = 0; x < 64; x++) {
            const uint32_t at = source * 1024 + (y * 64 + x) * 4;
            pixels[at] = (uint8_t)(x * 3 + source * 17);
            pixels[at + 1] = (uint8_t)(y * 61 + 7 + source * 11);
            pixels[at + 2] = (uint8_t)(255 - x * 2 - source * 23);
            pixels[at + 3] = 255;
         }
      for (uint32_t i = 0; i <= sources; i++) {
         const VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {64, 4, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = i < sources ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            : VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         };
         if (!step(compute, input, "create_image",
                   CALL(compute, CreateImage)(compute->device, &image_info, NULL, &compute->images[i]), NULL))
            return false;
         CALL(compute, GetImageMemoryRequirements)(compute->device, compute->images[i], &requirements);
         uint32_t image_type = 0;
         while (image_type < memory_properties.memoryTypeCount &&
                !(requirements.memoryTypeBits & (1u << image_type)))
            image_type++;
         const VkMemoryAllocateInfo image_allocate = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = image_type,
         };
         if (!step(compute, input, "allocate_image_memory",
                   CALL(compute, AllocateMemory)(compute->device, &image_allocate, NULL, &compute->image_memory[i]), NULL) ||
             !step(compute, input, "bind_image_memory",
                   CALL(compute, BindImageMemory)(compute->device, compute->images[i], compute->image_memory[i], 0), NULL))
            return false;
         const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = compute->images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         if (!step(compute, input, "create_image_view",
                   CALL(compute, CreateImageView)(compute->device, &view_info, NULL, &compute->views[i]), NULL))
            return false;
      }
      const VkSamplerCreateInfo sampler_info = {
         .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
         .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
         .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
         .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      };
      if (!step(compute, input, "create_sampler",
                CALL(compute, CreateSampler)(compute->device, &sampler_info, NULL, &compute->sampler), NULL))
         return false;
   }
   const VkDescriptorPoolSize pool_sizes[3] = {
      {input->descriptor_array ? VK_DESCRIPTOR_TYPE_SAMPLER :
       input->images ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       input->descriptor_array ? 2 : 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 6},
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = input->descriptor_array ? 3 : set_count,
      .poolSizeCount = input->descriptor_array ? 3 : set_count, .pPoolSizes = pool_sizes,
   };
   if (!step(compute, input, "create_descriptor_pool",
              CALL(compute, CreateDescriptorPool)(compute->device, &pool_info, NULL,
                                                  &compute->descriptor_pool), NULL))
      return false;
   const VkDescriptorSetAllocateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = compute->descriptor_pool,
      .descriptorSetCount = set_count, .pSetLayouts = compute->set_layout,
   };
   if (!step(compute, input, "allocate_descriptor_sets",
              CALL(compute, AllocateDescriptorSets)(compute->device, &set_info,
                                                    compute->descriptor_set), NULL))
      return false;
   const VkDescriptorBufferInfo buffer_binding = {
      .buffer = compute->buffer, .offset = 0, .range = VK_WHOLE_SIZE,
   };
   for (uint32_t i = 0; i < set_count; i++) {
      const VkDescriptorImageInfo image_binding = {
         .sampler = i == 0 ? compute->sampler : VK_NULL_HANDLE,
         .imageView = compute->views[i == 0 ? 0 : sources], .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
      const VkWriteDescriptorSet write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = compute->descriptor_set[i], .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = pool_sizes[i].type,
         .pBufferInfo = input->images ? NULL : &buffer_binding,
         .pImageInfo = input->images ? &image_binding : NULL,
      };
      CALL(compute, UpdateDescriptorSets)(compute->device, 1, &write, 0, NULL);
   }

   if (input->descriptor_array) {
      const VkDescriptorSetAllocateInfo extra = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = compute->descriptor_pool, .descriptorSetCount = 1,
         .pSetLayouts = &compute->set_layout[0],
      };
      if (!step(compute, input, "allocate_array_source",
                CALL(compute, AllocateDescriptorSets)(compute->device, &extra,
                                                      &compute->descriptor_set[2]), NULL))
         return false;
      VkDescriptorImageInfo images[3];
      for (uint32_t i = 0; i < 3; i++)
         images[i] = (VkDescriptorImageInfo){.imageView = compute->views[i],
                                            .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = compute->descriptor_set[2],
         .dstBinding = 2, .descriptorCount = 3, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .pImageInfo = images,
      };
      CALL(compute, UpdateDescriptorSets)(compute->device, 1, &write, 0, NULL);
      VkCopyDescriptorSet copies[2] = {
         {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = compute->descriptor_set[2],
          .srcBinding = 2, .dstSet = compute->descriptor_set[0], .dstBinding = 2,
          .descriptorCount = 3},
         {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = compute->descriptor_set[2],
          .srcBinding = 2, .dstSet = compute->descriptor_set[0], .dstBinding = 2,
          .dstArrayElement = 1, .descriptorCount = 2},
      };
      CALL(compute, UpdateDescriptorSets)(compute->device, 0, NULL, 2, copies);
      /* Final order [2,0,1]: the copied records must not alias their source. */
      write.dstSet = compute->descriptor_set[0];
      write.descriptorCount = 1;
      write.pImageInfo = &images[2];
      CALL(compute, UpdateDescriptorSets)(compute->device, 1, &write, 0, NULL);
      write.dstSet = compute->descriptor_set[2];
      write.dstArrayElement = 1;
      CALL(compute, UpdateDescriptorSets)(compute->device, 1, &write, 0, NULL);
   }

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
   VkBufferImageCopy image_copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {64, 4, 1},
   };
   if (input->images) {
      VkImageMemoryBarrier image_barriers[4] = {0};
      for (uint32_t i = 0; i <= sources; i++) {
         image_barriers[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = i < sources ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = compute->images[i],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
      }
      CALL(compute, CmdPipelineBarrier)(compute->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
         0, NULL, 0, NULL, sources + 1, image_barriers);
      for (uint32_t i = 0; i < sources; i++) {
         image_copy.bufferOffset = i * 1024;
         CALL(compute, CmdCopyBufferToImage)(compute->command, compute->buffer, compute->images[i],
                                           VK_IMAGE_LAYOUT_GENERAL, 1, &image_copy);
         image_barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         image_barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         image_barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      }
      CALL(compute, CmdPipelineBarrier)(compute->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, sources, image_barriers);
   }
   CALL(compute, CmdBindPipeline)(compute->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                   compute->pipeline);
   CALL(compute, CmdBindDescriptorSets)(compute->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                         compute->pipeline_layout, 0, set_count, compute->descriptor_set,
                                         0, NULL);
   if (input->indirect)
      CALL(compute, CmdDispatchIndirect)(compute->command, compute->buffer,
                                        PS5VK_COMPUTE_INDIRECT_OFFSET);
   else
      CALL(compute, CmdDispatch)(compute->command, PS5VK_COMPUTE_GROUP_COUNT,
                                input->images ? 4u : PS5VK_COMPUTE_GROUP_COUNT, PS5VK_COMPUTE_GROUP_COUNT);
   if (input->images) {
      const VkMemoryBarrier readback_barrier = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      };
      CALL(compute, CmdPipelineBarrier)(compute->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &readback_barrier, 0, NULL, 0, NULL);
      image_copy.bufferOffset = sources * 1024;
      CALL(compute, CmdCopyImageToBuffer)(compute->command, compute->images[sources],
                                        VK_IMAGE_LAYOUT_GENERAL, compute->buffer, 1, &image_copy);
      const VkMemoryBarrier host_barrier = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      CALL(compute, CmdPipelineBarrier)(compute->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, NULL, 0, NULL);
   }
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
   if (input->images) {
      const uint8_t *pixels = compute->mapped;
      for (uint32_t at = 0; at < 1024; at += 4) {
         const uint8_t expected[4] = {
            input->descriptor_array ? pixels[2048 + at] : pixels[at + 2],
            pixels[at + 1], input->descriptor_array ? pixels[1024 + at + 2] : pixels[at],
            pixels[at + 3]};
         if (memcmp(pixels + sources * 1024 + at, expected, 4) != 0)
            compute->mismatched_texels++;
      }
   }
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
   for (uint32_t i = 0; i < 4; i++) {
      if (i < 2 && compute->set_layout[i] != VK_NULL_HANDLE)
         CALL(compute, DestroyDescriptorSetLayout)(compute->device, compute->set_layout[i], NULL);
      if (compute->views[i] != VK_NULL_HANDLE)
         CALL(compute, DestroyImageView)(compute->device, compute->views[i], NULL);
      if (compute->images[i] != VK_NULL_HANDLE)
         CALL(compute, DestroyImage)(compute->device, compute->images[i], NULL);
      if (compute->image_memory[i] != VK_NULL_HANDLE)
         CALL(compute, FreeMemory)(compute->device, compute->image_memory[i], NULL);
   }
   if (compute->sampler != VK_NULL_HANDLE)
      CALL(compute, DestroySampler)(compute->device, compute->sampler, NULL);
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
