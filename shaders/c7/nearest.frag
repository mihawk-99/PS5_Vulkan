#version 450
// PS5 Vulkan - C7 mip-level sampling, nearest mip filter.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The frame is five horizontal bands of 432 rows and the LOD comes from the
// fragment's own row: the band index is floor(5 * y / 2160), and sampling there
// with a nearest mip filter must read exactly that level of the chain -- one
// draw reads all five levels, which is why the probe needs only one frame per
// filter. The row names the level rather than the interpolated texture
// coordinate, which keeps the varying's interpolation out of the measurement;
// the readback takes the five bands in the order they appear and requires each
// level's own grey once, in ascending or descending order, so the target's row
// order cannot decide the result either.

layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    float lod = floor(gl_FragCoord.y * 5.0 / 2160.0);
    color = textureLod(image, texture_coordinate, lod);
}
