/*
 * PS5 Vulkan driver - Phase B3 test: device memory.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B3 (docs/M5_PHASE_B.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h). Checks
 * that every allocation is mapped inside the GPU address window (high word 2),
 * page- or 2 MiB-aligned, readable and writable over its whole size, disjoint
 * from the others, and reusable after it is freed.
 */

#include "ps5vk_test.h"

#define PAGE_BYTES 0x4000u
#define LARGE_BYTES 0x200000u
#define SMALL_COUNT 16u

struct mapping {
   VkDeviceMemory memory;
   VkDeviceSize size;
   uint8_t *data;
};

static bool
in_window(const struct mapping *m)
{
   const uint64_t first = (uint64_t)(uintptr_t)m->data;
   return first >> 32 == 2 && ((first + m->size - 1) >> 32) == 2;
}

static bool
disjoint(const struct mapping *a, const struct mapping *b)
{
   return a->data + a->size <= b->data || b->data + b->size <= a->data;
}

/* Writes a pattern over [offset, offset + bytes) and reads it back. */
static bool
fill_and_verify(const struct mapping *m, VkDeviceSize offset, VkDeviceSize bytes, uint8_t seed)
{
   for (VkDeviceSize i = 0; i < bytes; i++)
      m->data[offset + i] = (uint8_t)(seed + i);
   for (VkDeviceSize i = 0; i < bytes; i++) {
      if (m->data[offset + i] != (uint8_t)(seed + i))
         return false;
   }
   return true;
}

/* The first and the last page of the requested size (all of it when smaller)
 * are writable and read back. */
static bool
round_trip(const struct mapping *m, uint8_t seed)
{
   const VkDeviceSize span = m->size < PAGE_BYTES ? m->size : PAGE_BYTES;
   return fill_and_verify(m, 0, span, seed) &&
          fill_and_verify(m, m->size - span, span, (uint8_t)(seed + 0x80)) &&
          (m->size < 2 * span || m->data[0] == seed);
}

static bool
allocate(VkInstance instance, VkDevice device, VkDeviceSize size, struct mapping *m)
{
   const VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = size,
      .memoryTypeIndex = 0,
   };
   m->memory = VK_NULL_HANDLE;
   m->size = size;
   m->data = NULL;
   if (VK_FUNCTION(instance, AllocateMemory)(device, &info, NULL, &m->memory) != VK_SUCCESS)
      return false;
   void *data = NULL;
   if (VK_FUNCTION(instance, MapMemory)(device, m->memory, 0, VK_WHOLE_SIZE, 0, &data) !=
       VK_SUCCESS)
      return false;
   m->data = data;
   return data != NULL;
}

static void
release(VkInstance instance, VkDevice device, struct mapping *m)
{
   if (m->memory == VK_NULL_HANDLE)
      return;
   if (m->data)
      VK_FUNCTION(instance, UnmapMemory)(device, m->memory);
   VK_FUNCTION(instance, FreeMemory)(device, m->memory, NULL);
   m->memory = VK_NULL_HANDLE;
   m->data = NULL;
}

int
main(void)
{
   test_begin("B3 memory");
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
   if (!test_create_device(&instance, &physical, &device)) {
      test_destroy_device(instance, device);
      return test_finish();
   }

   VkPhysicalDeviceMemoryProperties properties;
   memset(&properties, 0, sizeof(properties));
   VK_FUNCTION(instance, GetPhysicalDeviceMemoryProperties)(physical, &properties);
   const VkDeviceSize heap = properties.memoryHeaps[0].size;
   check(heap > 0 && heap <= UINT64_C(1) << 32, "the heap fits the 4 GiB GPU address window");

   struct mapping tiny;
   check(allocate(instance, device, 1, &tiny) && in_window(&tiny) &&
            ((uintptr_t)tiny.data & (PAGE_BYTES - 1)) == 0,
         "a 1-byte allocation maps page-aligned at high word 2");
   check(tiny.data && round_trip(&tiny, 0x11), "the 1-byte allocation reads back");

   struct mapping large;
   check(allocate(instance, device, 3 * LARGE_BYTES / 2 + 1, &large) && in_window(&large) &&
            ((uintptr_t)large.data & (LARGE_BYTES - 1)) == 0,
         "a 3 MiB allocation maps 2 MiB-aligned at high word 2");
   check(large.data && round_trip(&large, 0x5a), "the 3 MiB allocation reads back at both ends");
   check(tiny.data && large.data && disjoint(&tiny, &large), "the two allocations are disjoint");

   if (tiny.data) {
      tiny.data[0] = 0xc3;
      VK_FUNCTION(instance, UnmapMemory)(device, tiny.memory);
      void *again = NULL;
      const VkResult mapped =
         VK_FUNCTION(instance, MapMemory)(device, tiny.memory, 0, 1, 0, &again);
      check(mapped == VK_SUCCESS && again == tiny.data && tiny.data[0] == 0xc3,
            "unmapping and mapping again returns the same, unchanged memory");
   }
   if (large.data) {
      VK_FUNCTION(instance, UnmapMemory)(device, large.memory);
      void *offset_data = NULL;
      const VkResult mapped = VK_FUNCTION(instance, MapMemory)(device, large.memory, 0x1000,
                                                              VK_WHOLE_SIZE, 0, &offset_data);
      check(mapped == VK_SUCCESS && (uint8_t *)offset_data == large.data + 0x1000,
            "a mapping at an offset starts at that offset");
      const VkMappedMemoryRange range = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = large.memory,
         .offset = 0x1000,
         .size = VK_WHOLE_SIZE,
      };
      check(VK_FUNCTION(instance, FlushMappedMemoryRanges)(device, 1, &range) == VK_SUCCESS &&
               VK_FUNCTION(instance, InvalidateMappedMemoryRanges)(device, 1, &range) ==
                  VK_SUCCESS,
            "flush and invalidate succeed on coherent memory");
   }

   struct mapping small[SMALL_COUNT];
   bool all_mapped = true;
   bool all_disjoint = true;
   for (unsigned i = 0; i < SMALL_COUNT; i++) {
      all_mapped = allocate(instance, device, 4 * PAGE_BYTES, &small[i]) && in_window(&small[i]) &&
                   round_trip(&small[i], (uint8_t)i) && all_mapped;
      for (unsigned j = 0; all_mapped && j < i; j++)
         all_disjoint = all_disjoint && disjoint(&small[i], &small[j]);
   }
   check(all_mapped, "16 allocations of 64 KiB map at high word 2 and read back");
   check(all_mapped && all_disjoint, "the 16 allocations are pairwise disjoint");
   for (unsigned i = 0; i < SMALL_COUNT; i += 2)
      release(instance, device, &small[i]);
   bool reused = true;
   for (unsigned i = 0; i < SMALL_COUNT; i += 2)
      reused = allocate(instance, device, 4 * PAGE_BYTES, &small[i]) && in_window(&small[i]) &&
               reused;
   check(reused, "freed allocations can be allocated again");

   const VkMemoryAllocateInfo too_large = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = heap + PAGE_BYTES,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory failed = VK_NULL_HANDLE;
   const VkResult result =
      VK_FUNCTION(instance, AllocateMemory)(device, &too_large, NULL, &failed);
   check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY, "an allocation larger than the heap fails");
   if (result == VK_SUCCESS)
      VK_FUNCTION(instance, FreeMemory)(device, failed, NULL);

   for (unsigned i = 0; i < SMALL_COUNT; i++)
      release(instance, device, &small[i]);
   release(instance, device, &large);
   release(instance, device, &tiny);
   test_destroy_device(instance, device);
   return test_finish();
}
