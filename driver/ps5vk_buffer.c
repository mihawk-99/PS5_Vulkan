/*
 * PS5 Vulkan driver - buffers.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B3 (docs/M5_PHASE_B.md). A buffer is a range of one
 * device memory allocation. Its GPU address, set when it is bound, is the
 * allocation's mapping plus the bind offset, so it lies in the address window
 * (ps5vk_memory.c).
 *
 * Buffers are aligned to 256 bytes. The minimum uniform, storage and texel
 * buffer offset alignments are 256 too, so every address a descriptor can
 * name keeps a zero low byte, the rule the runner enforces for descriptor
 * addresses (docs/VULKAN_PROBE_PLAN.md).
 */

#include "ps5vk_private.h"
#include "ps5vk_debug.h"

#include <assert.h>

#include "vk_alloc.h"
#include "vk_util.h"

static VkResult
ps5vk_CreateBuffer_untimed(VkDevice _device, const VkBufferCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* A buffer must fit one address window to be bound at all. */
   if (align64(pCreateInfo->size, PS5VK_BUFFER_ALIGNMENT) > PS5VK_ADDRESS_WINDOW_BYTES)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct ps5vk_buffer *const buffer =
      vk_buffer_create(&device->vk, pCreateInfo, pAllocator, sizeof(*buffer));
   if (!buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* The device's list is what the runner's capture reads. */
   buffer->next_in_device = device->buffers;
   device->buffers = buffer;
   *pBuffer = ps5vk_buffer_to_handle(buffer);
   return VK_SUCCESS;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateBuffer(VkDevice _device, const VkBufferCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_CreateBuffer_untimed(_device, pCreateInfo, pAllocator, pBuffer);
   ps5vk_hitch_end(PS5VK_HITCH_BUFFER, hitch);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyBuffer(VkDevice _device, VkBuffer _buffer, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_buffer, buffer, _buffer);
   if (buffer) {
      /* The device's list is what the runner's capture reads, so a buffer
       * that goes away leaves it. */
      struct ps5vk_buffer **at = &device->buffers;
      while (*at != NULL && *at != buffer)
         at = &(*at)->next_in_device;
      if (*at == buffer)
         *at = buffer->next_in_device;
      vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
   }
}

/* Mesa's vkGetBufferMemoryRequirements(2) call this with a create info rebuilt
 * from the buffer, so the requirements depend on the create info alone. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_GetDeviceBufferMemoryRequirements(VkDevice _device,
                                        const VkDeviceBufferMemoryRequirements *pInfo,
                                        VkMemoryRequirements2 *pMemoryRequirements)
{
   (void)_device;
   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size = align64(pInfo->pCreateInfo->size, PS5VK_BUFFER_ALIGNMENT),
      .alignment = PS5VK_BUFFER_ALIGNMENT,
      .memoryTypeBits = PS5VK_MEMORY_TYPE_BITS,
   };
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_BindBufferMemory2(VkDevice _device, uint32_t bindInfoCount,
                        const VkBindBufferMemoryInfo *pBindInfos)
{
   (void)_device;
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(ps5vk_buffer, buffer, pBindInfos[i].buffer);
      VK_FROM_HANDLE(ps5vk_device_memory, memory, pBindInfos[i].memory);
      const VkDeviceSize offset = pBindInfos[i].memoryOffset;
      /* Valid usage: an aligned offset, and the requirements' size fits. */
      assert(offset % PS5VK_BUFFER_ALIGNMENT == 0);
      assert(offset + align64(buffer->vk.size, PS5VK_BUFFER_ALIGNMENT) <= memory->vk.size);

      buffer->memory = memory;
      buffer->vk.device_address = (uint64_t)(uintptr_t)memory->direct.address + offset;
      assert(ps5vk_gpu_range_valid(buffer->vk.device_address, buffer->vk.size));
   }
   return VK_SUCCESS;
}

/* The region a bound buffer names, which is what a replay has to pin: the
 * allocation the buffer is bound to, at the allocation's own address and
 * page-rounded size, because that is the size the allocator is asked for on
 * both sides. A buffer with no memory of its own -- Mesa's meta rectangles are
 * bound into a command buffer's table chunk -- is its own range. False for a
 * buffer that is not bound yet, which has no address a submission can name. */
static bool
ps5vk_buffer_region(const struct ps5vk_buffer *buffer, ps5vk_debug_stage *region)
{
   if (buffer->vk.device_address == 0)
      return false;
   if (buffer->memory != NULL) {
      *region = (ps5vk_debug_stage){.address = buffer->memory->direct.address,
                                    .bytes = buffer->memory->direct.bytes};
   } else {
      *region = (ps5vk_debug_stage){.address = (void *)(uintptr_t)buffer->vk.device_address,
                                    .bytes = buffer->vk.size};
   }
   return true;
}

