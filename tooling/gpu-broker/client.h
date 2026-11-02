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
 * allocation's direct-memory offset, which a placed mapping needs; without it none is kept and nothing is placed. */
void ps5_gpu_client_keep_sources(void);
/* Before binding: this client runs in the owner's own process (a Windows program run inside the title). Each host-visible
 * buffer is already mapped there by the owner, at the address it chose: the client uses that mapping as it is, never maps
 * over it, and leaves unmapping to the owner. */
void ps5_gpu_client_in_owner(void);
/* Whether the application's host memory can be given GPU access (VK_EXT_external_memory_host): only in the owner's own
 * process, whose address space the GPU runs in. */
bool ps5_gpu_client_host_memory(void);
/* For PS5_Proton's Wine module loader (its wine/ps5/pw_wine_dl.h), which calls it with its own symbol lookup as it
 * opens this module: bind then looks up ntdll's pw_ps5_child_doomed, which the client calls once, when it gives the
 * broker up for good (no answer in 15 s, the connection gone, a malformed reply, a free not taken back), so the
 * helper's loader ends the process 5 s later. Never called (a title's process, a host build): nothing is stamped. */
void pw_module_lookup(void *(*lookup)(void *,const char *));
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
