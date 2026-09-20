#version 450
// PS5 Vulkan - V0-formats' unsigned integer sampled formats.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same square as shaders/m3/texture.frag, sampling an unsigned integer
// texture: a UINT format needs a usampler and a fetch whose value is the
// integer itself, not a normalized one. The frame's target is R8G8B8A8_UNORM,
// so the shader writes the fetched value over 255 -- the byte the target keeps
// is the integer the texel held, which is what the case's expectation names.

layout(set = 0, binding = 0) uniform usampler2D image;
layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(texture(image, texture_coordinate)) / 255.0;
}
