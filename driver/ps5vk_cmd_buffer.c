/*
 * PS5 Vulkan driver - command buffers.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phases B5 and B7 (docs/M5_PHASE_B.md). Command pools,
 * allocation, reset and freeing are Mesa's common implementation. A command
 * buffer adds the PM4 words recorded into it (ps5vk_draw.c), which submission
 * copies into the queue's GPU-visible buffer (ps5vk_queue.c), and the register
 * tables those words point at. The tables live in GPU-visible chunks the
 * command buffer keeps across resets: Vulkan resets a command buffer only once
 * its submissions have completed, so by then the GPU no longer reads them.
 */

#include "ps5vk_private.h"
#include "ps5vk_debug.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "vk_alloc.h"
#include "vk_command_pool.h"
#include "util/os_time.h"

void
ps5vk_cmd_buffer_error(struct ps5vk_cmd_buffer *cmd_buffer, VkResult result,
                       const char *command, const char *file, int line, const char *format, ...)
{
   /* stderr is the title's trace even when it has no Vulkan debug messenger.
    * Keep Mesa's callback too, and evaluate the format arguments only once. */
   va_list args;
   va_start(args, format);
   fprintf(stderr, "[ps5vk] recording refusal in %s: ", command);
   vfprintf(stderr, format, args);
   fputc('\n', stderr);
   va_end(args);
   va_start(args, format);
   vk_command_buffer_set_error(&cmd_buffer->vk,
                               __vk_errorv(cmd_buffer, result, file, line, format, args));
   va_end(args);
}

static void
ps5vk_cmd_buffer_release_tables(struct ps5vk_cmd_buffer *cmd_buffer)
{
   struct ps5vk_device *const device =
      container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk);
   struct ps5vk_table_chunk *chunk = cmd_buffer->table_chunks;
   while (chunk != NULL) {
      struct ps5vk_table_chunk *const next = chunk->next_in_buffer;
      /* The device's list is what the runner's capture reads, so a chunk that
       * goes away leaves it. */
      struct ps5vk_table_chunk **at = &device->table_chunks;
      while (*at != NULL && *at != chunk)
         at = &(*at)->next_in_device;
      if (*at == chunk)
         *at = chunk->next_in_device;
      ps5vk_direct_mapping_destroy(&chunk->mapping);
      free(chunk);
      chunk = next;
   }
   cmd_buffer->table_chunks = NULL;
   cmd_buffer->table_chunk = NULL;
   cmd_buffer->table_bytes_used = 0;
}

/* Forgets what was recorded: words, table use, bound pipeline, bound sets and
 * rendering. */
static void
ps5vk_cmd_buffer_clear_state(struct ps5vk_cmd_buffer *cmd_buffer)
{
   util_dynarray_clear(&cmd_buffer->words);
   util_dynarray_clear(&cmd_buffer->targets);
   /* A vkCmdUpdateBuffer snapshot belongs to the recording it was made for. */
   util_dynarray_foreach (&cmd_buffer->copies, struct ps5vk_memory_copy, copy)
      free(copy->owned_source);
   util_dynarray_clear(&cmd_buffer->copies);
   /* The chunks stay mapped across resets -- a command buffer is reset only
    * once its submissions have completed -- but the next recording starts at
    * the first of them again. */
   cmd_buffer->table_chunk = cmd_buffer->table_chunks;
   cmd_buffer->table_bytes_used = 0;
   cmd_buffer->pipeline = NULL;
   cmd_buffer->primitive_restart = false;
   cmd_buffer->primitive_restart_splits = 0;
   util_dynarray_clear(&cmd_buffer->fence_patches);
   cmd_buffer->barrier_targets = 0;
   cmd_buffer->pass_first_target = UINT32_MAX;
   cmd_buffer->pass_drawn = false;
   cmd_buffer->samples_early = false;
   /* A set bound before the reset does not stay bound: the recording that
    * follows is a new command buffer's, and a stale set there would be one the
    * application never bound in it (ps5vk_descriptor_set.c). */
   memset(cmd_buffer->descriptor_sets, 0, sizeof(cmd_buffer->descriptor_sets));
   memset(cmd_buffer->descriptor_set_offsets, 0, sizeof(cmd_buffer->descriptor_set_offsets));
   cmd_buffer->rendering = false;
}

