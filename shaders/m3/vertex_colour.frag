#version 450
// PS5 Vulkan - M3 interpolated vertex colour.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the colour interpolated from the vertex buffer. The CPU gives every
// corner the same red and alpha, green rising left to right and blue rising
// top to bottom, so both triangles share one colour plane per channel.

layout(location = 0) in vec4 vertex_colour;
layout(location = 0) out vec4 color;

void main()
{
    color = vertex_colour;
}
