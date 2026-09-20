#version 450
// PS5 Vulkan - M3 textured-square geometry.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Position and texture coordinate come from a CPU-filled vertex buffer
// (16-byte records: vec2 position at offset 0, vec2 texture coordinate at
// offset 8), drawn through an index buffer.

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 coordinate;
layout(location = 0) out vec2 texture_coordinate;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    texture_coordinate = coordinate;
}
