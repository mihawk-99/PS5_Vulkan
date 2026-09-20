/*
 * PS5 Vulkan driver - pipeline caches.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B6 (docs/M5_REFERENCE.md). vkCreateGraphicsPipelines hands
 * its state to AGC and links the compiled microcode into a pipeline object each
 * time it is called: there is nothing this driver stores between runs, so its
 * pipeline cache is the empty cache Vulkan lets an implementation have. The
 * object exists, the cache data is zero bytes, merging one cache into another
 * is a no-op, and the cache handle a pipeline is created with is ignored
 * (ps5vk_pipeline.c) -- all of which the specification allows, because the data
 * in a cache is an optimization the implementation may decline to use, and the
 * implementation is what decides what a cache holds.
 *
 * The device's pipelineCacheUUID is the constant in ps5vk_physical_device.c:
 * one identifier for the one cache format this driver has, which never holds
 * anything. A cache created from data (pCreateInfo->initialData) is accepted
 * and ignored for the same reason; an application that saves the zero bytes it
 * gets back and passes them in again is asking for exactly the behavior it
 * gets.
 *
 * No probe was needed and no console run is owed: nothing here reaches the GPU
 * or the command stream, and driver/tests/vk_b6_pipeline_cache_test.c checks
 * the four entry points through the loader on the PC.
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
