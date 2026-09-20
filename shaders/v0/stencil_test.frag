#version 450
// PS5 Vulkan - round 12's stencil test pass.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The other pipeline the v0-stencil frames draw with: the stencil test enabled
// with FUNC EQUAL against the caller's reference, PASS and FAIL both KEEP, so
// this pass's fragments reach the colour target only where the plane holds the
// reference the setup pass wrote -- and where its own depth test passes. Its
// colour is the frame's signal: green means the stencil plane held the
// reference, red (the setup pass's own colour) means this pass was rejected.

layout(location = 0) in vec4 vertex_colour;
layout(location = 0) out vec4 color;

void main()
{
    // 0.25 is in front of the 0.75 the setup pass wrote, so the frame's depth
    // test passes wherever the stencil test did.
    gl_FragDepth = 0.25;
    color = vec4(0.0, 1.0, 0.0, 1.0);
}
