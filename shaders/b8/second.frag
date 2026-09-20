#version 450
// PS5 Vulkan - B8 second colour.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The colour of a frame's second draw in Phase B8, distinct from M2's. Each
// channel is an exact multiple of 1/255, so an RGBA8 UNORM target stores
// R=0xF0, G=0x30, B=0x60, A=0xFF exactly.

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(240.0 / 255.0, 48.0 / 255.0, 96.0 / 255.0, 1.0);
}
