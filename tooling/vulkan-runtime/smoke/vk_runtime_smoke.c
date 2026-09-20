/*
 * PS5 Vulkan compatibility probe - smoke test of Mesa's Vulkan runtime.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B1 (docs/M5_PHASE_B.md). Creates and destroys a
 * vk_instance through Mesa's runtime with the common instance entry points,
 * and checks what the runtime derives from its inputs:
 * 1. Without a driver EnumerateInstanceVersion the runtime is a Vulkan 1.0
 *    implementation, which the spec requires to reject apiVersion 1.3 with
 *    VK_ERROR_INCOMPATIBLE_DRIVER.
 * 2. With one that reports 1.3, as a driver's does, the instance is created;
 *    the application name and API version come from VkApplicationInfo and the
 *    driver version from PACKAGE_VERSION.
 * tools/check-vulkan-runtime.sh runs it on the PC and links it the way a
 * console title links on the PS5.
 *
 * PS5VK_EXPECTED_MESA is the pinned Mesa release, for example "26.2.0".
 */

#include <stdio.h>
#include <string.h>

#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_dispatch_table.h"
#include "vk_instance.h"
#include "vk_util.h"

static const char kApplicationName[] = "ps5vk-runtime-smoke";

/* A driver's instance-version entry point, as the PS5 driver will expose. */
static VkResult
smoke_EnumerateInstanceVersion(uint32_t *api_version)
{
   *api_version = VK_API_VERSION_1_3;
   return VK_SUCCESS;
}

int main(void)
{
   const VkApplicationInfo application = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = kApplicationName,
      .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
   };

   struct vk_instance_dispatch_table dispatch;
   vk_instance_dispatch_table_from_entrypoints(&dispatch, &vk_common_instance_entrypoints, true);
   const struct vk_instance_extension_table no_extensions = {0};
   struct vk_instance instance;
   int failures = 0;

   /* 1. The common entry points alone implement Vulkan 1.0. A failed init
    *    needs no vk_instance_finish, as in Mesa's drivers. */
   VkResult result = vk_instance_init(&instance, &no_extensions, &dispatch, &create_info,
                                      vk_default_allocator());
   if (result != VK_ERROR_INCOMPATIBLE_DRIVER) {
      printf("  Vulkan 1.0 runtime with apiVersion 1.3: result %d, expected "
             "VK_ERROR_INCOMPATIBLE_DRIVER (%d)\n", (int)result, (int)VK_ERROR_INCOMPATIBLE_DRIVER);
      if (result == VK_SUCCESS)
         vk_instance_finish(&instance);
      ++failures;
   }

   /* 2. A driver that reports Vulkan 1.3. */
   dispatch.EnumerateInstanceVersion = smoke_EnumerateInstanceVersion;
   result = vk_instance_init(&instance, &no_extensions, &dispatch, &create_info,
                             vk_default_allocator());
   if (result != VK_SUCCESS) {
      printf("vulkan runtime smoke: FAIL (Vulkan 1.3 vk_instance_init returned %d)\n", (int)result);
      return 1;
   }

   if (instance.app_info.api_version != VK_API_VERSION_1_3) {
      printf("  api_version 0x%x, expected 0x%x\n", instance.app_info.api_version,
             VK_API_VERSION_1_3);
      ++failures;
   }
   if (instance.app_info.app_name == NULL ||
       strcmp(instance.app_info.app_name, kApplicationName) != 0) {
      printf("  app_name \"%s\", expected \"%s\"\n",
             instance.app_info.app_name ? instance.app_info.app_name : "(null)", kApplicationName);
      ++failures;
   }

   unsigned major = 0, minor = 0, patch = 0;
   const int fields = sscanf(PS5VK_EXPECTED_MESA, "%u.%u.%u", &major, &minor, &patch);
   const uint32_t driver_version = vk_get_driver_version();
   if (fields != 3 || driver_version != VK_MAKE_VERSION(major, minor, patch)) {
      printf("  driver version %u.%u.%u, expected %s\n", VK_VERSION_MAJOR(driver_version),
             VK_VERSION_MINOR(driver_version), VK_VERSION_PATCH(driver_version),
             PS5VK_EXPECTED_MESA);
      ++failures;
   }

   vk_instance_finish(&instance);
   printf("vulkan runtime smoke: %s (api %u.%u, driver %u.%u.%u)\n", failures ? "FAIL" : "PASS",
          VK_API_VERSION_MAJOR(VK_API_VERSION_1_3), VK_API_VERSION_MINOR(VK_API_VERSION_1_3),
          VK_VERSION_MAJOR(driver_version), VK_VERSION_MINOR(driver_version),
          VK_VERSION_PATCH(driver_version));
   return failures != 0;
}
