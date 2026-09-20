#version 450
// PS5 Vulkan - V0-robust: an out-of-bounds uniform read.
//
// This shader does NOT compile with the AGC compiler: it refuses any uniform
// block larger than 16 bytes ("Shader compilation failed: internal error"), so
// the read at offset 16 here is not expressible and V0-robust's reachable path
// is the driver's index-count clamp (docs/M5_PHASE_C.md, V0-robust). It is kept
// as the record of what that path would have measured.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The uniform buffer is declared as two float4s and this stage reads the second
// one, at offset 16. The probe binds the same buffer through the same program
// twice: once with a range that covers both (the read is inside the bound and
// returns the buffer's own colour) and once with a range that covers only the
// first (the read is out of bounds). Vulkan 1.0's robustBufferAccess requires
// the second to be safe -- no fault, and no bytes from outside the buffer's
// bound -- so its frame must come back zero where the first came back the
// buffer's colour (src/diagnostics.cpp, run_vulkan_robust_frames).

layout(set = 0, binding = 0) uniform Bounds
{
    vec4 values[2];
} bounds;

layout(location = 0) out vec4 color;

void main()
{
    color = bounds.values[1];
}
