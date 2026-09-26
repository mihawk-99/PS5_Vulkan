/*
 * PS5 Vulkan driver - Phase B3 test: memory outside the address window.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phases B3 and B5 (docs/M5_PHASE_B.md), and R88
 * (docs/M5_PHASE_C.md). tools/check-driver.sh runs the direct PC build only.
 * The test sets PS5_HOST_DIRECT_MAPPING_BASE itself around the calls under
 * test, so the host kernel maps direct memory at high word 3 for them alone.
 * - VkDeviceMemory there is accepted: the GPU reaches it only through 48-bit
 *   addresses, and reads and writes it outside the window on the console
 *   (R86, R87). 64 allocations of 256 MiB, 16 GiB together, are made and
 *   freed one after another through the 4 GiB pool.
 * - A device is refused, because its queue's submission buffer is read
 *   through a 32-bit pointer and has to lie in the window.
 * - Afterwards, with the variable cleared, nearly the whole pool allocates
 *   again: nothing was kept.
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
      .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
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
   check(try_allocate(instance, device, 1) == VK_SUCCESS,
         "VkDeviceMemory mapped at high word 3 is accepted (R88)");
   check(try_allocate(instance, device, UINT64_C(0x300000)) == VK_SUCCESS,
         "a 2 MiB-aligned allocation mapped at high word 3 is accepted");

   /* 64 allocations of 256 MiB, each freed before the next: 16 GiB together,
    * four times the pool, so each must hand its direct memory back. */
   bool accepted = true;
   for (unsigned i = 0; i < 64; i++)
      accepted = try_allocate(instance, device, QUARTER_GIB) == VK_SUCCESS && accepted;
   check(accepted, "64 allocations of 256 MiB at high word 3 are made and freed in turn");

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

   /* 15 x 256 MiB = 3.75 GiB of the 4 GiB pool, beside the working device's
    * 2 MiB queue buffer, fits only if nothing kept its memory. */
   unsetenv(kMappingBase);
   const VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = QUARTER_GIB,
      .memoryTypeIndex = PS5VK_TEST_HOST_MEMORY_TYPE,
   };
   VkDeviceMemory kept[REUSED_COUNT];
   unsigned allocated = 0;
   while (allocated < REUSED_COUNT &&
          VK_FUNCTION(instance, AllocateMemory)(device, &info, NULL, &kept[allocated]) == VK_SUCCESS)
      ++allocated;
   check(allocated == REUSED_COUNT,
         "afterwards 3.75 GiB allocates at high word 2: nothing kept its memory");
   while (allocated > 0)
      VK_FUNCTION(instance, FreeMemory)(device, kept[--allocated], NULL);

   test_destroy_device(instance, device);
   return test_finish();
}
