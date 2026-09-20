#version 450
// PS5 Vulkan - V0-formats' signed integer colour targets.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The signed twin of shaders/v0/target_uint.frag: a constant integer colour
// whose channels are positive and distinct, so every width this case renders
// into (8, 16 and 32 bits signed) carries them exactly. The driver compiles it
// for SPI_SHADER_SINT16_ABGR, the export Mesa's ac_choose_spi_color_formats
// picks for a signed integer target.
layout(location = 0) out ivec4 color;

void main()
{
    color = ivec4(0x20, 0x40, 0x60, 0x7f);
}
