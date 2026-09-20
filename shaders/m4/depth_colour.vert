#version 450
// PS5 Vulkan - M4 depth-tested geometry.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Position with depth and a colour come from 28-byte vertex records: vec3
// position at offset 0 and vec4 colour at offset 12. The depth test compares
// the interpolated z of each fragment against the depth buffer.

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 colour;
layout(location = 0) out vec4 vertex_colour;

void main()
{
    gl_Position = vec4(position, 1.0);
    vertex_colour = colour;
}
