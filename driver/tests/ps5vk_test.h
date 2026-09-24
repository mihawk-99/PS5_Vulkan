/*
 * PS5 Vulkan driver - shared test scaffolding.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each test in driver/tests builds two ways (tools/check-driver.sh):
 * - through the Khronos Vulkan loader (PC): vkGetInstanceProcAddr from
 *   libvulkan, with VK_DRIVER_FILES naming only this driver;
 * - directly (PC and PS5, PS5VK_TEST_DIRECT): the driver's own
 *   vk_icdGetInstanceProcAddr, as a console title without a loader uses it.
 * Every Vulkan call goes through a function pointer from GetInstanceProcAddr,
 * so both builds share the same code.
 */

#ifndef PS5VK_TEST_H
#define PS5VK_TEST_H

/* The driver's mappable memory type (type 0 is device-local only and never
 * maps): what a test that writes or reads its allocation through vkMapMemory
 * allocates from. */
#define PS5VK_TEST_HOST_MEMORY_TYPE 1u

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* The driver's own debug API: ps5vk_debug_table and the entry shapes above. */
#include "../ps5vk_debug.h"

#ifdef PS5VK_TEST_DIRECT
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
#define TEST_MODE "direct"
#define GET_PROC vk_icdGetInstanceProcAddr
#else
#define TEST_MODE "loader"
#define GET_PROC vkGetInstanceProcAddr
#endif

/* A Vulkan function as a pointer of its own prototype's type. */
#define VK_FUNCTION(instance, name) ((__typeof__(&vk##name))GET_PROC((instance), "vk" #name))

static const char *g_test_name;
static unsigned g_checks;
static unsigned g_failures;

static inline void
test_begin(const char *name)
{
   g_test_name = name;
   printf("ps5vk %s test (%s)\n", g_test_name, TEST_MODE);
}

static inline void
check(bool passed, const char *what)
{
   ++g_checks;
   g_failures += passed ? 0 : 1;
   printf("  %s %s\n", passed ? "PASS" : "FAIL", what);
}

static inline int
test_finish(void)
{
   printf("ps5vk %s test (%s): %u of %u checks passed\n", g_test_name, TEST_MODE,
          g_checks - g_failures, g_checks);
   return g_failures != 0;
}

/* An instance, the physical device and a device with its one queue; false
 * (with a failed check) when any step fails. */
static inline bool
test_create_device(VkInstance *instance, VkPhysicalDevice *physical, VkDevice *device)
{
   const VkApplicationInfo application = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = g_test_name,
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
   };
   *instance = VK_NULL_HANDLE;
   *device = VK_NULL_HANDLE;
   if (VK_FUNCTION(VK_NULL_HANDLE, CreateInstance)(&instance_info, NULL, instance) != VK_SUCCESS) {
      check(false, "vkCreateInstance");
      return false;
   }
   uint32_t count = 1;
   *physical = VK_NULL_HANDLE;
   const VkResult enumerated =
      VK_FUNCTION(*instance, EnumeratePhysicalDevices)(*instance, &count, physical);
   if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || *physical == VK_NULL_HANDLE) {
      check(false, "vkEnumeratePhysicalDevices");
      return false;
   }
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
   if (VK_FUNCTION(*instance, CreateDevice)(*physical, &device_info, NULL, device) != VK_SUCCESS) {
      check(false, "vkCreateDevice");
      return false;
   }
   return true;
}

static inline void
test_destroy_device(VkInstance instance, VkDevice device)
{
   if (device != VK_NULL_HANDLE)
      VK_FUNCTION(instance, DestroyDevice)(device, NULL);
   if (instance != VK_NULL_HANDLE)
      VK_FUNCTION(instance, DestroyInstance)(instance, NULL);
}


/* R7's own report of the tables a draw built (ps5vk_debug_descriptor_tables):
 * two tables, one per set the stage reads, each with its own user-data dword and
 * its own pointer, and the two entry shapes an assertion reads out of them. The
 * debug API is reachable from the direct build alone, so these are too. */
