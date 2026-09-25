/*
 * PS5 Vulkan driver - occlusion queries.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Vulkan 1.0 requires occlusion queries, and this GPU has one counter for
 * them: GFX10's ZPASS_DONE, an EVENT_WRITE of the z-pass counter to an
 * address. ps5-opengl's runtime emits exactly that packet -- 0x46 with event
 * 0x115, then the address (ps5_agc_emit_occlusion_sample,
 * src/platform/ps5_agc_runtime_backend.c) -- and the console measured what the
 * counter counts: regions of a 4K target covering none, half and all of it read
 * 0, 259,200 and 518,400 counts for 0, 4,147,200 and 8,294,400 samples, so one
 * count is sixteen samples, and the hardware sets bit 63 of every counter it
 * writes (docs/M5_PHASE_C.md, V0-query; the bit ps5-opengl calls
 * PS5_OCCLUSION_VALID_BIT).
 *
 * A pool is one mapping the GPU writes and the CPU reads: two eight-byte
 * counters per query, the first sampled where vkCmdBeginQuery is recorded and
 * the second where vkCmdEndQuery is. vkGetQueryPoolResults subtracts them and
 * multiplies by sixteen, so a query answers in samples. Submission is
 * synchronous (ps5vk_queue.c), so a query's counters have been written by the
 * time vkQueueSubmit returns, and the sample registers the pool with the
 * command buffer so that submission evicts the counters' cache lines before the
 * CPU reads them.
 *
 * The count is coarse: one count covers sixteen samples, so a result is always
 * a multiple of sixteen. The device therefore reports occlusionQueryPrecise
 * false (ps5vk_physical_device.c); what a query returns is exact for any region
 * whose samples are a multiple of sixteen, which is every region a whole
 * fragment covers.
 */

#include "ps5vk_private.h"

#include <assert.h>
#include <string.h>

/* The counters of one query, and the packet that samples one of them. */
#define PS5VK_QUERY_COUNTERS 2
#define PS5VK_QUERY_BYTES (PS5VK_QUERY_COUNTERS * sizeof(uint64_t))
#define PS5VK_OCCLUSION_EVENT_WORDS 4
#define PS5VK_OCCLUSION_PACKET UINT32_C(0xc0024600)
#define PS5VK_OCCLUSION_EVENT UINT32_C(0x00000115)

/* The timestamp packet, measured on the console by V0-query's timestamp probe
 * (docs/M5_PHASE_C.md, pid 143): a RELEASE_MEM whose EOP selector names the
 * timestamp, which is how radv writes one (radv_write_timestamp). The op word
 * is the completion marker's -- event 40 (BOTTOM_OF_PIPE_TS), event index 5 and
 * the cache actions -- and the selector is EOP_DST_SEL(MEM) |
 * EOP_INT_SEL(SEND_DATA_AFTER_WR_CONFIRM) | EOP_DATA_SEL(TIMESTAMP) in Mesa's
 * encoding (sid.h). The packet is eight words: a RELEASE_MEM of seven is
 * refused by the runner's own framing check. The clock is about 100 MHz. */
#define PS5VK_TIMESTAMP_PACKET UINT32_C(0xc0064900)
#define PS5VK_TIMESTAMP_EVENT UINT32_C(0x0030c528)
#define PS5VK_TIMESTAMP_SELECT UINT32_C(0x63000000)
/* The console measured 99,786,056 ticks a second against CLOCK_MONOTONIC; the
 * reference clock is 100 MHz, and the difference is the submit and marker
 * latency the wall-clock interval carries (docs/HARDWARE_FINDINGS.md). */
#define PS5VK_TIMESTAMP_PERIOD_NS 10.0f

/* One count per this many samples, measured on the console. */
#define PS5VK_OCCLUSION_SAMPLES_PER_COUNT UINT64_C(16)

/* The address of one of a query's counters. */
static uint64_t
ps5vk_query_counter(const struct ps5vk_query_pool *pool, uint32_t query, uint32_t counter)
{
   return (uint64_t)(uintptr_t)pool->mapping.address + (uint64_t)query * PS5VK_QUERY_BYTES +
          (uint64_t)counter * sizeof(uint64_t);
}

