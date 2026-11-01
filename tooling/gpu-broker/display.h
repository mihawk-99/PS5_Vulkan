/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PS5_GPU_DISPLAY_H
#define PS5_GPU_DISPLAY_H
#include <stdint.h>
#include <stdio.h>
/* Only the owning title uses this display. Input is completed, linear RGBA8. */
struct Ps5GpuDisplay {
    int handle=-1, back=0;
    int64_t physical=-1;
    void *mapped=nullptr;
    uint64_t flips=0,last_copy_ns=0;
    bool registered=false;
    bool fast_copy=false;   /* PS5_Proton's PROTON_GPU_FAST_COPY=1, read when the display opens */
    /* cursor_x, cursor_y: where to draw the pointer on the 1920x1080 screen, or -1 for none. */
    int present(const void *pixels,unsigned width,unsigned height,unsigned pitch,FILE *log,const char *capture,bool bgra=false,
                int cursor_x=-1,int cursor_y=-1);
    bool close(FILE *log);
    /* A picture of the buffer the last present flipped to, written to path. */
    bool capture_last(const char *path);
    /* The buffer the last present flipped to, as linear 1920x1080 pixels in the display's channel order (the pointer
     * included, if one was drawn). Does not wait. */
    bool read_last(uint32_t *pixels) const;
};
#endif
