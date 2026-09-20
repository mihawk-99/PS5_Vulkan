#version 450
// PS5 Vulkan - V0-formats' vertex formats, as bytes (signed class).
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The signed twin of vertex_bytes_uint.vert. An ivec component can be negative
// and an 8-bit UNORM target cannot hold one, so the shader adds 128 before
// dividing: the frame holds value + 128 per component, which is a bijection over
// the whole signed byte range and therefore proves the sign as well as the
// value. The absent-component rule is the same as the unsigned twin's (0, and 1
// for w), so a frame of the one-component row reads 0x80, 0x80, 0x80, 0x81.

layout(location = 0) in ivec4 fetched;

layout(location = 0) out vec4 vertex_colour;

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vertex_colour = vec4(fetched + ivec4(128)) / 255.0;
}