static VkResult
ps5vk_cmd_buffer_create(struct vk_command_pool *pool, VkCommandBufferLevel level,
                        struct vk_command_buffer **out_command_buffer)
{
   struct vk_device *const device = pool->base.device;
   struct ps5vk_cmd_buffer *const cmd_buffer =
      vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkResult result = vk_command_buffer_init_with_params(
      &cmd_buffer->vk, &(struct vk_command_buffer_init_params){
         .pool = pool,
         .ops = &ps5vk_cmd_buffer_ops,
         .level = level,
         .needs_cmd_queue = level == VK_COMMAND_BUFFER_LEVEL_SECONDARY,
      });
   if (result != VK_SUCCESS) {
      vk_free(&pool->alloc, cmd_buffer);
      return result;
   }
   util_dynarray_init(&cmd_buffer->words, NULL);
   util_dynarray_init(&cmd_buffer->targets, NULL);
   util_dynarray_init(&cmd_buffer->fence_patches, NULL);
   cmd_buffer->pass_first_target = UINT32_MAX;
   util_dynarray_init(&cmd_buffer->copies, NULL);
   util_dynarray_init(&cmd_buffer->push_sets, NULL);
   ps5vk_cmd_buffer_clear_state(cmd_buffer);

   *out_command_buffer = &cmd_buffer->vk;
   return VK_SUCCESS;
}

static void
ps5vk_cmd_buffer_reset(struct vk_command_buffer *vk_cmd_buffer, VkCommandBufferResetFlags flags)
{
   struct ps5vk_cmd_buffer *const cmd_buffer =
      container_of(vk_cmd_buffer, struct ps5vk_cmd_buffer, vk);
   struct ps5vk_queue *const queue =
      ps5vk_device_profile_queue(container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk));
   struct ps5vk_queue_profile *const p = queue != NULL && queue->profile.enabled ? &queue->profile
                                                                                 : NULL;
   const uint64_t started = p != NULL ? ps5vk_profile_now() : 0;
   vk_command_buffer_reset(&cmd_buffer->vk);
   ps5vk_cmd_buffer_release_push_sets(cmd_buffer);
   const uint64_t common = p != NULL ? ps5vk_profile_now() : 0;
   if (flags & VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) {
      ps5vk_cmd_buffer_release_tables(cmd_buffer);
      util_dynarray_fini(&cmd_buffer->words);
      util_dynarray_init(&cmd_buffer->words, NULL);
   }
   ps5vk_cmd_buffer_clear_state(cmd_buffer);
   if (p != NULL) {
      p->resets++;
      p->reset_common_ns += common - started;
      p->reset_driver_ns += ps5vk_profile_now() - common;
   }
}

static void
ps5vk_cmd_buffer_destroy(struct vk_command_buffer *vk_cmd_buffer)
{
   struct ps5vk_cmd_buffer *const cmd_buffer =
      container_of(vk_cmd_buffer, struct ps5vk_cmd_buffer, vk);
   struct vk_command_pool *const pool = cmd_buffer->vk.pool;
   ps5vk_cmd_buffer_release_tables(cmd_buffer);
   ps5vk_cmd_buffer_release_push_sets(cmd_buffer);
   util_dynarray_fini(&cmd_buffer->push_sets);
   util_dynarray_foreach (&cmd_buffer->copies, struct ps5vk_memory_copy, copy)
      free(copy->owned_source);
   util_dynarray_fini(&cmd_buffer->targets);
   util_dynarray_fini(&cmd_buffer->fence_patches);
   util_dynarray_fini(&cmd_buffer->copies);
   util_dynarray_fini(&cmd_buffer->words);
   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&pool->alloc, cmd_buffer);
}

const struct vk_command_buffer_ops ps5vk_cmd_buffer_ops = {
   .create = ps5vk_cmd_buffer_create,
   .reset = ps5vk_cmd_buffer_reset,
   .destroy = ps5vk_cmd_buffer_destroy,
};

