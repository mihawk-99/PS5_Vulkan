#version 450
// PS5 Vulkan - V0-formats' signed integer storage texel buffers.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The signed twin: an iimageBuffer whose levels are kept non-negative, so the
// value the format stores is the level itself and the case's expected texel is
// the same word its fetch probe reads back. Red carries the level over 255 and
// blue the 0.25 constant, so a frame that stores nothing still shows whether it
// drew.

layout(set = 0, binding = 0) uniform writeonly iimageBuffer texels;
layout(location = 0) out vec4 color;

// 0x20, 0x40, 0x60 and 0x7f, the four levels the case's expected texels hold.
const int levels[4] = int[](0x20, 0x40, 0x60, 0x7f);

void main()
{
    const int index = int(gl_FragCoord.x) / 960;
    imageStore(texels, index, ivec4(levels[index]));
    color = vec4(float(levels[index]) / 255.0, 0.5, 0.25, 1.0);
}
