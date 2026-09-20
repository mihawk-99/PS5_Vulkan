#version 450
// PS5 Vulkan - C7 geometry diagnostic.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A constant colour, so a frame shows where the geometry lands with no texture
// sampling in the way. Used while bringing Phase C7's probe up.

layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(texture_coordinate, 0.5, 1.0);
}
