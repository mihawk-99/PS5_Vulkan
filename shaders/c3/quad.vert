#version 450
// PS5 Vulkan - C3 transformed quad vertex stage.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Position comes from a CPU-filled vertex buffer (8-byte records: vec2
// position at offset 0) and the transform from a 16-byte uniform buffer at set
// 0, binding 0, whose xy scales the position and whose zw offsets it. Where
// the quad lands therefore depends on both the bound vertex buffer and the
// bound descriptor: the first probe set with a vertex stage that reads a
// descriptor as well as vertex attributes, which is what Phase C3's
// transformed quad needs.
//
// No varyings are written, and no built-in beyond gl_Position is read, so the
// pixel stage cannot depend on vertex outputs.

layout(location = 0) in vec2 position;

layout(set = 0, binding = 0) uniform Transform
{
    vec4 scale_offset;
} transform;

void main()
{
    gl_Position = vec4(position * transform.scale_offset.xy + transform.scale_offset.zw,
                       0.0, 1.0);
}
