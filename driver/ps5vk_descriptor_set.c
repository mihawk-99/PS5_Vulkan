/*
 * PS5 Vulkan driver - descriptor pools and sets.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C3 (docs/M5_PHASE_C.md). The application's uniform
 * buffers reach a shader through set 0: a pool allocates sets from the
 * device, vkUpdateDescriptorSets records the buffer each binding names, and
 * vkCmdBindDescriptorSets stores the set in the command buffer, where the
 * draw writes it into the stage's set-0 table (ps5vk_draw.c).
 *
 * Each array element has its own record. Layouts number records in binding
 * order, independently of the byte strides in the GPU descriptor table.
 * Writes and copies may span consecutive compatible bindings, as Vulkan allows.
 *
 * A set holds its layout: vkUpdateDescriptorSets has no layout argument, so
 * the set is the only place its binding space is known, and the layout stays
 * reference counted until the set's last reference goes away. A set lives
 * from vkAllocateDescriptorSets until vkFreeDescriptorSets,
 * vkResetDescriptorPool or vkDestroyDescriptorPool returns it to its pool,
 * which is its only owner.
 */

#include "ps5vk_private.h"

#include <assert.h>

/* The pool's entry for one descriptor type, or NULL when the pool was not
 * created with that size. */
static struct ps5vk_descriptor_pool_size *
ps5vk_descriptor_pool_find(struct ps5vk_descriptor_pool *pool, VkDescriptorType type)
{
   for (uint32_t i = 0; i < pool->size_count; i++) {
      if (pool->sizes[i].type == type)
         return &pool->sizes[i];
   }
   return NULL;
}

/* Whether every descriptor a layout's bindings need is still in the pool. */
static bool
ps5vk_descriptor_pool_has(struct ps5vk_descriptor_pool *pool,
                          const struct ps5vk_descriptor_set_layout *layout)
{
   for (uint32_t b = 0; b < layout->binding_count; b++) {
      const struct ps5vk_descriptor_binding *const binding = &layout->bindings[b];
      if (binding->count == 0)
         continue;
      const struct ps5vk_descriptor_pool_size *const size =
         ps5vk_descriptor_pool_find(pool, binding->type);
      if (size == NULL || size->remaining < binding->count)
         return false;
   }
   return true;
}

/* Takes or returns the descriptors a layout's bindings need. A caller takes
 * them only after ps5vk_descriptor_pool_has said they are there, and a set's
 * release gives back exactly what its allocation took. */
static void
ps5vk_descriptor_pool_take(struct ps5vk_descriptor_pool *pool,
                           const struct ps5vk_descriptor_set_layout *layout)
{
   for (uint32_t b = 0; b < layout->binding_count; b++) {
      const struct ps5vk_descriptor_binding *const binding = &layout->bindings[b];
      if (binding->count != 0)
         ps5vk_descriptor_pool_find(pool, binding->type)->remaining -= binding->count;
   }
}

static void
ps5vk_descriptor_pool_give(struct ps5vk_descriptor_pool *pool,
                           const struct ps5vk_descriptor_set_layout *layout)
{
   for (uint32_t b = 0; b < layout->binding_count; b++) {
      const struct ps5vk_descriptor_binding *const binding = &layout->bindings[b];
      if (binding->count != 0)
         ps5vk_descriptor_pool_find(pool, binding->type)->remaining += binding->count;
   }
}

/* Returns a set to its pool: the descriptors it took, its layout reference
 * and the set itself. The caller unlinks it from the pool first. */
static void
ps5vk_descriptor_set_release(struct ps5vk_device *device, struct ps5vk_descriptor_pool *pool,
                             struct ps5vk_descriptor_set *set)
{
   ps5vk_descriptor_pool_give(pool, set->layout);
   vk_descriptor_set_layout_unref(&device->vk, &set->layout->vk);
   vk_object_free(&device->vk, NULL, set);
}

