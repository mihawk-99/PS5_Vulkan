#version 450
// PS5 Vulkan - V0-formats' storage images.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The full-target triangle's pixel stage storing into a 2D storage image: a
// writeonly image2D at set 0, binding 0, which is
// VK_DESCRIPTOR_TYPE_STORAGE_IMAGE -- a 32-byte image descriptor without the
// sampler a combined image sampler carries (docs/BLOCKERS.md, the descriptor
// types).
//
// The image is 64 texels wide and two rows deep, one row per 256-byte storage
// row the driver's row layout makes. The fragment's x names one of four
// quarters of the target's width; that quarter writes texel (quarter, 0) with its
// own level and texel (quarter, 1) with the next quarter's, so the two rows hold
// the same four levels in different places: a descriptor whose row pitch is
// wrong puts row 1's stores where row 0's belong, and the case reads them there.
// 960 is the target's width (kOutputWidth, src/diagnostics.cpp) over four.

layout(set = 0, binding = 0) uniform writeonly image2D target;
layout(location = 0) out vec4 color;

// 0x20, 0x40, 0x60 and 0x7f of 255, the four levels the case's expected texels
// hold, one to a quarter. The literals carry every significant digit of
// level/255, so the correctly rounded float32 they parse to is the one the case's
// own division produces: a division done in the shader is the compiler's to turn
// into a reciprocal multiply, which lands one ULP high for 96 and 127 and a
// 32-bit float texel keeps that (docs/M5_PHASE_C.md, blocker rounds 6 and 7).
const float levels[4] = float[](0.125490203499794, 0.250980406999588, 0.3764705955982208,
                                0.49803921580314636);

void main()
{
    const int quarter = int(gl_FragCoord.x) / 960;
    imageStore(target, ivec2(quarter, 0), vec4(levels[quarter]));
    imageStore(target, ivec2(quarter, 1), vec4(levels[(quarter + 1) % 4]));
    color = vec4(levels[quarter], 0.5, 0.25, 1.0);
}
