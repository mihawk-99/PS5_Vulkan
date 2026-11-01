/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_DISPLAY_OVERLAY_H
#define PW_DISPLAY_OVERLAY_H
#include "pw_display_copy.h"
/* An application's panel over the frame (PS5_Proton's own on-screen keyboard, in the bottom part of the screen with the
 * game live above it): linear B8G8R8A8 rows, pitch w * 4, drawn opaque into the display's tiled buffer after the frame is
 * copied there, inside the rectangle only. A rectangle whose corners are on multiples of 4 is written a 4x4 block (one
 * 64-byte line) at a time with streaming stores, as the fast copy does; any other a pixel at a time. Clipped to the
 * screen. Host test: PS5_Proton's tests/test_pw_display_overlay.c (nothing outside the rectangle changes). */
/* A whole tiled frame from cached memory into the display's buffer, a 64-byte line at a time with streaming stores (both
 * 64-byte aligned); the caller fences. */
static inline void pw_display_stream_copy(uint8_t *buffer, const uint8_t *frame, size_t bytes)
{
    for (size_t at = 0; at < bytes; at += 64)
        for (unsigned r = 0; r < 4; ++r)
            _mm_stream_si128((__m128i *)(void *)(buffer + at) + r, _mm_load_si128((const __m128i *)(const void *)(frame + at) + r));
}

static inline void pw_display_overlay_blit(uint8_t *buffer, const uint32_t *pixels, unsigned x, unsigned y, unsigned w,
                                           unsigned h)
{
    if (!buffer || !pixels || x >= PW_DISPLAY_WIDTH || y >= PW_DISPLAY_HEIGHT) return;
    const unsigned pitch = w;
    if (w > PW_DISPLAY_WIDTH - x) w = PW_DISPLAY_WIDTH - x;
    if (h > PW_DISPLAY_HEIGHT - y) h = PW_DISPLAY_HEIGHT - y;
    if (!(x & 3) && !(y & 3) && !(w & 3) && !(h & 3)) {
        for (unsigned by = 0; by < h; by += 4)
            for (unsigned bx = 0; bx < w; bx += 4) {
                __m128i *line = (__m128i *)(void *)(buffer + pw_display_tiled(x + bx, y + by));
                for (unsigned r = 0; r < 4; ++r)
                    _mm_stream_si128(line + r, _mm_loadu_si128((const __m128i *)(const void *)(pixels + (size_t)(by + r) * pitch + bx)));
            }
        _mm_sfence();
        return;
    }
    for (unsigned j = 0; j < h; ++j)
        for (unsigned i = 0; i < w; ++i)
            *(uint32_t *)(void *)(buffer + pw_display_tiled(x + i, y + j)) = pixels[(size_t)j * pitch + i];
}
#endif
