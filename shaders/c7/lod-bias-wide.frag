#version 450
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
// R79: a 512-square image and 1/16 coordinate derivatives select LOD 5, so
// biases from -5 to +4 each land on their own level of the ten, and larger
// ones clamp to the chain's ends. Only the sampler bias varies between frames;
// every pixel must agree.
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) out vec4 color;
void main()
{
    color = texture(image, gl_FragCoord.xy / 16.0);
}
