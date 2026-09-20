#version 450
// PS5 Vulkan - V0-formats' storage texel buffers.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The full-target triangle's pixel stage *storing* into a storage texel buffer:
// a writeonly imageBuffer at set 0, binding 0, which is
// VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER -- the same 16-byte buffer descriptor
// a samplerBuffer fetch reads, written as a hardware V# by the driver
// (docs/BLOCKERS.md, the descriptor types).
//
// The buffer holds four texels, one per quarter of the target's width, so the
// quarter a fragment sits in is the element it stores into. The case reads the
// buffer's own memory back after the frame and compares every texel with the
// encoding of its level, so a store that ignores the index, writes the wrong
// stride or runs off the end of the view lands in the wrong texel or leaves a
// texel at its initial zero. The red channel carries the same level and blue the
// 0.25 constant, so a frame that stores nothing still shows whether it drew.
// 960 is the target's width (kOutputWidth, src/diagnostics.cpp) over four.

layout(set = 0, binding = 0) uniform writeonly imageBuffer texels;
layout(location = 0) out vec4 color;

// 0x20, 0x40, 0x60 and 0x7f of 255, the four levels the case's expected texels
// hold. The literals carry every significant digit of level/255, so the
// correctly rounded float32 they parse to is the one the case's own division
// produces: a literal a digit short lands one ULP lower, which a 32-bit float
// texel keeps (Klog_Logs/v0-texel-buffer-store-run1.log), and a division done in
// the shader is the compiler's to turn into a reciprocal multiply, which lands
// one ULP higher for 96 and 127 (run 2).
const float levels[4] = float[](0.125490203499794, 0.250980406999588, 0.3764705955982208, 0.49803921580314636);


void main()
{
    const int index = int(gl_FragCoord.x) / 960;
    const float level = levels[index];
    imageStore(texels, index, vec4(level));
    color = vec4(level, 0.5, 0.25, 1.0);
}
