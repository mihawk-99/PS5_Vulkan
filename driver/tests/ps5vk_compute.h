/*
 * PS5 Vulkan driver - compute dispatch through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase D2 (docs/M5_REFERENCE.md). An ordinary Vulkan 1.0 compute
 * program: instance, device, one host-visible storage buffer, a shader module
 * from the caller's SPIR-V, a descriptor set holding that buffer, a compute
 * pipeline, one command buffer that binds both and dispatches, and a fence.
 * The caller reads the buffer back afterwards.
 *
 * Shared by the PC test driver/tests/vk_d2_compute_test.c and the console test
 * runner (src/diagnostics.cpp, AGC_VULKAN_DRIVER). Every Vulkan function comes
 * from the caller's GetInstanceProcAddr: the loader's, or the driver's own on
 * the console. The step report is the triangle harness's, so a console run logs
 * this program the same way it logs a frame.
 */

#ifndef PS5VK_COMPUTE_H
#define PS5VK_COMPUTE_H

#include "ps5vk_triangle.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* The buffer a dispatch runs over: bytes, and the word the caller expects the
 * shader to leave in it (the console writes it; a PC rebuild records the
 * dispatch without running it, so it reads the buffer's initial contents). */
#define PS5VK_COMPUTE_BUFFER_BYTES 4096u
#define PS5VK_COMPUTE_GROUP_COUNT 1u
/* Where an indirect dispatch's VkDispatchIndirectCommand sits in that buffer:
 * past the shader's own word, and read when the dispatch is recorded, so the
 * shader's write over the buffer does not reach it first. */
#define PS5VK_COMPUTE_INDIRECT_OFFSET 2048u

   struct ps5vk_compute_input
   {
      ps5vk_get_instance_proc_addr get_instance_proc_addr;
      const uint32_t *spirv;
      size_t spirv_bytes;
      const char *entry_point;
   /* Record vkCmdDispatchIndirect over the counts in the buffer's tail instead
    * of vkCmdDispatch: the same dispatch, its workgroup counts read from
    * memory. */
   bool indirect;
      /* Sample set 0 into set 1, then compare every output texel. */
      bool images;
      /* The word every element of the storage buffer starts at, and the word the
         * shader leaves there: the console's readback compares them. */
      uint32_t initial_word;
      uint32_t expected_word;
      struct ps5vk_triangle_report report;
   };

   struct ps5vk_compute
   {
      ps5vk_get_instance_proc_addr get_instance_proc_addr;
      VkInstance instance;
      VkPhysicalDevice physical;
      VkDevice device;
      VkQueue queue;
      VkBuffer buffer;
      VkDeviceMemory memory;
      void *mapped;
      VkShaderModule module;
      VkDescriptorSetLayout set_layout[2];
      VkPipelineLayout pipeline_layout;
      VkDescriptorPool descriptor_pool;
      VkDescriptorSet descriptor_set[2];
      VkPipeline pipeline;
      VkCommandPool command_pool;
      VkCommandBuffer command;
      VkFence fence;
      /* What the buffer holds when the dispatch's fence has signalled. */
      uint32_t result_word;
      VkImage images[2];
      VkDeviceMemory image_memory[2];
      VkImageView views[2];
      VkSampler sampler;
      uint32_t mismatched_texels;
   };

   /* Creates the program, records the dispatch, submits it and waits for its
     * fence. False when a step failed, which the report names; the program's
     * objects stay alive for ps5vk_compute_finish either way. */
   bool ps5vk_compute_run(const struct ps5vk_compute_input *input, struct ps5vk_compute *compute);

   /* Destroys whatever ps5vk_compute_run created. */
   void ps5vk_compute_finish(struct ps5vk_compute *compute);

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_COMPUTE_H */
