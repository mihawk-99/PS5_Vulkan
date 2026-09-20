#version 450
// PS5 Vulkan - C1 clear colour.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mesa's clear fragment shader (vk_meta_clear.c build_clear_shader) for one
// colour attachment, as the driver compiles it. vk_meta declares the clear
// colours as a push-constant block of MESA_VK_MAX_COLOR_ATTACHMENTS vec4s and
// reads the cleared attachment's element; the driver lowers push constants
// into a uniform buffer at set 0, binding 0 (docs/M5_PHASE_C.md, C1b question
// 1), which is what this block is.
//
// The CPU writes channels that are exact multiples of 1/255, so an 8-bit
// UNORM target stores the clear colour without rounding ambiguity.

layout(set = 0, binding = 0) uniform Clear
{
    vec4 color_values[8];
} clear;

layout(location = 0) out vec4 color;

void main()
{
    color = clear.color_values[0];
}
