#version 450
// PS5 Vulkan - C2 instancing.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each instance draws the same small quad at its own place: the instance index
// moves it sideways by 0.6 of NDC space and colours it, so a frame of three
// instances holds three squares side by side and the count is visible whatever
// order the instances run in. The position, not the colour, is what makes the
// count readable.

layout(location = 0) in vec2 position;
layout(location = 0) out vec4 color;

void main()
{
    float index = float(gl_InstanceIndex);
    gl_Position = vec4(position.x * 0.25 + (index - 1.0) * 0.6, position.y * 0.25, 0.0, 1.0);
    color = vec4(1.0 - index / 4.0, index / 4.0, 0.5, 1.0);
}
