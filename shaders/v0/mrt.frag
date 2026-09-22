#version 450
// PS5 Vulkan - R7 step 1b's MRT probe: one fragment output per colour attachment.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `VkPhysicalDeviceLimits.maxColorAttachments` is four and this driver
// advertises it, so a rendering may declare up to four colour attachments and a
// fragment shader may write one output per attachment. This program writes all
// four, unconditionally, with a *different* constant in each: a driver that
// programmed one target and copied it into the others, or wired two outputs to
// one attachment, produces a picture that a single-output probe would accept and
// this one cannot -- every attachment's readback is one of four values that no
// other attachment's data can be mistaken for.
//
// The attachment count is the **frame's** choice, not this shader's: the same
// program is drawn against one, two and the device's advertised maximum
// attachment count, and the pipeline's colour-blend state (one
// VkPipelineColorBlendAttachmentState per live attachment) is what declares how
// many of the four outputs reach a target. Vulkan allows a shader to write
// outputs for locations no attachment reads, which is exactly what makes one
// program serve all three frame shapes.
//
// No descriptors and no inputs: the outputs are constants, so a wrong readback
// cannot come from a texture, a uniform or an interpolation.

layout(location = 0) out vec4 color0;
layout(location = 1) out vec4 color1;
layout(location = 2) out vec4 color2;
layout(location = 3) out vec4 color3;

void main()
{
    color0 = vec4(1.0, 0.0, 0.0, 1.0);
    color1 = vec4(0.0, 1.0, 0.0, 1.0);
    color2 = vec4(0.0, 0.0, 1.0, 1.0);
    color3 = vec4(1.0, 1.0, 1.0, 1.0);
}
