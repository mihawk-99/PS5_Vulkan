#version 450
// PS5 Vulkan - R9 push-constant colour.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The colour comes from a uniform block at set 0, binding 32 -- the reserved
// binding this driver's push constants reach a stage through
// (PS5VK_PUSH_CONSTANT_BINDING, driver/ps5vk_private.h: libpsbc's gallium UBO
// slot 0 at PSBC_GALLIUM_UBO_BINDING_BASE). A pipeline layout that declares a
// push-constant range for this stage has the driver write the block's 16 bytes
// from vkCmdPushConstants, so two draws that upload different values must read
// back as different exact colours -- and a driver that ignored the upload would
// draw the first value twice. The CPU writes channels that are exact multiples
// of 1/255, so an RGBA8 UNORM target stores them without rounding ambiguity,
// exactly as the m3 uniform-buffer probe does.

layout(push_constant) uniform PushColour
{
    vec4 value;
} push_colour;

layout(location = 0) in vec4 vertex_colour;
layout(location = 0) out vec4 color;

void main()
{
    // The vertex stage's colour is carried so the pipeline still has an
    // interface to match; the push constant is what this stage exports, which is
    // what makes the readback a statement about the upload.
    color = push_colour.value + vec4(0.0, 0.0, 0.0, vertex_colour.a * 0.0);
}
