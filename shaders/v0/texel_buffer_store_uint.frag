#version 450
// PS5 Vulkan - V0-formats' unsigned integer storage texel buffers.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The unsigned twin of shaders/v0/texel_buffer_store.frag: a UINT format needs a
// uimageBuffer, and the integer it stores is the value itself, not a normalized
// one -- the case's expected texel holds that integer in the format's own
// encoding, which is the same word its fetch probe reads back. The red channel
// carries the level over 255 and blue the 0.25 constant, so a frame that stores
// nothing still shows whether it drew.

layout(set = 0, binding = 0) uniform writeonly uimageBuffer texels;
layout(location = 0) out vec4 color;

// 0x20, 0x40, 0x60 and 0x7f, the four levels the case's expected texels hold.
const uint levels[4] = uint[](0x20u, 0x40u, 0x60u, 0x7fu);

void main()
{
    const int index = int(gl_FragCoord.x) / 960;
    imageStore(texels, index, uvec4(levels[index]));
    color = vec4(float(levels[index]) / 255.0, 0.5, 0.25, 1.0);
}
