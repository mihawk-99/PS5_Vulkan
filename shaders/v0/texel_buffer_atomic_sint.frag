#version 450
// PS5 Vulkan - R32_SINT atomics through a storage texel buffer.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CTS requires VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT for
// R32_SINT (dEQP-VK.api.info.format_properties.r32_sint), and this stage is the
// probe that earns it: an imageAtomicAdd through the same uimageBuffer the
// store probe writes with imageStore. The texel each fragment touches is the one
// its 960-pixel band of the target names, so a full-screen triangle adds exactly
// one per fragment and the buffer's four texels must hold their band's fragment
// count -- a number the case computes rather than assumes, and one no
// non-atomic path can produce (a store would leave the last fragment's value,
// and a dropped atomic its initial zero).
// An atomic image is read as well as written, so it cannot be 'writeonly' and
// GLSL's 1.0 rule then requires the format qualifier.
layout(set = 0, binding = 0, r32i) uniform iimageBuffer texels;
layout(location = 0) out vec4 color;

void main()
{
    const int index = int(gl_FragCoord.x) / 960;
    imageAtomicAdd(texels, index, 1);
    color = vec4(0.5, 0.5, 0.25, 1.0);
}