void *
ps5vk_cmd_buffer_table(struct ps5vk_cmd_buffer *cmd_buffer, size_t bytes, size_t alignment)
{
   assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
   bytes = ALIGN_POT(bytes, sizeof(uint64_t));
   assert(bytes <= PS5VK_TABLE_CHUNK_BYTES);
   for (;;) {
      if (cmd_buffer->table_chunk != NULL) {
         const struct ps5vk_direct_mapping *const chunk = &cmd_buffer->table_chunk->mapping;
         /* A descriptor's address has to keep its low byte zero, and the
          * chunk's first byte is aligned, so padding between tables is
          * enough. */
         const size_t offset = ALIGN_POT(cmd_buffer->table_bytes_used, alignment);
         if (offset <= chunk->bytes && bytes <= chunk->bytes - offset) {
            void *const table = (uint8_t *)chunk->address + offset;
            cmd_buffer->table_bytes_used = offset + bytes;
            return table;
         }
         cmd_buffer->table_chunk = cmd_buffer->table_chunk->next_in_buffer;
         cmd_buffer->table_bytes_used = 0;
         continue;
      }

      struct ps5vk_table_chunk *const node = malloc(sizeof(*node));
      if (node == NULL) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                 "no memory to track register tables");
         return NULL;
      }
      *node = (struct ps5vk_table_chunk){};
      const int32_t result =
         ps5vk_direct_mapping_create(&node->mapping, PS5VK_TABLE_CHUNK_BYTES,
                                     PS5VK_DIRECT_PAGE_BYTES, PS5VK_DIRECT_TABLES);
      if (result != 0) {
         free(node);
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                 "register tables could not be mapped in the address window: "
                                 "0x%08x", (unsigned)result);
         return NULL;
      }
      /* Append to the command buffer's list: the next table goes in this
       * chunk, and the chunks before it are full. */
      node->next_in_buffer = NULL;
      struct ps5vk_table_chunk **tail = &cmd_buffer->table_chunks;
      while (*tail != NULL)
         tail = &(*tail)->next_in_buffer;
      *tail = node;
      cmd_buffer->table_chunk = node;
      /* The device's list is what the runner's capture reads. */
      struct ps5vk_device *const device =
         container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk);
      node->next_in_device = device->table_chunks;
      device->table_chunks = node;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   struct ps5vk_queue *const queue =
      ps5vk_device_profile_queue(container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk));
   const bool secondary = cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
   const uint64_t started = queue && queue->profile.enabled && secondary ? ps5vk_profile_now() : 0;
   if (queue)
      ps5vk_profile_enter(queue, PS5VK_PROFILE_AFTER_BEGIN);
   /* Resets a command buffer that is not in the initial state first. */
   vk_command_buffer_begin(&cmd_buffer->vk, pBeginInfo);
   /* Secondary commands are encoded only when the primary executes them. */
   if (queue)
      ps5vk_profile_leave(queue, PS5VK_PROFILE_AFTER_BEGIN);
   if (started != 0) {
      queue->profile.begin_secondary_calls++;
      queue->profile.begin_secondary_ns += ps5vk_profile_now() - started;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   struct ps5vk_queue *const queue =
      ps5vk_device_profile_queue(container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk));
   if (queue)
      ps5vk_profile_enter(queue, PS5VK_PROFILE_AFTER_END);
   /* The next command buffer starts from restart off (R64). */
   if (!vk_command_buffer_has_error(&cmd_buffer->vk))
      ps5vk_cmd_buffer_end_primitive_restart(cmd_buffer);
   const VkResult result = vk_command_buffer_end(&cmd_buffer->vk);
   if (queue)
      ps5vk_profile_leave(queue, PS5VK_PROFILE_AFTER_END);
   return result;
}

/* A synchronization split with no copy: everything the queue needs is where it
 * falls in the words (ps5vk_queue.c, ps5vk_draw.c). */