/* Appends one occlusion sample of a query's counter to a command buffer, and
 * registers the pool with it as a target: submission evicts a target's cache
 * lines once the GPU has run, which is what makes the counters readable
 * (ps5vk_queue.c, ps5vk_queue_flush_targets). */
static void
ps5vk_cmd_buffer_occlusion_sample(struct ps5vk_cmd_buffer *cmd_buffer,
                                  struct ps5vk_query_pool *pool, uint32_t query, uint32_t counter)
{
   const uint64_t address = ps5vk_query_counter(pool, query, counter);
   const uint32_t words[PS5VK_OCCLUSION_EVENT_WORDS] = {
      PS5VK_OCCLUSION_PACKET,
      PS5VK_OCCLUSION_EVENT,
      (uint32_t)(address & UINT32_MAX),
      (uint32_t)(address >> 32),
   };
   uint32_t *const recorded =
      util_dynarray_grow(&cmd_buffer->words, uint32_t, PS5VK_OCCLUSION_EVENT_WORDS);
   memcpy(recorded, words, sizeof(words));

   const struct ps5vk_render_target target = {
      .always = true,
      .address = pool->mapping.address,
      .bytes = pool->mapping.bytes,
      .video = -1,
      .buffer_index = 0,
   };
   util_dynarray_append(&cmd_buffer->targets, target);
}

void
ps5vk_timestamp_packet(uint32_t *words, uint64_t address)
{
   const uint32_t packet[PS5VK_TIMESTAMP_EVENT_WORDS] = {
      PS5VK_TIMESTAMP_PACKET,
      PS5VK_TIMESTAMP_EVENT,
      PS5VK_TIMESTAMP_SELECT,
      (uint32_t)(address & UINT32_MAX),
      (uint32_t)(address >> 32),
      0,
      0,
      0,
   };
   memcpy(words, packet, sizeof(packet));
}

/* Appends one timestamp write of a query's counter to a command buffer, and
 * registers the pool with it as a target the way the occlusion sample does:
 * submission evicts the counter's cache line once the GPU has run, which is
 * what makes the clock readable. */
static void
ps5vk_cmd_buffer_timestamp_sample(struct ps5vk_cmd_buffer *cmd_buffer,
                                  struct ps5vk_query_pool *pool, uint32_t query)
{
   uint32_t *const recorded =
      util_dynarray_grow(&cmd_buffer->words, uint32_t, PS5VK_TIMESTAMP_EVENT_WORDS);
   ps5vk_timestamp_packet(recorded, ps5vk_query_counter(pool, query, 0));

   const struct ps5vk_render_target target = {
      .always = true,
      .address = pool->mapping.address,
      .bytes = pool->mapping.bytes,
      .video = -1,
      .buffer_index = 0,
   };
   util_dynarray_append(&cmd_buffer->targets, target);
}

