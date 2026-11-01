/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PS5_GPU_SERVER_H
#define PS5_GPU_SERVER_H
#include <stdio.h>
/* One owning title, trusted clients, 32 slots. directory holds logs/captures.
 * is_absent(token) MUST prove the native client has ended, not merely EOF.
 * close takes an opaque application lifecycle token; zero means no child was
 * started. The application owns spawning and process termination. */
bool ps5_gpu_broker_start(const char *directory,bool capture,bool local,bool (*is_absent)(int));
/* Before start: the host-visible bytes each client may hold, a multiple of 16 KiB up to PW_GPU_HOST_LIMIT_MAX;
 * PW_GPU_LIMIT when not called. False, and nothing changed, for any other value or once the broker is running. */
bool ps5_gpu_broker_set_host_limit(unsigned long long bytes);
int ps5_gpu_broker_open(int *cookie);
/* Bind to the owner's acknowledged payload PID before bootstrap gives the
 * helper its broker descriptor. INIT must match; window packets require it. */
int ps5_gpu_broker_open_for_pid(int *cookie,int pid);
/* Game mode (src/pw_window_direct.h): the native pid whose full-screen frames are flipped directly, or 0. No lock. */
int ps5_gpu_broker_game_mode();
void ps5_gpu_broker_close(int cookie,int lifecycle_token);
bool ps5_gpu_broker_stop();
bool ps5_gpu_broker_warm(FILE *output);
void ps5_gpu_broker_set_desktop(int (*foreground)(),void (*cursor)(int *,int *));
/* Frames the display has shown since the broker started: an application's mark for a program's first frame (a
 * scripted run's clock, when the program presents through the broker). */
unsigned long long ps5_gpu_broker_flips(void);
/* The rate the broker's display kept, 120 or 60, opening the display now if no present has yet (0: the broker is not
 * running, or the display did not open). The title asks before any helper starts, so Wine offers 120 Hz modes only
 * when the display really runs at 119.88 Hz (PS5_Proton src/pw_display_rate.h, graphics patch 0029). */
unsigned ps5_gpu_broker_display_hz(void);
/* The next frame the display shows is also written to path, a PPM picture, once: false when the broker is not running,
 * a picture is already waiting for its frame, or the display stayed busy for 100 ms (it waits no longer than that). */
bool ps5_gpu_broker_capture_next(const char *path);
/* The application's cover, shown instead of the desktop until it is set again or cleared (nullptr): 1920x1080 pixels in
 * the display's channel order (B, G, R, A bytes), without the pointer, kept by the caller until the next call returns
 * true. While one is set, ps5_gpu_broker_flips stays where it was. False, and nothing changed, when display_lock could
 * not be had within 100 ms. */
bool ps5_gpu_broker_set_cover(const void *pixels);
/* The broker as of now, for an application's watchdog, without waiting on anything the broker holds: whether its
 * client lock and its display lock are held this instant (a trylock, released at once), the GPU submissions it made
 * and the last one the GPU completed (its fence page's value; 0 before the first submission). phase is the call the
 * client lock's holder waits in (drain, reap-free, stage-alloc, copy, submit, suspend, alloc, map, grant, munmap,
 * give-back, test-hold; null when in none of them), since phase_ms ago, on thread holder_tid: read with atomics alone.
 * Any thread. */
struct Ps5GpuBrokerHealth { unsigned lock_busy,display_busy,fences_issued,fences_completed;const char *phase;unsigned long long phase_ms;int holder_tid; };
void ps5_gpu_broker_health(Ps5GpuBrokerHealth *health);
/* Where the memory a client maps comes from: alloc gives a new object of bytes (or -1), touch writes through
 * [from, to) of one in another process before the broker maps it for the GPU (0, or -1). An application whose clients
 * outlive it sets both, so none of that memory is its own; unset, the broker makes it, as before. */
void ps5_gpu_broker_set_shared_memory(int (*alloc)(size_t bytes),int (*touch)(int fd,size_t from,size_t to));
/* Host-visible client buffers from direct memory: alloc makes `bytes` and grants this process and client_pid (the memory helper;
 * 0 with *offset, or -1), each side mapping it by offset (sceKernelMapDirectMemory); release gives a range back once nothing maps it. Unset: shared-memory objects, as before. */
void ps5_gpu_broker_set_dmem_memory(int (*alloc)(size_t bytes,int client_pid,int64_t *offset),int (*release)(int64_t offset,size_t bytes));
/* attach consumes fd. Frame-page format is frame_page.h. */
/* Global HWND route, configured before any clients start. Attach consumes fd
 * and returns 0 or an errno status, after lifetime/extent/capacity validation. */
void ps5_gpu_broker_set_windows(int (*snapshot)(void *,unsigned));
int ps5_gpu_broker_window_attach(int pid,int fd,size_t bytes);
void ps5_gpu_broker_frame_attach(int pid,int fd);
void ps5_gpu_broker_frame_detach(int pid);
#endif
