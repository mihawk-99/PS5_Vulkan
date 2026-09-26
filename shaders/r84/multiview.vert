// PS5 Vulkan compatibility probe - R84: a multiview draw's vertex stage.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The band quad squeezed into one half of the target, the half chosen by the
// view being drawn: view 0 the left, view 1 the right. A view index that did
// not reach this stage draws both views into the same half.
#version 450
#extension GL_EXT_multiview : require

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 coordinate;
layout(location = 0) out vec2 texture_coordinate;

void main()
{
    float half_offset = gl_ViewIndex == 0 ? -0.5 : 0.5;
    gl_Position = vec4(position.x * 0.5 + half_offset, position.y, 0.0, 1.0);
    texture_coordinate = coordinate;
}
