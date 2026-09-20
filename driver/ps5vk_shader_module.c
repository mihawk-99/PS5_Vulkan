/*
 * PS5 Vulkan driver - shader modules.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B6 (docs/M5_PHASE_B.md). A shader module keeps a copy of
 * its SPIR-V; pipelines compile it (ps5vk_pipeline.c). Mesa's
 * vk_shader_module.c is not used: it belongs to the full runtime, hashing
 * modules with BLAKE3 for pipeline caches this driver does not have yet.
 */

#include "ps5vk_private.h"

#include <assert.h>
#include <string.h>

#include "vk_alloc.h"

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateShaderModule(VkDevice _device, const VkShaderModuleCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* Valid usage: a non-zero whole number of SPIR-V words. */
   assert(pCreateInfo->codeSize != 0 && pCreateInfo->codeSize % sizeof(uint32_t) == 0);

   struct ps5vk_shader_module *const module =
      vk_object_alloc(&device->vk, pAllocator, sizeof(*module) + pCreateInfo->codeSize,
                      VK_OBJECT_TYPE_SHADER_MODULE);
   if (!module)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   module->size = pCreateInfo->codeSize;
   memcpy(module->words, pCreateInfo->pCode, pCreateInfo->codeSize);

   *pShaderModule = ps5vk_shader_module_to_handle(module);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyShaderModule(VkDevice _device, VkShaderModule _module,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_shader_module, module, _module);
   if (module)
      vk_object_free(&device->vk, pAllocator, module);
}
