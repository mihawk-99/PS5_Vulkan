#version 450
// PS5 Vulkan - V0-formats' signed integer sampled formats.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The unsigned probe's twin: a SINT format needs an isampler, and the texels
// the case uploads are non-negative so the RGBA8 target can carry them.
layout(set = 0, binding = 0) uniform isampler2D image;
layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(texture(image, texture_coordinate)) / 255.0;
}
