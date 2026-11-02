/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_DISPLAY_OVERLAY_H
#define PW_DISPLAY_OVERLAY_H
#include "pw_display_copy.h"
#include <math.h>
/* An application's panel over the frame (PS5_Proton's own on-screen keyboard, in the bottom part of the screen with the
 * game live above it): linear B8G8R8A8 rows, pitch w * 4. pw_display_overlay_blit draws it opaque into the display's
 * tiled buffer after the frame is copied there, inside the rectangle only; pw_display_overlay_compose (below) blends a
 * see-through one over the frame blurred under it. A rectangle whose corners are on multiples of 4 is written a 4x4 block (one
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

/* ---- A see-through panel (PS5_Proton's keyboard, "Frosted Meadow", the owner's pick of 2026-11-01): premultiplied
 * B8G8R8A8 over the game's frame blurred under it (8x smaller, a 3x3 box, stretched back bilinearly), and bubbles rising
 * in the side margins. `frame` is the clean frame, tiled, in cached memory (the display's shadow, or the buffer itself
 * when it is cached: a capture's picture); it may be `buffer`. The rectangle's corners and size on multiples of 8. ---- */
enum { PW_OVERLAY_BUBBLES = 14, PW_OVERLAY_MARGIN = 165 };
typedef struct PwOverlayBubble { int cx, cy, r; unsigned alpha; } PwOverlayBubble;

/* Where each bubble is at `seconds`: rising from below the rectangle to its top over 6..11 s, wobbling 4..12 px, fading in
 * and out; radius 9..35; even ones in the left margin, odd ones in the right. Only ones inside the rectangle count. */
static inline unsigned pw_overlay_bubbles(double seconds, unsigned y, unsigned h, unsigned screen_w, PwOverlayBubble *out)
{
    unsigned count = 0;
    for (unsigned i = 0; i < PW_OVERLAY_BUBBLES; ++i) {
        const int r = 9 + (int)((i * 7) % 27);
        const double period = 6 + (i * 5) % 6, wobble = 4 + (i * 3) % 9, phase = i * 0.71;
        const int room = PW_OVERLAY_MARGIN - 2 * r - 2 * (int)wobble;
        const int base = (int)wobble + r + (room > 0 ? (int)((i * 53) % (unsigned)room) : 0);
        double life = (seconds + phase) / period;
        life -= (double)(long long)life;
        const double rise = (h + 2.0 * r) * life, sway = sin(6.283185307 * (seconds + phase) * 2 / period) * wobble;
        const double fade = life < 0.12 ? life / 0.12 : life > 0.82 ? (1 - life) / 0.18 : 1;
        PwOverlayBubble b = {(i & 1) ? (int)screen_w - PW_OVERLAY_MARGIN + base + (int)sway : base + (int)sway,
                             (int)(y + h + r - rise), r, (unsigned)(fade * 255)};
        if (b.alpha && b.cy + r > (int)y && b.cy - r < (int)(y + h)) out[count++] = b;
    }
    return count;
}

/* One bubble's white over a pixel: a faint body, a rim, a highlight up and left; 0..255 */
static inline unsigned pw_overlay_bubble_alpha(const PwOverlayBubble *b, int px, int py)
{
    const float dx = px + 0.5f - b->cx, dy = py + 0.5f - b->cy, d = sqrtf(dx * dx + dy * dy);
    if (d > b->r + 1.5f) return 0;
    float a = d < b->r ? 0.10f : 0;
    const float rim = 1.5f - fabsf(d - b->r + 0.75f);
    if (rim > 0) a += 0.55f * (rim > 1 ? 1 : rim);
    const float hx = dx + b->r * 0.35f, hy = dy + b->r * 0.35f, hd = sqrtf(hx * hx + hy * hy), hr = b->r * 0.28f;
    if (hd < hr + 1) a += 0.55f * (hd < hr ? 1 - hd / (hr * 2) : hr + 1 - hd);
    if (a > 0.9f) a = 0.9f;
    return (unsigned)(a * b->alpha + 0.5f);
}

