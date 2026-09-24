/*
 * PS5 Vulkan driver - Phase B3 test: buffers.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B3 (docs/M5_PHASE_B.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h). Checks the
 * memory requirements of buffers of several sizes, packing two buffers into
 * one allocation at their required alignment, binding a large buffer at an
 * offset, aliasing, and the 4 GiB size bound.
 */

#include "ps5vk_test.h"

#define ALIGNMENT 256u
#define ALL_USAGE                                                                                  \
   (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |                          \
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |          \
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |                      \
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |                         \
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)

static VkInstance g_instance;
static VkDevice g_device;

static VkResult
create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer)
{
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   *buffer = VK_NULL_HANDLE;
   return VK_FUNCTION(g_instance, CreateBuffer)(g_device, &info, NULL, buffer);
}

static void
destroy_buffer(VkBuffer buffer)
{
   VK_FUNCTION(g_instance, DestroyBuffer)(g_device, buffer, NULL);
}

static VkMemoryRequirements
requirements(VkBuffer buffer)
{
   VkMemoryRequirements r;
   memset(&r, 0, sizeof(r));
   VK_FUNCTION(g_instance, GetBufferMemoryRequirements)(g_device, buffer, &r);
   return r;
}

static bool
allocate(VkDeviceSize size, VkDeviceMemory *memory, uint8_t **data)
{
   const VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = size,
      .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
   };
   *memory = VK_NULL_HANDLE;
   void *mapped = NULL;
   if (VK_FUNCTION(g_instance, AllocateMemory)(g_device, &info, NULL, memory) != VK_SUCCESS ||
       VK_FUNCTION(g_instance, MapMemory)(g_device, *memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
          VK_SUCCESS)
      return false;
   *data = mapped;
   return true;
}

static void
free_memory(VkDeviceMemory memory)
{
   VK_FUNCTION(g_instance, FreeMemory)(g_device, memory, NULL);
}

static VkResult
bind(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
   return VK_FUNCTION(g_instance, BindBufferMemory)(g_device, buffer, memory, offset);
}

static void
check_requirements(void)
{
   static const VkDeviceSize sizes[] = {1, 255, 256, 4097, 3u << 20};
   bool valid = true;
   bool stable = true;
   for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
      VkBuffer buffer;
      VkBuffer twin;
      if (create_buffer(sizes[i], ALL_USAGE, &buffer) != VK_SUCCESS ||
          create_buffer(sizes[i], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &twin) != VK_SUCCESS) {
         valid = false;
         continue;
      }
      const VkMemoryRequirements r = requirements(buffer);
      const VkMemoryRequirements t = requirements(twin);
      valid = valid && r.memoryTypeBits == 3 && r.alignment == ALIGNMENT &&
              r.size >= sizes[i] && r.size < sizes[i] + ALIGNMENT && r.size % ALIGNMENT == 0;
      /* A subset of the usage must not need more (resources.adoc). */
      stable = stable && t.memoryTypeBits == r.memoryTypeBits && t.size <= r.size &&
               t.alignment <= r.alignment;
      destroy_buffer(twin);
      destroy_buffer(buffer);
   }
   check(valid, "buffers of 1 B to 3 MiB take either memory type, 256-byte alignment, the size rounded up");
   check(stable, "a subset of the usage needs no more memory");

   VkBuffer largest;
   check(create_buffer(UINT64_C(1) << 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &largest) ==
               VK_SUCCESS &&
            requirements(largest).size == UINT64_C(1) << 32,
         "a 4 GiB buffer can be created");
   destroy_buffer(largest);
   VkBuffer too_large;
   const VkResult result =
      create_buffer((UINT64_C(1) << 32) + 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &too_large);
   check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY,
         "a buffer larger than the 4 GiB address window is refused");
   if (result == VK_SUCCESS)
      destroy_buffer(too_large);
}

static void
check_binding(void)
{
   VkBuffer vertices;
   VkBuffer uniforms;
   if (create_buffer(1000, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertices) != VK_SUCCESS ||
       create_buffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &uniforms) != VK_SUCCESS) {
      check(false, "vkCreateBuffer");
      return;
   }
   const VkMemoryRequirements rv = requirements(vertices);
   const VkMemoryRequirements ru = requirements(uniforms);
   const VkDeviceSize uniform_offset = (rv.size + ru.alignment - 1) / ru.alignment * ru.alignment;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   uint8_t *data = NULL;
   bool packed = allocate(uniform_offset + ru.size, &memory, &data) &&
                 bind(vertices, memory, 0) == VK_SUCCESS &&
                 bind(uniforms, memory, uniform_offset) == VK_SUCCESS;
   check(packed, "two buffers bind next to each other in one allocation");
   if (packed) {
      for (unsigned i = 0; i < 1000; i++)
         data[i] = (uint8_t)i;
      memset(data + uniform_offset, 0xee, 16);
      bool intact = data[999] == (uint8_t)999 && data[uniform_offset] == 0xee &&
                    data[uniform_offset + 15] == 0xee;
      check(intact, "their contents stay apart through the mapping");
   }

   VkBuffer alias;
   check(create_buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &alias) == VK_SUCCESS &&
            bind(alias, memory, uniform_offset) == VK_SUCCESS,
         "a second buffer can alias the same range");

   destroy_buffer(alias);
   destroy_buffer(uniforms);
   destroy_buffer(vertices);
   free_memory(memory);

   VkBuffer large;
   VkDeviceMemory large_memory = VK_NULL_HANDLE;
   check(create_buffer(3u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &large) == VK_SUCCESS &&
            allocate((4u << 20), &large_memory, &data) &&
            bind(large, large_memory, 8 * ALIGNMENT) == VK_SUCCESS,
         "a 3 MiB buffer binds at a 2 KiB offset of a 4 MiB allocation");
   /* Memory may be freed before the buffers bound to it are destroyed. */
   free_memory(large_memory);
   destroy_buffer(large);
}

int
main(void)
{
   test_begin("B3 buffer");
   VkPhysicalDevice physical;
   if (test_create_device(&g_instance, &physical, &g_device)) {
      check_requirements();
      check_binding();
   }
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
