/*
 * PS5 Vulkan driver - device memory on direct memory.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B3 (docs/M5_PHASE_B.md). Each VkDeviceMemory is one
 * direct-memory allocation, mapped for the CPU and the GPU at once, as the
 * runner and ps5-opengl map every GPU-visible allocation. The mapping lasts as
 * long as the memory: the GPU uses the same virtual addresses, and memory is
 * host-coherent, so vkMapMemory is only a view of it and flushing does
 * nothing. Of the two memory types only the host-visible one maps.
 *
 * Shaders combine 32-bit pointers with a fixed address high word, so every
 * mapping must lie inside that word's 4 GiB window; allocations the kernel
 * places elsewhere fail with VK_ERROR_OUT_OF_DEVICE_MEMORY
 * (ps5vk_direct_memory.c).
 */

#include "ps5vk_private.h"

#include <assert.h>

#include "util/u_atomic.h"
#include "vk_alloc.h"
#include "vk_util.h"

/* The alignment ps5-opengl gives render allocations (PS5_RENDER_ALIGNMENT).
 * Allocations of at least that size get it, so resources that need it can be
 * bound at aligned offsets; smaller ones are page aligned. */
#define PS5VK_LARGE_ALIGNMENT UINT64_C(0x200000)

static void
ps5vk_device_memory_release(struct ps5vk_device *device, struct ps5vk_device_memory *memory,
                            const VkAllocationCallbacks *allocator)
{
   ps5vk_direct_mapping_destroy(&memory->direct);
   vk_device_memory_destroy(&device->vk, allocator, &memory->vk);
   p_atomic_dec(&device->memory_allocation_count);
}

static VkResult
ps5vk_AllocateMemory_untimed(VkDevice _device, const VkMemoryAllocateInfo *pAllocateInfo,
                     const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* Valid usage: an existing type. */
   assert(pAllocateInfo->memoryTypeIndex < PS5VK_MEMORY_TYPE_COUNT);

   /* The heap is the direct-memory pool (R88): nothing larger can be had. */
   const int64_t pool = sceKernelGetDirectMemorySize();
   if (pool <= 0 || pAllocateInfo->allocationSize > (uint64_t)pool)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   const uint64_t bytes = align64(pAllocateInfo->allocationSize, PS5VK_DIRECT_PAGE_BYTES);
   const uint64_t alignment =
      bytes >= PS5VK_LARGE_ALIGNMENT ? PS5VK_LARGE_ALIGNMENT : PS5VK_DIRECT_PAGE_BYTES;

   if (p_atomic_inc_return(&device->memory_allocation_count) > PS5VK_MAX_MEMORY_ALLOCATIONS) {
      p_atomic_dec(&device->memory_allocation_count);
      return vk_error(device, VK_ERROR_TOO_MANY_OBJECTS);
   }
   struct ps5vk_device_memory *const memory =
      vk_device_memory_create(&device->vk, pAllocateInfo, pAllocator, sizeof(*memory));
   if (!memory) {
      p_atomic_dec(&device->memory_allocation_count);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   const int32_t result =
      ps5vk_direct_mapping_create(&memory->direct, (size_t)bytes, (size_t)alignment,
                                  PS5VK_DIRECT_MEMORY);
   if (result != 0) {
      ps5vk_device_memory_release(device, memory, pAllocator);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "%" PRIu64 " bytes of direct memory could not be allocated and mapped "
                       "for the GPU: 0x%08x", bytes, (unsigned)result);
   }

   *pMemory = ps5vk_device_memory_to_handle(memory);
   return VK_SUCCESS;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_AllocateMemory(VkDevice _device, const VkMemoryAllocateInfo *pAllocateInfo,
                     const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_AllocateMemory_untimed(_device, pAllocateInfo, pAllocator, pMemory);
   ps5vk_hitch_end(PS5VK_HITCH_MEMORY, hitch);
   return result;
}

static void
ps5vk_FreeMemory_untimed(VkDevice _device, VkDeviceMemory _memory, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_device_memory, memory, _memory);
   if (memory)
      ps5vk_device_memory_release(device, memory, pAllocator);
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_FreeMemory(VkDevice _device, VkDeviceMemory _memory, const VkAllocationCallbacks *pAllocator)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   ps5vk_FreeMemory_untimed(_device, _memory, pAllocator);
   ps5vk_hitch_end(PS5VK_HITCH_MEMORY, hitch);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_GetDeviceMemoryCommitment(VkDevice _device, VkDeviceMemory _memory,
                                VkDeviceSize *pCommittedMemoryInBytes)
{
   (void)_device;
   VK_FROM_HANDLE(ps5vk_device_memory, memory, _memory);
   /* The whole allocation is committed: ps5vk_AllocateMemory maps it into the
    * address window in one piece, and nothing here allocates lazily
    * (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT is not reported). */
   *pCommittedMemoryInBytes = memory->vk.size;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_MapMemory2(VkDevice _device, const VkMemoryMapInfo *pMemoryMapInfo, void **ppData)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_device_memory, memory, pMemoryMapInfo->memory);
   /* Valid usage forbids mapping a type without HOST_VISIBLE; refusing it
    * keeps the device-local type's promise that the CPU never caches it
    * outside the driver's own paths. */
   if (memory->vk.memory_type_index != PS5VK_MEMORY_TYPE_HOST)
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "memory type %u is not host-visible", memory->vk.memory_type_index);
   /* VK_MEMORY_MAP_PLACED_BIT_EXT needs VK_EXT_map_memory_placed, which is not
    * exposed; valid usage leaves offset and size inside the allocation. */
   assert(pMemoryMapInfo->flags == 0);
   assert(pMemoryMapInfo->offset < memory->vk.size);
   /* The first map evicts the whole allocation: until now the CPU reached it
    * only through the driver's own paths, and from now on the application may
    * read it directly, so no line cached before this may be what it reads. */
   if (!memory->host_mapped) {
      ps5vk_flush_cpu_cache(memory->direct.address, memory->direct.bytes);
      memory->host_mapped = true;
   }
   *ppData = (uint8_t *)memory->direct.address + pMemoryMapInfo->offset;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_UnmapMemory2(VkDevice _device, const VkMemoryUnmapInfo *pMemoryUnmapInfo)
{
   (void)_device;
   (void)pMemoryUnmapInfo;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_FlushMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                              const VkMappedMemoryRange *pMemoryRanges)
{
   (void)_device;
   (void)memoryRangeCount;
   (void)pMemoryRanges;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                                   const VkMappedMemoryRange *pMemoryRanges)
{
   (void)_device;
   (void)memoryRangeCount;
   (void)pMemoryRanges;
   return VK_SUCCESS;
}
