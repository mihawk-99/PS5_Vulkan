/*
 * PS5 Vulkan driver - GPU-visible direct memory.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B7 (docs/M5_PHASE_B.md). Every GPU-visible allocation of
 * the driver is direct memory as the test runner allocates it: VkDeviceMemory
 * (ps5vk_memory.c), the queue's submission buffer (ps5vk_queue.c), pipeline
 * stage workspaces (ps5vk_pipeline.c) and command buffers' register tables
 * (ps5vk_cmd_buffer.c). Each is type 12, mapped for CPU and GPU read and
 * write at one address. What shaders reach through 32-bit pointers must lie
 * in the address window, where the kernel puts a mapping asked for with no
 * address. VkDeviceMemory goes to the device memory region instead (R88), so
 * the heap is the whole direct-memory pool rather than the window's 4 GiB.
 */

#include "ps5vk_private.h"

#include <assert.h>
#include <immintrin.h>
#include <pthread.h>

#include "util/log.h"
#include "util/u_atomic.h"

/* The direct memory the driver holds, mappings and bytes: the profile reports
 * both every ten seconds, so an allocation that is never released shows as a
 * count that only grows over a long run. */
static uint64_t ps5vk_direct_live_count;
static uint64_t ps5vk_direct_live_bytes;
static uint64_t ps5vk_direct_kind_count[PS5VK_DIRECT_KIND_COUNT];
static uint64_t ps5vk_direct_kind_bytes[PS5VK_DIRECT_KIND_COUNT];

static const char *const ps5vk_direct_kind_names[PS5VK_DIRECT_KIND_COUNT] = {
   [PS5VK_DIRECT_MEMORY] = "memory", [PS5VK_DIRECT_STAGE] = "stage",
   [PS5VK_DIRECT_COMPUTE] = "compute", [PS5VK_DIRECT_TABLES] = "tables",
   [PS5VK_DIRECT_QUERY] = "query", [PS5VK_DIRECT_QUEUE] = "queue",
   [PS5VK_DIRECT_DISPLAY] = "display",
};

void
ps5vk_direct_memory_live(uint64_t *count, uint64_t *bytes)
{
   *count = p_atomic_read(&ps5vk_direct_live_count);
   *bytes = p_atomic_read(&ps5vk_direct_live_bytes);
}

void
ps5vk_direct_memory_kind_live(enum ps5vk_direct_kind kind, uint64_t *count, uint64_t *bytes)
{
   *count = p_atomic_read(&ps5vk_direct_kind_count[kind]);
   *bytes = p_atomic_read(&ps5vk_direct_kind_bytes[kind]);
}

const char *
ps5vk_direct_memory_kinds(char *out, size_t size)
{
   size_t used = 0;
   out[0] = '\0';
   for (unsigned kind = 0; kind < PS5VK_DIRECT_KIND_COUNT && used < size; kind++) {
      const int written =
         snprintf(out + used, size - used, "%s%s:%" PRIu64, kind == 0 ? "" : ",",
                  ps5vk_direct_kind_names[kind], p_atomic_read(&ps5vk_direct_kind_count[kind]));
      if (written < 0)
         break;
      used += (size_t)written;
   }
   return out;
}

/* R86, test-only (ps5vk_debug_device_memory_base): a base VkDeviceMemory is
 * placed from instead of the device memory region, or 0 for the region. Each
 * mapping asks for the next address after the last one's. How many
 * VkDeviceMemory mappings landed outside the window since the last call is
 * counted for a probe to assert. */
static uint64_t ps5vk_device_memory_base;
static uint64_t ps5vk_device_memory_next;
static uint64_t ps5vk_device_memory_outside;

/* R88: the device memory region's granules, a bit each, set while a mapping
 * holds them. First fit, so freed ranges are used again and a long session
 * does not walk through the region. The kernel has the last word on where a
 * mapping goes: one it puts elsewhere gives its granules back at once. */
