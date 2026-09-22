#version 450
// PS5 Vulkan - R10 subpass input probe, second subpass: read it and write it out.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Subpass 1 draws over the whole of colour attachment 1 -- a target subpass 0
// never wrote and never reads -- with what `subpassLoad` returns from colour
// attachment 0, the subpass's own input attachment. The value is written
// unchanged, so the second target's readback *is* the first target's content,
// band for band (shaders/v0/subpass_write.frag is the writer): a read that
// missed, that came from a tile or a stale row, or that was taken at the wrong
// coordinate shows up as a band that is not there.
//
// The access is a texel fetch through the input attachment's own descriptor:
// this driver binds one 32-byte image entry per input attachment binding, built
// from the subpass's attachment view (driver/ps5vk_draw.c), and the subpass
// boundary is where the first subpass's writes are flushed (the colour-buffer
// barrier and the submission split the draw that samples a target this command
// buffer rendered into already carries).
//
// B is written as the input's blue band, so a target that was never written by
// subpass 0 -- the clear colour, say -- cannot be mistaken for a successful
// read: the check is against the writer's own values, channel by channel.

layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput input_colour;

layout(location = 0) out vec4 color;

void main()
{
    const vec4 read = subpassLoad(input_colour);
    // A read that returns nothing writes magenta instead of black, so the
    // frame itself says which of the two happened: the writer's bands are never
    // magenta (its blue channel is 128/255), and magenta is not the clear
    // colour either (src/diagnostics.cpp, run_vulkan_subpass_frames). A driver
    // whose read is broken then fails with a word that names the failure rather
    // than with a black frame that a clear could also have produced.
    color = (read.r + read.g + read.b + read.a) > 0.0 ? read : vec4(1.0, 0.0, 1.0, 1.0);
}
