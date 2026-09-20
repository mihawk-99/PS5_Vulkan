#version 450
// PS5 Vulkan - C8 sample positions.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the sample's own position inside its pixel and its index, so a
// four-sample target's storage names where each of its four sample words sits:
// red and green are the position within the pixel (0 at its left/top edge, 255
// at its right/bottom one), blue the sample index over eight, alpha one. A
// fragment shader that reads gl_SamplePosition runs per sample, which is what
// makes the four words differ.

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(gl_SamplePosition, float(gl_SampleID) / 8.0, 1.0);
}
