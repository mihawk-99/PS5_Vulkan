/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PS5_GPU_CLIENT_H
#define PS5_GPU_CLIENT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Call after reserving protocol.h host ranges, before Vulkan initialization.
 * fd is the transferred SOCK_SEQPACKET endpoint; it remains owned by caller.
 * reserved must describe actual successful early range reservation. */
int ps5_gpu_client_bind(int fd,bool reserved);
/* For an application that maps memory where it chooses (VK_EXT_map_memory_placed; a 32-bit Windows program under
 * WoW64 maps all of its memory below 4 GiB): call before Vulkan allocates. The client then keeps each host-visible
 * allocation's descriptor, one per allocation, which a placed mapping needs; without it none is kept. */
void ps5_gpu_client_keep_sources(void);
int ps5_gpu_client_done(int result,uint64_t images,uint64_t hash);
int ps5_gpu_client_image(unsigned cycle,unsigned pixels,uint64_t hash);
int ps5_gpu_client_coherence(unsigned slot,unsigned words,uint64_t hash);
int ps5_gpu_client_timing(unsigned slot,unsigned count,uint64_t ns);
int ps5_gpu_client_present(const void *,unsigned,unsigned,unsigned,unsigned);
int ps5_gpu_client_swapchain(const void *,unsigned,unsigned,unsigned,unsigned);
int ps5_gpu_client_ping(void);
void ps5_gpu_client_submit_stats(uint64_t *,uint64_t *);
uint64_t ps5_gpu_client_reply_transit(void);
void ps5_gpu_client_rpc_split(uint64_t[3]);
#ifdef __cplusplus
}
#endif
#endif
