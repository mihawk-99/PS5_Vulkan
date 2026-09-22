#version 450
// PS5 Vulkan - R2's separated sampler and image: one texture, two sets, two types.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// R2 (PS5_vkQuake's PS5_VULKAN_REQUESTS.md, and docs/VULKAN_PROBE_ACTIVE.md): the
// separated form Vulkan's guidance recommends and every renderer reaching for one
// independent sampler writes. The image and the sampler are two descriptors in two
// sets -- `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` at set 0, binding 0 and
// `VK_DESCRIPTOR_TYPE_SAMPLER` at set 1, binding 0 -- and one texture is fetched
// through them.
//
// Nothing here is optional. The two declarations are what make the SPIR-V carry a
// separate texture and sampler operand into OpSampledImage; a driver that writes
// only the image half or only the sampler half still draws a picture, so the scene
// is deliberately the plainest one there is (one texture, nearest sampling, the
// vertex stage's own coordinate) and the *evidence* is elsewhere: the frame this
// program draws is compared texel for texel with the frame
// probes/m3-texture's combined image sampler draws of the same texture. Only the
// identity distinguishes "the separated pair fetched through its own halves" from
// "something was drawn"; a check that merely renders would accept both.
//
// The nearest sampler is the canary's: a bilinear one would blend the same texels
// differently between the two forms only if the sampler state were read from the
// wrong half, which is what the comparison is looking for.

layout(set = 0, binding = 0) uniform texture2D bridge_texture;
layout(set = 1, binding = 0) uniform sampler bridge_sampler;

layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 color;

void main()
{
    color = texture(sampler2D(bridge_texture, bridge_sampler), texture_coordinate);
}