/* Unlinks a set from its pool; false when the pool does not hold it. */
static bool
ps5vk_descriptor_pool_unlink(struct ps5vk_descriptor_pool *pool,
                             struct ps5vk_descriptor_set *set)
{
   struct ps5vk_descriptor_set **at = &pool->sets;
   while (*at != NULL && *at != set)
      at = &(*at)->next_in_pool;
   if (*at != set)
      return false;
   *at = set->next_in_pool;
   set->next_in_pool = NULL;
   pool->set_count--;
   return true;
}

/* Returns every set the pool holds, which destroying it and resetting it both
 * do. */
static void
ps5vk_descriptor_pool_clear(struct ps5vk_device *device, struct ps5vk_descriptor_pool *pool)
{
   while (pool->sets != NULL) {
      struct ps5vk_descriptor_set *const set = pool->sets;
      ps5vk_descriptor_pool_unlink(pool, set);
      ps5vk_descriptor_set_release(device, pool, set);
   }
}

const struct ps5vk_descriptor_buffer *
ps5vk_cmd_buffer_descriptor(const struct ps5vk_cmd_buffer *cmd_buffer, uint32_t set_index,
                            uint32_t binding, uint32_t element)
{
   if (set_index >= PS5VK_DESCRIPTOR_SET_COUNT)
      return NULL;
   const struct ps5vk_descriptor_set *const set = cmd_buffer->descriptor_sets[set_index];
   if (set == NULL || binding >= set->layout->binding_count ||
       element >= set->layout->bindings[binding].count)
      return NULL;
   const struct ps5vk_descriptor_buffer *const buffer = &set->buffers[set->layout->bindings[binding].record_index + element];
   return buffer->type == VK_DESCRIPTOR_TYPE_MAX_ENUM ? NULL : buffer;
}

static VkResult
ps5vk_CreateDescriptorPool_untimed(VkDevice _device, const VkDescriptorPoolCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkDescriptorPool *pDescriptorPool)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* vkFreeDescriptorSets and vkResetDescriptorPool are the pool behaviour
    * this driver has, so freeing sets is the only flag it knows; update after
    * bind would need a draw to rewrite a table an earlier bind named. */
   if (pCreateInfo->flags &
       ~(VkDescriptorPoolCreateFlags)VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "descriptor pool flags 0x%x are not supported yet",
                       (unsigned)pCreateInfo->flags);
   /* Valid usage: maxSets is at least 1. */
   assert(pCreateInfo->maxSets > 0);

   struct ps5vk_descriptor_pool *const pool =
      vk_object_zalloc(&device->vk, pAllocator,
                       sizeof(*pool) + pCreateInfo->poolSizeCount * sizeof(pool->sizes[0]),
                       VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   pool->max_sets = pCreateInfo->maxSets;
   pool->size_count = pCreateInfo->poolSizeCount;
   for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; i++)
      pool->sizes[i] = (struct ps5vk_descriptor_pool_size){
         .type = pCreateInfo->pPoolSizes[i].type,
         .remaining = pCreateInfo->pPoolSizes[i].descriptorCount,
      };

   *pDescriptorPool = ps5vk_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateDescriptorPool(VkDevice _device, const VkDescriptorPoolCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkDescriptorPool *pDescriptorPool)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_CreateDescriptorPool_untimed(_device, pCreateInfo, pAllocator, pDescriptorPool);
   ps5vk_hitch_end(PS5VK_HITCH_DESCRIPTOR, hitch);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool _descriptorPool,
                            const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_descriptor_pool, pool, _descriptorPool);
   if (!pool)
      return;
   /* Destroying a pool frees the sets it still holds, which is why a set is
    * never anything but the pool's. */
   ps5vk_descriptor_pool_clear(device, pool);
   vk_object_free(&device->vk, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_ResetDescriptorPool(VkDevice _device, VkDescriptorPool _descriptorPool,
                          VkDescriptorPoolResetFlags flags)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_descriptor_pool, pool, _descriptorPool);
   /* Valid usage: flags is 0, reserved for future use. */
   assert(flags == 0);
   (void)flags;
   if (!pool)
      return VK_SUCCESS;
   ps5vk_descriptor_pool_clear(device, pool);
   return VK_SUCCESS;
}

