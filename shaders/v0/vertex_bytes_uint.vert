#version 450
// PS5 Vulkan - V0-formats' vertex formats, as bytes (unsigned class).
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The unsigned twin of vertex_bytes_float.vert: the attribute is a uvec4, which
// is the class an R32_UINT, R32G32_UINT or R32G32B32A32_UINT attribute fetches
// into, and each component is written as a byte (value / 255) so the frame holds
// the integer the attribute carried. A component the bound format does not have
// is filled by the fetch with 0 (and w with 1), which is Vulkan's rule for
// absent components and is what makes a one- or two-component row's expectation
// differ from a four-component one's: the frame names both the value that is
// there and the zero that is not.

layout(location = 0) in uvec4 fetched;

layout(location = 0) out vec4 vertex_colour;

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vertex_colour = vec4(fetched) / 255.0;
}
