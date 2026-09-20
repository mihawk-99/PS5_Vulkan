#version 450
// PS5 Vulkan - D1 array layers: a sampler2DArray fetch.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Samples a combined image sampler the application bound as a 2D array at set
// 0, binding 0. The layer comes from the vertex stage, so one frame reads two
// layers of one view: the descriptor's DEPTH and BASE_ARRAY fields (word 4) are
// what this shader's fetch exercises.

layout(set = 0, binding = 0) uniform sampler2DArray image;

layout(location = 0) in vec3 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    color = texture(image, texture_coordinate);
}
