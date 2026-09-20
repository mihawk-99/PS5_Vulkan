#version 450
// PS5 Vulkan - M2 solid colour.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each channel is an exact multiple of 1/255, so an RGBA8 UNORM target stores
// R=0x20, G=0xA0, B=0xFF, A=0xFF with no rounding ambiguity. Read back as a
// B,G,R,A-byte word that is 0xff20a0ff.

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(32.0 / 255.0, 160.0 / 255.0, 255.0 / 255.0, 1.0);
}
