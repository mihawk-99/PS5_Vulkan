#version 450
// PS5 Vulkan - V0-formats' unsigned integer colour targets.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fullscreen triangle's fragment stage for a UINT colour target: a constant
// integer colour, distinct in every channel and small enough that every width
// this case renders into (8, 10, 16 and 32 bits) carries it exactly. The driver
// compiles it for the export Mesa's ac_choose_spi_color_formats picks for an
// unsigned integer target, SPI_SHADER_UINT16_ABGR, and the console reads the
// target's storage back.
layout(location = 0) out uvec4 color;

void main()
{
    color = uvec4(0x40u, 0x80u, 0xc0u, 0xffu);
}