/* vkCmdWriteTimestamp: the GPU writes its clock into the query's counter at
 * this point in the command order. The pool has to be a timestamp one; Vulkan
 * requires the queue family to support timestamps, which this one does
 * (timestampValidBits 64, ps5vk_physical_device.c). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdWriteTimestamp(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage,
                        VkQueryPool queryPool, uint32_t query)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   (void)pipelineStage;
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   if (pool == NULL || pool->type != VK_QUERY_TYPE_TIMESTAMP || query >= pool->count) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "vkCmdWriteTimestamp names a timestamp query pool and a query inside "
                              "it; this is %s with query %u of %u",
                              pool == NULL ? "no pool this driver created"
                              : pool->type != VK_QUERY_TYPE_TIMESTAMP
                                 ? "a pool that is not a timestamp one"
                                 : "a query past the pool's end",
                              query, pool != NULL ? pool->count : 0u);
      return;
   }
   ps5vk_cmd_buffer_timestamp_sample(cmd_buffer, pool, query);
}

/* Zeroes a range of counters and writes them back for the GPU to update. */
static void
ps5vk_query_pool_reset(struct ps5vk_query_pool *pool, uint32_t first, uint32_t count)
{
   assert(first + count <= pool->count);
   uint8_t *const first_counter = (uint8_t *)pool->mapping.address + (size_t)first * PS5VK_QUERY_BYTES;
   const size_t bytes = (size_t)count * PS5VK_QUERY_BYTES;
   memset(first_counter, 0, bytes);
   ps5vk_flush_cpu_cache(first_counter, bytes);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateQueryPool(VkDevice _device, const VkQueryPoolCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* Two query sources are recorded on this hardware: the z-pass counter
    * (V0-query) and the GPU clock (V0-query's timestamp probe, pid 143).
    * Pipelines and transform feedback have no probe. */
   if (pCreateInfo->queryType != VK_QUERY_TYPE_OCCLUSION &&
       pCreateInfo->queryType != VK_QUERY_TYPE_TIMESTAMP)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "query type %d is neither the occlusion query the z-pass counter answers "
                       "nor the timestamp one the GPU clock answers (docs/M5_REFERENCE.md, "
                       "V0-query)", (int)pCreateInfo->queryType);
   assert(pCreateInfo->queryCount > 0);
   /* A timestamp query uses the first of its two counters; the stride is the
    * same so one address arithmetic serves both pool types. */
   const size_t bytes = (size_t)pCreateInfo->queryCount * PS5VK_QUERY_BYTES;
   const size_t mapping_bytes = (size_t)ALIGN_POT(bytes, PS5VK_DIRECT_PAGE_BYTES);

   struct ps5vk_query_pool *const pool =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*pool), VK_OBJECT_TYPE_QUERY_POOL);
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   pool->type = pCreateInfo->queryType;
   pool->count = pCreateInfo->queryCount;
   const int32_t mapped =
      ps5vk_direct_mapping_create(&pool->mapping, mapping_bytes, PS5VK_DIRECT_PAGE_BYTES);
   if (mapped != 0) {
      vk_object_free(&device->vk, pAllocator, pool);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "the query counters could not be mapped in the address window: 0x%08x",
                       (unsigned)mapped);
   }
   /* Zero is what a query that never sampled reads, and the GPU updates the
    * counters in place. */
   ps5vk_query_pool_reset(pool, 0, pool->count);
   *pQueryPool = ps5vk_query_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyQueryPool(VkDevice _device, VkQueryPool queryPool,
                       const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   if (!pool)
      return;
   ps5vk_direct_mapping_destroy(&pool->mapping);
   vk_object_free(&device->vk, pAllocator, pool);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_ResetQueryPool(VkDevice _device, VkQueryPool queryPool, uint32_t firstQuery,
                     uint32_t queryCount)
{
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   (void)_device;
   if (pool)
      ps5vk_query_pool_reset(pool, firstQuery, queryCount);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery,
                        uint32_t queryCount)
{
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   (void)commandBuffer;
   /* Zeroing the counters is CPU work with no GPU order to keep: a command
    * buffer's samples write the counters when they run, and the reset is what
    * a later submission reads them from (Vulkan orders the reset before the
    * samples of the command buffer it is recorded in). */
   if (pool)
      ps5vk_query_pool_reset(pool, firstQuery, queryCount);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query,
                    VkQueryControlFlags flags)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   assert(cmd_buffer && pool && query < pool->count);
   /* VK_QUERY_CONTROL_PRECISE_BIT asks for a bit-exact count, which a counter
    * that ticks once per sixteen samples cannot give; the results are still
    * what a non-precise query promises, so the flag is accepted and the
    * device's occlusionQueryPrecise is false. */
   (void)flags;
   ps5vk_cmd_buffer_occlusion_sample(cmd_buffer, pool, query, 0);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   assert(cmd_buffer && pool && query < pool->count);
   ps5vk_cmd_buffer_occlusion_sample(cmd_buffer, pool, query, 1);
}

