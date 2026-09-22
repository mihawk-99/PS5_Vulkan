#version 450
// PS5 Vulkan - R7 multi-set colour in vkQuake's shape.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vkQuake's world and md5 pipeline layouts declare five descriptor sets; its
// texture sets collapse into one set of three combined image samplers at set 0,
// bindings 0, 1 and 2, and the frame's uniform block lives in the next set. This
// program is that shape in the smallest form that shows each value: the three
// samplers supply one channel each and set 1's block tints the result.
//
// Every part of it is load-bearing. A driver that keeps one table per stage
// cannot hold three 48-byte image-sampler entries at set 0 *and* the 16-byte
// uniform entry at set 1; a driver that writes one user-data pointer leaves one
// of the two sets reading an unwritten SGPR, which this hardware reads as zero
// -- a black frame, or a frame with a missing channel, rather than a refusal
// (docs/M5_PHASE_C.md, R9). The tint's green is 64/255 rather than a power of
// two so that no single wrong binding can produce the same word.
//
// The three textures are one texel value each, sampled with the vertex stage's
// own coordinate, so the readback is exactly tint * (texel_r.r, texel_g.g,
// texel_b.b, 1) whatever the sampling positions are.

layout(set = 0, binding = 0) uniform sampler2D red_image;
layout(set = 0, binding = 1) uniform sampler2D green_image;
layout(set = 0, binding = 2) uniform sampler2D blue_image;

layout(set = 1, binding = 0) uniform TintBlock
{
    vec4 tint;
} tint_block;

layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    color = tint_block.tint * vec4(texture(red_image, texture_coordinate).r,
                                   texture(green_image, texture_coordinate).g,
                                   texture(blue_image, texture_coordinate).b, 1.0);
}
