#version 450
// PS5 Vulkan - D1 array layers: two bands, one per layer.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Position and an array coordinate come from a CPU-filled vertex buffer
// (20-byte records: vec2 position at offset 0, vec3 texture coordinate at
// offset 8). Each band's four vertices carry the same z, so the layer the
// fragment stage samples is the band's own: the bottom band reads layer 0 and
// the top band layer 1, which is what makes one frame prove both.

layout(location = 0) in vec2 position;
layout(location = 1) in vec3 coordinate;

layout(location = 0) out vec3 texture_coordinate;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    texture_coordinate = coordinate;
}
