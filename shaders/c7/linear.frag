#version 450
// PS5 Vulkan - C7 mip-level sampling, linear mip filter.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same five bands as the nearest set, sampling half a level further: at a
// half-integer LOD a linear mip filter reads the mean of the two levels around
// it, which the probe's solid levels make a whole number (55, 150, 120, 50, and
// level 4 alone in the last band, where the range's own top clamps the LOD).

layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    float lod = floor(gl_FragCoord.y * 5.0 / 2160.0) + 0.5;
    color = textureLod(image, texture_coordinate, lod);
}
