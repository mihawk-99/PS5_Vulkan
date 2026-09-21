#version 450
// PS5 Vulkan - R7 multi-set colour: one binding taken from each of two sets.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two sets, and two *kinds* of binding: set 0 binding 0 is a uniform block (the
// colour) and set 1 binding 0 is a combined image sampler (the scale). Either
// half alone can be compiled by a driver that keeps one set-0 table, so the two
// together are what say the compiler built a layout per set: set 1's table has
// to be sized from an image-sampler binding (48-byte entries, no uniform block)
// and its pointer is a second user-data dword the metadata has to name
// (tooling/psbc/patch-descriptor-sets.py, docs/M5_PHASE_C.md, R7).
//
// The scale texture is one texel, sampled at the vertex stage's coordinate, so
// the fragment's colour is the block's colour times a value the console can set
// exactly: a driver that bound set 1 wrongly, or left its pointer unwritten, has
// nowhere to get that value from and the readback is not the expected colour.

layout(set = 0, binding = 0) uniform ColourBlock
{
    vec4 colour;
} colour_block;

layout(set = 1, binding = 0) uniform sampler2D scale_image;

layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    color = colour_block.colour * texture(scale_image, texture_coordinate).r;
}
