#version 450
// PS5 Vulkan - round 12's stencil setup pass.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One of the two pipelines the v0-stencil frames draw with. This one writes the
// stencil plane: its pipeline state has the stencil test enabled with FUNC
// ALWAYS and PASS REPLACE over the whole reference, so every fragment the
// geometry rasterises stores the reference into the plane, and its fragment
// shader writes gl_FragDepth 0.75 into the depth plane. The colour names the
// pass in the frame:
// red is what the setup pass leaves, so a green frame is the test pass passing
// and an all-red frame is it failing.

layout(location = 0) in vec4 vertex_colour;
layout(location = 0) out vec4 color;

void main()
{
    // The depth plane of the combined format: 0.75, which the test pass's 0.25
    // passes LESS against and its deep twin's 0.9 does not.
    gl_FragDepth = 0.75;
    color = vec4(1.0, 0.0, 0.0, 1.0);
}
