/*
 * PS5 Vulkan driver - pipeline caches.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The optional application-managed Vulkan cache remains empty. The driver's
 * separate disk shader cache persists immutable compiler outputs even when an
 * application supplies VK_NULL_HANDLE here (ps5vk_shader_cache.c).
 */

#include "ps5vk_private.h"

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreatePipelineCache(VkDevice _device, const VkPipelineCacheCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkPipelineCache *pPipelineCache)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT only says how the
    * application will call this cache; there is nothing here to synchronize. */
   (void)pCreateInfo;
   struct ps5vk_pipeline_cache *const cache =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*cache), VK_OBJECT_TYPE_PIPELINE_CACHE);
   if (cache == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   *pPipelineCache = ps5vk_pipeline_cache_to_handle(cache);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyPipelineCache(VkDevice _device, VkPipelineCache _pipelineCache,
                           const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_pipeline_cache, cache, _pipelineCache);
   if (cache == NULL)
      return;
   vk_object_free(&device->vk, pAllocator, cache);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetPipelineCacheData(VkDevice _device, VkPipelineCache _pipelineCache, size_t *pDataSize,
                           void *pData)
{
   VK_FROM_HANDLE(ps5vk_pipeline_cache, cache, _pipelineCache);
   (void)_device;
   (void)cache;
   if (pData == NULL) {
      /* Vulkan's two-call idiom: the size the data needs, which for a cache
       * that holds nothing is none. */
      *pDataSize = 0;
      return VK_SUCCESS;
   }
   *pDataSize = 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_MergePipelineCaches(VkDevice _device, VkPipelineCache _dstCache, uint32_t srcCacheCount,
                          const VkPipelineCache *pSrcCaches)
{
   VK_FROM_HANDLE(ps5vk_pipeline_cache, dst, _dstCache);
   (void)_device;
   (void)dst;
   /* Every cache is empty, so every merge is already done. */
   (void)srcCacheCount;
   (void)pSrcCaches;
   return VK_SUCCESS;
}
