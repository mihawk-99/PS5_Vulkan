#version 450
// PS5 Vulkan - round 12's stencil depth control.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stencil test pass's own twin for the frame that has to fail the DEPTH
// test: the same green and the same stencil state as stencil_test.frag, with
// gl_FragDepth 0.9 behind the 0.75 the setup pass wrote, so a fragment that
// passes the stencil test is rejected by the depth comparison. A frame that
// stays red with this shader is the depth half of the combined attachment
// working; without it, "green" could not tell a depth pass from a depth test
// that never happened.

layout(location = 0) in vec4 vertex_colour;
layout(location = 0) out vec4 color;

void main()
{
    gl_FragDepth = 0.9;
    color = vec4(0.0, 1.0, 0.0, 1.0);
}
