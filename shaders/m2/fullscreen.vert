#version 450
// PS5 Vulkan - M2 full-target triangle.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One oversized triangle generated from gl_VertexIndex, so no vertex buffer or
// descriptor is needed. After clipping it covers the entire render target.

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