static VkResult
ps5vk_AllocateDescriptorSets_untimed(VkDevice _device, const VkDescriptorSetAllocateInfo *pAllocateInfo,
                             VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_descriptor_pool, pool, pAllocateInfo->descriptorPool);
   for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++)
      pDescriptorSets[i] = VK_NULL_HANDLE;

   if (pool->set_count + pAllocateInfo->descriptorSetCount > pool->max_sets)
      return vk_errorf(device, VK_ERROR_OUT_OF_POOL_MEMORY, "the pool holds %u of its %u sets",
                       pool->set_count, pool->max_sets);

   VkResult result = VK_SUCCESS;
   uint32_t allocated = 0;
   for (; allocated < pAllocateInfo->descriptorSetCount; allocated++) {
      VK_FROM_HANDLE(ps5vk_descriptor_set_layout, layout, pAllocateInfo->pSetLayouts[allocated]);
      if (!ps5vk_descriptor_pool_has(pool, layout)) {
         result = vk_errorf(device, VK_ERROR_OUT_OF_POOL_MEMORY,
                            "the pool has no descriptors left for the set's bindings");
         break;
      }
      ps5vk_descriptor_pool_take(pool, layout);
      struct ps5vk_descriptor_set *const set =
         vk_object_zalloc(&device->vk, NULL,
                          sizeof(*set) + layout->descriptor_count * sizeof(set->buffers[0]),
                          VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (!set) {
         ps5vk_descriptor_pool_give(pool, layout);
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         break;
      }
      set->layout = layout;
      vk_descriptor_set_layout_ref(&layout->vk);
      /* A binding no write has named keeps this type, which is what a draw
       * refuses (ps5vk_cmd_buffer_descriptor). */
      for (uint32_t b = 0; b < layout->descriptor_count; b++)
         set->buffers[b].type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
      set->next_in_pool = pool->sets;
      pool->sets = set;
      pool->set_count++;
      pDescriptorSets[allocated] = ps5vk_descriptor_set_to_handle(set);
   }
   if (result != VK_SUCCESS) {
      /* Vulkan leaves pDescriptorSets undefined on failure: nothing of a
       * failed allocation stays allocated, and no entry names a set. */
      while (allocated > 0) {
         allocated--;
         VK_FROM_HANDLE(ps5vk_descriptor_set, set, pDescriptorSets[allocated]);
         if (set != NULL && ps5vk_descriptor_pool_unlink(pool, set))
            ps5vk_descriptor_set_release(device, pool, set);
         pDescriptorSets[allocated] = VK_NULL_HANDLE;
      }
   }
   return result;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_AllocateDescriptorSets(VkDevice _device, const VkDescriptorSetAllocateInfo *pAllocateInfo,
                             VkDescriptorSet *pDescriptorSets)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_AllocateDescriptorSets_untimed(_device, pAllocateInfo, pDescriptorSets);
   ps5vk_hitch_end(PS5VK_HITCH_DESCRIPTOR, hitch);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_FreeDescriptorSets(VkDevice _device, VkDescriptorPool _descriptorPool,
                         uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_descriptor_pool, pool, _descriptorPool);
   if (!pool)
      return VK_SUCCESS;
   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      VK_FROM_HANDLE(ps5vk_descriptor_set, set, pDescriptorSets[i]);
      if (set != NULL && ps5vk_descriptor_pool_unlink(pool, set))
         ps5vk_descriptor_set_release(device, pool, set);
   }
   return VK_SUCCESS;
}

