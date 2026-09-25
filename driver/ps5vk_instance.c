/*
 * PS5 Vulkan driver - instance and loader entry points.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B2 (docs/M5_PHASE_B.md). Follows the instance setup of
 * Mesa's own drivers (lavapipe): the runtime's vk_instance with this driver's
 * entry points, and a physical-device enumeration callback.
 */

#include "ps5vk_private.h"

#include <assert.h>

#include "vk_alloc.h"
#include "vk_util.h"

/* Mesa's runtime implements VK_KHR_get_physical_device_properties2 for every
 * driver: vkGetPhysicalDeviceFeatures2 and vkGetPhysicalDeviceProperties2 are
 * common entry points over the driver's feature and property tables. It also
 * implements VK_EXT_debug_report and VK_EXT_debug_utils, through which
 * applications receive the driver's log messages, such as why a command was
 * refused (Phase B7). VK_KHR_surface and VK_KHR_display present to VideoOut
 * (ps5vk_wsi.c, Phase C1). */
static const struct vk_instance_extension_table ps5vk_instance_extensions = {
   .KHR_get_physical_device_properties2 = true,
   .KHR_surface = true,
   .KHR_display = true,
   .EXT_debug_report = true,
   .EXT_debug_utils = true,
};

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   *pApiVersion = PS5VK_INSTANCE_API_VERSION;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
                                           VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
   return vk_enumerate_instance_extension_properties(&ps5vk_instance_extensions,
                                                     pPropertyCount, pProperties);
}

/* The driver has no layers: every query reports zero of them. */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
   (void)pProperties;
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

/* The console has one GPU. */
static VkResult
ps5vk_enumerate_physical_devices(struct vk_instance *vk_instance)
{
   struct ps5vk_instance *const instance = container_of(vk_instance, struct ps5vk_instance, vk);
   struct ps5vk_physical_device *device = NULL;
   const VkResult result = ps5vk_physical_device_create(instance, &device);
   if (result == VK_SUCCESS)
      list_addtail(&device->vk.link, &vk_instance->physical_devices.list);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   struct ps5vk_instance *const instance =
      vk_zalloc(pAllocator, sizeof(*instance), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &ps5vk_instance_entrypoints, true);
   const VkResult result = vk_instance_init(&instance->vk, &ps5vk_instance_extensions,
                                            &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

   /* Opt-in: /app0/ps5vk-log.txt turns Mesa's messages on, so every refusal the
    * driver explains with vk_errorf reaches stderr (the title's trace). Off by
    * default, because a refusal repeated per draw would then log per frame. */
   FILE *log_flag = fopen("/app0/ps5vk-log.txt", "rb");
   if (log_flag != NULL) {
      fclose(log_flag);
      instance->vk.enable_debug_logging = true;
      ps5vk_census_enabled = true;
   }
   /* The diagnostic A/B switches do not need the log: R68's submission modes
    * are read by the profile alone, which logs nothing per frame. */
   ps5vk_ab_load();
   FILE *dump_flag = fopen("/app0/ps5vk-spirv-dump.txt", "rb");
   if (dump_flag != NULL) {
      fclose(dump_flag);
      ps5vk_spirv_dump_enabled = true;
   }

   /* The one AGC initialisation, while the application's own code calls
    * (ps5vk_agc_ensure). */
   ps5vk_agc_ensure();

   instance->vk.physical_devices.enumerate = ps5vk_enumerate_physical_devices;
   instance->vk.physical_devices.destroy = ps5vk_physical_device_destroy;

   *pInstance = ps5vk_instance_to_handle(instance);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyInstance(VkInstance _instance, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_instance, instance, _instance);
   (void)pAllocator;
   if (!instance)
      return;
   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ps5vk_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(ps5vk_instance, instance, _instance);
   return vk_instance_get_proc_addr(instance ? &instance->vk : NULL, &ps5vk_instance_entrypoints,
                                    pName);
}

/* The loader's entry into the driver. Mesa's runtime exports the other two
 * loader-interface functions, vk_icdNegotiateLoaderICDInterfaceVersion and
 * vk_icdGetPhysicalDeviceProcAddr. On the PS5, with no loader, applications
 * start here too. */
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return ps5vk_GetInstanceProcAddr(instance, pName);
}

/* The entry point a frontend that loads this driver itself resolves. RetroArch
 * dlopens the library and dlsyms vkGetInstanceProcAddr, then resolves every
 * other command through it (its gfx/common/vulkan_common.c), so the PS5 shared
 * object exports this spelling beside the loader-interface one above; both
 * dispatch through the same table. */
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName);
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return ps5vk_GetInstanceProcAddr(instance, pName);
}
