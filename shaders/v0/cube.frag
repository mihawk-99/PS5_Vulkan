#version 450
// PS5 Vulkan - D1 cubemaps: a samplerCube fetch.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Samples a combined image sampler the application bound as a cube at set 0,
// binding 0. The direction comes from the vertex stage, and the face the
// hardware picks from it is what the descriptor's TYPE field (11 for a cube) and
// its six slices decide.

layout(set = 0, binding = 0) uniform samplerCube image;

layout(location = 0) in vec3 cube_direction;
layout(location = 0) out vec4 color;

void main()
{
    color = texture(image, cube_direction);
}
