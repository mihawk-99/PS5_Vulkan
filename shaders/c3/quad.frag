#version 450
// PS5 Vulkan - C3 transformed-quad colour.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A constant colour with no inputs, so a readback depends only on where the
// transformed quad landed. The channels are the M3 canary purple, exact
// multiples of 1/255, so an RGBA8 UNORM target stores R=0x80, G=0x40, B=0xC0,
// A=0xFF with no rounding ambiguity: read back as a B,G,R,A-byte word it is
// 0xffc04080.

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(0x80 / 255.0, 0x40 / 255.0, 0xc0 / 255.0, 1.0);
}