uint32_t
ps5vk_debug_buffers(VkDevice _device, ps5vk_debug_stage *buffers, uint32_t capacity)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   uint32_t count = 0;
   for (const struct ps5vk_buffer *buffer = device ? device->buffers : NULL; buffer != NULL;
        buffer = buffer->next_in_device) {
      ps5vk_debug_stage region;
      if (ps5vk_buffer_region(buffer, &region))
         count++;
   }
   if (buffers == NULL || capacity == 0)
      return count;
   /* The device's list is newest first, and the capture wants the order the
    * allocations were made in instead: a replay hands a region to the first
    * allocation of its size, so two buffers of the same size only come back to
    * the addresses the console gave them when the oldest is listed first
    * (tools/golden.py, a driver run's replay). */
   uint32_t at = count;
   for (const struct ps5vk_buffer *buffer = device ? device->buffers : NULL;
        buffer != NULL && at > 0; buffer = buffer->next_in_device) {
      ps5vk_debug_stage region;
      if (!ps5vk_buffer_region(buffer, &region))
         continue;
      at--;
      if (at < capacity)
         buffers[at] = region;
   }
   return count;
}

/* vkCreateBufferView: a view of a buffer's bytes as texels. What consumes one
 * is a uniform or storage texel buffer, which is Phase D2 and which the
 * descriptor-set layout refuses by name; this command's own rule is the
 * reporting rule an application can check with vkGetPhysicalDeviceFormatProperties:
 * the format must carry the texel-buffer feature the buffer's usage asks for.
 * No format reports one yet (docs/V0_FORMATS_AUDIT.md), so every valid-shaped
 * call refuses by name and says which format and which feature -- the same
 * answer as "this device samples no texel buffer", reached through the
 * reporting rather than by having no path at all. The object exists for the
 * step that reports one. */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateBufferView(VkDevice _device, const VkBufferViewCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator, VkBufferView *pView)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_buffer, buffer, pCreateInfo->buffer);
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
   /* The B2 device test records every 1.0 command with the handles it has
    * (none), so a missing buffer is a refusal rather than a crash. */
   if (buffer == NULL) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "vkCreateBufferView names no buffer this driver created");
   }
   const struct ps5vk_format *const entry = ps5vk_find_format(pCreateInfo->format);
   const VkFormatFeatureFlags wanted =
      (buffer->vk.usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT) != 0
         ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT
         : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
   if (entry == NULL || (entry->buffer_features & wanted) == 0) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "vkCreateBufferView names format %d, whose buffer features do not carry "
                       "%s: no probe has proved a texel buffer for it, so the device reports "
                       "none (docs/V0_FORMATS_AUDIT.md)",
                       (int)pCreateInfo->format,
                       wanted == VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT
                          ? "VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT"
                          : "VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT");
   }
   /* VK_WHOLE_SIZE means "from offset to the end of the buffer", not the
    * sentinel value: Vulkan defines it that way and every other site in this
    * driver resolves it (ps5vk_cmd_buffer.c, ps5vk_descriptor_set.c,
    * ps5vk_draw.c). This one compared it literally, so a whole-buffer view of any
    * buffer smaller than ~1.8e19 bytes was refused as out of bounds -- a
    * vkQuake palette-octree view is what found it, and any application that
    * writes VK_WHOLE_SIZE meets the same refusal (R3 of that port's requests).
    * The resolved range is what the view keeps, so the texel-buffer descriptor
    * gets a byte count rather than a sentinel. */
   const VkDeviceSize range = pCreateInfo->range == VK_WHOLE_SIZE
                                 ? buffer->vk.size - pCreateInfo->offset
                                 : pCreateInfo->range;
   /* Valid usage: the view names bytes the buffer holds. A whole-buffer view of
    * an offset at the end therefore resolves to zero and is refused with the
    * rest. */
   if (pCreateInfo->offset >= buffer->vk.size || range == 0 ||
       range > buffer->vk.size - pCreateInfo->offset) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "vkCreateBufferView names %llu bytes from offset %llu of a buffer of %llu "
                       "bytes",
                       (unsigned long long)pCreateInfo->range,
                       (unsigned long long)pCreateInfo->offset,
                       (unsigned long long)buffer->vk.size);
   }
   struct ps5vk_buffer_view *const view =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*view), VK_OBJECT_TYPE_BUFFER_VIEW);
   if (view == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   view->buffer = buffer;
   view->format = pCreateInfo->format;
   view->offset = pCreateInfo->offset;
   view->range = range;
   *pView = ps5vk_buffer_view_to_handle(view);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyBufferView(VkDevice _device, VkBufferView bufferView,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_buffer_view, view, bufferView);
   if (view == NULL)
      return;
   vk_object_free(&device->vk, pAllocator, view);
}
