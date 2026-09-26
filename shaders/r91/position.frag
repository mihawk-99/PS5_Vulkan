#version 450
// PS5 Vulkan - R91's position frame.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every fragment of the m2 fullscreen triangle writes its own position into an
// unsigned integer target, so the target's storage says, element by element,
// which texel the hardware put there: the tiled maps are then read off the
// storage instead of assumed. The push constant picks the encoding the target
// can hold: 0 is the whole position, (x, y, 0xa5, 0x5a), for elements of 8 and
// 16 bytes; 1 is (x & 255, y & 255, x >> 8, y >> 8) for four bytes and, in its
// first two channels, for two; 2 and 3 are x & 255 and y & 255 alone, the two
// frames a one-byte element needs.

layout(push_constant) uniform Part
{
    uvec4 part;
} part;

layout(location = 0) out uvec4 color;

void main()
{
    const uint x = uint(gl_FragCoord.x);
    const uint y = uint(gl_FragCoord.y);
    const uint which = part.part.x;
    if (which == 0u)
        color = uvec4(x, y, 0xa5u, 0x5au);
    else if (which == 1u)
        color = uvec4(x & 255u, y & 255u, x >> 8u, y >> 8u);
    else if (which == 2u)
        color = uvec4(x & 255u, 0u, 0u, 0u);
    else
        color = uvec4(y & 255u, 0u, 0u, 0u);
}
