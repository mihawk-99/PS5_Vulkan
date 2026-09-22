#version 450
// PS5 Vulkan - R9's hard-coded twin of shaders/v0/spec.frag.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same expression as spec.frag with one set of specialization values written
// as literals (tint_red = false, level = 1): a module compiled from this source has
// to come out byte-identical to spec.frag compiled with those values through
// VkSpecializationInfo, which is what proves the constants are applied rather than
// ignored (driver/tests/vk_v0_spec_test.c; R9 of the port's requests).

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(0.0, 64.0 / 255.0, 1.0, 1.0);
}
