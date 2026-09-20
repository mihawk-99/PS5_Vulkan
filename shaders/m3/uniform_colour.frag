#version 450
// PS5 Vulkan - M3 uniform-buffer colour.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The colour comes from a uniform buffer at set 0, binding 0 that the CPU
// fills before each frame, so frames with different buffer contents must read
// back as different exact colours. The CPU writes channels that are exact
// multiples of 1/255, so an RGBA8 UNORM target stores them without rounding
// ambiguity.

layout(set = 0, binding = 0) uniform Colour
{
    vec4 value;
} colour;

layout(location = 0) out vec4 color;

void main()
{
    color = colour.value;
}