bool
ps5vk_cmd_buffer_split(struct ps5vk_cmd_buffer *cmd_buffer)
{
   struct ps5vk_memory_copy *const split =
      util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
   if (!split) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record a submission split");
      return false;
   }
   *split = (struct ps5vk_memory_copy){
      .after_words = (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t),
   };
   return true;
}

/* A copy is a CPU memcpy here: memory is shared, and the queue performs it at
 * a submission split point so it keeps Vulkan's command order. Recording it
 * is therefore a record of the ranges and where they fall in the words
 * (ps5vk_queue.c). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdCopyMemoryKHR(VkCommandBuffer commandBuffer, const VkCopyDeviceMemoryInfoKHR *pInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   for (uint32_t r = 0; r < pInfo->regionCount; r++) {
      const VkDeviceMemoryCopyKHR *const region = &pInfo->pRegions[r];
      /* The common entry point copies one region as the same size on both
       * sides; the smaller of the two ranges is what a copy may touch. */
      const uint64_t bytes = MIN2(region->srcRange.size, region->dstRange.size);
      /* An unbound buffer has no address, and a copy of no bytes copies
       * nothing. */
      if (region->srcRange.address == 0 || region->dstRange.address == 0 || bytes == 0)
         continue;
      struct ps5vk_memory_copy *const copy =
         util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
      if (!copy) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                 "no memory to record a buffer copy");
         return;
      }
      /* after_words is where the copy falls in this command buffer's own
       * words, which is what the queue splits the submission at. */
      *copy = (struct ps5vk_memory_copy){
         .source = region->srcRange.address,
         .destination = region->dstRange.address,
         .bytes = bytes,
         .after_words = (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t),
      };
   }
}

/* Secondary commands use Mesa's owned command queue. Encoding into the primary
 * binds the current subpass attachments, including an unspecified inheritance
 * framebuffer. No GPU INDIRECT_BUFFER is used (the B8 hardware invariant). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount,
                         const VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   if (cmd_buffer->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "vkCmdExecuteCommands is recorded into a primary: a secondary that "
                              "executes another would have to be copied twice");
      return;
   }
   struct ps5vk_queue *const queue =
      ps5vk_device_profile_queue(container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk));
   const uint64_t started = queue && queue->profile.enabled ? ps5vk_profile_now() : 0;
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(ps5vk_cmd_buffer, secondary, pCommandBuffers[i]);
      if (secondary == NULL || secondary->vk.level != VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "vkCmdExecuteCommands executes secondary command buffers");
         return;
      }
      vk_cmd_queue_execute(&secondary->vk.cmd_queue, commandBuffer,
                            cmd_buffer->vk.base.device->command_dispatch_table);
   }
   if (started != 0) {
      queue->profile.execute_calls++;
      queue->profile.execute_buffers += commandBufferCount;
      queue->profile.execute_ns += ps5vk_profile_now() - started;
   }
}

/* vkCmdFillBuffer and vkCmdUpdateBuffer are CPU work at the same submission
 * split point a copy uses: memory is shared, so the queue writes where the
 * recording reaches it and Vulkan's command order holds (ps5vk_queue.c). The
 * application's data is snapshotted here, because it may free it as soon as the
 * call returns. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
                    VkDeviceSize size, uint32_t data)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_buffer, buffer, dstBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   /* Valid usage: the range is inside the buffer and four-byte aligned, and
    * VK_WHOLE_SIZE fills the rest of the buffer. */
   assert((dstOffset % 4) == 0 && dstOffset <= buffer->vk.size);
   if (size == VK_WHOLE_SIZE)
      size = buffer->vk.size - dstOffset;
   assert((size % 4) == 0 && size <= buffer->vk.size - dstOffset);
   /* A buffer that is not bound has no address, and filling no bytes fills
    * nothing; valid usage forbids the first. */
   if (buffer->vk.device_address == 0 || size == 0)
      return;
   struct ps5vk_memory_copy *const copy =
      util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
   if (!copy) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record a buffer fill");
      return;
   }
   *copy = (struct ps5vk_memory_copy){
      .destination = buffer->vk.device_address + dstOffset,
      .bytes = size,
      .fill = true,
      .fill_value = data,
      .after_words = (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t),
   };
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
                      VkDeviceSize dataSize, const void *pData)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_buffer, buffer, dstBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   /* Valid usage: at most 64 KiB, four-byte aligned, inside the buffer. */
   assert((dstOffset % 4) == 0 && (dataSize % 4) == 0 && dataSize <= 65536 &&
          dstOffset + dataSize <= buffer->vk.size);
   if (buffer->vk.device_address == 0 || dataSize == 0)
      return;
   void *const snapshot = malloc((size_t)dataSize);
   if (!snapshot) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record a buffer update");
      return;
   }
   memcpy(snapshot, pData, (size_t)dataSize);
   struct ps5vk_memory_copy *const copy =
      util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
   if (!copy) {
      free(snapshot);
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record a buffer update");
      return;
   }
   *copy = (struct ps5vk_memory_copy){
      .source = (uint64_t)(uintptr_t)snapshot,
      .destination = buffer->vk.device_address + dstOffset,
      .bytes = dataSize,
      .owned_source = snapshot,
      .after_words = (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t),
   };
}