/* Record one element; the caller resolves its position in the set. */
static void
ps5vk_descriptor_write_one(struct ps5vk_descriptor_buffer *record,
                           const VkWriteDescriptorSet *write, uint32_t element)
{
   /* A texel buffer's write names a view: what the draw needs is the view's
    * buffer, offset, range and format, and the view's own rule is that the
    * format carries the texel-buffer feature the buffer's usage asks for
    * (ps5vk_buffer.c, ps5vk_draw.c). */
   if ((write->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
        write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) &&
       write->pTexelBufferView != NULL) {
      *record = (struct ps5vk_descriptor_buffer){
         .type = write->descriptorType,
         .buffer_view = write->pTexelBufferView[element],
      };
      return;
   }
   /* A storage image's write names a view too, and no sampler: the draw builds
    * its 32-byte descriptor from the image the view names (ps5vk_draw.c). */
   if ((write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
        write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
        write->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) &&
       write->pImageInfo != NULL) {
      *record = (struct ps5vk_descriptor_buffer){
         .type = write->descriptorType,
         .view = write->pImageInfo[element].imageView,
      };
      return;
   }
   /* A bare sampler's write names a sampler and no view (Vulkan's own shape for
    * the type): the sampler's words are what the draw puts in that binding's
    * entry (ps5vk_draw.c, R2). */
   if (write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && write->pImageInfo != NULL) {
      *record = (struct ps5vk_descriptor_buffer){
         .type = write->descriptorType,
         .sampler = write->pImageInfo[element].sampler,
      };
      return;
   }
   if (write->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
       write->pImageInfo != NULL) {
      /* The view and sampler, not the image they name: what the image holds
       * can change between the write and the draw, and only recording fails on
       * what the hardware has not sampled (ps5vk_draw.c). */
      *record = (struct ps5vk_descriptor_buffer){
         .type = write->descriptorType,
         .view = write->pImageInfo[element].imageView,
         .sampler = write->pImageInfo[element].sampler,
      };
      return;
   }
   /* A dynamic uniform buffer writes the same record: its dynamic offset is
    * the application's, added when the draw names the address (ps5vk_draw.c,
    * VkBindDescriptorSetsInfo.pDynamicOffsets). */
   if ((write->descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        write->descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
        write->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ||
       write->pBufferInfo == NULL) {
      /* A type whose table entry this driver cannot fill is still recorded, so
       * that the draw refuses it by name (a storage buffer is D2). */
      *record = (struct ps5vk_descriptor_buffer){.type = write->descriptorType};
      return;
   }
   VK_FROM_HANDLE(ps5vk_buffer, buffer, write->pBufferInfo[element].buffer);
   if (buffer == NULL || buffer->vk.device_address == 0) {
      /* An unbound buffer has no address: the draw refuses the binding rather
       * than writing a descriptor that names the wrong memory. */
      *record = (struct ps5vk_descriptor_buffer){.type = write->descriptorType};
      return;
   }
   const VkDeviceSize offset = write->pBufferInfo[element].offset;
   const VkDeviceSize range = write->pBufferInfo[element].range;
   *record = (struct ps5vk_descriptor_buffer){
      .address = buffer->vk.device_address + offset,
      .size = range == VK_WHOLE_SIZE ? buffer->vk.size - offset : range,
      .type = write->descriptorType,
   };
}

/* Compatible consecutive bindings are consecutive records, including past
 * zero-count bindings. Valid Vulkan writes/copies require matching types. */
static bool
ps5vk_descriptor_range(const struct ps5vk_descriptor_set *set, uint32_t binding,
                       uint32_t element, uint32_t count, uint32_t *first)
{
   if (set == NULL || binding >= set->layout->binding_count ||
       element >= set->layout->bindings[binding].count)
      return false;
   *first = set->layout->bindings[binding].record_index + element;
   return count <= set->layout->descriptor_count - *first;
}

static void
ps5vk_descriptor_set_write(struct ps5vk_device *device, const VkWriteDescriptorSet *write)
{
   VK_FROM_HANDLE(ps5vk_descriptor_set, set, write->dstSet);
   uint32_t first;
   if (!ps5vk_descriptor_range(set, write->dstBinding, write->dstArrayElement,
                               write->descriptorCount, &first)) {
      vk_errorf(device, VK_ERROR_UNKNOWN, "descriptor write exceeds the set's records");
      return;
   }
   for (uint32_t i = 0; i < write->descriptorCount; i++)
      ps5vk_descriptor_write_one(&set->buffers[first + i], write, i);
}

static void
ps5vk_descriptor_set_copy(struct ps5vk_device *device, const VkCopyDescriptorSet *copy)
{
   VK_FROM_HANDLE(ps5vk_descriptor_set, source, copy->srcSet);
   VK_FROM_HANDLE(ps5vk_descriptor_set, destination, copy->dstSet);
   uint32_t src, dst;
   if (!ps5vk_descriptor_range(source, copy->srcBinding, copy->srcArrayElement,
                               copy->descriptorCount, &src) ||
       !ps5vk_descriptor_range(destination, copy->dstBinding, copy->dstArrayElement,
                               copy->descriptorCount, &dst)) {
      vk_errorf(device, VK_ERROR_UNKNOWN, "descriptor copy exceeds the set's records");
      return;
   }
   memmove(&destination->buffers[dst], &source->buffers[src],
           copy->descriptorCount * sizeof(source->buffers[0]));
}

static void
ps5vk_UpdateDescriptorSets_untimed(VkDevice _device, uint32_t descriptorWriteCount,
                           const VkWriteDescriptorSet *pDescriptorWrites,
                           uint32_t descriptorCopyCount,
                           const VkCopyDescriptorSet *pDescriptorCopies)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   for (uint32_t i = 0; i < descriptorWriteCount; i++)
      ps5vk_descriptor_set_write(device, &pDescriptorWrites[i]);
   for (uint32_t i = 0; i < descriptorCopyCount; i++)
      ps5vk_descriptor_set_copy(device, &pDescriptorCopies[i]);
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_UpdateDescriptorSets(VkDevice _device, uint32_t descriptorWriteCount,
                           const VkWriteDescriptorSet *pDescriptorWrites,
                           uint32_t descriptorCopyCount,
                           const VkCopyDescriptorSet *pDescriptorCopies)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   ps5vk_UpdateDescriptorSets_untimed(_device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
   ps5vk_hitch_end(PS5VK_HITCH_DESCRIPTOR, hitch);
}

/* vkCmdBindDescriptorSets reaches this through the runtime's common entry
 * point, which turns its arguments into a VkBindDescriptorSetsInfo
 * (vk_common_CmdBindDescriptorSets); vkCmdBindDescriptorSets2KHR and
 * vkCmdBindDescriptorSets2 arrive directly. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdBindDescriptorSets2KHR(VkCommandBuffer commandBuffer,
                                const VkBindDescriptorSetsInfo *pBindDescriptorSetsInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   const VkBindDescriptorSetsInfo *const info = pBindDescriptorSetsInfo;
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   uint32_t next_offset = 0;
   for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
      const uint32_t index = info->firstSet + i;
      if (index >= PS5VK_DESCRIPTOR_SET_COUNT) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "descriptor set %u: more than the %u sets this driver advertises "
                                 "(VkPhysicalDeviceLimits.maxBoundDescriptorSets; "
                                 "PS5VK_DESCRIPTOR_SET_COUNT, ps5vk_private.h)", index,
                                 PS5VK_DESCRIPTOR_SET_COUNT);
         return;
      }
      VK_FROM_HANDLE(ps5vk_descriptor_set, set, info->pDescriptorSets[i]);
      cmd_buffer->descriptor_sets[index] = set;
      memset(cmd_buffer->descriptor_set_offsets[index], 0,
             sizeof(cmd_buffer->descriptor_set_offsets[index]));
      if (set == NULL)
         continue;
      /* Vulkan orders offsets by set, then binding, then array element. */
      for (uint32_t b = 0; b < set->layout->binding_count; b++) {
         const struct ps5vk_descriptor_binding *binding = &set->layout->bindings[b];
         if (binding->count == 0 || binding->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            continue;
         if (binding->dynamic_index > PS5VK_DYNAMIC_UNIFORM_COUNT ||
             binding->count > PS5VK_DYNAMIC_UNIFORM_COUNT - binding->dynamic_index ||
             binding->count > info->dynamicOffsetCount - next_offset) {
            ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                    "set %u binding %u: unsupported dynamic uniform array/count",
                                    index, b);
            return;
         }
         for (uint32_t element = 0; element < binding->count; element++)
            cmd_buffer->descriptor_set_offsets[index][binding->dynamic_index + element] =
               info->pDynamicOffsets[next_offset++];
      }
   }
   if (next_offset != info->dynamicOffsetCount)
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "bind consumed %u dynamic offsets but received %u",
                              next_offset, info->dynamicOffsetCount);
}

