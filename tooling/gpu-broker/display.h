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
    bool high_frame_rate=false;   /* the output at 119.88 Hz (PS5_Proton's PROTON_120HZ=1), restored at close */
    /* An application's panel over every frame inside its rectangle, premultiplied: blended over the frame blurred under it,
     * with bubbles in its margins (pw_display_overlay_compose). Owned here, set and cleared
     * under the broker's display lock (ps5_gpu_broker_set_overlay). */
    uint32_t *overlay=nullptr;
    unsigned overlay_x=0,overlay_y=0,overlay_w=0,overlay_h=0;
    uint64_t last_present_ns=0;
    /* While a panel is up: the last frame, tiled, without the panel or the pointer, in cached memory (the broker allocates
     * it with the panel). A re-show streams it out instead of reading the display back: 137 ms a panel change in a
     * waiting game before (osk-lag-desktop-base-2026-11-01-01, the owner: "insane input lag"). shadow_valid: it holds
     * the last frame; the first re-show after the panel came reads that frame back once. */
    uint8_t *shadow=nullptr;
    uint32_t *blur=nullptr;   /* the see-through panel's blurred frame (pw_display_overlay_compose's scratch), with the shadow */
    bool shadow_valid=false;
    int last_cursor_x=-1,last_cursor_y=-1;
    /* Makes the shadow (on) or drops it (off), with the panel. */
    void keep_shadow(bool on);
    /* cursor_x, cursor_y: where to draw the pointer on the 1920x1080 screen, or -1 for none. */
    int present(const void *pixels,unsigned width,unsigned height,unsigned pitch,FILE *log,const char *capture,bool bgra=false,
                int cursor_x=-1,int cursor_y=-1);
    /* The last frame shown again with the overlay as it is now, for a panel that changed while no frame came (0, or the
     * flip's status). Over the previous panel's pixels: a panel taken off shows the game again with its next frame. */
    int reshow(FILE *log);
    bool close(FILE *log);
    /* Opens the display now, as the first present would (0, or the first failure): the title reads the rate it kept
     * before any helper starts (graphics patch 0029). */
    int open(FILE *log);
    /* 120 or 60 once the display is open and registered, 0 before. */
    unsigned refresh_hz() const { return registered ? (high_frame_rate ? 120u : 60u) : 0u; }
    /* A picture of the buffer the last present flipped to, written to path. */
    bool capture_last(const char *path);
    /* The buffer the last present flipped to, as linear 1920x1080 pixels in the display's channel order (the pointer
     * included, if one was drawn). Does not wait. */
    bool read_last(uint32_t *pixels) const;
};
#endif
