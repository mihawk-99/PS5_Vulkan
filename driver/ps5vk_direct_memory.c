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

/* R86, test-only (ps5vk_debug_device_memory_base): where VkDeviceMemory is
 * placed instead of the address window, or 0 for the window. Each mapping asks
 * for the next address after the last one's, and how many landed outside the
 * window since the base was set is counted for the probe to assert. */
static uint64_t ps5vk_device_memory_base;
static uint64_t ps5vk_device_memory_next;
static uint64_t ps5vk_device_memory_outside;

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

   void *address = NULL;
   const uint64_t base = kind == PS5VK_DIRECT_MEMORY ? p_atomic_read(&ps5vk_device_memory_base) : 0;
   if (base != 0) {
      const uint64_t step = align64(bytes, UINT64_C(0x200000));
      address = (void *)(uintptr_t)(base + p_atomic_add_return(&ps5vk_device_memory_next, step) - step);
   }
   result = sceKernelMapDirectMemory(&address, bytes, PS5VK_MAP_PROTECTION, 0, start, alignment);
   if (result == 0 && address) {
      mapping->address = address;
      if (ps5vk_address_range_valid((uint64_t)(uintptr_t)address, bytes))
         return 0;
      if (base != 0 && ps5vk_gpu_range_valid((uint64_t)(uintptr_t)address, bytes)) {
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
