/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_GPU_BROKER_H
#define PW_GPU_BROKER_H
#include <stddef.h>
#include <stdint.h>
#define PW_GPU_MAGIC 0x47505233u
#define PW_GPU_LIMIT (UINT64_C(128)<<20)
/* The most host-visible memory an owner may grant a client (ps5_gpu_broker_set_host_limit); PW_GPU_LIMIT is the default.
 * An application chooses: a program that uploads large textures through staging memory needs more than the default
 * (PS5_Proton: a 32-bit game's 64 MiB uploads through Zink). */
#define PW_GPU_HOST_LIMIT_MAX (UINT64_C(4)<<30)
/* Device-local memory a client may be offered, and the largest alignment either kind may ask. */
#define PW_GPU_LOCAL_MAX (UINT64_C(16)<<30)
#define PW_GPU_ALIGNMENT_MAX (UINT64_C(2)<<20)
/* PW_GPU_ALLOC argument[2]: the owner's direct memory with no CPU mapping in the client, zeroed when asked. The INIT
 * reply carries the client's device-local and host-visible limits in argument[2] and argument[3]. */
#define PW_GPU_ALLOC_LOCAL UINT64_C(1)
#define PW_GPU_ALLOC_ZERO UINT64_C(2)
/* The address ranges host-visible client buffers come from: one in RADV's 32-bit shader window, one high, outside the
 * owner RADV's own device-memory region (0x4000000000, 256 GiB). The owner reserves both whole when its broker starts
 * and hands out ranges of them, reusing what is freed; a client reserves the same ranges before its runtime starts, so its own mappings never land where the owner places a buffer, and maps each buffer over
 * its reservation. */
#define PW_GPU_HOST_WINDOW_BASE UINT64_C(0x240000000)
#define PW_GPU_HOST_WINDOW_BYTES (UINT64_C(512)<<20)
/* 0x2000000000 is taken in the owning title; 0x1000000000, 0x3000000000, 0x3c00000000, 0x7c00000000 and 0x8000000000
 * could each be reserved for 16 GiB in the owner and in its helpers (graphics-child-gpu-ranges-02). */
#define PW_GPU_HOST_HIGH_BASE UINT64_C(0x3000000000)
#define PW_GPU_HOST_HIGH_BYTES (UINT64_C(16)<<30)
/* sceKernelReserveVirtualRange: exactly at the address, and never over anything already there. */
#define PW_GPU_RESERVE_FIXED 0x10
#define PW_GPU_RESERVE_NO_OVERWRITE 0x80
enum { PW_GPU_INIT=1, PW_GPU_ALLOC, PW_GPU_FREE, PW_GPU_SUBMIT,
       PW_GPU_TESS, PW_GPU_OFFCHIP, PW_GPU_DONE, PW_GPU_IMAGE, PW_GPU_PRESENT, PW_GPU_SWAPCHAIN, PW_GPU_WINDOW_SWAPCHAIN,
       PW_GPU_TIMING, PW_GPU_PRESENT_LINK };
/* PW_GPU_PRESENT_LINK: a second connection for this client's presents (SWAPCHAIN, WINDOW_SWAPCHAIN and TIMING, nothing
 * else), its socket in the reply. Each connection is one call at a time, so on a single one a present the display took
 * 6.4 ms to copy held the client's submissions behind it: 8.05 ms of round trips a frame for a Direct3D 12 game with the
 * asynchronous present (PS5_Proton, 2026-10-17). On its own connection, served by its own thread in the owner,
 * a present no longer holds them. */
/* PW_GPU_TIMING: the client's share of its frames' time since its last one (PS5_Proton's src/pw_gpu_timing.h), sent
 * about once a second: argument[0] presents, [1] nanoseconds waiting for their fences, [2] flushing their frames,
 * [3] in their round trips; bytes its submissions and address their round trips' nanoseconds. Sums only: the owner
 * adds them to the client's report. */
typedef struct PwGpuMessage {
    uint32_t magic, operation, sequence; int32_t status;
    uint64_t address, bytes, argument[4];
} PwGpuMessage;
/* expected=0 is the standalone legacy route. Owned helpers bind their actual
 * spawn result before bootstrap; a client cannot replace that declaration. */
static inline int pw_gpu_client_pid(int expected,uint64_t requested,int owner)
{
    /* The owner's own pid only for the slot the owner opened for itself (ps5_gpu_broker_open_in_owner). */
    return expected>=0 && requested>0 && requested<=INT32_MAX && (requested!=(uint64_t)owner || expected==owner) &&
           (!expected || requested==(uint64_t)expected);
}
/* Window swapchain: width/height packed in argument[0], stride/full HWND in
 * argument[1], generation in argument[2], BGRA in argument[3]. Native PID comes
 * from the owner-managed client slot established by the native helper. No process guessing. */
static inline int pw_gpu_window_image(const PwGpuMessage *m)
{
    const uint32_t width=(uint32_t)m->argument[0],height=(uint32_t)(m->argument[0]>>32);
    const uint32_t stride=(uint32_t)m->argument[1],hwnd=(uint32_t)(m->argument[1]>>32);
    return width && width<=1920 && height && height<=1080 && stride>=width*4 && stride<=1920*4 &&
           !(stride&3) && hwnd && m->argument[2] && m->argument[3]<=1 &&
           m->bytes==(uint64_t)height*stride && m->address && !(m->address&3);
}
/* Reject wrapped resources; subtraction bounds the requested range. */
static inline int pw_gpu_contains(uint64_t base,uint64_t length,uint64_t address,uint64_t bytes)
{
    return length && length<=UINT64_MAX-base && bytes && address>=base && address-base<=length && bytes<=length-(address-base);
}
/* In a client process, before anything else maps memory: reserve both host ranges, each exactly where it belongs and
 * never over anything already there. reserve is sceKernelReserveVirtualRange, unmap sceKernelMunmap (passed in, so this
 * header needs no kernel declarations). 1 when both are reserved; otherwise neither is kept. */
static inline int pw_gpu_reserve_host_ranges(int (*reserve)(void **,size_t,int,size_t),int (*unmap)(void *,size_t))
{
    const uint64_t bases[2]={PW_GPU_HOST_WINDOW_BASE,PW_GPU_HOST_HIGH_BASE},sizes[2]={PW_GPU_HOST_WINDOW_BYTES,PW_GPU_HOST_HIGH_BYTES};
    int kept=0;
    for(;kept<2;++kept) {
        void *at=(void *)(uintptr_t)bases[kept];
        if(reserve(&at,(size_t)sizes[kept],PW_GPU_RESERVE_FIXED|PW_GPU_RESERVE_NO_OVERWRITE,16384) || at!=(void *)(uintptr_t)bases[kept]) {
            if(at && at!=(void *)(uintptr_t)bases[kept])unmap(at,(size_t)sizes[kept]);
            break;
        }
    }
    if(kept==2)return 1;
    while(kept--)unmap((void *)(uintptr_t)bases[kept],(size_t)sizes[kept]);
    return 0;
}
static inline int pw_gpu_host_range(uint64_t address,uint64_t bytes)
{
    if(pw_gpu_contains(PW_GPU_HOST_WINDOW_BASE,PW_GPU_HOST_WINDOW_BYTES,address,bytes))return 0;
    if(pw_gpu_contains(PW_GPU_HOST_HIGH_BASE,PW_GPU_HOST_HIGH_BYTES,address,bytes))return 1;
    return -1;
}
#endif
