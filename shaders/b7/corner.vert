#version 450
// PS5 Vulkan - B7 corner triangle.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vertices at clip-space (-1, -1), (1, -1) and (-1, 1), generated from
// gl_VertexIndex. Vulkan puts clip y = -1 on the framebuffer's first row, so
// the triangle covers the target's top-left half, above the diagonal from the
// top-right corner to the bottom-left corner. OpenGL's orientation would put
// it on the bottom-left half, below the other diagonal.

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2(-1.0,  1.0)
);

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
