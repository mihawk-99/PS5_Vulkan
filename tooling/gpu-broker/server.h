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
int ps5_gpu_broker_open(int *cookie);
void ps5_gpu_broker_close(int cookie,int lifecycle_token);
bool ps5_gpu_broker_stop();
bool ps5_gpu_broker_warm(FILE *output);
void ps5_gpu_broker_set_desktop(int (*foreground)(),void (*cursor)(int *,int *));
/* Frames the display has shown since the broker started: an application's mark for a program's first frame (a
 * scripted run's clock, when the program presents through the broker). */
unsigned long long ps5_gpu_broker_flips(void);
/* The next frame the display shows is also written to path, a PPM picture, once: false when the broker is not running
 * or a picture is already waiting for its frame. */
bool ps5_gpu_broker_capture_next(const char *path);
/* Where the memory a client maps comes from: alloc gives a new object of bytes (or -1), touch writes through
 * [from, to) of one in another process before the broker maps it for the GPU (0, or -1). An application whose clients
 * outlive it sets both, so none of that memory is its own; unset, the broker makes it, as before. */
void ps5_gpu_broker_set_shared_memory(int (*alloc)(size_t bytes),int (*touch)(int fd,size_t from,size_t to));
/* attach consumes fd. Frame-page format is frame_page.h. */
void ps5_gpu_broker_frame_attach(int pid,int fd);
void ps5_gpu_broker_frame_detach(int pid);
#endif
