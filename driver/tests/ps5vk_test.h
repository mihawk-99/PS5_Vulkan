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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

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

#endif /* PS5VK_TEST_H */
