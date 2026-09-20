#version 450
// PS5 Vulkan - D1 cubemaps: six bands, one per face.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Position and a cube direction come from a CPU-filled vertex buffer (20-byte
// records: vec2 position at offset 0, vec3 direction at offset 8). Every vertex
// of a band carries that face's own direction, so the fragment stage samples one
// face a band and one frame covers all six.

layout(location = 0) in vec2 position;
layout(location = 1) in vec3 direction;

layout(location = 0) out vec3 cube_direction;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    cube_direction = direction;
}
