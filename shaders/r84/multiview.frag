// PS5 Vulkan compatibility probe - R84: a multiview draw's pixel stage.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Red in view 0 and green in view 1: the view index, read by this stage too.
#version 450
#extension GL_EXT_multiview : require

layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    color = gl_ViewIndex == 0 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0);
}
