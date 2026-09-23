#version 450
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
// A 256-square image and 1/64 coordinate derivatives select LOD 2.
// Only the sampler bias varies between frames; every pixel must agree.
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) out vec4 color;
void main()
{
    color = texture(image, gl_FragCoord.xy / 64.0);
}