/* Four premultiplied panel pixels over one colour: p + under * (255 - p.a) / 255, opaque */
static inline __m128i pw_overlay_over4(__m128i p, __m128i under16)
{
    const __m128i zero = _mm_setzero_si128(), c255 = _mm_set1_epi16(255), c128 = _mm_set1_epi16(128);
    __m128i half[2] = {_mm_unpacklo_epi8(p, zero), _mm_unpackhi_epi8(p, zero)};
    for (int i = 0; i < 2; ++i) {
        __m128i a = _mm_shufflelo_epi16(half[i], 0xff);
        a = _mm_shufflehi_epi16(a, 0xff);
        __m128i x = _mm_add_epi16(_mm_mullo_epi16(under16, _mm_sub_epi16(c255, a)), c128);
        x = _mm_srli_epi16(_mm_add_epi16(x, _mm_srli_epi16(x, 8)), 8);
        half[i] = _mm_add_epi16(half[i], x);
    }
    return _mm_or_si128(_mm_packus_epi16(half[0], half[1]), _mm_set1_epi32((int)0xff000000u));
}

/* The mean of one channel (shift 0, 8, 16) of a 64-byte line: four vectors of four pixels */
static inline unsigned pw_overlay_line_sum(const __m128i *line, int shift)
{
    const __m128i mask = _mm_set1_epi32(0xff), zero = _mm_setzero_si128();
    __m128i sum = zero;
    for (int k = 0; k < 4; ++k) {
        __m128i v = line[k];
        if (shift == 8) v = _mm_srli_epi32(v, 8);
        if (shift == 16) v = _mm_srli_epi32(v, 16);
        sum = _mm_add_epi64(sum, _mm_sad_epu8(_mm_and_si128(v, mask), zero));
    }
    return (unsigned)_mm_cvtsi128_si32(sum) + (unsigned)_mm_cvtsi128_si32(_mm_srli_si128(sum, 8));
}

/* The glass sweep's light at diagonal position `diag` (x + 0.45 (y - top)), `k` seconds into it (the owner's pick,
 * 2026-11-01, mockup video B: a soft band crossing the panel in 1.8 s); 0..1 */
static inline float pw_overlay_sweep_band(float diag, double k)
{
    double s = k / 1.8;
    s = s < 0 ? 0 : s > 1 ? 1 : s;
    const float centre = (float)(-300 + (PW_DISPLAY_WIDTH + 600) * s * s * (3 - 2 * s)), d = (diag - centre) / 110;
    return d > 3.5f || d < -3.5f ? 0 : expf(-d * d);
}

/* scratch: (w / 8 + 2) * (h / 8 + 2) * 2 + (w / 4) * (h / 4) uint32. fresh_blur 0 keeps the last call's blur (every second
 * frame: half the cost, the owner's "halve it"); `sweep`: seconds into the glass sweep, or below 0 for none. */
