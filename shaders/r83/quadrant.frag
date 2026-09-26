// PS5 Vulkan compatibility probe - R83: the pass that writes a depth/stencil
// attachment's two planes in one quadrant of the target.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every fragment outside the lower right quadrant (x >= 1920, y >= 1080 of the
// 3840x2160 target) is discarded, so the depth test's ALWAYS write of the
// quad's depth 0 and the stencil test's REPLACE land only there, and the rest
// of the attachment keeps the pass's clear. The detached pass then samples the
// attachment and must find that quadrant, texel for texel.
#version 450
layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;
void main()
{
    if (gl_FragCoord.x < 1920.0 || gl_FragCoord.y < 1080.0)
        discard;
    color = vec4(texture_coordinate, 0.0, 1.0);
}
