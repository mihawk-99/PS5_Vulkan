/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PS5_GPU_STAGING_H
#define PS5_GPU_STAGING_H
#include "radv_ps5_platform.h"
#include <string.h>

/* The broker's spare staging buffers: owner-private direct memory a submission's command words are copied into, kept
 * once the fence has passed the submission that used it and taken again by a later one, with no kernel call.
 *
 * A 2026-10-16 run: the broker made one staging buffer per submission and freed it once its fence passed, about
 * 420 of each a second (fences 6842 to 8100 from flight-recorder t=30 to t=33). At t=34 one of those frees (phase
 * reap-free) did not return for 15 s with every fence done (8125/8125), in the seconds the game's helper held Wine's
 * virtual mutex and its direct-memory lock inside sceKernelMprotect of direct memory.
 *
 * Contract (the caller, tooling/gpu-broker/server.cpp, makes and frees the memory; this is bookkeeping only):
 *   - Only a buffer no submission in flight can use is given back: its fence done, or never submitted.
 *   - A buffer is made ps5_gpu_staging_round(need) bytes (radv_ps5_memory_alloc keeps a size that is a multiple of its
 *     16 KiB page), and a submission takes only a spare of exactly that size. So a buffer is made only when every one
 *     of its size is in flight (per size, never more than were once in flight together), and a small submission never
 *     holds a large buffer. Taking the smallest spare that fits instead let small submissions hold the large ones, so
 *     the next large submission made another: in PS5_Proton's host model (tests/test_pw_gpu_staging.c) the spares
 *     grew to about 262 MiB in 10 minutes and 279 MiB in an hour, 14 of them 18 MiB, held until the broker stopped.
 *   - The spares hold at most PS5_GPU_STAGING_BUDGET bytes: give refuses a buffer past it, and the caller frees it.
 *   - Otherwise spares are freed only where the broker gives its fence page back (fences_reset). */
enum { PS5_GPU_STAGING_SPARES = 512 };
/* The smallest buffer: about 4 KiB a submission on average in a 2026-10-16 run (the helper's radv/ps5 line: 2350
 * submissions, 2442 kwords). Powers of two up to 2 MiB, then 2 MiB steps (direct memory is 2 MiB-aligned from there). */
#define PS5_GPU_STAGING_MIN UINT64_C(0x10000)
#define PS5_GPU_STAGING_LARGE UINT64_C(0x200000)
/* 32 MiB: the fence ring's 512 at the smallest size, or one buffer for the largest submission (16 MiB and the fence
 * packet, 18 MiB) and 14 MiB of others. The host model's spares held 25.3 MiB after 10 minutes and 26.8 MiB after an
 * hour (51 and 53 buffers, one of them 18 MiB), none refused. */
#define PS5_GPU_STAGING_BUDGET (PS5_GPU_STAGING_SPARES * PS5_GPU_STAGING_MIN)
struct ps5_gpu_staging {
    struct radv_ps5_memory spare[PS5_GPU_STAGING_SPARES];
    unsigned count;
    uint64_t bytes;
};

static inline uint64_t ps5_gpu_staging_round(uint64_t need)
{
    if (need > PS5_GPU_STAGING_LARGE)
        return (need + PS5_GPU_STAGING_LARGE - 1) & ~(PS5_GPU_STAGING_LARGE - 1);
    uint64_t bytes = PS5_GPU_STAGING_MIN;
    while (bytes < need)
        bytes <<= 1;
    return bytes;
}

/* A spare of need's rounded size, taken out into *out: 1. None: 0 (*out zeroed), and the caller makes one. */
static inline int ps5_gpu_staging_take(struct ps5_gpu_staging *pool, uint64_t need, struct radv_ps5_memory *out)
{
    const uint64_t bytes = ps5_gpu_staging_round(need);
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < pool->count; ++i) {
        if (pool->spare[i].bytes != bytes)
            continue;
        *out = pool->spare[i];
        pool->bytes -= bytes;
        pool->spare[i] = pool->spare[--pool->count];
        memset(&pool->spare[pool->count], 0, sizeof(pool->spare[0]));
        return 1;
    }
    return 0;
}

/* A buffer no submission can use any more, kept: 1. 0 when keeping it would pass the budget; the caller frees it. */
static inline int ps5_gpu_staging_give(struct ps5_gpu_staging *pool, struct radv_ps5_memory memory)
{
    if (pool->count == PS5_GPU_STAGING_SPARES || pool->bytes + memory.bytes > PS5_GPU_STAGING_BUDGET)
        return 0;
    pool->spare[pool->count++] = memory;
    pool->bytes += memory.bytes;
    return 1;
}
#endif
