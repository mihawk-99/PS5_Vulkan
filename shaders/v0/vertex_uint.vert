#version 450
// PS5 Vulkan - V0-formats: a three-component unsigned-integer vertex position.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The position attribute is R32G32B32_UINT (12 bytes at offset 0), the colour
// the m3-vertex square's R32G32B32A32_SFLOAT at offset 16, records 32 bytes
// apart. An unsigned component has no negative value to hold, so x and y are 0
// and 1 and half a unit is subtracted -- exactly representable, so the square
// lands where the float-format frames put it -- and the third component becomes
// the fragment's alpha, which is what proves the third component is fetched and
// not padding: it rises from 0 to 1 across the square like green does.

layout(location = 0) in uvec3 position;
layout(location = 1) in vec4 colour;

layout(location = 0) out vec4 vertex_colour;

void main()
{
    gl_Position = vec4(vec2(position.xy) - vec2(0.5), 0.0, 1.0);
    vertex_colour = vec4(colour.rgb, float(position.z));
}
