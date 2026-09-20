#version 450
// PS5 Vulkan - C1 clear rectangle.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mesa's rectangle vertex shader for meta draws (vk_meta_draw_rects_vs_nir),
// which the driver's clears go through. Each rectangle is six vertices, two
// triangles, in a vertex buffer the command buffer fills; the position comes
// from the first three components, whose bits vk_meta writes as floats, so
// NIR reads them straight as a vec4 and this shader reinterprets them.
//
// vk_meta's shader also writes gl_Layer from gl_InstanceIndex and the fourth
// component. The driver renders one layer (vkCmdBeginRendering refuses
// layerCount != 1), so that output is always zero and the driver drops it
// before compiling; this probe records the vertex stage the driver ends up
// with.

layout(location = 0) in uvec4 vtx_in;

void main()
{
    gl_Position = vec4(uintBitsToFloat(vtx_in.xyz), 1.0);
}
