#version 450
// PS5 Vulkan - R71 dual-source blending.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the interpolated vertex colour as the first source and a constant as
// the second (location 0, index 1), which a pipeline blending with the SRC1
// factors reads: the compiler exports the second source to MRT1 with MRT0's
// format (psbc_compile.c). The constant is the one the blend-constant probe
// used, so a frame blended with SRC1_COLOR and ONE_MINUS_SRC1_COLOR is that
// probe's frame, word for word.

layout(location = 0) in vec4 vertex_colour;
layout(location = 0, index = 0) out vec4 color;
layout(location = 0, index = 1) out vec4 factor;

void main()
{
    color = vertex_colour;
    factor = vec4(0.25, 0.5, 0.75, 1.0);
}
