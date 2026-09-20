#version 450
// PS5 Vulkan - V0-formats' storage image atomics.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The full-target triangle's pixel stage *adding* to a 32-bit storage image: a
// int uimage2D at set 0, binding 0, whose descriptor is the same 32-byte
// storage image entry round 7 proved (docs/BLOCKERS.md, the descriptor types).
//
// The image is 64 texels wide and two rows deep. The case zeroes the two rows'
// first four texels itself, then every fragment of a quarter adds its quarter's
// texel: one to texel (quarter, 0) and two to texel (quarter, 1). Every pixel of
// the target is covered exactly once by the fullscreen triangle, so the count
// each texel accumulates is known exactly -- the quarter's 960x2160 pixels --
// and the sum is order-independent: a lost update, the shape an implemented-as-
// read-modify-write instruction would produce under that many concurrent
// writers, leaves the texel short of its count. 960 is the target's width
// (kOutputWidth, src/diagnostics.cpp) over four.

layout(set = 0, binding = 0, r32i) uniform iimage2D target;
layout(location = 0) out vec4 color;

// 0x20, 0x40, 0x60 and 0x7f of 255, the four levels the frame's readback check
// expects in red, one to a quarter: the colour export is the storage image
// probes' own, so this case's frame is checked exactly as theirs is.
const int levels[4] = int[](0x20, 0x40, 0x60, 0x7f);

void main()
{
    const int quarter = int(gl_FragCoord.x) / 960;
    imageAtomicAdd(target, ivec2(quarter, 0), 1);
    imageAtomicAdd(target, ivec2(quarter, 1), 2);
    color = vec4(float(levels[quarter]) / 255.0, 0.5, 0.25, 1.0);
}