/* The debug API's view of a pool: where its counters are (ps5vk_debug.h). */
void *
ps5vk_debug_query_pool_storage(VkQueryPool queryPool, size_t *bytes)
{
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   if (!pool) {
      *bytes = 0;
      return NULL;
   }
   *bytes = pool->mapping.bytes;
   return pool->mapping.address;
}

/* The result one query's counters hold: for an occlusion query the difference
 * of its two counters, one count per sixteen samples; for a timestamp query the
 * clock the GPU wrote into its first counter. vkGetQueryPoolResults and a
 * query-result copy both use this. */
static uint64_t
ps5vk_query_result(const struct ps5vk_query_pool *pool, uint32_t query)
{
   const uint64_t *const counters = (const uint64_t *)pool->mapping.address;
   const uint64_t *const pair = counters + (size_t)query * PS5VK_QUERY_COUNTERS;
   if (pool->type == VK_QUERY_TYPE_TIMESTAMP)
      return pair[0];
   return (pair[1] - pair[0]) * PS5VK_OCCLUSION_SAMPLES_PER_COUNT;
}

/* Whether a query's result has been written. An occlusion pair is meaningful as
 * soon as its samples have run -- submission is synchronous, so by the time a
 * result is asked for they have -- while a timestamp's counter is zero until
 * the GPU writes the clock into it, which is what tells a query that ran and
 * read zero apart from one no command ever wrote. */
static uint64_t
ps5vk_query_available(const struct ps5vk_query_pool *pool, uint32_t query)
{
   if (pool->type != VK_QUERY_TYPE_TIMESTAMP)
      return 1;
   const uint64_t *const counters = (const uint64_t *)pool->mapping.address;
   return counters[(size_t)query * PS5VK_QUERY_COUNTERS] != 0 ? 1u : 0u;
}

/* One recorded query-result copy, run at its split point (ps5vk_queue.c): the
 * GPU has finished the words before the split, so the pool's counters hold the
 * results the copy writes into the destination buffer. The layout is Vulkan's:
 * stride bytes between results, 32 or 64 bits each as VK_QUERY_RESULT_64_BIT
 * asks, and an availability value after a result when
 * VK_QUERY_RESULT_WITH_AVAILABILITY_BIT asks for one. */