#define PS5VK_REGION_GRANULES                                                                      \
   ((uint32_t)(PS5VK_DEVICE_MEMORY_REGION_BYTES / PS5VK_DEVICE_MEMORY_GRANULE))
static uint64_t ps5vk_region_used[PS5VK_REGION_GRANULES / 64];
static pthread_mutex_t ps5vk_region_lock = PTHREAD_MUTEX_INITIALIZER;

static void
ps5vk_region_mark(uint32_t first, uint32_t count, bool used)
{
   for (uint32_t granule = first; granule < first + count; granule++) {
      const uint64_t bit = UINT64_C(1) << (granule % 64);
      if (used)
         ps5vk_region_used[granule / 64] |= bit;
      else
         ps5vk_region_used[granule / 64] &= ~bit;
   }
}

/* count free granules in a row, taken; UINT32_MAX when the region has none. */
static uint32_t
ps5vk_region_take(uint32_t count)
{
   if (count == 0 || count > PS5VK_REGION_GRANULES)
      return UINT32_MAX;
   pthread_mutex_lock(&ps5vk_region_lock);
   uint32_t run = 0;
   for (uint32_t granule = 0; granule < PS5VK_REGION_GRANULES; granule++) {
      if (granule % 64 == 0 && ps5vk_region_used[granule / 64] == UINT64_MAX) {
         granule += 63;
         run = 0;
         continue;
      }
      if (ps5vk_region_used[granule / 64] & (UINT64_C(1) << (granule % 64))) {
         run = 0;
         continue;
      }
      if (++run == count) {
         const uint32_t first = granule + 1 - count;
         ps5vk_region_mark(first, count, true);
         pthread_mutex_unlock(&ps5vk_region_lock);
         return first;
      }
   }
   pthread_mutex_unlock(&ps5vk_region_lock);
   return UINT32_MAX;
}

static void
ps5vk_region_give(struct ps5vk_direct_mapping *mapping)
{
   if (mapping->granules == 0)
      return;
   pthread_mutex_lock(&ps5vk_region_lock);
   ps5vk_region_mark(mapping->granule, mapping->granules, false);
   pthread_mutex_unlock(&ps5vk_region_lock);
   mapping->granules = 0;
}

/* Where a new mapping of this kind asks to go: the test switch's next address,
 * the device memory region's first fit for VkDeviceMemory, or NULL -- the
 * kernel's own choice, the window -- for everything else. */
static void *
ps5vk_direct_mapping_hint(struct ps5vk_direct_mapping *mapping)
{
   if (mapping->kind != PS5VK_DIRECT_MEMORY)
      return NULL;
   const uint64_t step = align64(mapping->bytes, PS5VK_DEVICE_MEMORY_GRANULE);
   const uint64_t base = p_atomic_read(&ps5vk_device_memory_base);
   if (base != 0)
      return (void *)(uintptr_t)(base + p_atomic_add_return(&ps5vk_device_memory_next, step) - step);
   const uint32_t count = (uint32_t)(step / PS5VK_DEVICE_MEMORY_GRANULE);
   const uint32_t first = ps5vk_region_take(count);
   if (first == UINT32_MAX)
      return NULL;
   mapping->granule = first;
   mapping->granules = count;
   return (void *)(PS5VK_DEVICE_MEMORY_REGION + (uintptr_t)first * PS5VK_DEVICE_MEMORY_GRANULE);
}

uint64_t
ps5vk_debug_device_memory_base(uint64_t base)
{
   p_atomic_set(&ps5vk_device_memory_next, 0);
   p_atomic_set(&ps5vk_device_memory_base, base);
   return p_atomic_xchg(&ps5vk_device_memory_outside, 0);
}

int32_t
ps5vk_direct_mapping_create(struct ps5vk_direct_mapping *mapping, size_t bytes, size_t alignment,
                            enum ps5vk_direct_kind kind)
{
   assert(bytes != 0 && bytes % PS5VK_DIRECT_PAGE_BYTES == 0);
   assert(kind < PS5VK_DIRECT_KIND_COUNT);
   *mapping =
      (struct ps5vk_direct_mapping){.start = -1, .bytes = bytes, .address = NULL, .kind = kind};

