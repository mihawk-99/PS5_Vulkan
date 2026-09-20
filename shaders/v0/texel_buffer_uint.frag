#version 450
// PS5 Vulkan - V0-formats' uniform texel buffers.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The full-target triangle's pixel stage fetching a uniform texel buffer: a
// samplerBuffer at set 0, binding 0, which is
// VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER -- a buffer view the driver turns
// into a hardware V# (docs/BLOCKERS.md, the descriptor types).
//
// The buffer holds four texels, one per quarter of the target's width, so the
// quarter a fragment sits in is the element it fetches: the red channel is that
// element's first component, so a fetch that ignores the index, reads the wrong
// stride or runs off the end of the view lands in the wrong quarter or holds the
// wrong value. The green channel is element **0**'s first component, fetched
// with a constant index, which is what tells a wrong *index* apart from a fetch
// that returns nothing at all, and the blue channel is a constant the shader
// always writes, which is what proves the frame itself drew. 960 is the target's
// width (kOutputWidth, src/diagnostics.cpp) over four.

layout(set = 0, binding = 0) uniform usamplerBuffer texels;
layout(location = 0) out vec4 color;

void main()
{
    const int index = int(gl_FragCoord.x) / 960;
    const uvec4 constant = texelFetch(texels, 0);
    const uvec4 quarter = texelFetch(texels, index);
    color = vec4(float(quarter.r) / 255.0, float(constant.r) / 255.0, 0.25, 1.0);
}
