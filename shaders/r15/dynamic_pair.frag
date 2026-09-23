#version 450
// PS5 Vulkan - independent dynamic offsets around a static binding.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
layout(set = 0, binding = 0) uniform First { vec4 value; } first;
layout(set = 0, binding = 2) uniform Static { vec4 value; } fixed_colour;
layout(set = 0, binding = 5) uniform Second { vec4 value; } second;
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(first.value.r, second.value.g, fixed_colour.value.b, 1.0);
}
