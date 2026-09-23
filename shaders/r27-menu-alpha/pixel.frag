#version 450
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
layout(location = 1) in vec4 vertex_colour;
layout(location = 2) in float fog_distance;
layout(location = 0) out vec4 colour;
void main()
{
    colour = vec4(vertex_colour.rgb * clamp(fog_distance, 0.0, 1.0), vertex_colour.a);
}
