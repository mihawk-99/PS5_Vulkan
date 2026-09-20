#version 450
// PS5 Vulkan - M3 sampled texture.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Samples a CPU-uploaded RGBA8 texture bound as a combined image sampler at
// set 0, binding 0. The title switches the sampler between nearest and
// bilinear filtering without recompiling the shader.

layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    color = texture(image, texture_coordinate);
}