/* vkCmdPushDescriptorSetKHR reaches this through the runtime's common entry
 * point (vk_common_CmdPushDescriptorSetKHR). A pushed set is an ordinary set
 * object that the command buffer owns: it is written with the same code a
 * vkUpdateDescriptorSets write uses and bound at the set index, and a draw
 * reads it while it is recorded (ps5vk_cmd_buffer_descriptor), so it only has
 * to outlive the recording; the command buffer frees it on reset and destroy
 * (ps5vk_cmd_buffer_release_push_sets). Mesa's vk_meta blits, resolves and
 * copies bind their source images this way, which is what moves those
 * operations from the CPU onto the GPU. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdPushDescriptorSet2KHR(VkCommandBuffer commandBuffer,
                               const VkPushDescriptorSetInfoKHR *pPushDescriptorSetInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   const VkPushDescriptorSetInfoKHR *const info = pPushDescriptorSetInfo;
   struct ps5vk_device *const device =
      container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);
   if (info->set >= PS5VK_DESCRIPTOR_SET_COUNT || pipeline_layout == NULL ||
       info->set >= pipeline_layout->set_count || pipeline_layout->set_layouts[info->set] == NULL) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a push descriptor set %u the pipeline layout does not declare",
                              info->set);
      return;
   }
   struct ps5vk_descriptor_set_layout *const layout =
      container_of(pipeline_layout->set_layouts[info->set], struct ps5vk_descriptor_set_layout, vk);
   struct ps5vk_descriptor_set *const set =
      vk_object_zalloc(&device->vk, NULL,
                       sizeof(*set) + layout->descriptor_count * sizeof(set->buffers[0]),
                       VK_OBJECT_TYPE_DESCRIPTOR_SET);
   struct ps5vk_descriptor_set **const slot =
      set != NULL ? util_dynarray_grow(&cmd_buffer->push_sets, struct ps5vk_descriptor_set *, 1)
                  : NULL;
   if (slot == NULL) {
      if (set != NULL)
         vk_object_free(&device->vk, NULL, set);
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory for a push descriptor set");
      return;
   }
   *slot = set;
   set->layout = layout;
   set->push = true;
   vk_descriptor_set_layout_ref(&layout->vk);
   for (uint32_t b = 0; b < layout->descriptor_count; b++)
      set->buffers[b].type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
   for (uint32_t i = 0; i < info->descriptorWriteCount; i++) {
      VkWriteDescriptorSet write = info->pDescriptorWrites[i];
      write.dstSet = ps5vk_descriptor_set_to_handle(set);
      ps5vk_descriptor_set_write(device, &write);
   }
   cmd_buffer->descriptor_sets[info->set] = set;
   memset(cmd_buffer->descriptor_set_offsets[info->set], 0,
          sizeof(cmd_buffer->descriptor_set_offsets[info->set]));
}

void
ps5vk_cmd_buffer_release_push_sets(struct ps5vk_cmd_buffer *cmd_buffer)
{
   struct ps5vk_device *const device =
      container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk);
   util_dynarray_foreach (&cmd_buffer->push_sets, struct ps5vk_descriptor_set *, set) {
      vk_descriptor_set_layout_unref(&device->vk, &(*set)->layout->vk);
      vk_object_free(&device->vk, NULL, *set);
   }
   util_dynarray_clear(&cmd_buffer->push_sets);
}
