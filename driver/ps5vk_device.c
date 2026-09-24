/*
 * PS5 Vulkan driver - the logical device.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phases B2 and B5 (docs/M5_PHASE_B.md). vk_device_init rejects
 * device extensions and features the physical device does not support; this
 * file initialises AGC, the one queue of the one queue family
 * (ps5vk_queue.c) and the command buffer vtable of Mesa's common command
 * pools (ps5vk_cmd_buffer.c).
 */

#include "ps5vk_private.h"

#include <assert.h>

#include "vk_alloc.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"
#include "vk_util.h"

/* AGC's library state belongs to the process: it is initialised once, with
 * the version the test runner passes (src/diagnostics.cpp). */
#define PS5VK_AGC_VERSION 8

static once_flag ps5vk_agc_once = ONCE_FLAG_INIT;
static int32_t ps5vk_agc_result;

static void
ps5vk_agc_init(void)
{
   ps5vk_agc_result = sceAgcInit(PS5VK_AGC_VERSION);
   if (ps5vk_agc_result != 0) {
      char line[96];
      snprintf(line, sizeof(line), "[ps5vk] sceAgcInit(%d) failed: 0x%08x\n", PS5VK_AGC_VERSION,
               (unsigned)ps5vk_agc_result);
      fputs(line, stderr);
   }
}

/* AGC is initialised once per process, and from the application's own code:
 * vkCreateInstance calls this too. A libretro core that negotiates the device
 * (PPSSPP) makes the first vkCreateDevice from its own code, which a title loads
 * into anonymous memory, and sceAgcInit made there failed where the same call
 * from the frontend succeeds. */
void
ps5vk_agc_ensure(void)
{
   call_once(&ps5vk_agc_once, ps5vk_agc_init);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(ps5vk_physical_device, physical_device, physicalDevice);
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

   ps5vk_agc_ensure();
   if (ps5vk_agc_result != 0)
      return vk_errorf(physical_device, VK_ERROR_INITIALIZATION_FAILED,
                       "sceAgcInit(%d) failed: 0x%08x", PS5VK_AGC_VERSION,
                       (unsigned)ps5vk_agc_result);

   const VkAllocationCallbacks *const instance_alloc = &physical_device->vk.instance->alloc;
   struct ps5vk_device *const device = vk_zalloc2(instance_alloc, pAllocator, sizeof(*device), 8,
                                                  VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &vk_cmd_enqueue_unless_primary_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &ps5vk_device_entrypoints, false);
   VkResult result = vk_device_init(&device->vk, &physical_device->vk, &dispatch_table,
                                    pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(instance_alloc, pAllocator, device);
      return result;
   }
   device->vk.command_buffer_ops = &ps5vk_cmd_buffer_ops;
   /* Mesa owns the deep copies for deferred secondary commands. Replay them
    * into the primary, where its render pass supplies the actual attachments. */
   vk_device_dispatch_table_from_entrypoints(&device->command_dispatch,
                                             &ps5vk_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&device->command_dispatch,
                                             &vk_common_device_entrypoints, false);
   device->vk.command_dispatch_table = &device->command_dispatch;
   /* Keep invalid nested execution a recording-time refusal. */
   device->vk.dispatch_table.CmdExecuteCommands = ps5vk_CmdExecuteCommands;

   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *const info = &pCreateInfo->pQueueCreateInfos[i];
      /* Valid usage guarantees an existing family and at most its queueCount
       * queues, created once: here family 0 with one queue. */
      assert(info->queueFamilyIndex == 0 && info->queueCount == 1 && !device->queue_initialized);
      result = ps5vk_queue_init(device, &device->queue, info);
      if (result != VK_SUCCESS) {
         vk_device_finish(&device->vk);
         vk_free2(instance_alloc, pAllocator, device);
         return result;
      }
      device->queue_initialized = true;
   }

   /* Holds the shader compiler's shared state for the device's pipelines;
    * psbc_init is reference counted. */
   psbc_init();

   /* Mesa's shared meta operations draw the colour clears (ps5vk_draw.c,
    * C1b); they belong to the device and build their own pipelines through
    * this driver's entry points. */
   result = ps5vk_meta_init(device);
   if (result != VK_SUCCESS) {
      psbc_shutdown();
      if (device->queue_initialized)
         ps5vk_queue_finish(&device->queue);
      vk_device_finish(&device->vk);
      vk_free2(instance_alloc, pAllocator, device);
      return result;
   }

   *pDevice = ps5vk_device_to_handle(device);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   (void)pAllocator;
   if (!device)
      return;
   ps5vk_meta_finish(device);
   if (device->queue_initialized)
      ps5vk_queue_finish(&device->queue);
   psbc_shutdown();
   vk_device_finish(&device->vk);
   vk_free(&device->vk.alloc, device);
}