void
ps5vk_debug_push_constants(VkDevice _device, const void **block, uint32_t *bytes,
                           const uint32_t **descriptor)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   if (block != NULL)
      *block = device != NULL ? device->push_constant_block : NULL;
   if (bytes != NULL)
      *bytes = device != NULL ? device->push_constant_bytes : 0;
   if (descriptor != NULL)
      *descriptor = device != NULL ? device->push_constant_descriptor : NULL;
}

void
ps5vk_debug_push_constant_user_data(VkDevice _device, uint32_t *stage, uint32_t *dword,
                                    uint32_t *low, uint32_t *high)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   const bool written = device != NULL && device->push_constant_user_data_dword != UINT32_MAX;
   if (stage != NULL)
      *stage = written ? device->push_constant_user_data_stage : 0;
   if (dword != NULL)
      *dword = written ? device->push_constant_user_data_dword : UINT32_MAX;
   if (low != NULL)
      *low = written ? device->push_constant_user_data_low : 0;
   if (high != NULL)
      *high = written ? device->push_constant_user_data_high : 0;
}

uint32_t
ps5vk_debug_descriptor_tables(VkDevice _device, ps5vk_debug_table *tables, uint32_t capacity)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   const uint32_t count = device != NULL ? device->descriptor_table_count : 0;
   if (tables == NULL || capacity == 0)
      return count;
   const uint32_t copied = MIN2(count, capacity);
   for (uint32_t at = 0; at < copied; at++)
      tables[at] = device->descriptor_tables[at];
   return count;
}

uint32_t
ps5vk_debug_colour_targets(VkDevice _device, ps5vk_debug_target *targets, uint32_t capacity)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   const uint32_t count = device != NULL ? device->target_attachment_count : 0;
   if (device == NULL || targets == NULL || capacity == 0)
      return count;
   const uint32_t copied = MIN2(count, capacity);
   for (uint32_t at = 0; at < copied; at++)
      targets[at] = (ps5vk_debug_target){
         .index = at,
         .base_offset = device->target_base_offsets[at],
         .base_value = device->target_base_values[at],
      };
   return count;
}

uint32_t
ps5vk_debug_table_chunks(VkDevice _device, ps5vk_debug_stage *chunks, uint32_t capacity)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   uint32_t count = 0;
   for (const struct ps5vk_table_chunk *chunk = device ? device->table_chunks : NULL;
        chunk != NULL; chunk = chunk->next_in_device)
      count++;
   if (chunks == NULL || capacity == 0)
      return count;
   uint32_t at = 0;
   for (const struct ps5vk_table_chunk *chunk = device ? device->table_chunks : NULL;
        chunk != NULL && at < capacity; chunk = chunk->next_in_device) {
      chunks[at++] = (ps5vk_debug_stage){
         .address = chunk->mapping.address,
         .bytes = chunk->mapping.bytes,
      };
   }
   return count;
}
