/*
 * PS5 Vulkan driver - Phase B6 test: pipeline caches.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B6 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * This checks the optional application-managed Vulkan cache API, which remains
 * empty. The independent driver disk cache also serves VK_NULL_HANDLE callers;
 * tools/check-shader-cache.sh verifies its persistent compiler outputs.
 */

#include "ps5vk_test.h"

int
main(void)
{
   test_begin("B6 pipeline caches");
   VkInstance instance = VK_NULL_HANDLE;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   if (!test_create_device(&instance, &physical, &device)) {
      test_destroy_device(instance, device);
      return test_finish();
   }

   VkPhysicalDeviceProperties properties = {0};
   VK_FUNCTION(instance, GetPhysicalDeviceProperties)(physical, &properties);
   bool uuid_is_set = false;
   for (size_t byte = 0; byte < sizeof(properties.pipelineCacheUUID); byte++)
      uuid_is_set = uuid_is_set || properties.pipelineCacheUUID[byte] != 0;
   check(uuid_is_set, "the device reports a pipeline cache UUID, the identifier of an empty cache");

   const VkPipelineCacheCreateInfo cache_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
   };
   VkPipelineCache cache = VK_NULL_HANDLE;
   check(VK_FUNCTION(instance, CreatePipelineCache)(device, &cache_info, NULL, &cache) == VK_SUCCESS &&
            cache != VK_NULL_HANDLE,
         "a pipeline cache");
   if (cache == VK_NULL_HANDLE)
      goto finish;

   /* Vulkan's two-call idiom: the size first, then the data. A cache that holds
    * nothing reports no bytes either way, and a buffer with room in it is left
    * as it was. */
   size_t size = 0;
   check(VK_FUNCTION(instance, GetPipelineCacheData)(device, cache, &size, NULL) == VK_SUCCESS &&
            size == 0,
         "the cache reports its size as no bytes when asked with no buffer");
   uint8_t data[64];
   memset(data, 0xa5, sizeof(data));
   size = sizeof(data);
   const VkResult read =
      VK_FUNCTION(instance, GetPipelineCacheData)(device, cache, &size, data);
   bool untouched = true;
   for (size_t byte = 0; byte < sizeof(data); byte++)
      untouched = untouched && data[byte] == 0xa5;
   check(read == VK_SUCCESS && size == 0 && untouched,
         "the cache's data is no bytes, and the buffer it was given is not written");

   VkPipelineCache other = VK_NULL_HANDLE;
   if (VK_FUNCTION(instance, CreatePipelineCache)(device, &cache_info, NULL, &other) == VK_SUCCESS) {
      const VkPipelineCache sources[1] = {other};
      check(VK_FUNCTION(instance, MergePipelineCaches)(device, cache, 1, sources) == VK_SUCCESS,
            "merging another cache into it succeeds, because both are empty");
      size = 0;
      check(VK_FUNCTION(instance, GetPipelineCacheData)(device, cache, &size, NULL) == VK_SUCCESS &&
               size == 0,
            "the merged cache still holds no bytes");
      VK_FUNCTION(instance, DestroyPipelineCache)(device, other, NULL);
   } else {
      check(false, "a second pipeline cache to merge");
   }

   VK_FUNCTION(instance, DestroyPipelineCache)(device, cache, NULL);
finish:
   test_destroy_device(instance, device);
   return test_finish();
}
