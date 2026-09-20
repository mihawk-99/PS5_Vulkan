#version 450
// PS5 Vulkan - V0-formats' vertex formats, as bytes (float class).
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One vertex attribute at location 0, declared as the vec4 the *float* class of
// vertex formats fetches into, and a full-screen triangle generated from
// gl_VertexIndex so the attribute bytes are the only vertex data. The fetched
// value goes to the fragment stage unchanged (m3/vertex_colour.frag writes it),
// so the frame holds the colour the attribute's format decodes to: for a
// normalised or packed format that is the format's own bytes back, which is what
// v0-vertex-formats compares. A row binds R8G8B8A8_UNORM, B8G8R8A8_UNORM,
// A8B8G8R8_UNORM_PACK32 or A2B10G10R10_UNORM_PACK32 here; the driver's vertex
// format table is what turns the VkFormat into the attribute's format word.

layout(location = 0) in vec4 fetched;

layout(location = 0) out vec4 vertex_colour;

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vertex_colour = fetched;
}
