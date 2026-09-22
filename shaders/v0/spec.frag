#version 450
// PS5 Vulkan - R9's specialization-constant probe: the output colour is chosen by
// the pipeline's own values.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Four pipelines are built from this one module, and what tells them apart is the
// VkSpecializationInfo the application attaches to the stage: a bool that selects
// the red channel and an int that selects the green one, so a frame read back says
// *which* set of values arrived rather than merely that two frames differ. The
// values themselves are the pipeline's, not the shader's: OpSpecConstant is
// rewritten before translation and OpSpecConstantOp is folded (R9 of the port's
// requests; another application would use the same mechanism for its own variants,
// which is why this is the general case and not a probe-only path).
//
// The hard-coded twin beside it, shaders/v0/spec_hardcoded.frag, is the same
// expression with one set of values written as literals: a compile of the two has
// to come out the same, which is the compiler half of the acceptance.

layout(constant_id = 0) const bool tint_red = false;
layout(constant_id = 1) const int level = 0;

layout(location = 0) out vec4 color;

void main()
{
    // Bytes a readback can compare exactly: 64 and 128 are whole 1/255 steps.
    color = vec4(tint_red ? 1.0 : 0.0,
                 level == 1 ? 64.0 / 255.0 : (level == 2 ? 128.0 / 255.0 : 0.0), 1.0, 1.0);
}
