/*
 * PS5 Vulkan driver - Phase B3 negative test: memory outside the address window.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phases B3 and B5 (docs/M5_PHASE_B.md). tools/check-driver.sh
 * runs the direct PC build only. The test sets PS5_HOST_DIRECT_MAPPING_BASE
 * itself around the calls under test, so the host kernel maps direct memory
 * at high word 3 for them alone. Shaders could not address such memory, and
 * the driver must refuse it:
 * - memory allocations fail with VK_ERROR_OUT_OF_DEVICE_MEMORY;
 * - so does creating a device, whose queue maps its submission buffer;
 * - the refused memory is released: after refusals totalling four times the
 *   heap, the variable is cleared and nearly the whole heap is allocated at
 *   high word 2.
 * The test runs only on the PC, where it may change its environment.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "ps5vk_test.h"

#define QUARTER_GIB UINT64_C(0x10000000)
#define REUSED_COUNT 15u

static const char kMappingBase[] = "PS5_HOST_DIRECT_MAPPING_BASE";

static VkResult
try_allocate(VkInstance instance, VkDevice device, VkDeviceSize size)
{
   const VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = size,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   const VkResult result = VK_FUNCTION(instance, AllocateMemory)(device, &info, NULL, &memory);
   if (result == VK_SUCCESS)
      VK_FUNCTION(instance, FreeMemory)(device, memory, NULL);
   return result;
}

int
main(void)
{
   test_begin("B3 address window");
   unsetenv(kMappingBase);
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
   if (!test_create_device(&instance, &physical, &device)) {
      test_destroy_device(instance, device);
      return test_finish();
   }

   setenv(kMappingBase, "0x300000000", 1);
   check(try_allocate(instance, device, 1) == VK_ERROR_OUT_OF_DEVICE_MEMORY,
         "a page mapped at high word 3 is refused");
   check(try_allocate(instance, device, UINT64_C(0x300000)) == VK_ERROR_OUT_OF_DEVICE_MEMORY,
         "a 2 MiB-aligned allocation mapped at high word 3 is refused");

   /* 64 refusals of 256 MiB: 16 GiB together, four times the heap. Each one
    * reaches the mapping, so each must hand its direct memory back. */
   bool refused = true;
   for (unsigned i = 0; i < 64; i++)
      refused = try_allocate(instance, device, QUARTER_GIB) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                refused;
   check(refused, "64 allocations of 256 MiB mapped at high word 3 are refused");

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
   VkDevice refused_device = VK_NULL_HANDLE;
   const VkResult created =
      VK_FUNCTION(instance, CreateDevice)(physical, &device_info, NULL, &refused_device);
   check(created == VK_ERROR_OUT_OF_DEVICE_MEMORY,
         "a device whose queue buffer maps at high word 3 is refused");
   if (created == VK_SUCCESS)
      VK_FUNCTION(instance, DestroyDevice)(refused_device, NULL);

   /* 15 x 256 MiB = 3.75 GiB of the 4 GiB heap, beside the working device's
    * 2 MiB queue buffer, fits only if nothing refused kept its memory. */
   unsetenv(kMappingBase);
   const VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = QUARTER_GIB,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory kept[REUSED_COUNT];
   unsigned allocated = 0;
   while (allocated < REUSED_COUNT &&
          VK_FUNCTION(instance, AllocateMemory)(device, &info, NULL, &kept[allocated]) == VK_SUCCESS)
      ++allocated;
   check(allocated == REUSED_COUNT,
         "afterwards 3.75 GiB allocates at high word 2: refused memory was released");
   while (allocated > 0)
      VK_FUNCTION(instance, FreeMemory)(device, kept[--allocated], NULL);

   test_destroy_device(instance, device);
   return test_finish();
}
