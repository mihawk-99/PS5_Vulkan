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
 * write at one address, and must lie inside the address window.
 */

#include "ps5vk_private.h"

#include <assert.h>
#include <immintrin.h>

#include "util/log.h"
#include "util/u_atomic.h"

/* The direct memory the driver holds, mappings and bytes: the profile reports
 * both every ten seconds, so an allocation that is never released shows as a
 * count that only grows over a long run. */
static uint64_t ps5vk_direct_live_count;
static uint64_t ps5vk_direct_live_bytes;

void
ps5vk_direct_memory_live(uint64_t *count, uint64_t *bytes)
{
   *count = p_atomic_read(&ps5vk_direct_live_count);
   *bytes = p_atomic_read(&ps5vk_direct_live_bytes);
}

int32_t
ps5vk_direct_mapping_create(struct ps5vk_direct_mapping *mapping, size_t bytes, size_t alignment)
{
   assert(bytes != 0 && bytes % PS5VK_DIRECT_PAGE_BYTES == 0);
   *mapping = (struct ps5vk_direct_mapping){.start = -1, .bytes = bytes, .address = NULL};

   int64_t start = -1;
   int32_t result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes,
                                                  alignment, PS5VK_DIRECT_MEMORY_TYPE, &start);
   if (result != 0)
      return result;
   mapping->start = start;
   p_atomic_inc(&ps5vk_direct_live_count);
   p_atomic_add(&ps5vk_direct_live_bytes, (uint64_t)bytes);

   void *address = NULL;
   result = sceKernelMapDirectMemory(&address, bytes, PS5VK_MAP_PROTECTION, 0, start, alignment);
   if (result == 0 && address) {
      mapping->address = address;
      if (ps5vk_address_range_valid((uint64_t)(uintptr_t)address, bytes))
         return 0;
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
   if (mapping->start >= 0) {
      const int32_t result = sceKernelReleaseDirectMemory(mapping->start, mapping->bytes);
      if (result != 0)
         mesa_loge("sceKernelReleaseDirectMemory(0x%" PRIx64 ", %zu bytes) failed: 0x%08x",
                   (uint64_t)mapping->start, mapping->bytes, (unsigned)result);
      p_atomic_dec(&ps5vk_direct_live_count);
      p_atomic_add(&ps5vk_direct_live_bytes, -(int64_t)mapping->bytes);
   }
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
