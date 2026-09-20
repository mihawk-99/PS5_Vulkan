#version 450
// PS5 Vulkan - M3 vertex-buffer geometry.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Position and colour come from a CPU-filled vertex buffer (24-byte records:
// vec2 position at offset 0, vec4 colour at offset 8), and an index buffer
// selects the vertices of each triangle.

layout(location = 0) in vec2 position;
layout(location = 1) in vec4 colour;
layout(location = 0) out vec4 vertex_colour;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    vertex_colour = colour;
}