void
ps5vk_query_execute(const struct ps5vk_memory_copy *copy)
{
   const struct ps5vk_query_pool *const pool = copy->query_pool;
   const bool wide = (copy->query_flags & VK_QUERY_RESULT_64_BIT) != 0;
   const bool available = (copy->query_flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   ps5vk_flush_cpu_cache(pool->mapping.address, pool->mapping.bytes);
   for (uint32_t index = 0; index < copy->query_count; index++) {
      const uint64_t samples = ps5vk_query_result(pool, copy->query_first + index);
      const uint64_t written = ps5vk_query_available(pool, copy->query_first + index);
      uint8_t *const result =
         (uint8_t *)(uintptr_t)(copy->destination + (uint64_t)index * copy->query_stride);
      if (wide) {
         const uint64_t values[2] = {samples, written};
         memcpy(result, values, available ? sizeof(values) : sizeof(values[0]));
      } else {
         const uint32_t values[2] = {(uint32_t)samples, (uint32_t)written};
         memcpy(result, values, available ? sizeof(values) : sizeof(values[0]));
      }
      ps5vk_flush_cpu_cache(result, available ? (wide ? 16u : 8u) : (wide ? 8u : 4u));
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetQueryPoolResults(VkDevice _device, VkQueryPool queryPool, uint32_t firstQuery,
                          uint32_t queryCount, size_t dataSize, void *pData, VkDeviceSize stride,
                          VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   /* The engine reads the frame's timestamps here, at the top of the frame's
    * setup, so this is where the recording stretch starts (R31, default off). */
   struct ps5vk_queue *const queue = ps5vk_device_profile_queue(device);
   if (queue)
      ps5vk_profile_enter(queue, PS5VK_PROFILE_AFTER_QUERY);
   if (!pool || queryCount == 0)
      goto out;
   assert(firstQuery + queryCount <= pool->count);
   const bool wide = (flags & VK_QUERY_RESULT_64_BIT) != 0;
   const bool available = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   (void)dataSize;
   /* Submission is synchronous, so every sample the command buffers recorded
    * has run by the time this is asked: the results are available -- Vulkan's
    * VK_QUERY_RESULT_WAIT_BIT asks for exactly that and needs no wait here. */
   ps5vk_flush_cpu_cache(pool->mapping.address, pool->mapping.bytes);
   for (uint32_t index = 0; index < queryCount; index++) {
      /* Both counters carry the hardware's valid bit, so the difference is the
       * counts the region added; one count is sixteen samples. */
      const uint64_t samples = ps5vk_query_result(pool, firstQuery + index);
      const uint64_t written = ps5vk_query_available(pool, firstQuery + index);
      uint8_t *const result = (uint8_t *)pData + (size_t)index * stride;
      if (wide) {
         const uint64_t values[2] = {samples, written};
         memcpy(result, values, available ? sizeof(values) : sizeof(values[0]));
      } else {
         const uint32_t values[2] = {(uint32_t)samples, (uint32_t)written};
         memcpy(result, values, available ? sizeof(values) : sizeof(values[0]));
      }
   }
out:
   if (queue)
      ps5vk_profile_leave(queue, PS5VK_PROFILE_AFTER_QUERY);
   return VK_SUCCESS;
}

/* vkCmdCopyQueryPoolResults: the same results, written into the application's
 * buffer at the point in the command order the command was recorded at. The work
 * is the CPU's -- a query's counters are memory the GPU wrote -- so it is
 * recorded as a copy at a split point, exactly like a fill, an update or an
 * image clear (ps5vk_queue.c). The pool joins the submission's targets so the
 * queue evicts its counters' cache lines once the GPU has run. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdCopyQueryPoolResults(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                              uint32_t firstQuery, uint32_t queryCount, VkBuffer dstBuffer,
                              VkDeviceSize dstOffset, VkDeviceSize stride,
                              VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_query_pool, pool, queryPool);
   VK_FROM_HANDLE(ps5vk_buffer, buffer, dstBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || queryCount == 0)
      return;
   /* Invalid handles are a refusal, not an assertion: the B2 device test
    * records every 1.0 command with the handles it has (none), which is how a
    * missing entry point or a crash in one shows up there. */
   if (pool == NULL || buffer == NULL || firstQuery + queryCount > pool->count) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a query-result copy names a query pool and a buffer; one of them "
                              "is not an object of this driver, or the queries are past the "
                              "pool's %u", pool != NULL ? pool->count : 0u);
      return;
   }
   if (buffer->vk.device_address == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a query-result copy names a buffer with no GPU address: it has to "
                              "be bound to memory");
      return;
   }
   const uint32_t result_bytes = ((flags & VK_QUERY_RESULT_64_BIT) != 0 ? 8u : 4u) *
                                 ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0 ? 2u : 1u);
   if (stride < result_bytes ||
       dstOffset + (VkDeviceSize)(queryCount - 1u) * stride + result_bytes > buffer->vk.size) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a query-result copy of %u results every %llu bytes from offset "
                              "%llu reaches past the buffer's %llu bytes",
                              queryCount, (unsigned long long)stride,
                              (unsigned long long)dstOffset,
                              (unsigned long long)buffer->vk.size);
      return;
   }
   /* The pool's counters are read after the GPU has run the words before the
    * split, and the submission evicts their cache lines (ps5vk_queue.c). */
   const struct ps5vk_render_target target = {
      .address = pool->mapping.address,
      .bytes = pool->mapping.bytes,
      .video = -1,
      .buffer_index = 0,
   };
   util_dynarray_append(&cmd_buffer->targets, target);
   struct ps5vk_memory_copy *const record =
      util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
   if (!record) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record a query-result copy");
      return;
   }
   *record = (struct ps5vk_memory_copy){
      .query = true,
      .query_pool = pool,
      .query_first = firstQuery,
      .query_count = queryCount,
      .query_stride = stride,
      .query_flags = (uint32_t)flags,
      .destination = buffer->vk.device_address + dstOffset,
      .after_words = (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t),
   };
}