   int64_t start = -1;
   int32_t result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes,
                                                  alignment, PS5VK_DIRECT_MEMORY_TYPE, &start);
   if (result != 0)
      return result;
   mapping->start = start;
   p_atomic_inc(&ps5vk_direct_live_count);
   p_atomic_add(&ps5vk_direct_live_bytes, (uint64_t)bytes);
   p_atomic_inc(&ps5vk_direct_kind_count[kind]);
   p_atomic_add(&ps5vk_direct_kind_bytes[kind], (uint64_t)bytes);

   void *const hint = ps5vk_direct_mapping_hint(mapping);
   void *address = hint;
   result = sceKernelMapDirectMemory(&address, bytes, PS5VK_MAP_PROTECTION, 0, start, alignment);
   if (result != 0 && hint != NULL) {
      /* An address the kernel would not map at: its own choice instead. */
      ps5vk_region_give(mapping);
      address = NULL;
      result = sceKernelMapDirectMemory(&address, bytes, PS5VK_MAP_PROTECTION, 0, start, alignment);
   }
   if (result == 0 && address) {
      mapping->address = address;
      if (address != hint)
         ps5vk_region_give(mapping);
      if (ps5vk_address_range_valid((uint64_t)(uintptr_t)address, bytes))
         return 0;
      if (kind == PS5VK_DIRECT_MEMORY &&
          ps5vk_gpu_range_valid((uint64_t)(uintptr_t)address, bytes)) {
         p_atomic_inc(&ps5vk_device_memory_outside);
         return 0;
      }
   }
   if (result == 0)
      result = PS5VK_DIRECT_OUTSIDE_WINDOW;
   ps5vk_direct_mapping_destroy(mapping);
   return result;
}

void
ps5vk_direct_mapping_destroy(struct ps5vk_direct_mapping *mapping)
{
   if (mapping->address) {
      const int32_t result = sceKernelMunmap(mapping->address, mapping->bytes);
      if (result != 0)
         mesa_loge("sceKernelMunmap(%p, %zu bytes) failed: 0x%08x", mapping->address,
                   mapping->bytes, (unsigned)result);
   }
   /* A mapping that was never created -- a zeroed one, such as a graphics
    * pipeline's compute code -- holds no bytes, and there is nothing to release
    * or to count. */
   if (mapping->start >= 0 && mapping->bytes != 0) {
      const int32_t result = sceKernelReleaseDirectMemory(mapping->start, mapping->bytes);
      if (result != 0)
         mesa_loge("sceKernelReleaseDirectMemory(0x%" PRIx64 ", %zu bytes) failed: 0x%08x",
                   (uint64_t)mapping->start, mapping->bytes, (unsigned)result);
      p_atomic_dec(&ps5vk_direct_live_count);
      p_atomic_add(&ps5vk_direct_live_bytes, -(int64_t)mapping->bytes);
      p_atomic_dec(&ps5vk_direct_kind_count[mapping->kind]);
      p_atomic_add(&ps5vk_direct_kind_bytes[mapping->kind], -(int64_t)mapping->bytes);
   }
   ps5vk_region_give(mapping);
   mapping->start = -1;
   mapping->address = NULL;
}

void
ps5vk_evict_cpu_lines(const void *address, size_t bytes)
{
   const uintptr_t first = (uintptr_t)address & ~(uintptr_t)63;
   const uintptr_t end = (uintptr_t)address + bytes;
   for (uintptr_t at = first; at < end; at += 64)
      _mm_clflush((const void *)at);
}

void
ps5vk_cpu_fence(void)
{
   _mm_mfence();
}

void
ps5vk_flush_cpu_cache(const void *address, size_t bytes)
{
   const char *const begin = address;
   for (const char *at = begin; at < begin + bytes; at += 64)
      _mm_clflush(at);
   _mm_mfence();
}