#if defined(PS5VK_TEST_DIRECT)
/* The two entry shapes an assertion reads out of those tables: the uniform
 * descriptor's four words and the combined image sampler's twelve, as far as a
 * caller can know them without being the driver (driver/ps5vk_draw.c,
 * ps5vk_write_image_descriptor). */
#define PS5VK_TEST_UNIFORM_STRIDE 16
#define PS5VK_TEST_IMAGE_ENTRY_WORDS 12

/* The uniform descriptor's four words: the address, the stride in the high half
 * of word 1, the elements the range covers and the flags -- the same shape
 * driver/ps5vk_draw.c writes for a bound uniform buffer. */
static inline bool
test_table_uniform_entry(const uint32_t *entry, uint32_t bytes)
{
   return entry[0] != 0 && (entry[1] >> 16) == PS5VK_TEST_UNIFORM_STRIDE &&
          entry[2] == bytes / PS5VK_TEST_UNIFORM_STRIDE && entry[3] != 0;
}

/* The combined image sampler's twelve words, as far as a caller can know them
 * without being the driver: the texture's own extent in word 2 -- width minus
 * one in the low bits and the height minus one from bit 14 -- the sampler's
 * address word in word 8 and the sampler's filter word in word 10. */
static inline bool
test_table_image_entry(const uint32_t *entry, VkExtent2D extent)
{
   const uint32_t width = (extent.width - 1u) >> 2;
   const uint32_t height = extent.height - 1u;
   return (entry[2] & 0x3fffu) == width && ((entry[2] >> 14) & 0xffffu) == height &&
          entry[8] != 0 && entry[10] != 0;
}

/* R7's report itself: one table per set the stage reads, each with its own
 * user-data dword and its own pointer. Both of R7's frames start here. */
static inline void
check_two_tables(VkDevice device, ps5vk_debug_table *tables, uint32_t capacity,
                 const ps5vk_debug_table **set0_out, const ps5vk_debug_table **set1_out)
{
   *set0_out = NULL;
   *set1_out = NULL;
   const uint32_t count = ps5vk_debug_descriptor_tables(device, tables, capacity);
   check(count == 2, "the draw built one table per set the stage reads");
   if (count != 2)
      return;
   const ps5vk_debug_table *const set0 = tables[0].set == 0 ? &tables[0] : &tables[1];
   const ps5vk_debug_table *const set1 = tables[0].set == 1 ? &tables[0] : &tables[1];
   check(set0->set == 0 && set1->set == 1, "the two tables are set 0's and set 1's");
   for (unsigned at = 0; at < count; at++)
      printf("  (table %u: stage %u set %u dword %u address %08x words %p bytes %zu)\n", at,
             (unsigned)tables[at].stage, (unsigned)tables[at].set,
             (unsigned)tables[at].user_data_dword, tables[at].address_low,
             (const void *)tables[at].words, tables[at].bytes);
   check(set0->user_data_dword != set1->user_data_dword,
         "each set's pointer has a user-data dword of its own");
   check(set0->words != NULL && set1->words != NULL && set0->words != set1->words &&
            set0->address_low != set1->address_low,
         "the two sets' tables are two different tables");
   check(set0->stage == set1->stage,
         "both tables belong to the one stage that reads descriptors");
   /* The pointer in the ABI is the table the draw allocated: one dword in the
    * 32-bit-pointer build these programs compile for, as R9's push-constant test
    * reads its own (ps5vk_debug.h). */
   check((uint32_t)(uintptr_t)set0->words == set0->address_low &&
            (uint32_t)(uintptr_t)set1->words == set1->address_low,
         "each set's table is the pointer its user-data dword carries");
   *set0_out = set0;
   *set1_out = set1;
}
#endif

#endif /* PS5VK_TEST_H */