static inline void pw_display_overlay_compose(uint8_t *buffer, const uint8_t *frame, const uint32_t *panel, unsigned x,
                                              unsigned y, unsigned w, unsigned h, double seconds, uint32_t *scratch,
                                              int fresh_blur, double sweep)
{
    if (!buffer || !frame || !panel || (x | y | w | h) & 7 || x + w > PW_DISPLAY_WIDTH || y + h > PW_DISPLAY_HEIGHT) return;
    const unsigned sw = w / 8, sh = h / 8, stride = sw + 2, mw = w / 4;
    uint32_t *small = scratch, *soft = scratch + (size_t)stride * (sh + 2), *mid = soft + (size_t)stride * (sh + 2);
    if (fresh_blur) {
    /* 1. the frame under the rectangle, 8x smaller: each 8x8 block's mean (four 4x4 lines of the tiling) */
    for (unsigned by = 0; by < sh; ++by)
        for (unsigned bx = 0; bx < sw; ++bx) {
            unsigned r = 0, g = 0, b = 0;
            for (unsigned q = 0; q < 4; ++q) {
                const __m128i *line = (const __m128i *)(const void *)(frame + pw_display_tiled(x + bx * 8 + (q & 1) * 4, y + by * 8 + (q >> 1) * 4));
                b += pw_overlay_line_sum(line, 0), g += pw_overlay_line_sum(line, 8), r += pw_overlay_line_sum(line, 16);
            }
            small[(by + 1) * stride + bx + 1] = (r / 64) << 16 | (g / 64) << 8 | (b / 64);
        }
    /* edges repeated, then a 3x3 box (the mockup's second shrink) */
    for (unsigned by = 1; by <= sh; ++by) small[by * stride] = small[by * stride + 1], small[by * stride + sw + 1] = small[by * stride + sw];
    for (unsigned bx = 0; bx < stride; ++bx) small[bx] = small[stride + bx], small[(sh + 1) * stride + bx] = small[sh * stride + bx];
    for (unsigned by = 1; by <= sh; ++by)
        for (unsigned bx = 1; bx <= sw; ++bx) {
            unsigned r = 0, g = 0, b = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const uint32_t c = small[(by + j) * stride + bx + i];
                    r += c >> 16 & 0xff, g += c >> 8 & 0xff, b += c & 0xff;
                }
            soft[by * stride + bx] = (r / 9) << 16 | (g / 9) << 8 | (b / 9);
        }
    for (unsigned by = 1; by <= sh; ++by) soft[by * stride] = soft[by * stride + 1], soft[by * stride + sw + 1] = soft[by * stride + sw];
    for (unsigned bx = 0; bx < stride; ++bx) soft[bx] = soft[stride + bx], soft[(sh + 1) * stride + bx] = soft[sh * stride + bx];
    /* 2. stretched back bilinearly to one colour per 4x4 block (at the block's middle; 8.8 fixed point) */
    for (unsigned my = 0; my < h / 4; ++my)
        for (unsigned mx = 0; mx < mw; ++mx) {
            const unsigned fx = mx * 128 + 192, fy = my * 128 + 192, sx = fx >> 8, sy = fy >> 8, ax = fx & 255, ay = fy & 255;
            const uint32_t c00 = soft[sy * stride + sx], c01 = soft[sy * stride + sx + 1];
            const uint32_t c10 = soft[(sy + 1) * stride + sx], c11 = soft[(sy + 1) * stride + sx + 1];
            uint32_t c = 0;
            for (int shift = 0; shift < 24; shift += 8) {
                const unsigned top = (c00 >> shift & 0xff) * (256 - ax) + (c01 >> shift & 0xff) * ax;
                const unsigned bottom = (c10 >> shift & 0xff) * (256 - ax) + (c11 >> shift & 0xff) * ax;
                c |= ((top * (256 - ay) + bottom * ay) >> 16) << shift;
            }
            mid[my * mw + mx] = c;
        }
    }
    PwOverlayBubble bubbles[PW_OVERLAY_BUBBLES];
    const unsigned nb = pw_overlay_bubbles(seconds, y, h, PW_DISPLAY_WIDTH, bubbles);
    /* 3. each 4x4 block: the panel over its colour, four pixels at a time; the bubbles in the margins; streamed out */
    const __m128i zero = _mm_setzero_si128();
    for (unsigned oy = 0; oy < h; oy += 4)
        for (unsigned ox = 0; ox < w; ox += 4) {
            __m128i *at = (__m128i *)(void *)(buffer + pw_display_tiled(x + ox, y + oy));
            const __m128i under16 = _mm_unpacklo_epi8(_mm_set1_epi32((int)mid[(oy / 4) * mw + ox / 4]), zero);
            __m128i rows[4];
            for (unsigned r = 0; r < 4; ++r)
                rows[r] = pw_overlay_over4(_mm_loadu_si128((const __m128i *)(const void *)(panel + (size_t)(oy + r) * w + ox)), under16);
            const int bx = (int)(x + ox), by = (int)(y + oy);
            /* the glass sweep: the band's light at the block's middle, more on more opaque glass (the keys') */
            const float band = sweep < 0 ? 0 : pw_overlay_sweep_band((float)ox + 2 + ((float)oy + 2) * 0.45f, sweep);
            if (band > 0.004f) {
                /* 0.10 of the way to white on clear glass, up to 0.26 on the keys' (alpha 0.75 to 0.95), x65536 */
                static unsigned glass[256];
                if (!glass[0])
                    for (unsigned a = 0; a < 256; ++a) {
                        const float pa = a / 255.0f;
                        glass[a] = (unsigned)((0.10f + 0.16f * (pa < 0.75f ? 0 : pa > 0.95f ? 1 : (pa - 0.75f) / 0.2f)) * 65536);
                    }
                const unsigned band256 = (unsigned)(band * 256);
                const __m128i c255 = _mm_set1_epi16(255), opaque = _mm_set1_epi32((int)0xff000000u);
                for (unsigned r = 0; r < 4; ++r) {
                    const uint32_t *alpha = panel + (size_t)(oy + r) * w + ox;
                    unsigned s[4];
                    for (unsigned i = 0; i < 4; ++i) s[i] = (band256 * glass[alpha[i] >> 24]) >> 16;
                    /* v + (255 - v) * s / 256, two pixels a half, each pixel's s in its four lanes */
                    const __m128i lo = _mm_unpacklo_epi8(rows[r], zero), hi = _mm_unpackhi_epi8(rows[r], zero);
                    const __m128i slo = _mm_set_epi16((short)s[1], (short)s[1], (short)s[1], (short)s[1], (short)s[0], (short)s[0], (short)s[0], (short)s[0]);
                    const __m128i shi = _mm_set_epi16((short)s[3], (short)s[3], (short)s[3], (short)s[3], (short)s[2], (short)s[2], (short)s[2], (short)s[2]);
                    const __m128i l = _mm_add_epi16(lo, _mm_srli_epi16(_mm_mullo_epi16(_mm_sub_epi16(c255, lo), slo), 8));
                    const __m128i h = _mm_add_epi16(hi, _mm_srli_epi16(_mm_mullo_epi16(_mm_sub_epi16(c255, hi), shi), 8));
                    rows[r] = _mm_or_si128(_mm_packus_epi16(l, h), opaque);
                }
            }
            if (nb && (bx < PW_OVERLAY_MARGIN || bx + 4 > PW_DISPLAY_WIDTH - PW_OVERLAY_MARGIN))
                for (unsigned i = 0; i < nb; ++i) {
                    const PwOverlayBubble *bb = &bubbles[i];
                    if (bx + 4 < bb->cx - bb->r - 2 || bx > bb->cx + bb->r + 2 || by + 4 < bb->cy - bb->r - 2 || by > bb->cy + bb->r + 2) continue;
                    uint32_t px[16];
                    for (unsigned r = 0; r < 4; ++r) _mm_storeu_si128((__m128i *)(void *)(px + r * 4), rows[r]);
                    for (unsigned k = 0; k < 16; ++k) {
                        const unsigned a = pw_overlay_bubble_alpha(bb, bx + (int)(k & 3), by + (int)(k >> 2));
                        if (!a) continue;
                        uint32_t mixed = 0xff000000u;
                        for (int shift = 0; shift < 24; shift += 8)
                            mixed |= ((255 * a + (px[k] >> shift & 0xff) * (255 - a) + 127) / 255) << shift;
                        px[k] = mixed;
                    }
                    for (unsigned r = 0; r < 4; ++r) rows[r] = _mm_loadu_si128((const __m128i *)(const void *)(px + r * 4));
                }
            for (unsigned r = 0; r < 4; ++r) _mm_stream_si128(at + r, rows[r]);
        }
    _mm_sfence();
}

#endif
