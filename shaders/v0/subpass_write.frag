#version 450
// PS5 Vulkan - R10 subpass input probe, first subpass: write a positional colour.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Subpass 0 of a two-subpass render pass draws this over the whole of colour
// attachment 0. The colour is a function of the fragment's own position -- four
// vertical bands in R and four horizontal bands in G -- so that what subpass 1
// reads back says *where* it read, not merely that it read something:
//
//   R = floor(x / 960) / 3   ->  0, 85, 170, 255 over the four quarter-widths
//   G = floor(y / 540) / 3   ->  0, 85, 170, 255 over the four quarter-heights
//   B = 128/255, A = 1
//
// Every value is an exact multiple of 1/255, so an RGBA8 UNORM target stores
// them with no rounding ambiguity, and a read that is transposed, flipped,
// offset by a tile or taken from the wrong attachment lands in a different band
// than the one it should (shaders/v0/subpass_read.frag is the reader).
//
// No descriptors and no vertex inputs beyond the full-target triangle's.

layout(location = 0) out vec4 color;

void main()
{
    const float band_x = floor(gl_FragCoord.x / 960.0);
    const float band_y = floor(gl_FragCoord.y / 540.0);
    color = vec4(band_x / 3.0, band_y / 3.0, 128.0 / 255.0, 1.0);
}
