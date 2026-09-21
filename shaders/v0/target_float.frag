#version 450
// PS5 Vulkan - the single-channel 32-bit float colour target.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fullscreen triangle's fragment stage for an R32_SFLOAT target. Its
// texel is one 32-bit float, and the driver compiles this stage for the export
// the single-channel 32-bit rows name (SPI_SHADER_32_R), so the value has to sit
// in the red channel and be exact in binary: 0.75 is 0x3F400000, which the
// console's readback compares word for word. A driver that exported the wrong
// channel -- or a hardware path that encoded the float some other way -- lands a
// different word in the target, and the case says so.
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(0.75, 0.0, 0.0, 1.0);
}
