#version 450
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
// Match the UI's 24-byte position/UV/UNORM colour vertex and sparse PS inputs.
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 coordinate;
layout(location = 2) in vec4 colour;
layout(location = 0) out vec4 unused_coordinate;
layout(location = 1) out vec4 vertex_colour;
layout(location = 2) out float fog_distance;
void main()
{
    gl_Position = vec4(position, 1.0);
    unused_coordinate = vec4(coordinate, 0.0, 0.0);
    vertex_colour = colour;
    fog_distance = gl_Position.w;
}
