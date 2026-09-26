/*
 * PS5 Vulkan driver - formats and images.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B3 (docs/M5_PHASE_B.md).
 *
 * Formats: only formats whose features the hardware has shown through the test
 * runner are reported. The specification's mandatory format table is far
 * larger (Vulkan-Docs v1.4.354, chapters/formats.adoc, SHA-256 9a0c780e...,
 * lines 3248-3744); formats join as probes prove them.
 *
 * Images: storage follows ps5-opengl's resource layouts (ps5_resource_layout
 * and ps5_resource_create_unlocked in src/gallium/ps5/ps5_screen.c,
 * GPL-3.0-or-later), which the runner matched on the hardware:
 * - Rows: images that are not attachments store each level row-major, rows
 *   padded to 256 bytes (the M3 64x36 texture).
 * - Tiles: colour and depth attachments store each level in tiles of 64 KiB,
 *   128x128 texels for 4-byte texels, rounded up to 2 MiB and 2 MiB-aligned
 *   (the M2-M4 3840x2160 colour and D32 depth targets).
 * Only single-level, single-layer, single-sample 2D storage has been read back
 * on the hardware. Mip chains, array layers and 4x storage are sized with the
 * same rules (per level, per layer, and ps5-opengl's 4x tile sizes); their
 * texel placement is proven by the probes that first render into them.
 *
 * Phase C4 adds the two halves of the texture path an application samples
 * through: the sampler object, whose state is exactly what the M3 texture
 * canary ran (clamp-to-edge, nearest or linear, one level), and the staging
 * upload's vkCmdCopyBufferToImage, which lands on
 * ps5vk_CmdCopyMemoryToImageKHR as a CPU copy into the row layout above.
 */

#include "ps5vk_private.h"
#include "ps5vk_debug.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "util/format/u_format.h"
#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_util.h"

/* ps5-opengl's PS5_RENDER_ALIGNMENT and tile size. */
#define PS5VK_TILED_ALIGNMENT UINT64_C(0x200000)
#define PS5VK_TILE_BYTES UINT64_C(0x10000)
/* Rows pad to this many bytes; images need it as their alignment, since the
 * descriptor rule keeps a zero low byte in every texture address. */
#define PS5VK_ROW_ALIGNMENT UINT64_C(256)

/* Allowed Extent Values and the limits of ps5vk_physical_device.c: the
 * complete mip chain of a 4096x4096 image, and maxImageArrayLayers. */
#define PS5VK_MAX_EXTENT_2D 16384
#define PS5VK_MAX_MIP_LEVELS 13
#define PS5VK_MAX_ARRAY_LAYERS 256

static const struct ps5vk_format ps5vk_formats[] = {
   /* Colour target (M2), sampled nearest exactly and bilinear within one
    * level (M3 step 3), rendered to and then sampled (M4), and copied --
    * one-to-one blitted included -- row layout to tile layout and read back
    * (C7's copy frames). Blending with the programmed state alone is incomplete
    * (M4 step 2), so there is no COLOR_ATTACHMENT_BLEND. Sampled formats must
    * also support transfer (formats.adoc, maintenance1). */
   {VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    56 /* 8_8_8_8_UNORM */, PS5VK_FORMAT_SWIZZLE_RGBA},
   /* V0-formats: the same 4-byte layout under its other Vulkan name, the sRGB
    * and signed-normalised forms of it, the 1- and 2-channel 8-bit formats, and
    * the 16- and 32-bit float and 16-bit unorm families. Each is sampled by the
    * v0-formats probe on the console and reported with the features the fetch
    * proves; the descriptor's format word is the register database's value and
    * its swizzle the fill-in rule for the channels the format does not have
    * (ps5vk_private.h, PS5VK_FORMAT_SWIZZLE_R001). */
   {VK_FORMAT_A8B8G8R8_UNORM_PACK32,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    56 /* 8_8_8_8_UNORM */, PS5VK_FORMAT_SWIZZLE_WZYX},
   /* B8G8R8A8_SRGB (round 13): the byte-reversed sRGB form. Its memory order is
    * B, G, R, A, so the fetch's first three components -- the ones the console's
    * sRGB curve linearises, before any selector -- are the format's whole colour
    * triple, and the ZYXW selectors B8G8R8A8_UNORM already uses put them in the
    * right outputs. That is what pid 160's measurement of the *packed*
    * A8B8G8R8_SRGB_PACK32 does not hold for (docs/BLOCKERS.md, "Which of the two
    * rows the measurement actually refutes"), and the runner's
    * v0-formats-sampled, c7-blit-formats, v0-blit-dst and v0-transfer-formats
    * cases are what prove it, and round 14's v0-targets frames prove the
    * colour-attachment pair through its CB_COLOR0_INFO word (8_8_8_8 with the
    * sRGB number type and SWAP_ALT) and the blended frame's export path. */
   {VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
    0, 130 /* 8_8_8_8_SRGB */, PS5VK_FORMAT_SWIZZLE_ZYXW},
   {VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    0, 130 /* 8_8_8_8_SRGB */, PS5VK_FORMAT_SWIZZLE_RGBA},
   {VK_FORMAT_R8G8B8A8_SNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    57 /* 8_8_8_8_SNORM */, PS5VK_FORMAT_SWIZZLE_RGBA},
   /* The packed byte-reversed sRGB format (round 17). Its Vulkan layout is A, B,
    * G, R -- red last -- and the console's sRGB curve covers the first three
    * *fetched* components, so a texel stored that way could never have its red
    * linearised (pid 160, docs/HARDWARE_FINDINGS.md). The image therefore stores
    * it the way its R8G8B8A8_SRGB twin is stored (storage_reversed, the four
    * bytes swapped at every application boundary), and the descriptor's own word
    * is that twin's: the register database's 8_8_8_8_SRGB (130) with the
    * straight RGBA selectors, which curves R, G and B and leaves A alone. */
   {VK_FORMAT_A8B8G8R8_SRGB_PACK32,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
    0, 130 /* 8_8_8_8_SRGB */, PS5VK_FORMAT_SWIZZLE_RGBA, true},
   {VK_FORMAT_A8B8G8R8_SNORM_PACK32,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    57 /* 8_8_8_8_SNORM */, PS5VK_FORMAT_SWIZZLE_WZYX},
   /* The sixteen-bit SNORM layouts (blocker round 2): the hardware's words 8, 24
    * and 66 fetch them and the vertex descriptor takes them; their sampled and
    * blit bits are conditional requirements no probe has claimed, so the entries
    * carry the vertex buffer alone. */
   {VK_FORMAT_R16_SNORM, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, 8 /* 16_SNORM */,
    PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R16G16_SNORM, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, 24 /* 16_16_SNORM */,
    PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R16G16B16A16_SNORM, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,
    66 /* 16_16_16_16_SNORM */, PS5VK_FORMAT_SWIZZLE_RGBA},
   {VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 15 /* 8_8_SNORM */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R8_SNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 2 /* 8_SNORM */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R8_UNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 1 /* 8_UNORM */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R8G8_UNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 14 /* 8_8_UNORM */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R16_UNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, 7 /* 16_UNORM */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R16G16_UNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, 23 /* 16_16_UNORM */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R16G16B16A16_UNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, 65 /* 16_16_16_16_UNORM */, PS5VK_FORMAT_SWIZZLE_RGBA},
   {VK_FORMAT_R16_SFLOAT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 13 /* 16_FLOAT */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R16G16_SFLOAT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 29 /* 16_16_FLOAT */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    71 /* 16_16_16_16_FLOAT */, PS5VK_FORMAT_SWIZZLE_RGBA},
   {VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    /* The CTS requires three buffer bits for R32_SFLOAT
     * (dEQP-VK.api.info.format_properties.r32_sfloat), and all three are here
     * because the console proved each: the two texel-buffer halves in round 8
     * (v0-formats-texel-buffer, v0-formats-texel-buffer-store) and VERTEX_BUFFER
     * in round 9, where ps5vk_vertex_formats gained its row and the
     * v0-vertex-bytes-float case fetched a single R32_SFLOAT attribute
     * (docs/M5_PHASE_C.md). */
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT | VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    22 /* 32_FLOAT */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    /* Vertex positions and texture coordinates too (M3 steps 2 and 3): one
     * entry per format, so a format that is both carries both. */
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT, 64 /* 32_32_FLOAT */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R32G32B32A32_SFLOAT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    /* Colours and 3D positions too (M3, M4). */
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    77 /* 32_32_32_32_FLOAT */, PS5VK_FORMAT_SWIZZLE_RGBA},
   /* V0-formats' packed families: two- and four-byte texels whose components are
    * not byte-aligned. The hardware format is chosen by the component sizes --
    * GFX10_FORMAT_5_6_5, _1_5_5_5, _4_4_4_4, _5_9_9_9_FLOAT, _10_11_11_FLOAT,
    * _2_10_10_10_UNORM (the pinned register database, and Mesa's ac_formats.c
    * picks the same words) -- and the channel order is the descriptor's DST_SEL,
    * which the sampled-format probe's console frame proves: R5G6B5's memory is
    * B, G, R, so R comes from Z, and so on. The shared-exponent and mantissa
    * forms (E5B9G9R9, B10G11R11) need no exponent handling here: the hardware
    * decodes them. */
   /* V0-formats' unsigned integer families (a usampler probe, run
    * v0-formats-sampled-uint). The word is the register database's UINT form of
    * the same field widths; an integer format is never linearly filtered, so
    * the entry claims no SAMPLED_IMAGE_FILTER_LINEAR. */
   {VK_FORMAT_R8_UINT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
       VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 5 /* 8_UINT */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R8G8_UINT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
       VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 18 /* 8_8_UINT */, PS5VK_FORMAT_SWIZZLE_RG01},
   /* V0-formats' signed integer families (the isampler probe, run
    * v0-formats-sampled-sint): the unsigned entries' twin, the register
    * database's SINT word of the same field widths and the same selectors. The
    * two-channel pair was fitted a word at a time: 8_8_SINT 19 with RG01 is the
    * fit, the SNORM word 15 was refuted (it hands the shader the SNORM decode,
    * not the pair), and the earlier selector candidates had asked a signed
    * fetch to read its second byte 0x80 back as +128, which no signed decode
    * can produce (docs/HARDWARE_FINDINGS.md). Like the unsigned entries, an
    * integer format carries no SAMPLED_IMAGE_FILTER_LINEAR. */
   {VK_FORMAT_R8_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 6 /* 8_SINT */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R8G8_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 19 /* 8_8_SINT */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R8G8B8A8_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    61 /* 8_8_8_8_SINT */, PS5VK_FORMAT_SWIZZLE_RGBA},
   {VK_FORMAT_A8B8G8R8_SINT_PACK32,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    61 /* 8_8_8_8_SINT */, PS5VK_FORMAT_SWIZZLE_WZYX},
   {VK_FORMAT_R16_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
    12 /* 16_SINT */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R16G16_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
    28 /* 16_16_SINT */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R16G16B16A16_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    70 /* 16_16_16_16_SINT */, PS5VK_FORMAT_SWIZZLE_RGBA},
   /* The eight-byte unsigned integer form, the signed one's twin: the fetch
    * word 69, the transfer pair, and the blit source and destination the
    * resampler's decode and encode give it. Its attachment waits on the
    * eight-byte tiled colour map (round 7, docs/M5_PHASE_C.md). */
   {VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
       VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    69 /* 16_16_16_16_UINT */, PS5VK_FORMAT_SWIZZLE_RGBA},
   {VK_FORMAT_R32_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT,
    /* The CTS requires the atomic bit for this format
     * (dEQP-VK.api.info.format_properties.r32_uint and .r32_sint), and the console
     * proved it: imageAtomicAdd through the same imageBuffer the store probe
     * writes, its texels holding their band's fragment count
     * (docs/M5_PHASE_C.md, CTS round 11). */
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT,
    21 /* 32_SINT */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R32G32_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    63 /* 32_32_SINT */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R32G32B32A32_SINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    76 /* 32_32_32_32_SINT */, PS5VK_FORMAT_SWIZZLE_RGBA},
   /* The ten-bit unsigned form (2_10_10_10_UINT 54, the UNORM twin's selectors):
    * the usampler probe samples it too, and its texels hold 64, 128 and 192 so
    * the RGBA8 target keeps the integers. */
   {VK_FORMAT_A2B10G10R10_UINT_PACK32,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT, VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
    54 /* 2_10_10_10_UINT */, PS5VK_FORMAT_SWIZZLE_RGBA},
   /* The same unsigned family at the wider widths: 16_UINT 11, 16_16_UINT 27,
    * 32_UINT 20, 32_32_UINT 62, 32_32_32_32_UINT 75. The probe's shader writes
    * the fetched value over 255 into an RGBA8 target, so its texels hold 64,
    * 128 and 192 -- a value wider than a byte would clamp (docs/HARDWARE_FINDINGS.md). */
   {VK_FORMAT_R16_UINT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
       VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
    11 /* 16_UINT */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R16G16_UINT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
    27 /* 16_16_UINT */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R32_UINT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT,
    /* The CTS requires the atomic bit for this format
     * (dEQP-VK.api.info.format_properties.r32_uint and .r32_sint), and the console
     * proved it: imageAtomicAdd through the same imageBuffer the store probe
     * writes, its texels holding their band's fragment count
     * (docs/M5_PHASE_C.md, CTS round 11). */
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT,
    20 /* 32_UINT */, PS5VK_FORMAT_SWIZZLE_R001},
   {VK_FORMAT_R32G32_UINT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
       VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    62 /* 32_32_UINT */, PS5VK_FORMAT_SWIZZLE_RG01},
   {VK_FORMAT_R8G8B8A8_UINT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    60 /* 8_8_8_8_UINT */, PS5VK_FORMAT_SWIZZLE_RGBA},
   {VK_FORMAT_R5G6B5_UNORM_PACK16,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, 0,
    133 /* 5_6_5_UNORM */, PS5VK_FORMAT_SWIZZLE_ZYX1},
   {VK_FORMAT_A1R5G5B5_UNORM_PACK16,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, 0,
    134 /* 1_5_5_5_UNORM */, PS5VK_FORMAT_SWIZZLE_ZYXW},
   {VK_FORMAT_B4G4R4A4_UNORM_PACK16,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT, 0,
    136 /* 4_4_4_4_UNORM */, PS5VK_FORMAT_SWIZZLE_YZWX},
   {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT, 0,
    132 /* 5_9_9_9_FLOAT */, PS5VK_FORMAT_SWIZZLE_XYZ1},
   {VK_FORMAT_B10G11R11_UFLOAT_PACK32,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT, VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
    36 /* 10_11_11_FLOAT */, PS5VK_FORMAT_SWIZZLE_XYZ1},
   {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
    50 /* 2_10_10_10_UNORM */, PS5VK_FORMAT_SWIZZLE_RGBA},
   /* Colour target in B, G, R, A bytes: the VideoOut framebuffers (M2,
    * CB_COLOR0_INFO SWAP_ALT) and the swapchain images (Phase C1). Not yet
    * sampled. */
   /* B8G8R8A8_UNORM's sampler word is its own byte order: the 8_8_8_8 layout
    * with the memory channels B, G, R, A, so the fetch's X is the memory's Z,
    * Y its Y, Z its X and W its W -- Mesa's radv_compose_swizzle for the
    * format's description, the same swap the colour target's COMP_SWAP ALT
    * names (round 6, docs/M5_PHASE_C.md). */
   {VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, 56 /* 8_8_8_8_UNORM */, PS5VK_FORMAT_SWIZZLE_ZYXW},
   /* Tiled depth attachment with stored depth equal to clip z (M4 step 1). Its
    * transfers are the two measured depth maps: the one-sample four-byte one
    * (M4 step 1, C5) and the four-sample sixteen-byte one (C8's depth 4x, the
    * row tools/check-mip-layout.sh compares with AddrLib), which is what lets a
    * depth image be created for a copy at all. What a depth transfer refuses is
    * a shape, not the format: a readback to a buffer and an upload from one
    * name their phase (ps5vk_image.c, ps5vk_refusals.c). */
   /* D16_UNORM's depth attachment (round 15): its DB_Z_INFO word is Z_16 (1)
    * where D32F's measured word carries 3, its fetch word 16_UNORM (7) is inert
    * until a sampler can reach it (the compiler fault, round 13), and its
    * single-sample two-byte depth map is ps5vk_tiled_depth2_terms. */
   {VK_FORMAT_D16_UNORM,
    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
    0, 7 /* 16_UNORM */, PS5VK_FORMAT_SWIZZLE_R001},
   /* The two depth formats (round 13): the register database's fetch words
    * 16_UNORM (7) and 32_FLOAT (22) with the R001 selectors, which are inert
    * until a feature is proved, and D32's measured depth attachment with its
    * transfer pair. Neither claims a *sampled* or *blit source* bit: a shader
    * whose descriptors include a depth image trips the compiler fault the
    * signed shaders do (radv_nir_lower_descriptors -> aco::lower_branches,
    * round 13), so docs/V0_FORMATS_AUDIT.md quotes that fault as their blocker
    * rather than a probe's word. D16 has no attachment bit yet either: its
    * DB_Z_INFO word is the next probe's. */
   {VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT,
    0, 22 /* 32_FLOAT */, PS5VK_FORMAT_SWIZZLE_R001},
   /* The two depth/stencil formats of the audit's second `must:` clause (round
    * 12), which the runner's v0-stencil case proves on the console: a rendering
    * through an image of the format clears its depth, writes its stencil plane
    * with one pipeline and reads it back with the next, so the clause's
    * requirement -- the feature for at least one of the two -- is satisfied.
    * D24_UNORM_S8_UINT's depth word is Z_24 (2) where D32F's is Z_32_FLOAT (3);
    * D32_SFLOAT_S8_UINT keeps D32F's measured word. Both carry the separate
    * one-byte stencil plane ps5vk_image_stencil_plane lays out. Neither claims a
    * transfer: no readback or upload of a stencil plane has been measured, and
    * the paths that would need one refuse it by name.
    */
   {VK_FORMAT_D24_UNORM_S8_UINT,
    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
    0, 141 /* 8_24_UNORM, inert until a fetch path reaches a depth format */,
    PS5VK_FORMAT_SWIZZLE_R001},
   /* R83: D32_SFLOAT_S8_UINT is also sampled and a transfer source and
    * destination, one aspect at a time (ps5vk_image_plane): its depth plane is
    * D32_SFLOAT's surface, fetched and copied as D32_SFLOAT's is, and its
    * stencil plane is fetched as R8_UINT and copied a byte a texel through the
    * one-byte Z_X map (ps5vk_tiled_stencil_terms). LRPS2 creates its depth
    * target in this format with both usages. */
   {VK_FORMAT_D32_SFLOAT_S8_UINT,
    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
    0, 22 /* 32_FLOAT */, PS5VK_FORMAT_SWIZZLE_R001},
   /* Vertex attributes the probes drew with exactly and no other use: 3D
    * positions (M4), which the table's sampled SFLOAT entries above do not
    * already carry. */
   {VK_FORMAT_R32G32B32_SFLOAT, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
   /* The rectangle attribute of Mesa's meta draws, which the c1-clear probe
    * drew from on the console (Phase C1b). */
   /* V0-formats' widest unsigned integer format: sampled (the usampler probe)
    * and a vertex attribute (V0-formats' integer vertex probe) at once, which is
    * one entry, not two -- the table's first match wins. */
   {VK_FORMAT_R32G32B32A32_UINT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
       VK_FORMAT_FEATURE_BLIT_DST_BIT |
       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT, 75 /* 32_32_32_32_UINT */,
    PS5VK_FORMAT_SWIZZLE_RGBA},
   /* The two three-component integer positions (V0-formats): the runner's
    * v0-vertex-sint and v0-vertex-uint drew the m3-vertex square with them on
    * the console (pid 146), all three components exact -- the third read back as
    * the fragment's alpha -- which is what these two of the audit's 55 rows
    * waited for. */
   {VK_FORMAT_R32G32B32_SINT, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
   {VK_FORMAT_R32G32B32_UINT, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT},
   /* The byte-reversed unsigned integer form: no probe has fetched it, so it
    * carries only what V0-formats' colour-target frames prove, the attachment,
    * with the same CB_COLOR0_INFO word as the A8B8G8R8_UNORM form and the UINT
    * number type (ps5vk_colour_formats). Its signed twin's attachment waits on
    * the compiler crash the sint case hit (docs/M5_PHASE_C.md). */
   /* The byte-reversed unsigned integer form samples in Vulkan's own byte
    * order, not the UNORM packed family's reversed one: the console's integer
    * fetch of it reads R first (v0-blit-dst, run pid 290), so its descriptor
    * word is 8_8_8_8_UINT with the straight RGBA selectors. */
   {VK_FORMAT_A8B8G8R8_UINT_PACK32,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
       VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
       VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
       VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT,
    60 /* 8_8_8_8_UINT */, PS5VK_FORMAT_SWIZZLE_RGBA},
};

const struct ps5vk_format *
ps5vk_find_format(VkFormat format)
{
   for (size_t i = 0; i < ARRAY_SIZE(ps5vk_formats); i++) {
      if (ps5vk_formats[i].format == format)
         return &ps5vk_formats[i];
   }
   return NULL;
}

/* The register database's CB_COLOR0_INFO encodings (gfx103, the
 * V_028C70_COLOR_/NUMBER_/SWAP_ values of AMD's register headers, which
 * ps5-opengl's third_party/opengnm carries). Each row is the word
 * sceGnmCreateRenderTarget would write for that format's
 * GnmDataFormat -- the data format from the channel sizes, the number type from
 * the channel type, the component order from the channel layout
 * (sceGnmDfGetRtChannelType/Order) -- and Mesa's ac_get_cb_format (channel
 * sizes), ac_get_cb_number_type (channel type, colorspace) and
 * ac_translate_colorswap (channel order) pick the same three values, which is
 * the cross-check docs/V0_FORMATS_AUDIT.md asks for. Only four-byte texels are
 * here: the tiled storage map the readback decodes (128x128 words a block) is
 * proved for them, and every one of these writes one word a texel. */
static const struct ps5vk_colour_format ps5vk_colour_formats[] = {
   /* B8G8R8A8_UNORM is the VideoOut framebuffer and swapchain format (M2, C1);
    * its COMP_SWAP is the SWAP_ALT the driver has programmed since M2. */
   {VK_FORMAT_R8G8B8A8_UNORM, 10 /* 8_8_8_8 */, 0 /* UNORM */, 0 /* SWAP_STD */, 0},
   {VK_FORMAT_B8G8R8A8_UNORM, 10, 0, 1 /* SWAP_ALT */, 0},
   /* V0-formats' other 8-bit layouts and the sRGB form: the same data format
    * with the WZYX byte order (SWAP_STD_REV) or the sRGB number type. */
   {VK_FORMAT_A8B8G8R8_UNORM_PACK32, 10, 0, 2 /* SWAP_STD_REV */, 0},
   {VK_FORMAT_R8G8B8A8_SRGB, 10, 6 /* SRGB */, 0, 0},
   /* The packed byte-reversed sRGB target (round 17): the same word with
    * SWAP_STD rather than SWAP_STD_REV, because its storage holds the channels
    * in the R, G, B, A order the format table's storage_reversed field names. */
   {VK_FORMAT_A8B8G8R8_SRGB_PACK32, 10, 6 /* SRGB */, 0, 0},
   /* The byte-reversed sRGB target (round 14): the same 8_8_8_8 data format and
    * sRGB number type with SWAP_ALT, the swap the swapchain's B8G8R8A8 byte
    * order uses, which is what makes the hardware encode the curve into bytes
    * 2, 1 and 0. */
   {VK_FORMAT_B8G8R8A8_SRGB, 10, 6 /* SRGB */, 1 /* SWAP_ALT */, 0},
   /* The two ten-bit ten-eleven forms' renderable half. */
   {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 9 /* 2_10_10_10 */, 0, 0, 0},
   /* The half-float pair at four bytes: 16_16 with the FLOAT number type, whose
    * export Mesa's ac_choose_spi_color_formats picks as FP16_ABGR, the same one
    * the normalized targets blend with. */
   {VK_FORMAT_R16G16_SFLOAT, 5 /* 16_16 */, 7 /* FLOAT */, 0, 0},
   /* The integer targets at four bytes, of which v0-targets-uint proves the
    * unsigned five (run pid 205); the signed four are the words a probe would
    * use and carry no feature bit yet, because the signed case's shader aborts
    * ACO's register allocator on its second compile in one process
    * (docs/M5_PHASE_C.md). Their number types are UINT (4) and SINT (5), the CB
    * bypasses blending for them (Mesa's "according to docs if SINT/UINT",
    * ps5vk_draw.c) and their export is UINT16_ABGR (7) or SINT16_ABGR (8)
    * whatever the blend state, so the driver always compiles their pixel stages
    * for it. The single-channel 32-bit ones export 32_R (1), which is the
    * COLOR_32 case's normal choice. */
   {VK_FORMAT_R8G8B8A8_UINT, 10, 4 /* UINT */, 0, 7},
   {VK_FORMAT_A8B8G8R8_UINT_PACK32, 10, 4, 2 /* SWAP_STD_REV */, 7},
   {VK_FORMAT_A2B10G10R10_UINT_PACK32, 9 /* 2_10_10_10 */, 4, 0, 7},
   {VK_FORMAT_R16G16_UINT, 5 /* 16_16 */, 4, 0, 7},
   /* The single-channel 32-bit float target: the same 32 data format and 32_R
    * export as its integer twins, with the FLOAT number type. The CTS requires
    * a colour-attachment bit for R32_SFLOAT whatever the driver's taste
    * (dEQP-VK.api.info.format_properties.r32_sfloat), and the row above its
    * integer twins says the export exists; what the probe proves is that the
    * console's hardware encodes a float into that target the way the export
    * promises (runs/v0-target-float, one frame). */
   {VK_FORMAT_R32_SFLOAT, 4 /* 32 */, 7 /* FLOAT */, 0 /* SWAP_STD */, 1 /* 32_R */},
   {VK_FORMAT_R32_UINT, 4 /* 32 */, 4, 0, 1 /* 32_R */},
   {VK_FORMAT_R8G8B8A8_SINT, 10, 5 /* SINT */, 0, 8},
   {VK_FORMAT_A8B8G8R8_SINT_PACK32, 10, 5, 2, 8},
   {VK_FORMAT_R16G16_SINT, 5, 5, 0, 8},
   {VK_FORMAT_R32_SINT, 4, 5, 0, 1 /* 32_R */},
   /* The signed rows at eight and sixteen bytes, whose colour words the console
    * has not read yet (round 17's one-frame-a-process cases): Mesa's
    * ac_choose_spi_color_formats sends every SINT 16_16_16_16 to SINT16_ABGR and
    * every 32_32_32_32 to 32_ABGR whatever the number type, and a 32_32 SINT
    * target takes the same four-slot 32_ABGR export its unsigned twin does
    * (round 10: 32_AR leaves the second channel unwritten). */
   {VK_FORMAT_R16G16B16A16_SINT, 12 /* 16_16_16_16 */, 5, 0, 8 /* SINT16_ABGR */},
   {VK_FORMAT_R32G32_SINT, 11 /* 32_32 */, 5, 0, 9 /* 32_ABGR */},
   {VK_FORMAT_R32G32B32A32_SINT, 14 /* 32_32_32_32 */, 5, 0, 9 /* 32_ABGR */},
   /* The narrow targets (round 11): the 8-bit and 16-bit CB data formats with
    * the number types their Vulkan formats name, and Mesa's
    * ac_choose_spi_color_formats exports for them -- FP16_ABGR for a normalized
    * 8-bit target, UNORM16_ABGR is what a 16-bit *UNORM* target would take and
    * none is claimed here, FP16_ABGR for the half-float one, and
    * UINT16_ABGR/SINT16_ABGR for the integer families. */
   {VK_FORMAT_R8_UNORM, 1 /* 8 */, 0 /* UNORM */, 0 /* SWAP_STD */, 4 /* FP16_ABGR */},
   {VK_FORMAT_R8G8_UNORM, 3 /* 8_8 */, 0, 0, 4 /* FP16_ABGR */},
   {VK_FORMAT_R16_SFLOAT, 2 /* 16 */, 7 /* FLOAT */, 0, 4 /* FP16_ABGR */},
   {VK_FORMAT_R8_UINT, 1, 4 /* UINT */, 0, 7 /* UINT16_ABGR */},
   {VK_FORMAT_R8_SINT, 1, 5 /* SINT */, 0, 8 /* SINT16_ABGR */},
   {VK_FORMAT_R8G8_UINT, 3, 4, 0, 7},
   {VK_FORMAT_R8G8_SINT, 3, 5, 0, 8},
   {VK_FORMAT_R16_UINT, 2, 4, 0, 7},
   {VK_FORMAT_R16_SINT, 2, 5, 0, 8},
   {VK_FORMAT_R5G6B5_UNORM_PACK16, 16 /* 5_6_5 */, 0, 0, 4 /* FP16_ABGR */},
   {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 17 /* 1_5_5_5 */, 0, 0, 4 /* FP16_ABGR */},
   /* The eight-byte targets (round 8): 16_16_16_16 and 32_32 with the number
    * types their formats name, and the exports Mesa's
    * ac_choose_spi_color_formats picks for them -- UINT16_ABGR for the unsigned
    * integer, FP16_ABGR for the half-float, 32_AR for the two 32-bit-channel
    * ones (a float colour stored as two full floats). */
   {VK_FORMAT_R16G16B16A16_UINT, 12 /* 16_16_16_16 */, 4 /* UINT */, 0, 7 /* UINT16_ABGR */},
   /* R82: the 16-bit normalized targets (LRPS2's colour-clip target is
    * R16G16B16A16_UNORM, and it blends). Mesa's ac_choose_spi_color_formats
    * exports them as UNORM16_ABGR only while nothing blends, since the CB
    * cannot blend a 16-bit normalized export, and as 32-bit floats when it
    * does. The default blended export here is FP16_ABGR, whose 11-bit
    * mantissa would round 16-bit values, so each row names its 32-bit export
    * whatever the blend state: 32_R for one channel, and the four-slot 32_ABGR
    * the two-channel 32-bit rows already take (round 10). */
   {VK_FORMAT_R16_UNORM, 2 /* 16 */, 0 /* UNORM */, 0, 1 /* 32_R */},
   {VK_FORMAT_R16G16_UNORM, 5 /* 16_16 */, 0, 0, 9 /* 32_ABGR */},
   {VK_FORMAT_R16G16B16A16_UNORM, 12 /* 16_16_16_16 */, 0, 0, 9 /* 32_ABGR */},
   {VK_FORMAT_R16G16B16A16_SFLOAT, 12, 7 /* FLOAT */, 0, 4 /* FP16_ABGR */},
   /* The two-channel 32-bit rows take the four-slot export: with 32_AR the
    * console wrote the first channel and left the second untouched (round 10's
    * first attempt), so their pixel stages export all four 32-bit values and
    * the CB stores the two its format has -- the same export the 32_32_32_32
    * rows use. */
   {VK_FORMAT_R32G32_UINT, 11 /* 32_32 */, 4, 0, 9 /* 32_ABGR */},
   {VK_FORMAT_R32G32_SFLOAT, 11, 7, 0, 9 /* 32_ABGR */},
   {VK_FORMAT_R32G32B32A32_UINT, 14 /* 32_32_32_32 */, 4, 0, 9 /* 32_ABGR */},
   {VK_FORMAT_R32G32B32A32_SFLOAT, 14, 7, 0, 9 /* 32_ABGR */},
};

const struct ps5vk_colour_format *
ps5vk_find_colour_format(VkFormat format)
{
   for (size_t i = 0; i < ARRAY_SIZE(ps5vk_colour_formats); i++) {
      if (ps5vk_colour_formats[i].format == format)
         return &ps5vk_colour_formats[i];
   }
   return NULL;
}

/* The image usages a format's optimal-tiling features allow. */
static VkImageUsageFlags
ps5vk_format_usage(const struct ps5vk_format *format)
{
   const VkFormatFeatureFlags features = format->optimal_features;
   VkImageUsageFlags usage = 0;
   if (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)
      usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   if (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
      usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
      usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
      usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
   /* A storage image is what a shader's imageStore reaches (ps5vk_draw.c's
    * 32-byte entry): the bit is the format's own, so an entry that does not
    * carry it refuses the usage by name like every other one. */
   if (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
      usage |= VK_IMAGE_USAGE_STORAGE_BIT;
   /* The specification's Format Feature Dependent Image Usage Flags table
    * (formats-v1.4.354.adoc, the copy in .deps/native/vulkan-docs):
    * VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT requires
    * VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT **or**
    * VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT. So a format an attachment
    * can be is one an input attachment may name, and the clause is about the
    * feature bits rather than about any application's usage set: every format
    * this table carries an attachment bit for must answer the usage.
    *
    * What this does *not* do is read one: the descriptor type, its stride, its
    * write path and subpass reads are R2 and unchanged, so an image created with
    * the usage is created and its input-attachment uses are still refused where a
    * descriptor or a subpass would need them. Refusing the *image* refused
    * something the specification requires the driver to allow. */
   /* The same table: TRANSIENT_ATTACHMENT needs an attachment feature too. It
    * is a hint that the contents need not outlive a render pass (lazily
    * allocated memory, which this device does not report); the image is
    * created with ordinary memory and behaves as an attachment always does.
    * PPSSPP's depth buffers ask for it. */
   if (features & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
      usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
   return usage;
}

/* Whether images of these parameters can be created: a reported format,
 * optimal tiling, 2D, no create flags, and only usages its features allow. */
static bool
ps5vk_image_supported(VkFormat format, VkImageType type, VkImageTiling tiling,
                      VkImageUsageFlags usage, VkImageCreateFlags flags)
{
   const struct ps5vk_format *const entry = ps5vk_find_format(format);
   /* Cube compatibility (D1): a 2D image whose layers are whole cubes, which is
    * what makes a cube view and the descriptor's TYPE 11 possible. Mutable
    * format: views may name another format of the same texel size, whose
    * surface layout is the same one (the tile mode follows the texel size);
    * a view of another size is refused where it is created. PPSSPP's libretro
    * presentation images ask for it. Every other flag is still unsupported. */
   const VkImageCreateFlags allowed =
      VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   /* A stencil-bearing format is the single-level, single-layer attachment its
    * plane layout covers (ps5vk_image_stencil_plane): a chain or an array would
    * need AddrLib's per-level and per-layer plane bases, and an image with no
    * attachment usage would have no plane at all. Both are refused here rather
    * than created with a plane nothing placed. */
   if (vk_format_has_stencil(format) &&
       (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
      return false;
   /* A depth/stencil attachment may carry transfer and sampled usage its format
    * does not report: PPSSPP creates every framebuffer's depth image with both
    * whatever the format says, and uses neither unless a game copies or samples
    * depth. The image is created; a copy or a sample of it is refused by name
    * where it is recorded, which is where a use would be wrong. The format's
    * reported features stay truthful, so an application that asks first is
    * told no. */
   VkImageUsageFlags tolerated = 0;
   if (entry && vk_format_has_stencil(format) &&
       (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
      tolerated = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
   return entry && type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL &&
          (flags & ~allowed) == 0 && usage != 0 &&
          (usage & ~(ps5vk_format_usage(entry) | tolerated)) == 0;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format,
                                         VkFormatProperties2 *pFormatProperties)
{
   (void)physicalDevice;
   const struct ps5vk_format *const entry =
      (ps5vk_ab_flags & PS5VK_AB_NO_D24) && format == VK_FORMAT_D24_UNORM_S8_UINT
         ? NULL
         : ps5vk_find_format(format);
   /* No linear tiling for any format yet. */
   pFormatProperties->formatProperties = (VkFormatProperties){
      .linearTilingFeatures = 0,
      .optimalTilingFeatures = entry ? entry->optimal_features : 0,
      .bufferFeatures = entry ? entry->buffer_features : 0,
   };
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
                                              const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
                                              VkImageFormatProperties2 *pImageFormatProperties)
{
   (void)physicalDevice;
   const VkPhysicalDeviceImageFormatInfo2 *const info = pImageFormatInfo;
   /* R84: no external memory handle type is supported (Vulkan 1.1's external
    * memory capabilities report none), so an image that would be exported or
    * imported is not a supported combination. */
   const VkPhysicalDeviceExternalImageFormatInfo *const external =
      vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   if (!ps5vk_image_supported(info->format, info->type, info->tiling, info->usage, info->flags) ||
       (external != NULL && external->handleType != 0)) {
      /* capabilities.adoc: unsupported combinations report all zeros. */
      memset(&pImageFormatProperties->imageFormatProperties, 0,
             sizeof(pImageFormatProperties->imageFormatProperties));
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   /* 2D, optimal and not cube-compatible, so the sample counts cover the
    * framebuffer and sampled-image limits: 1 and 4 (Supported Sample Counts). */
   /* A stencil format's image is one level and one layer, the shape its
    * stencil plane is laid out for (ps5vk_image_stencil_plane; R83). */
   const bool stencil = vk_format_has_stencil(info->format);
   pImageFormatProperties->imageFormatProperties = (VkImageFormatProperties){
      .maxExtent = {PS5VK_MAX_EXTENT_2D, PS5VK_MAX_EXTENT_2D, 1},
      .maxMipLevels = stencil ? 1u : PS5VK_MAX_MIP_LEVELS,
      .maxArrayLayers = stencil ? 1u : PS5VK_MAX_ARRAY_LAYERS,
      /* A stencil format's plane is laid out for one sample only
       * (ps5vk_image_stencil_plane), so its images are created with one. */
      .sampleCounts = vk_format_has_stencil(info->format) ? VK_SAMPLE_COUNT_1_BIT
                                                          : PS5VK_SAMPLE_COUNTS,
      .maxResourceSize = PS5VK_ADDRESS_WINDOW_BYTES,
   };
   return VK_SUCCESS;
}

/* R84: no external memory handle type is supported, for buffers either. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   (void)physicalDevice;
   (void)pExternalBufferInfo;
   pExternalBufferProperties->externalMemoryProperties = (VkExternalMemoryProperties){0};
}

/* Sparse resources are not supported: no properties for any parameters. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
   uint32_t *pPropertyCount, VkSparseImageFormatProperties2 *pProperties)
{
   (void)physicalDevice;
   (void)pFormatInfo;
   (void)pProperties;
   *pPropertyCount = 0;
}

/* Whether the image's format stores its texels with the four bytes reversed
 * (ps5vk_format.storage_reversed): every boundary where an application's bytes
 * meet the image's swaps them, and every other path works in the stored order. */
static bool
ps5vk_image_storage_reversed(const struct ps5vk_image *image)
{
   const struct ps5vk_format *const entry = ps5vk_find_format(image->vk.format);
   return entry != NULL && entry->storage_reversed;
}

/* Tile dimensions in texels: ps5_tiled_color_surface_size and
 * ps5_tiled_color_msaa4_tile for colour, ps5_tiled_surface_size and the RGBA8
 * 4x size for depth. Every 4x tile is half as wide and high as the 1x one. */
static void
ps5vk_tile_extent(unsigned texel_bytes, bool depth, VkSampleCountFlagBits samples,
                  unsigned *width, unsigned *height)
{
   /* A depth element is four bytes except the two-byte one, whose row is its
    * own table and whose tile is 256x128 texels (ps5vk_tiled_depth2_terms),
    * and the one-byte stencil plane's, 256x256 (R83,
    * ps5vk_tiled_stencil_terms). */
   if (depth && texel_bytes != 2 && texel_bytes != 1)
      texel_bytes = 4;
   /* A 64 KiB tile holds 2^n texels of every sample, n = 16 - log2(texel bytes)
    * - log2(samples), laid out 2^ceil(n/2) wide and 2^floor(n/2) high: AddrLib's
    * gfx10 block dimension, which reproduces each measured tile (128x128 four
    * bytes, 256x128 two, 64x64 four bytes at four samples) and gives 128x64 at
    * two samples and 64x32 at eight (R78). */
   unsigned log2_bytes = 2;
   switch (texel_bytes) {
   case 1: log2_bytes = 0; break;
   case 2: log2_bytes = 1; break;
   case 8: log2_bytes = 3; break;
   case 16: log2_bytes = 4; break;
   default: log2_bytes = 2; break;
   }
   const unsigned n = 16u - log2_bytes - util_logbase2(samples);
   *width = 1u << ((n + 1u) / 2u);
   *height = 1u << (n / 2u);
}

/* Where one level of a row-layout image starts, its row pitch and its extent:
 * smaller levels precede larger ones and each row pads to 256 bytes. R18
 * measures these reverse-order bases on PS5, matching AddrLib linear layout.
 * Storage extents round up; Vulkan copies still use the actual floor extents. */
static void
ps5vk_image_level_layout(const struct ps5vk_image *image, uint32_t level, uint64_t *offset,
                         uint64_t *row_pitch, VkExtent2D *extent)
{
   const unsigned texel_bytes = vk_format_get_blocksize(image->vk.format);
   uint64_t at = 0;
   for (uint32_t index = level + 1; index < image->vk.mip_levels; index++) {
      const uint64_t width = MAX2(DIV_ROUND_UP(image->vk.extent.width, UINT64_C(1) << index), 1u);
      const uint64_t height = MAX2(DIV_ROUND_UP(image->vk.extent.height, UINT64_C(1) << index), 1u);
      at += align64(width * texel_bytes, PS5VK_ROW_ALIGNMENT) * height;
   }
   const uint64_t width = MAX2(DIV_ROUND_UP(image->vk.extent.width, UINT64_C(1) << level), 1u);
   const uint64_t height = MAX2(DIV_ROUND_UP(image->vk.extent.height, UINT64_C(1) << level), 1u);
   *offset = at;
   *row_pitch = align64(width * texel_bytes, PS5VK_ROW_ALIGNMENT);
   extent->width = (uint32_t)width;
   extent->height = (uint32_t)height;
}

/* The tiled mip chains AddrLib's rule has been measured for. A tiled chain's
 * levels are not "each level's tiles after the last": the hardware walks a
 * chain from its base address by its own rule, and the rule's answer for a
 * shape is what tools/mip-layout-oracle.cpp prints out of the pinned AddrLib
 * and tools/check-mip-layout.sh holds the console's measurement against
 * (docs/HARDWARE_FINDINGS.md, C7). Four-byte texels, one sample, one layer --
 * the shapes the oracle has been run for. An upload into a tiled chain whose
 * shape is not here refuses by name rather than placing texels where the
 * hardware would not look for them. */
const struct ps5vk_tiled_chain {
   uint32_t width;
   uint32_t height;
   uint32_t levels;
   uint64_t bytes;
   uint64_t bases[8];
} ps5vk_tiled_chains[] = {
   /* The single-level shapes D1's array probe fills, which are also the slices
    * the array rows above are made of. */
   {256, 256, 1, 0x40000, {0x0}},
   {64, 64, 1, 0x10000, {0x0}},
   {256, 256, 5, 0x60000, {0x20000, 0x10000, 0x8400, 0x4800, 0x800}},
   {256, 256, 6, 0x60000, {0x20000, 0x10000, 0x8400, 0x4800, 0x800, 0x400}},
   {128, 128, 4, 0x20000, {0x10000, 0x8400, 0x4800, 0x800}},
   {512, 512, 5, 0x160000, {0x60000, 0x20000, 0x10000, 0x8400, 0x4800}},
   {512, 512, 7, 0x160000, {0x60000, 0x20000, 0x10000, 0x8400, 0x4800, 0x800, 0x400}},
   {1024, 1024, 8, 0x560000,
    {0x160000, 0x60000, 0x20000, 0x10000, 0x8400, 0x4800, 0x800, 0x400}},
   {256, 128, 4, 0x40000, {0x20000, 0x10000, 0x8400, 0x4800}},
   {64, 64, 3, 0x10000, {0x8400, 0x4800, 0x800}},
   {32, 32, 2, 0x10000, {0x8400, 0x4800}},
};

/* The measured chain for one slice of an image's shape, or NULL when the
 * oracle has not been run for it. A slice is a chain of its own whatever the
 * layer count (docs/HARDWARE_FINDINGS.md: the oracle's array rows are their own
 * chain's size, layers consecutive), so the array's layers are not part of the
 * lookup. */
static const struct ps5vk_tiled_chain *
ps5vk_tiled_chain_of(const struct ps5vk_image *image)
{
   if (vk_format_get_blocksize(image->vk.format) != 4 || image->vk.samples != VK_SAMPLE_COUNT_1_BIT ||
       image->vk.mip_levels > 8)
      return NULL;
   for (size_t i = 0; i < ARRAY_SIZE(ps5vk_tiled_chains); i++) {
      const struct ps5vk_tiled_chain *const chain = &ps5vk_tiled_chains[i];
      if (chain->width == image->vk.extent.width && chain->height == image->vk.extent.height &&
          chain->levels == image->vk.mip_levels)
         return chain;
   }
   return NULL;
}

/* Where one level of a tiled image starts, or UINT64_MAX for a level no
 * measurement covers. Level 0 of a chain that is not in the table is its base,
 * which is the single-level case every tiled image has been. */
uint64_t
ps5vk_image_tiled_level_base(const struct ps5vk_image *image, uint32_t level)
{
   if (level == 0 && image->vk.mip_levels == 1)
      return 0;
   const struct ps5vk_tiled_chain *const chain = ps5vk_tiled_chain_of(image);
   if (chain == NULL || level >= chain->levels)
      return UINT64_MAX;
   return chain->bases[level];
}

/* One layer's bytes -- the slice size -- for an image whose per-layer shape the
 * measured chain table covers, or 0 for a shape it does not. This is what makes
 * a layer's base "slice bytes times the layer": a slice is a chain of its own
 * and the layers are consecutive (docs/HARDWARE_FINDINGS.md, the oracle's array
 * rows). Row-layout images stack their layers the same way, one level-stacked
 * slice each, so the same rule serves both storages. */
uint64_t
ps5vk_image_layer_bytes(const struct ps5vk_image *image)
{
   if (image->storage == PS5VK_IMAGE_STORAGE_TILES) {
      const struct ps5vk_tiled_chain *const chain = ps5vk_tiled_chain_of(image);
      if (chain != NULL)
         return chain->bytes;
      /* One level needs no chain: a layer is its tile grid, which is what
       * ps5vk_image_storage summed, and a colour attachment's texels have always
       * been placed at base 0 of it. */
      if (image->vk.mip_levels == 1 && !vk_format_has_stencil(image->vk.format))
         return image->size / MAX2(image->vk.array_layers, 1u);
      return 0;
   }
   uint64_t slice = 0;
   for (uint32_t level = 0; level < image->vk.mip_levels; level++) {
      uint64_t offset = 0, row_pitch = 0;
      VkExtent2D extent = {0, 0};
      ps5vk_image_level_layout(image, level, &offset, &row_pitch, &extent);
      slice += row_pitch * extent.height;
   }
   return slice;
}

/* Where one level of one layer of a tiled image starts: the layer's slice plus
 * the level's base inside it, or UINT64_MAX for a shape no measurement covers. */
uint64_t
ps5vk_image_tiled_layer_level_base(const struct ps5vk_image *image, uint32_t level, uint32_t layer)
{
   /* Layer 0 starts at the level's own base whatever a slice measures -- a
    * stencil-bearing image has no slice size (its allocation holds the stencil
    * plane too), and its one layer is still at the start (R83). */
   if (layer == 0)
      return ps5vk_image_tiled_level_base(image, level);
   const uint64_t slice = ps5vk_image_layer_bytes(image);
   if (slice == 0)
      return UINT64_MAX;
   const uint64_t base = ps5vk_image_tiled_level_base(image, level);
   if (base == UINT64_MAX)
      return UINT64_MAX;
   return slice * layer + base;
}

/* Storage size and alignment of an image (see the top of this file). */
/* Where a stencil-bearing depth image's stencil plane starts and how large it
 * is, from the depth surface's own storage size. AddrLib computes the plane
 * beside the depth surface, in the same 64 KiB Z_X swizzle mode but with one
 * byte an element, and places it at the alignment it reports for that surface
 * (ac_surface.c, gfx9_compute_surface's second miptree: `flags.stencil = 1`,
 * `bpp = 8`, `stencil_offset = align(surf_size, baseAlign)`); ps5-opengl's
 * runtime asks for the same 64 KiB alignment of its separate stencil buffer,
 * which is what its hardware-run `append_depth_target_state` writes to
 * DB_STENCIL_READ_BASE/DB_STENCIL_WRITE_BASE. A one-byte Z_X tile is 256x256
 * texels (tools/mip-layout-oracle, `swizzle 1 1 64kb_z_x`), so the plane is
 * the tile count the depth surface's shape pads to. Zero bytes for a format
 * with no stencil. */
#define PS5VK_STENCIL_TILE_WIDTH 256u
#define PS5VK_STENCIL_TILE_HEIGHT 256u

static uint64_t
ps5vk_image_stencil_plane(const VkImageCreateInfo *info, uint64_t depth_bytes, uint64_t *offset)
{
   if (!vk_format_has_stencil(info->format))
      return 0;
   uint64_t bytes = 0;
   for (uint32_t level = 0; level < info->mipLevels; level++) {
      const uint64_t width = MAX2(info->extent.width >> level, 1u);
      const uint64_t height = MAX2(info->extent.height >> level, 1u);
      bytes += DIV_ROUND_UP(width, PS5VK_STENCIL_TILE_WIDTH) *
               DIV_ROUND_UP(height, PS5VK_STENCIL_TILE_HEIGHT) * PS5VK_TILE_BYTES;
   }
   bytes *= info->arrayLayers;
   /* One plane, whole image: the sample count does not change a one-byte
    * element's tile, and a four-sample stencil plane is the next thing to
    * measure rather than to guess (a four-sample stencil attachment is refused
    * by name below). */
   *offset = align64(depth_bytes, PS5VK_TILE_BYTES);
   return bytes;
}

static void
ps5vk_image_storage(const VkImageCreateInfo *info, enum ps5vk_image_storage *storage,
                    uint64_t *size, uint64_t *alignment, uint64_t *stencil_offset)
{
   const unsigned texel_bytes = vk_format_get_blocksize(info->format);
   const bool depth = vk_format_has_depth(info->format);
   const bool padded_single = !depth && !vk_format_is_compressed(info->format) &&
                              info->mipLevels == 1 && info->arrayLayers == 1 &&
                              info->samples == VK_SAMPLE_COUNT_1_BIT &&
                              (texel_bytes == 1 || texel_bytes == 2 || texel_bytes == 4 ||
                               texel_bytes == 8 || texel_bytes == 16) &&
                              (info->extent.width * texel_bytes) % 256 != 0;
   /* A format with a stencil is always tiled: its stencil plane exists only
    * beside a tiled depth surface (ps5vk_image_stencil_plane), whatever the
    * image is used for (R83). */
   const bool tiled = (info->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0 ||
                      vk_format_has_stencil(info->format) ||
                      ((ps5vk_ab_flags & PS5VK_AB_TILE_PADDED) && padded_single);
   uint64_t layer_bytes = 0;
   *stencil_offset = 0;

   if (tiled) {
      unsigned tile_width;
      unsigned tile_height;
      ps5vk_tile_extent(texel_bytes, depth, info->samples, &tile_width, &tile_height);
      for (uint32_t level = 0; level < info->mipLevels; level++) {
         const uint64_t width = MAX2(info->extent.width >> level, 1u);
         const uint64_t height = MAX2(info->extent.height >> level, 1u);
         layer_bytes += DIV_ROUND_UP(width, tile_width) * DIV_ROUND_UP(height, tile_height) *
                        PS5VK_TILE_BYTES;
      }
      *storage = PS5VK_IMAGE_STORAGE_TILES;
      /* A measured chain is stored in the bytes the hardware's rule needs, which
       * is a level's tail packed beside its mip tail rather than each level's
       * tiles in turn (ps5vk_tiled_chains): the table is the authority for a
       * shape it covers, and the sum above stands for the rest. */
      for (size_t i = 0; i < ARRAY_SIZE(ps5vk_tiled_chains); i++) {
         const struct ps5vk_tiled_chain *const chain = &ps5vk_tiled_chains[i];
         if (texel_bytes == 4 && info->samples == VK_SAMPLE_COUNT_1_BIT &&
             chain->width == info->extent.width && chain->height == info->extent.height &&
             chain->levels == info->mipLevels) {
            layer_bytes = chain->bytes;
            break;
         }
      }
      *size = align64(layer_bytes * info->arrayLayers, PS5VK_TILED_ALIGNMENT);
      *alignment = PS5VK_TILED_ALIGNMENT;
      /* A depth format that carries a stencil gains the plane beside the depth
       * surface, in the same allocation: the depth surface first, then the
       * one-byte plane at the alignment AddrLib places it at
       * (ps5vk_image_stencil_plane, image->stencil_offset). */
      const uint64_t stencil_bytes = ps5vk_image_stencil_plane(info, layer_bytes, stencil_offset);
      if (stencil_bytes != 0)
         *size = align64(*stencil_offset + stencil_bytes, PS5VK_TILED_ALIGNMENT);
      return;
   }

   /* ps5_linear_mip_storage_extent rounds each level's extent up. */
   for (uint32_t level = 0; level < info->mipLevels; level++) {
      const uint64_t width = MAX2(DIV_ROUND_UP(info->extent.width, UINT64_C(1) << level), 1u);
      const uint64_t height = MAX2(DIV_ROUND_UP(info->extent.height, UINT64_C(1) << level), 1u);
      layer_bytes += align64(width * texel_bytes, PS5VK_ROW_ALIGNMENT) * height;
   }
   *storage = PS5VK_IMAGE_STORAGE_ROWS;
   *size = layer_bytes * info->arrayLayers * info->samples;
   *alignment = PS5VK_ROW_ALIGNMENT;
}

static VkResult
ps5vk_CreateImage_untimed(VkDevice _device, const VkImageCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* An unsupported combination is **refused by name**, not asserted: three
    * rounds read this assert's abort as a compiler fault, because the abort
    * backtrace walks whatever ACO frames the stack still held (rounds 4, 13 and
    * 16 -- docs/HARDWARE_FINDINGS.md, "An unsupported image is refused, not
    * asserted"). VK_ERROR_FORMAT_NOT_SUPPORTED is a legal vkCreateImage result,
    * and a probe that asks for a usage its format's entry does not claim now
    * fails with the usage named instead of killing the title. */
   if (!ps5vk_image_supported(pCreateInfo->format, pCreateInfo->imageType, pCreateInfo->tiling,
                              pCreateInfo->usage, pCreateInfo->flags))
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "format %d: type %d, tiling %d, usage 0x%x, flags 0x%x is not a supported "
                       "image",
                       (int)pCreateInfo->format, (int)pCreateInfo->imageType,
                       (int)pCreateInfo->tiling, (unsigned)pCreateInfo->usage,
                       (unsigned)pCreateInfo->flags);
   /* A stencil-bearing image is created for the one shape its plane layout has
    * been derived for: a single-level, single-layer, one-sample attachment. A
    * chain or an array needs AddrLib's per-level and per-layer plane bases and a
    * four-sample plane needs its own tile rule, neither of which anything has
    * measured (ps5vk_image_stencil_plane). */
   if (vk_format_has_stencil(pCreateInfo->format) &&
       (pCreateInfo->mipLevels != 1 || pCreateInfo->arrayLayers != 1 ||
        pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT))
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "a stencil format's plane layout is the single-level, single-layer, "
                       "one-sample attachment it was derived for; this image asks for %u levels, "
                       "%u layers and %u samples",
                       pCreateInfo->mipLevels, pCreateInfo->arrayLayers,
                       (unsigned)pCreateInfo->samples);
   assert(pCreateInfo->extent.width <= PS5VK_MAX_EXTENT_2D &&
          pCreateInfo->extent.height <= PS5VK_MAX_EXTENT_2D &&
          pCreateInfo->mipLevels <= PS5VK_MAX_MIP_LEVELS &&
          pCreateInfo->arrayLayers <= PS5VK_MAX_ARRAY_LAYERS);

   enum ps5vk_image_storage storage;
   uint64_t size;
   uint64_t alignment;
   uint64_t stencil_offset;
   ps5vk_image_storage(pCreateInfo, &storage, &size, &alignment, &stencil_offset);
   /* capabilities.adoc: an image beyond maxResourceSize fails this way. */
   if (size > PS5VK_ADDRESS_WINDOW_BYTES)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct ps5vk_image *const image =
      vk_image_create(&device->vk, pCreateInfo, pAllocator, sizeof(*image));
   if (!image)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   image->storage = storage;
   image->size = size;
   image->alignment = alignment;
   image->video = -1;
   /* The stencil plane's own base, or zero when the format has none: what a
    * draw programs DB_STENCIL_INFO's enable word and bases from
    * (ps5vk_depth_registers). ps5vk_image_storage placed it. */
   image->stencil_offset = stencil_offset;

   *pImage = ps5vk_image_to_handle(image);
   return VK_SUCCESS;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateImage(VkDevice _device, const VkImageCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_CreateImage_untimed(_device, pCreateInfo, pAllocator, pImage);
   ps5vk_hitch_end(PS5VK_HITCH_IMAGE, hitch);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyImage(VkDevice _device, VkImage _image, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_image, image, _image);
   if (image)
      vk_image_destroy(&device->vk, pAllocator, &image->vk);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_GetImageMemoryRequirements2(VkDevice _device, const VkImageMemoryRequirementsInfo2 *pInfo,
                                  VkMemoryRequirements2 *pMemoryRequirements)
{
   (void)_device;
   VK_FROM_HANDLE(ps5vk_image, image, pInfo->image);
   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size = image->size,
      .alignment = image->alignment,
      .memoryTypeBits = PS5VK_MEMORY_TYPE_BITS,
   };
}

/* Sparse resources are not supported: no sparse requirements. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_GetImageSparseMemoryRequirements2(VkDevice _device,
                                        const VkImageSparseMemoryRequirementsInfo2 *pInfo,
                                        uint32_t *pSparseMemoryRequirementCount,
                                        VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   (void)_device;
   (void)pInfo;
   (void)pSparseMemoryRequirements;
   *pSparseMemoryRequirementCount = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_BindImageMemory2(VkDevice _device, uint32_t bindInfoCount,
                       const VkBindImageMemoryInfo *pBindInfos)
{
   (void)_device;
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(ps5vk_image, image, pBindInfos[i].image);
      VK_FROM_HANDLE(ps5vk_device_memory, memory, pBindInfos[i].memory);
      const VkDeviceSize offset = pBindInfos[i].memoryOffset;
      /* Valid usage: an aligned offset, and the requirements' size fits. */
      assert(offset % image->alignment == 0);
      assert(offset + image->size <= memory->vk.size);

      image->memory = memory;
      image->address = (uint64_t)(uintptr_t)memory->direct.address + offset;
      assert(ps5vk_gpu_range_valid(image->address, image->size));
   }
   return VK_SUCCESS;
}

/* Image views are Mesa's common objects; rendering reads their image, format,
 * type and levels (ps5vk_draw.c). */
static VkResult
ps5vk_CreateImageView_untimed(VkDevice _device, const VkImageViewCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(vk_image, image, pCreateInfo->image);
   /* A view in another format reinterprets the image's texels, which this
    * driver does only when they are the same size (a mutable-format image,
    * ps5vk_image_supported): the surface layout then stays the image's. */
   if (pCreateInfo->format != image->format &&
       vk_format_get_blocksize(pCreateInfo->format) != vk_format_get_blocksize(image->format))
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "an image view in format %d of an image in format %d: texels of another "
                       "size are not supported",
                       (int)pCreateInfo->format, (int)image->format);
   struct vk_image_view *const view =
      vk_image_view_create(&device->vk, pCreateInfo, pAllocator, sizeof(*view));
   if (!view)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   *pView = vk_image_view_to_handle(view);
   return VK_SUCCESS;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateImageView(VkDevice _device, const VkImageViewCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_CreateImageView_untimed(_device, pCreateInfo, pAllocator, pView);
   ps5vk_hitch_end(PS5VK_HITCH_IMAGE, hitch);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyImageView(VkDevice _device, VkImageView imageView,
                       const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(vk_image_view, view, imageView);
   if (view)
      vk_image_view_destroy(&device->vk, pAllocator, view);
}

/* The two sampler words the M3 texture canary ran (src/diagnostics.cpp,
 * kTextureNearestSampler and kTextureBilinearSampler). The filter pair is the
 * whole of what varies between them: the clamp-to-edge address modes, the
 * single level, the LOD range and the absent border colour are words 8, 5, 9
 * and 11 of the descriptor the draw writes (ps5vk_draw.c). */
#define PS5VK_SAMPLER_WORD_NEAREST UINT32_C(0x08000000)
#define PS5VK_SAMPLER_WORD_LINEAR UINT32_C(0x09500000)

/* One LOD as word 9's 12-bit field: 1/256 of a level, clamped at 15, which is
 * ps5_texture_descriptor_unsigned_lod's own packing (Phase C7). */
static uint32_t
ps5vk_sampler_unsigned_lod(float value)
{
   if (value <= 0.0f)
      return 0;
   if (value >= 15.0f)
      return 15u << 8;
   return (uint32_t)(value * 256.0f);
}

/* vkCreateSampler. Only the state the console's M3 texture canary ran is
 * accepted, and every refusal names C4, the step a runner probe that widens it
 * belongs to (docs/M5_REFERENCE.md). */
static VkResult
ps5vk_CreateSampler_untimed(VkDevice _device, const VkSamplerCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   const VkSamplerCreateInfo *const info = pCreateInfo;
   if (info->flags != 0)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "sampler flags 0x%x are not the none the texture canary ran; a runner probe "
                       "settles them (docs/M5_REFERENCE.md, C4)", (unsigned)info->flags);
   /* No extension structure is exposed and the canary's descriptor words carry
    * no reduction mode, YCbCr conversion or custom border colour, so a chain
    * would be state nothing has run. */
   if (info->pNext != NULL)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "a sampler creation structure in pNext is not the bare VkSamplerCreateInfo "
                       "the texture canary ran; a runner probe settles it "
                       "(docs/M5_REFERENCE.md, C4)");
   /* The address modes are per-sampler state in Vulkan but three 3-bit fields of
    * the *descriptor* this driver writes, word 8: U in bits 0-2, V in 3-5 and W
    * in 6-8 (ps5vk_draw.c, ps5vk_write_image_descriptor). The encoding is
    * ps5-opengl's own: its ps5_texture_descriptor_wrap maps PIPE_TEX_WRAP_REPEAT
    * to 0, MIRROR_REPEAT to 1 and CLAMP_TO_EDGE to 2, and 2 is what the M3
    * texture canary's descriptor has always carried. Repeat is the mode a zeroed
    * VkSamplerCreateInfo holds, so refusing it refused the default. */
   uint32_t address_word = 0;
   const VkSamplerAddressMode modes[3] = {info->addressModeU, info->addressModeV,
                                          info->addressModeW};
   for (unsigned axis = 0; axis < 3; axis++) {
      uint32_t clamp = 0;
      switch (modes[axis]) {
      case VK_SAMPLER_ADDRESS_MODE_REPEAT:
         clamp = 0;
         break;
      case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
         clamp = 1;
         break;
      case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
         clamp = 2;
         break;
      case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
         /* SQ_TEX_CLAMP_BORDER, the same field's 6 (RADV's radv_tex_wrap), with
          * the colour in word 11 below; R57's runner probe samples all three. */
         clamp = 6;
         break;
      case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
         /* R81: SQ_TEX_MIRROR_ONCE_LAST_TEXEL, the field's 3 (RADV's
          * radv_tex_wrap): the coordinate is mirrored once about zero and then
          * clamped to the edge texel, as the mode's formula says. Valid only
          * with VK_KHR_sampler_mirror_clamp_to_edge enabled
          * (VUID-VkSamplerCreateInfo-addressModeU-01079). */
         if (!device->vk.enabled_extensions.KHR_sampler_mirror_clamp_to_edge)
            return vk_errorf(device, VK_ERROR_UNKNOWN,
                             "sampler axis %u asks mirror-clamp-to-edge, but the device did "
                             "not enable VK_KHR_sampler_mirror_clamp_to_edge",
                             axis);
         clamp = 3;
         break;
      default:
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "sampler axis %u has address mode %d, which is not a Vulkan address "
                          "mode",
                          axis, (int)modes[axis]);
      }
      address_word |= clamp << (3u * axis);
   }
   /* The mip filter, bits 26-27 of word 10: ps5-opengl's own encoding of
    * PIPE_TEX_MIPFILTER_NEAREST (1) and LINEAR (2), and the bits the canary's
    * two words already carry (2, from a runtime that ran a single level, where
    * the field cannot matter). A mipmapped view is what gives it meaning; the
    * runner's C7 probe samples a chain with both modes. */
   uint32_t mip_filter;
   if (info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST)
      mip_filter = 1u;
   else if (info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR)
      mip_filter = 2u;
   else
      return vk_errorf(device, VK_ERROR_UNKNOWN, "sampler mipmap mode %d is not a Vulkan mode",
                       (int)info->mipmapMode);
   /* Word 2's filters, as RADV builds them (radv_tex_filter,
    * ac_build_sampler_descriptor): XY_MAG_FILTER bits 20-21 and XY_MIN_FILTER
    * 22-23 -- point 0, bilinear 1, their anisotropic forms 2 and 3 when the
    * sampler filters anisotropically -- Z_FILTER 24-25, which follows the
    * minification filter as the canary's two words did (0x08000000 nearest,
    * 0x09500000 linear), and MIP_FILTER 26-27 below. The canary's pairs are the
    * words this builds for nearest/nearest and linear/linear; a mixed pair,
    * which PPSSPP creates from the PSP's own filter state, is the same fields
    * set independently. */
   const bool anisotropic = info->anisotropyEnable && info->maxAnisotropy > 1.0f;
   uint32_t aniso_ratio = 0;
   if (anisotropic) {
      const float ratio = MIN2(info->maxAnisotropy, 16.0f);
      aniso_ratio = ratio >= 16.0f ? 4u : ratio >= 8.0f ? 3u : ratio >= 4.0f ? 2u : ratio >= 2.0f ? 1u : 0u;
   }
   const uint32_t aniso_filter = aniso_ratio > 0 ? 2u : 0u;
   const uint32_t mag = (info->magFilter == VK_FILTER_LINEAR ? 1u : 0u) | aniso_filter;
   const uint32_t min = (info->minFilter == VK_FILTER_LINEAR ? 1u : 0u) | aniso_filter;
   if ((info->minFilter != VK_FILTER_NEAREST && info->minFilter != VK_FILTER_LINEAR) ||
       (info->magFilter != VK_FILTER_NEAREST && info->magFilter != VK_FILTER_LINEAR))
      return vk_errorf(device, VK_ERROR_UNKNOWN, "sampler filters %d and %d are not Vulkan 1.0 filters",
                       (int)info->minFilter, (int)info->magFilter);
   const uint32_t word = (mag << 20) | (min << 22) |
                         ((info->minFilter == VK_FILTER_LINEAR ? 1u : 0u) << 24) | (2u << 26);
   const float bias_limit = device->vk.physical->properties.maxSamplerLodBias;
   if (!(info->mipLodBias >= -bias_limit && info->mipLodBias <= bias_limit))
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "sampler LOD bias %f exceeds the reported +/- %f range",
                       (double)info->mipLodBias, (double)bias_limit);
   /* maxAnisotropy and compareOp are ignored while their enable flags are
    * VK_FALSE (Valid Usage), so the flags are the whole of that state. With the
    * flag set, Vulkan's valid usage puts maxAnisotropy inside
    * [1, maxSamplerAnisotropy]: this device reports 1.0 (ps5vk_get_properties),
    * so 1.0 is the only legal value there is and the flag cannot ask for
    * anything. Accepting it is therefore a no-op and not a workaround --
    * anisotropic filtering with one sample *is* isotropic -- and refusing it
    * refuses a conformant application: vkQuake's R_InitSamplers creates its
    * point_aniso_sampler in the same unconditional block as its point_sampler
    * and calls Sys_Error on any failure, so the refusal is what keeps the port
    * out of a map. What is refused is a value past the limit this device
    * reports, by that name. */
   const float reported_anisotropy = device->vk.physical->properties.maxSamplerAnisotropy;
   if (info->anisotropyEnable && info->maxAnisotropy > reported_anisotropy)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "sampler anisotropy %.1f is past the maxSamplerAnisotropy %.1f this device "
                       "reports; one sample is isotropic, and more needs a runner probe "
                       "(docs/M5_REFERENCE.md, C4)",
                       (double)info->maxAnisotropy, (double)reported_anisotropy);
   if (info->compareEnable)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "comparison sampling is not the none the texture canary ran; shadow "
                       "samplers need a runner probe (docs/M5_REFERENCE.md, C4)");
   /* Word 11's BORDER_COLOR_TYPE, bits 30-31, for the six core border colours:
    * transparent black 0 (the canary's zero word), opaque black 1, opaque white
    * 2, the float and integer forms sharing a type as in RADV's
    * radv_tex_bordercolor. R57's runner probe samples all three types. Custom
    * border colours are VK_EXT_custom_border_color, which is not exposed. */
   uint32_t border_type;
   switch (info->borderColor) {
   case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
   case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:
      border_type = 0;
      break;
   case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
   case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
      border_type = 1;
      break;
   case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
   case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
      border_type = 2;
      break;
   default:
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "sampler border colour %d is not one of Vulkan 1.0's six; custom border "
                       "colours are VK_EXT_custom_border_color, which is not exposed",
                       (int)info->borderColor);
   }
   if (info->unnormalizedCoordinates)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "unnormalized coordinates are not the normalized ones the texture canary "
                       "ran; they need a runner probe (docs/M5_REFERENCE.md, C4)");
   /* The LOD range, word 9: minLod and maxLod as ps5-opengl packs them, a
    * 12-bit field each holding 1/256 of a level (ps5_texture_descriptor_unsigned_lod).
    * The canary's sampler reached level 0 only and its word is 0x00fff000, which
    * the single-level path keeps (ps5vk_draw.c); a view with more than one level
    * writes this word instead, and C7's runner probe is what proves it. */
   if (info->minLod < 0.0f || info->maxLod < info->minLod)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "sampler LOD range %f to %f is not a range Vulkan allows",
                       (double)info->minLod, (double)info->maxLod);
   struct ps5vk_sampler *const sampler =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*sampler), VK_OBJECT_TYPE_SAMPLER);
   if (!sampler)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   /* A sampler that reaches no mip level keeps the canary's own word: its two
    * words carry mip filter 2 (linear) from a runtime where a single level made
    * the field moot, and every descriptor before Phase C7 holds them exactly.
    * maxLod above 0 asks for levels, and C7's runner probe is what proves the
    * encoding the application asked for. */
   /* The mip filter replaces bits 26-27: the canary's words already carry 2
    * there (linear), so OR-ing the request in would leave 3, a value neither
    * mode names -- which the first C7 run read as a wrong level. */
   sampler->word = info->maxLod > 0.0f ? ((word & ~(UINT32_C(3) << 26)) | (mip_filter << 26))
                                       : word;
   /* GFX10 sampler word 2 carries signed 8-fraction-bit LOD bias in bits
    * 0..13 (Mesa ac_build_sampler_descriptor), room for +/-32. R26 measures
    * implicit LOD selection for both signs and fractional biases on the
    * console, and R79 the reported +/-16: every level of a ten-level chain
    * and both of its ends at +/-8 and +/-16. */
   sampler->word |= (uint32_t)(int32_t)(info->mipLodBias * 256.0f) & 0x3fffu;
   sampler->lod_word = ps5vk_sampler_unsigned_lod(info->minLod) |
                       (ps5vk_sampler_unsigned_lod(info->maxLod) << 12);
   /* Word 0's anisotropy fields, RADV's values for the ratio's log2 (at most 4,
    * 16x): MAX_ANISO_RATIO bits 9-11, ANISO_THRESHOLD 16-18 (the ratio halved)
    * and ANISO_BIAS 21-26 (the ratio); word 1's PERF_MIP (24-27) is ratio + 6
    * when the sampler reaches levels. Zero for an isotropic sampler, which is
    * every word before this. */
   sampler->address_word = address_word | (aniso_ratio << 9) | ((aniso_ratio >> 1) << 16) |
                           (aniso_ratio << 21);
   if (aniso_ratio != 0)
      sampler->lod_word |= (aniso_ratio + 6u) << 24;
   sampler->border_word = border_type << 30;

   *pSampler = ps5vk_sampler_to_handle(sampler);
   return VK_SUCCESS;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateSampler(VkDevice _device, const VkSamplerCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_CreateSampler_untimed(_device, pCreateInfo, pAllocator, pSampler);
   ps5vk_hitch_end(PS5VK_HITCH_IMAGE, hitch);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroySampler(VkDevice _device, VkSampler _sampler, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_sampler, sampler, _sampler);
   if (sampler)
      vk_object_free(&device->vk, pAllocator, sampler);
}

/* R83: the plane an aspect names. A colour image has one, its format's own.
 * A depth format's depth aspect is its depth surface at the image's address,
 * whose texel is the format's element for a depth-only format and four bytes
 * for a combined one (D32_SFLOAT_S8_UINT's depth plane is D32_SFLOAT's
 * surface). A combined format's stencil aspect is the one-byte plane beside
 * it (ps5vk_image_stencil_plane), which only a tiled image has. Both depth
 * planes walk the Z maps (ps5vk_tiled_depth_offset). */
struct ps5vk_image_plane {
   uint64_t offset;
   uint32_t element_bytes;
   bool depth_map;
};

static bool
ps5vk_image_plane(const struct ps5vk_image *image, VkImageAspectFlags aspect,
                  struct ps5vk_image_plane *plane)
{
   const VkFormat format = image->vk.format;
   const bool depth = vk_format_has_depth(format);
   const bool stencil = vk_format_has_stencil(format);
   if (!depth && !stencil) {
      if (aspect != VK_IMAGE_ASPECT_COLOR_BIT)
         return false;
      *plane = (struct ps5vk_image_plane){0, vk_format_get_blocksize(format), false};
      return true;
   }
   if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT && depth) {
      *plane = (struct ps5vk_image_plane){0, stencil ? 4u : vk_format_get_blocksize(format), true};
      return true;
   }
   if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT && stencil && image->stencil_offset != 0) {
      *plane = (struct ps5vk_image_plane){image->stencil_offset, 1u, true};
      return true;
   }
   return false;
}

/* vkCmdCopyBufferToImage lands here: the runtime's
 * vk_common_CmdCopyBufferToImage2 turns the source buffer into the address
 * range this takes and forwards to disp->CmdCopyMemoryToImageKHR
 * (vk_command_buffer.c). The copy is a CPU memcpy at a submission split point,
 * as the buffer copy is (ps5vk_cmd_buffer.c): CPU and GPU share the memory, so
 * the words recorded before it have to run and complete first, which keeps
 * Vulkan's command order (ps5vk_queue.c). A row-layout image's rows are padded
 * to 256 bytes (ps5vk_image_storage) while the source's rows need not be, so
 * one record per row lands at the same split point. imageLayout and
 * addressFlags carry no state here: layouts do not (ps5vk_draw.c), and the
 * access flags describe a GPU copy this CPU one makes itself. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdCopyMemoryToImageKHR(VkCommandBuffer commandBuffer,
                              const VkCopyDeviceMemoryImageInfoKHR *pCopyMemoryInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   const VkCopyDeviceMemoryImageInfoKHR *const info = pCopyMemoryInfo;
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || info->regionCount == 0)
      return;
   VK_FROM_HANDLE(ps5vk_image, image, info->image);
   if (image == NULL)
      return;
   /* An attachment is stored in 64 KiB tiles. Its texels are placed by the
    * measured map (ps5vk_tiled_texel_offset) at the level's own base
    * (ps5vk_image_tiled_level_base), which is what the hardware's rule puts in
    * the table above; a shape the oracle has not been run for refuses here
    * rather than placing texels where the sampler will not look. */
   const bool tiled = image->storage != PS5VK_IMAGE_STORAGE_ROWS;
   if (image->address == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a copy into an image with no GPU address: it has to be bound to "
                              "memory");
      return;
   }
   /* Valid usage: a copy's destination is a transfer destination, its format
    * has texels and it is not multisampled. */
   assert((image->vk.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0);
   assert(vk_format_get_blocksize(image->vk.format) != 0 &&
          image->vk.samples == VK_SAMPLE_COUNT_1_BIT);
   /* The copy falls where this command buffer's words end now: the queue splits
    * the submission there and runs the records once the words before them have
    * completed. The element size and the tile a tiled destination's map walks
    * are each region's aspect's (R83; ps5vk_tile_extent below). */
   const uint32_t after_words =
      (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t);

   for (uint32_t r = 0; r < info->regionCount; r++) {
      const VkDeviceMemoryImageCopyKHR *const region = &info->pRegions[r];
      /* Valid usage: the copy names one of the image's levels. A level's rows
       * pad to 256 bytes and the levels stack in order, which is the layout
       * ps5vk_image_storage sizes and the descriptor's level count tells the
       * hardware to read (ps5vk_image_level_layout, C7's mip probe). */
      assert(region->imageSubresource.mipLevel < image->vk.mip_levels);
      uint64_t level_offset = 0;
      uint64_t row_pitch = 0;
      VkExtent2D level_extent = {0, 0};
      ps5vk_image_level_layout(image, region->imageSubresource.mipLevel, &level_offset, &row_pitch,
                               &level_extent);
      level_offset += ps5vk_image_layer_bytes(image) * region->imageSubresource.baseArrayLayer;
      /* One layer a region (D1): its base is the layer's slice plus the level
       * inside it, so a region that names several layers at once refuses --
       * Vulkan's own striding between them is a bufferImageHeight rule no probe
       * has recorded. */
      if (region->imageSubresource.layerCount != 1) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "an upload of %u layers in one region from layer %u; one layer "
                                 "a region is what D1's measured slices support "
                                 "(docs/M5_REFERENCE.md)",
                                 region->imageSubresource.layerCount,
                                 region->imageSubresource.baseArrayLayer);
         return;
      }
      if (region->imageSubresource.baseArrayLayer >= image->vk.array_layers) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "an upload names layer %u of a %u-layer image",
                                 region->imageSubresource.baseArrayLayer,
                                 image->vk.array_layers);
         return;
      }
      /* Valid usage: a 2D image copies one slice with an aspect its format
       * has -- colour for a colour format, depth for a depth one (round 21's
       * depth pair uploads a D16 image), and depth or stencil for a combined
       * one, each into its own plane (R83, ps5vk_image_plane) -- at a
       * non-negative offset and inside the image. Any other aspect is refused
       * by name, as is a stencil plane of more than one level or layer. */
      struct ps5vk_image_plane plane;
      if (!ps5vk_image_plane(image, region->imageSubresource.aspectMask, &plane) ||
          (plane.offset != 0 && (image->vk.mip_levels != 1 || image->vk.array_layers != 1))) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "an upload names aspect 0x%x of a format %d image with %u levels "
                                 "and %u layers",
                                 (unsigned)region->imageSubresource.aspectMask,
                                 (int)image->vk.format, image->vk.mip_levels,
                                 image->vk.array_layers);
         return;
      }
      const unsigned texel_bytes = plane.element_bytes;
      uint32_t tile_width = 0;
      uint32_t tile_height = 0;
      if (tiled)
         ps5vk_tile_extent(texel_bytes, plane.depth_map, image->vk.samples, &tile_width,
                           &tile_height);
      assert(region->imageOffset.x >= 0 && region->imageOffset.y >= 0 &&
             region->imageOffset.z == 0);
      assert(region->imageExtent.depth == 1);
      assert((uint64_t)region->imageOffset.x + region->imageExtent.width <= level_extent.width);
      assert((uint64_t)region->imageOffset.y + region->imageExtent.height <= level_extent.height);
      /* addressRowLength is in texels and 0 means tightly packed (Valid Usage);
       * addressImageHeight only strides slices, and this copy is one slice. */
      if (region->addressRowLength != 0 && region->addressRowLength < region->imageExtent.width) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "a source row of %u texels is shorter than the %u the copy reads",
                                 region->addressRowLength, region->imageExtent.width);
         return;
      }
      const uint64_t source_pitch =
         (region->addressRowLength == 0 ? region->imageExtent.width : region->addressRowLength) *
         texel_bytes;
      const uint64_t row_bytes = (uint64_t)region->imageExtent.width * texel_bytes;
      const uint64_t needed =
         region->imageExtent.height == 0
            ? 0
            : (uint64_t)(region->imageExtent.height - 1) * source_pitch + row_bytes;
      /* An unbound source has no address and no rows copy nothing; a range too
       * short for the rows would read past it. */
      if (region->addressRange.address == 0 || needed == 0)
         continue;
      if (region->addressRange.size < needed) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "the source range holds %" PRIu64 " of the %" PRIu64
                                 " bytes the copy reads", region->addressRange.size, needed);
         return;
      }
      if (tiled) {
         /* A tiled destination's row is not a run of bytes: every texel lives at
          * the map's own place, so the region is one record the queue walks
          * texel by texel (ps5vk_image_write_execute). */
         const uint64_t level_base = ps5vk_image_tiled_layer_level_base(
            image, region->imageSubresource.mipLevel, region->imageSubresource.baseArrayLayer);
         if (level_base == UINT64_MAX) {
            ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                    "an upload into the %ux%u %u-level tiled chain: the oracle "
                                    "has not been run for that shape, so the level bases are "
                                    "unmeasured (tools/check-mip-layout.sh, C7)",
                                    image->vk.extent.width, image->vk.extent.height,
                                    image->vk.mip_levels);
            return;
         }
         struct ps5vk_memory_copy *const copy =
            util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
         if (!copy) {
            ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                    "no memory to record an image copy");
            return;
         }
         /* A depth plane walks the Z maps a depth target is written in, which
          * the readback of every C5 frame proved (ps5vk_tiled_depth_offset):
          * the colour map would scatter its texels (R83). */
         *copy = (struct ps5vk_memory_copy){
            .image_write = true,
            .source = region->addressRange.address,
            .source_pitch = source_pitch,
            .destination_side =
               {
                  .address = image->address + plane.offset + (level_base & ~UINT64_C(0xffff)),
                  .tile_xor = (uint32_t)(level_base & UINT64_C(0xffff)),
                  .level_width = level_extent.width,
                  .tile_width = tile_width,
                  .tile_height = tile_height,
                  .element_bytes = texel_bytes,
                  .samples = 1,
                  .depth = plane.depth_map,
                  .tiled = true,
               },
            .destination_x = (uint32_t)region->imageOffset.x,
            .destination_y = (uint32_t)region->imageOffset.y,
            .width = region->imageExtent.width,
            .height = region->imageExtent.height,
            .destination_texel_bytes = texel_bytes,
            .reverse_texel_bytes = ps5vk_image_storage_reversed(image) ? texel_bytes : 0,
            .after_words = after_words,
         };
         continue;
      }
      /* Keep one record per region, as image-to-image copies do. Expanding
       * every row into a 272-byte record can exhaust a title's host heap while
       * staging a map; the queue already knows how to walk two pitched sides. */
      struct ps5vk_memory_copy *const copy =
         util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
      if (!copy) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                 "no memory to record an image copy");
         return;
      }
      *copy = (struct ps5vk_memory_copy){
         .image_copy = true,
         .source_side = {.address = region->addressRange.address, .row_pitch = source_pitch},
         .destination_side = {.address = image->address + level_offset, .row_pitch = row_pitch},
         .destination_x = (uint32_t)region->imageOffset.x,
         .destination_y = (uint32_t)region->imageOffset.y,
         .width = region->imageExtent.width,
         .height = region->imageExtent.height,
         .source_texel_bytes = texel_bytes,
         .reverse_texel_bytes = ps5vk_image_storage_reversed(image) ? texel_bytes : 0,
         .after_words = after_words,
      };
   }
}

/* ---------------- vkCmdCopyImage and vkCmdBlitImage (Phase C7) ------------
 *
 * Both are CPU copies at a submission split point, like the buffer-to-image
 * copy above: what the words before them recorded has to have run and
 * completed before the bytes move, and the words after them run once it has
 * (ps5vk_queue.c). Neither is a GPU operation, so both refuse by name what a
 * probe has not proved yet.
 *
 * A row-layout side is linear with its level's padded row pitch; a tiled side
 * is the measured RGBA8 map: 128x128-texel blocks of 0x10000 bytes in
 * row-major order with the XOR bit map inside a block that the C7 address map
 * fitted to the byte (docs/HARDWARE_FINDINGS.md, and src/diagnostics.cpp's
 * tiled_rgba8_offset, the same map read back on the console). Inside a tiled
 * row the map keeps 16 bytes contiguous and no more, so a tiled side copies one
 * 16-byte chunk at a time.
 *
 * A two-byte element's map is its own row of the same table, not a scaling of
 * the four-byte one: AddrLib picks the swizzle pattern by the element size, so
 * a 2-byte texel's placement inside a tile differs from a 4-byte one's in the
 * high address bits as well (the tile is 256x128 texels rather than 128x128,
 * and the y terms differ). Both rows below are the console's configuration as
 * AddrLib itself computes it -- sixteen pipes, a non-RbPlus revision, no
 * pipe/bank rotation -- which tools/mip-layout-oracle derives with
 * `swizzle <bytes> <samples> <mode>` and tools/check-mip-layout.sh holds these
 * tables against. With a default one-pipe library AddrLib answers with a
 * different map, which is the trap that check exists for
 * (docs/HARDWARE_FINDINGS.md).
 *
 * Only single-level tiled images have a map: a chain's levels sit at the
 * AddrLib bases the hardware uses, which ps5vk_image.c sizes but does not lay
 * out yet, and a four-sample depth image's texel is four samples four bytes
 * apart, which no probe has walked. Both are refused by name below, and the C7
 * records carry what closes them.
 */

/* One term of the map: ((coord << shift) & mask), the form this driver writes
 * the address library's per-bit pattern in, with coord 0 the tile-local x and 1
 * its y. One row per element size; a four-sample texel is four sample planes at
 * one of these positions, which ps5vk_image_copy_address's sample offsets
 * (PS5VK_SAMPLE_PLANE_BYTES). */
struct ps5vk_tiled_term {
   uint8_t coord;
   uint8_t shift;
   uint32_t mask;
};

static const struct ps5vk_tiled_term ps5vk_tiled_4b_terms[] = {
   {0, 2, 0x0cu},   {0, 5, 0x380u}, {0, 4, 0x400u},  {0, 6, 0x800u}, {0, 9, 0xa000u},
   {1, 4, 0x70u},   {1, 5, 0xf00u}, {1, 9, 0x1000u}, {1, 8, 0x4000u},
};

/* The one-byte colour map, AddrLib's row for a 64 KiB R_X tile of one-byte
 * elements (tile 256x256 texels, thirteen terms): what an 8-bit target needs
 * and the driver had used the four-byte row for (round 11, docs/M5_PHASE_C.md). */
static const struct ps5vk_tiled_term ps5vk_tiled_1b_terms[] = {
   {0, 0, 0x7u},    {0, 3, 0x40u},   {0, 5, 0x300u},  {0, 6, 0x800u},  {0, 4, 0x400u},
   {0, 7, 0x2000u}, {0, 8, 0x8000u}, {1, 4, 0x10u},   {1, 2, 0x8u},    {1, 3, 0xa0u},
   {1, 5, 0xf00u},  {1, 6, 0x1000u}, {1, 7, 0x4000u},
};

/* The eight-byte colour map, AddrLib's own row for a 64 KiB R_X tile of
 * eight-byte elements (tools/check-mip-layout.sh asks the oracle for it, tile
 * 128x64 texels, ten terms) -- what an eight-byte colour attachment needs and
 * nothing had applied yet (docs/M5_PHASE_C.md, round 8). */
static const struct ps5vk_tiled_term ps5vk_tiled_8b_terms[] = {
   {0, 3, 0x8u},    {0, 4, 0x460u},  {0, 5, 0x300u},  {0, 10, 0x2000u}, {0, 6, 0x800u},
   {0, 9, 0x8000u}, {1, 4, 0x10u},   {1, 6, 0x80u},   {1, 10, 0x5000u}, {1, 5, 0x700u},
};

/* The sixteen-byte colour map, AddrLib's row for a 64 KiB R_X tile of
 * sixteen-byte elements (tile 64x64 texels, eight terms): the map a
 * 32_32_32_32 target needs (docs/M5_PHASE_C.md, round 9). */
static const struct ps5vk_tiled_term ps5vk_tiled_16b_terms[] = {
   {0, 4, 0x10u},   {0, 5, 0x340u}, {0, 11, 0xa000u}, {0, 6, 0x800u},
   {1, 5, 0x720u},  {1, 6, 0x80u},  {1, 10, 0x1000u}, {1, 11, 0x4000u},
};

static const struct ps5vk_tiled_term ps5vk_tiled_2b_terms[] = {
   {0, 1, 0x0eu},   {0, 4, 0x480u}, {0, 5, 0x300u},  {0, 6, 0x800u},
   {0, 7, 0x2000u}, {0, 8, 0x8000u}, {1, 4, 0x70u},  {1, 5, 0xf00u},
   {1, 8, 0x5000u},
};

/* The pixel count one tiled row's runs are made of: 16 bytes of texels. */

/* Where texel (x, y) of a single-level tiled image lives, relative to the
 * image's address: its 64 KiB tile's offset in the tile grid, with the texel's
 * swizzled position *inside* that tile written to swizzle, in bytes. The
 * swizzle is the map of the texel's place *inside* its tile, so it is computed
 * from the texel's coordinates modulo the tile. level_width is the level's own
 * width in texels, which is what the block grid strides by.
 *
 * A four-sample tile's coordinates are turned by the block's own place in the
 * grid before the swizzle: bit 5 of the block-local y is XORed with bit 0 of
 * the block's column and bit 5 of the block-local x with bit 0 of its row,
 * which is what the console measured on thirteen blocks (block (0,0) is
 * unturned, (1,0) turns y, (0,1) turns x, (1,1) turns both, (2,0) and (0,2)
 * are unturned again, docs/M5_PHASE_C.md). A one-sample tile is not turned:
 * that map's own measurement has no XOR in it. A four-sample texel is four
 * bytes of four sample planes at one swizzled position, so its map is the
 * four-byte one. */
static uint64_t
ps5vk_tiled_texel_offset(uint32_t x, uint32_t y, uint32_t level_width, uint32_t tile_width,
                         uint32_t tile_height, uint32_t element_bytes, uint32_t samples,
                         uint64_t *swizzle)
{
   /* The common RGBA8 one-sample map has a fixed 128x128 tile. Keep the
    * measured equation, but avoid table interpretation and variable integer
    * division at every source tap of CPU copies and mip blits. */
   if (element_bytes == 4u && samples == 1u && tile_width == 128u && tile_height == 128u) {
      const uint32_t sx = x & 127u, sy = y & 127u;
      *swizzle = ((sx << 2) & 0x0cu) ^ ((sx << 5) & 0x380u) ^
                 ((sx << 4) & 0x400u) ^ ((sx << 6) & 0x800u) ^
                 ((sx << 9) & 0xa000u) ^ ((sy << 4) & 0x70u) ^
                 ((sy << 5) & 0xf00u) ^ ((sy << 9) & 0x1000u) ^
                 ((sy << 8) & 0x4000u);
      return ((uint64_t)(y >> 7) * DIV_ROUND_UP(level_width, 128u) + (x >> 7)) * PS5VK_TILE_BYTES;
   }
   uint32_t in_x = x & (tile_width - 1u);
   uint32_t in_y = y & (tile_height - 1u);
   if (samples == 4u) {
      in_x ^= (y / tile_height & 1u) * (tile_width / 2u);
      in_y ^= (x / tile_width & 1u) * (tile_height / 2u);
   } else if (element_bytes == 16u) {
      /* The ramp frame's measurement (docs/HARDWARE_FINDINGS.md, 2026-09-20): a
       * sixteen-byte element's tile carries the whole block XOR a four-sample
       * image's does -- the tile row twisted by the tile column's parity and the
       * tile column by the tile row's -- and both halves are what the probe's
       * candidate check read: the y-only candidate matched everywhere the image
       * has texels and the two together matched every probe tile. */
      in_x ^= (y / tile_height & 1u) * (tile_width / 2u);
      in_y ^= (x / tile_width & 1u) * (tile_height / 2u);
   }
   const struct ps5vk_tiled_term *const terms =
      element_bytes == 1u    ? ps5vk_tiled_1b_terms
      : element_bytes == 2u  ? ps5vk_tiled_2b_terms
      : element_bytes == 8u  ? ps5vk_tiled_8b_terms
      : element_bytes == 16u ? ps5vk_tiled_16b_terms
                             : ps5vk_tiled_4b_terms;
   const unsigned term_count = element_bytes == 1u    ? (unsigned)ARRAY_SIZE(ps5vk_tiled_1b_terms)
                               : element_bytes == 2u  ? (unsigned)ARRAY_SIZE(ps5vk_tiled_2b_terms)
                               : element_bytes == 8u  ? (unsigned)ARRAY_SIZE(ps5vk_tiled_8b_terms)
                               : element_bytes == 16u ? (unsigned)ARRAY_SIZE(ps5vk_tiled_16b_terms)
                                                      : (unsigned)ARRAY_SIZE(ps5vk_tiled_4b_terms);
   uint64_t local = 0;
   for (unsigned index = 0; index < term_count; index++) {
      const uint64_t value = terms[index].coord == 0u ? in_x : in_y;
      local ^= (value << terms[index].shift) & terms[index].mask;
   }
   const uint64_t blocks_per_row = DIV_ROUND_UP(level_width, tile_width);
   const uint64_t block = (uint64_t)(y / tile_height) * blocks_per_row + x / tile_width;
   *swizzle = local;
   return block * PS5VK_TILE_BYTES;
}

/* The two-byte depth map, AddrLib's own Z_X row for that element size: a
 * 256x128-texel tile with thirteen terms. It is what a D16_UNORM depth target's
 * readback decodes, and round 14's groundwork for the row (docs/M5_PHASE_C.md);
 * tools/check-mip-layout.sh compares it with the oracle. */
static const struct ps5vk_tiled_term ps5vk_tiled_depth2_terms[] = {
   {0, 1, 0x2u},    {0, 2, 0x8u},   {0, 3, 0x20u},   {0, 4, 0x480u},
   {0, 5, 0x300u},  {0, 6, 0x800u}, {0, 7, 0x2000u}, {0, 8, 0x8000u},
   {1, 2, 0x4u},    {1, 3, 0x10u},  {1, 4, 0x40u},   {1, 5, 0xf00u},
   {1, 8, 0x5000u},
};

/* R83: the one-byte stencil plane's map, AddrLib's 64 KiB Z_X row for a
 * one-byte element: a 256x256-texel tile with fifteen terms, which
 * `mip-layout-oracle swizzle 1 1 64kb_z_x` prints and verifies over all 65536
 * texels of the tile (tools/check-mip-layout.sh holds the table against it).
 * It is the plane the depth block writes stencil into (ps5vk_image_stencil_plane)
 * and what an upload, readback, copy or clear of the stencil aspect walks. */
static const struct ps5vk_tiled_term ps5vk_tiled_stencil_terms[] = {
   {0, 0, 0x1u},   {0, 1, 0x4u},   {0, 2, 0x10u},   {0, 3, 0x40u},   {0, 5, 0x300u},
   {0, 6, 0x800u}, {0, 4, 0x400u}, {0, 7, 0x2000u}, {0, 8, 0x8000u}, {1, 1, 0x2u},
   {1, 2, 0x8u},   {1, 3, 0xa0u},  {1, 5, 0xf00u},  {1, 6, 0x1000u}, {1, 7, 0x4000u},
};

/* Where texel (x, y) of a single-level tiled *depth* image lives, relative to
 * the image's address: 128x128-texel blocks of 0x10000 bytes in row-major order
 * with fixed XOR masks for the block-local coordinates, which is ps5-opengl's
 * ps5_tiled_depth_offset (src/diagnostics.cpp, tiled_depth_offset, the same
 * table). The console proved it: every pixel of every C5 frame reads its own
 * depth value through it (run_vulkan_depth_frames' check, the c5-depth
 * battery), so a depth clear may write through it. A two-byte element's row is
 * its own table and tile (256x128 texels), ps5vk_tiled_depth2_terms. */
static uint64_t
ps5vk_tiled_depth_offset(uint32_t x, uint32_t y, uint32_t level_width, uint32_t element_bytes)
{
   if (element_bytes == 1u) {
      /* The stencil plane's row (R83), over a 256x256-texel tile. */
      const uint32_t in_x = x & 255u;
      const uint32_t in_y = y & 255u;
      uint64_t local = 0;
      for (unsigned index = 0; index < ARRAY_SIZE(ps5vk_tiled_stencil_terms); index++) {
         const uint64_t value = ps5vk_tiled_stencil_terms[index].coord == 0u ? in_x : in_y;
         local ^= (value << ps5vk_tiled_stencil_terms[index].shift) &
                  ps5vk_tiled_stencil_terms[index].mask;
      }
      const uint64_t blocks_per_row = DIV_ROUND_UP(level_width, 256u);
      return (((uint64_t)y >> 8) * blocks_per_row + (x >> 8)) * PS5VK_TILE_BYTES + local;
   }
   if (element_bytes == 2u) {
      /* The two-byte row's own table, over a 256x128-texel tile. */
      const uint32_t in_x = x & 255u;
      const uint32_t in_y = y & 127u;
      uint64_t local = 0;
      for (unsigned index = 0; index < ARRAY_SIZE(ps5vk_tiled_depth2_terms); index++) {
         const uint64_t value = ps5vk_tiled_depth2_terms[index].coord == 0u ? in_x : in_y;
         local ^= (value << ps5vk_tiled_depth2_terms[index].shift) & ps5vk_tiled_depth2_terms[index].mask;
      }
      const uint64_t blocks_per_row = DIV_ROUND_UP(level_width, 256u);
      return (((uint64_t)y >> 7) * blocks_per_row + (x >> 8)) * PS5VK_TILE_BYTES + local;
   }
   static const uint16_t x_masks[7] = {0x0004, 0x0010, 0x0040, 0x0100, 0x2200, 0x0800, 0x8400};
   static const uint16_t y_masks[7] = {0x0008, 0x0020, 0x0080, 0x1100, 0x0200, 0x0400, 0x4800};
   uint64_t local = 0;
   for (unsigned bit = 0; bit < 7; bit++) {
      if (((x >> bit) & 1u) != 0)
         local ^= x_masks[bit];
      if (((y >> bit) & 1u) != 0)
         local ^= y_masks[bit];
   }
   const uint64_t blocks_per_row = DIV_ROUND_UP(level_width, 128u);
   return (((uint64_t)y >> 7) * blocks_per_row + (x >> 7)) * PS5VK_TILE_BYTES + local;
}

/* Where one sample of a four-sample texel lives inside its tile: the samples
 * are four 0x4000-byte planes, in sample order, and each plane walks the
 * same four-byte swizzle the map is for a 64x64-texel block
 * (ps5vk_image_copy_address). The console measured the planes: a four-sample
 * frame's tile holds four identical planes, each one word a texel, and the
 * plane's word order is the swizzle of the texel's place in the block
 * (docs/M5_PHASE_C.md, C8). A four-sample *depth* texel is not planes: its four
 * samples sit four bytes apart inside one sixteen-byte texel
 * (PS5VK_DEPTH_SAMPLE_BYTES). */
#define PS5VK_SAMPLE_PLANE_BYTES UINT64_C(0x4000)
#define PS5VK_DEPTH_SAMPLE_BYTES UINT64_C(4)

/* The depth map's terms for a four-sample tile, from AddrLib's own rows
 * (tools/mip-layout-oracle, `swizzle 4 4 64kb_z_x`). A colour tile's samples
 * are four 0x4000-byte planes -- that row's sample term is address bits 14 and
 * 15 -- while a depth tile's four samples are address bits 2 and 3, four bytes
 * apart inside one sixteen-byte texel, which is why the two modes need
 * different maps and why this one carries no sample term: the sample's offset
 * is PS5VK_DEPTH_SAMPLE_BYTES times its index. */
static const struct ps5vk_tiled_term ps5vk_tiled_depth4_terms[] = {
   {0, 4, 0x10u},  {0, 5, 0x340u}, {0, 13, 0x8000u}, {0, 9, 0x2000u}, {0, 6, 0x800u},
   {1, 5, 0x720u}, {1, 6, 0x80u},  {1, 12, 0x4000u}, {1, 9, 0x1000u},
};

/* Where the four samples of one texel of a four-sample depth image live,
 * relative to the image's address: a 64x64-texel tile of 0x10000 bytes, the
 * texel's place inside it from the depth map above, and the tile's own place in
 * the grid XORed into the high bits of the texel's position -- the column's low
 * bit into bit 10, and the row's two low bits into bits 11, 14 and 15 -- which
 * is what the oracle's multi-tile derivation adds to the single-tile one. */
static uint64_t
ps5vk_tiled_depth4_offset(uint32_t x, uint32_t y, uint32_t level_width, uint32_t tile_width,
                          uint32_t tile_height)
{
   const uint32_t in_x = x & (tile_width - 1u);
   const uint32_t in_y = y & (tile_height - 1u);
   const uint32_t column = x / tile_width;
   const uint32_t row = y / tile_height;
   uint64_t local = 0;
   for (unsigned index = 0; index < ARRAY_SIZE(ps5vk_tiled_depth4_terms); index++) {
      const uint64_t value = ps5vk_tiled_depth4_terms[index].coord == 0u ? in_x : in_y;
      local ^= (value << ps5vk_tiled_depth4_terms[index].shift) & ps5vk_tiled_depth4_terms[index].mask;
   }
   local ^= (uint64_t)(column & 1u) << 10;
   local ^= (uint64_t)(row & 1u) << 11;
   local ^= (uint64_t)(row & 1u) << 14;
   local ^= (uint64_t)((row >> 1) & 1u) << 15;
   const uint64_t blocks_per_row = DIV_ROUND_UP(level_width, tile_width);
   return ((uint64_t)row * blocks_per_row + column) * PS5VK_TILE_BYTES + local;
}

/* Whether an image can be one side of a copy, and the level layout of the one
 * level a copy names. A tiled side needs a map: one level, and an element whose
 * size the map covers (two or four bytes; a four-sample texel is the four-byte
 * map with its samples counted). */
static bool
ps5vk_image_copy_side(struct ps5vk_image *image, const VkImageSubresourceLayers *subresource,
                      uint32_t *texel_bytes, struct ps5vk_image_copy_side *side)
{
   /* The subresource's aspect picks the plane (R83): a combined depth/stencil
    * image copies its depth and its stencil separately. */
   struct ps5vk_image_plane plane;
   if (!ps5vk_image_plane(image, subresource->aspectMask, &plane))
      return false;
   const uint32_t element_bytes = plane.element_bytes;
   if (element_bytes == 0)
      return false;
   /* A four-sample image's texel is its four samples, sixteen bytes of a
    * four-byte format, and its tile is half as wide (ps5vk_tile_extent). Two
    * and eight samples have no measured texel map, so the CPU does not walk
    * them (R78): the GPU renders, samples and resolves them. */
   if (image->vk.samples != VK_SAMPLE_COUNT_1_BIT && image->vk.samples != VK_SAMPLE_COUNT_4_BIT)
      return false;
   const uint32_t samples = image->vk.samples == VK_SAMPLE_COUNT_4_BIT ? 4u : 1u;
   *texel_bytes = element_bytes * samples;
   const bool tiled = image->storage == PS5VK_IMAGE_STORAGE_TILES;
   /* A depth image's tiled map is its own (ps5vk_tiled_depth_offset), and so is
    * its stencil plane's. The stencil plane is laid out for one level and one
    * layer only (ps5vk_image_stencil_plane sums its levels' tiles, a shape no
    * probe has checked for more), and a four-sample one is not measured. */
   const bool depth = plane.depth_map;
   if (plane.offset != 0 && (image->vk.mip_levels != 1 || image->vk.array_layers != 1 ||
                             image->vk.samples != VK_SAMPLE_COUNT_1_BIT))
      return false;
   uint64_t level_offset = 0;
   VkExtent2D extent = {0, 0};
   uint64_t row_pitch = 0;
   uint32_t tile_width = 0;
   uint32_t tile_height = 0;
   if (tiled) {
      /* A two-byte element is mapped for one sample only: its four-sample texel
       * would be eight bytes of two sample planes no probe has walked, and a
       * four-byte element's sixteen-byte four-sample texel is the map's own
       * case. An eight-byte element is mapped for one sample (round 8).
       * Anything else has no map. */
      if (element_bytes != 4 && element_bytes != 8 && element_bytes != 16 &&
          !((element_bytes == 1 || element_bytes == 2) && samples == 1))
         return false;
      ps5vk_tile_extent(element_bytes, depth, image->vk.samples, &tile_width, &tile_height);
      if (image->vk.mip_levels != 1) {
         const uint64_t base =
            ps5vk_image_tiled_layer_level_base(image, subresource->mipLevel,
                                               subresource->baseArrayLayer);
         if (base == UINT64_MAX)
            return false;
         const VkExtent2D extent = {
            (uint32_t)MAX2(image->vk.extent.width >> subresource->mipLevel, 1u),
            (uint32_t)MAX2(image->vk.extent.height >> subresource->mipLevel, 1u)};
         *side = (struct ps5vk_image_copy_side){
            .address = image->address + (base & ~UINT64_C(0xffff)),
            .tile_xor = (uint32_t)(base & UINT64_C(0xffff)),
            .row_pitch = 0,
            .level_width = extent.width,
            .tile_width = tile_width,
            .tile_height = tile_height,
            .element_bytes = element_bytes,
            .samples = samples,
            .depth = depth,
            .tiled = true,
         };
         return true;
      }
      if (image->vk.array_layers > 1) {
         const uint64_t slice = ps5vk_image_layer_bytes(image);
         if (slice == 0)
            return false;
         level_offset = slice * subresource->baseArrayLayer;
      }
      level_offset += 0;
      extent = (VkExtent2D){image->vk.extent.width, image->vk.extent.height};
      row_pitch = 0;
      /* A four-sample depth image is mapped: its texel is one sixteen-byte unit
       * holding all four samples, so a copy needs no per-sample walk
       * (ps5vk_tiled_depth4_offset). A one-sample depth plane is four bytes a
       * texel, a stencil plane one (R83), and anything else has no map. */
      if (depth && *texel_bytes != 4 && *texel_bytes != 16 && *texel_bytes != 1)
         return false;
      if (depth && image->vk.mip_levels != 1)
         return false;
   } else {
      /* A multi-sample image's linear layout is unmeasured: only the tiled map
       * has a measured sample-plane layout (docs/M5_PHASE_C.md, C8). */
      if (samples != 1)
         return false;
      ps5vk_image_level_layout(image, subresource->mipLevel, &level_offset, &row_pitch, &extent);
   }
   *side = (struct ps5vk_image_copy_side){
      .address = image->address + plane.offset + level_offset,
      .row_pitch = tiled ? 0 : row_pitch,
      .level_width = extent.width,
      .tile_width = tile_width,
      .tile_height = tile_height,
      .element_bytes = element_bytes,
      .samples = samples,
      .depth = depth,
      .tiled = tiled,
   };
   return true;
}

uint64_t
ps5vk_image_copy_address(const struct ps5vk_image_copy_side *side, int32_t x, int32_t y,
                         uint32_t texel_bytes, uint32_t sample)
{
   if (side->tiled) {
      /* A tile is 0x10000 bytes whatever the texel, and the swizzled position
       * is a byte offset. A depth image's map is its own: a one-sample texel is
       * one word at ps5vk_tiled_depth_offset's position, and a four-sample one
       * is a sixteen-byte texel at ps5vk_tiled_depth4_offset's, whose four
       * samples are PS5VK_DEPTH_SAMPLE_BYTES apart inside it. A colour image's
       * four-sample texel is instead four 0x4000-byte planes at one swizzled
       * position (PS5VK_SAMPLE_PLANE_BYTES), one a sample. */
      if (side->depth) {
         if (side->samples == 4)
            return side->address +
                   ps5vk_tiled_depth4_offset((uint32_t)MAX2(x, 0), (uint32_t)MAX2(y, 0),
                                             side->level_width, side->tile_width,
                                             side->tile_height) +
                   (uint64_t)(sample & 3u) * PS5VK_DEPTH_SAMPLE_BYTES;
         return side->address +
                ps5vk_tiled_depth_offset((uint32_t)MAX2(x, 0), (uint32_t)MAX2(y, 0),
                                         side->level_width, texel_bytes);
      }

      uint64_t swizzle = 0;
      const uint64_t tile = ps5vk_tiled_texel_offset(
         x, y, side->level_width, side->tile_width, side->tile_height, side->element_bytes,
         side->samples, &swizzle);
      const uint64_t plane =
         side->samples == 4 ? (uint64_t)(sample & 3u) * PS5VK_SAMPLE_PLANE_BYTES : 0;
      return side->address + tile + plane + (swizzle ^ side->tile_xor);
   }
   return side->address + (uint64_t)y * side->row_pitch + (uint64_t)x * texel_bytes;
}

/* Records the copies of one region: extent texels from (source_x, source_y) to
 * (destination_x, destination_y), each side placed by its own map. The record
 * carries the two sides, the rectangle and the texel size, and the queue walks
 * the runs at the split point (ps5vk_image_copy_execute): one record a region,
 * not one a run, because a four-sample depth image's sixteen-byte texel is one
 * run each and a 4K copy of one is 8.3 million records, more than an
 * application's allocator holds (the console run that found this is in
 * docs/HARDWARE_FINDINGS.md). */
static void
ps5vk_cmd_buffer_copy_image_region(struct ps5vk_cmd_buffer *cmd_buffer, uint32_t after_words,
                                   const struct ps5vk_image_copy_side *source,
                                   const struct ps5vk_image_copy_side *destination, uint32_t source_x,
                                   uint32_t source_y, uint32_t destination_x, uint32_t destination_y,
                                   VkExtent3D extent, uint32_t texel_bytes)
{
   struct ps5vk_memory_copy *const copy =
      util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
   if (!copy) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record an image copy");
      return;
   }
   *copy = (struct ps5vk_memory_copy){
      .image_copy = true,
      .source_side = *source,
      .destination_side = *destination,
      .source_x = (int32_t)source_x,
      .source_y = (int32_t)source_y,
      .destination_x = destination_x,
      .destination_y = destination_y,
      .width = extent.width,
      .height = extent.height,
      .source_texel_bytes = texel_bytes,
      .reverse_texel_bytes = 0,
      .after_words = after_words,
   };
}

/* Whether a blit region copies its source rectangle as it stands: the same
 * extent on both sides and no negative scale, which is the shape vkCmdBlitImage
 * shares with vkCmdCopyImage. A scaled or mirrored region needs the blit draw,
 * which is C7's next piece (docs/M5_REFERENCE.md). */
static bool
ps5vk_blit_region_is_copy(const VkImageBlit2 *region)
{
   const int32_t source_width = region->srcOffsets[1].x - region->srcOffsets[0].x;
   const int32_t source_height = region->srcOffsets[1].y - region->srcOffsets[0].y;
   const int32_t destination_width = region->dstOffsets[1].x - region->dstOffsets[0].x;
   const int32_t destination_height = region->dstOffsets[1].y - region->dstOffsets[0].y;
   return source_width > 0 && source_height > 0 && source_width == destination_width &&
          source_height == destination_height && region->srcOffsets[0].z == 0 &&
          region->dstOffsets[0].z == 0 && region->srcOffsets[1].z == 1 &&
          region->dstOffsets[1].z == 1;
}

/* What a copy and a blit both need of their two images: bound, 2D ones of one
 * kind, and a tiled side this driver has a measured map for. same_format is true
 * for a copy, which Vulkan defines between images of one format, and false for a
 * blit, whose formats may differ (the resampler handles four-byte texels and a
 * filtered blit R8G8B8A8_UNORM alone). A depth image copies into a depth image
 * of its own format: both sides are placed by a measured depth map -- the
 * one-sample four-byte one or the four-sample sixteen-byte one, and
 * ps5vk_image_copy_side refuses any other depth shape -- and a depth copy moves
 * texels as they stand, with no decode. False with the recording refused by
 * name. */
static bool
ps5vk_image_transfer_check(struct ps5vk_cmd_buffer *cmd_buffer, struct ps5vk_image *source,
                           struct ps5vk_image *destination, const char *what, bool same_format)
{
   if (source->address == 0 || destination->address == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "%s names an image with no GPU address: both have to be bound to "
                              "memory", what);
      return false;
   }
   if (same_format && source->vk.format != destination->vk.format) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "%s between a format %u and a format %u image; only images of one "
                              "format copy (docs/M5_REFERENCE.md, C7)",
                              what, (unsigned)source->vk.format, (unsigned)destination->vk.format);
      return false;
   }
   if (source->vk.image_type != VK_IMAGE_TYPE_2D ||
       destination->vk.image_type != VK_IMAGE_TYPE_2D) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "%s of non-2D images; C7's are 2D ones (docs/M5_REFERENCE.md)",
                              what);
      return false;
   }
   const bool source_depth =
      vk_format_has_depth(source->vk.format) || vk_format_has_stencil(source->vk.format);
   const bool destination_depth =
      vk_format_has_depth(destination->vk.format) || vk_format_has_stencil(destination->vk.format);
   if (source_depth != destination_depth ||
       (source_depth && source->vk.format != destination->vk.format)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "%s between a %s image and a %s one; a depth image copies into a "
                              "depth image of its own format (docs/M5_REFERENCE.md, C7)",
                              what, source_depth ? "depth or stencil" : "colour",
                              destination_depth ? "depth or stencil" : "colour");
      return false;
   }
   /* Each tiled side needs its measured element map and, for a chain, the
    * measured per-level bases already used by uploads and readback. */
   const struct ps5vk_image *images[2] = {source, destination};
   for (unsigned i = 0; i < 2; i++) {
      const struct ps5vk_image *image = images[i];
      if (image->storage != PS5VK_IMAGE_STORAGE_TILES)
         continue;
      const unsigned bytes = vk_format_get_blocksize(image->vk.format);
      const bool mapped = bytes == 4 || bytes == 8 || bytes == 16 ||
         ((bytes == 1 || bytes == 2) && image->vk.samples == VK_SAMPLE_COUNT_1_BIT);
      const bool chain_mapped = image->vk.mip_levels == 1 ||
         (!vk_format_has_depth(image->vk.format) && ps5vk_tiled_chain_of(image) != NULL);
      if (!mapped || !chain_mapped) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
            "%s of a %ux%u %u-level tiled image, format %u: no measured element/chain map",
            what, image->vk.extent.width, image->vk.extent.height,
            image->vk.mip_levels, (unsigned)image->vk.format);
         return false;
      }
   }
   return true;
}

/* One region's two sides, refused by name when an image's texel placement is
 * one no probe has recorded. */
static bool
ps5vk_image_region_sides(struct ps5vk_cmd_buffer *cmd_buffer, struct ps5vk_image *source,
                         struct ps5vk_image *destination, const struct ps5vk_image_copy *region,
                         const char *what, uint32_t *source_texel_bytes,
                         struct ps5vk_image_copy_side *source_side,
                         struct ps5vk_image_copy_side *destination_side)
{
   uint32_t destination_texel_bytes = 0;
   /* A four-sample *depth* image copies as it stands: its texel is one
    * sixteen-byte unit holding all four samples (ps5vk_tiled_depth4_offset), so
    * the region walks texels, not samples. A four-sample colour image's texel is
    * four sample planes, which only vkCmdResolveImage walks (Phase C8), and a
    * linear multi-sample image has no measured layout at all. */
   const bool depth4 = source->vk.samples == VK_SAMPLE_COUNT_4_BIT &&
                       destination->vk.samples == VK_SAMPLE_COUNT_4_BIT &&
                       vk_format_has_depth(source->vk.format) &&
                       vk_format_has_depth(destination->vk.format) &&
                       source->storage == PS5VK_IMAGE_STORAGE_TILES &&
                       destination->storage == PS5VK_IMAGE_STORAGE_TILES;
   if (!depth4 && (source->vk.samples != VK_SAMPLE_COUNT_1_BIT ||
                   destination->vk.samples != VK_SAMPLE_COUNT_1_BIT)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "%s of a multi-sample image; its texel is four sample planes, which "
                              "only vkCmdResolveImage walks (Phase C8)", what);
      return false;
   }
   if (region->source.layerCount != 1 || region->destination.layerCount != 1 ||
       region->source.baseArrayLayer != 0 || region->destination.baseArrayLayer != 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "%s of %u layers from layer %u; array layers are D1 "
                              "(docs/M5_REFERENCE.md)", what, region->source.layerCount,
                              region->source.baseArrayLayer);
      return false;
   }
   /* Valid usage: a copy between images of one format names the same aspect on
    * both sides, so a combined depth/stencil copy moves one plane into the
    * same plane (R83). */
   if (region->source.aspectMask != region->destination.aspectMask) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "%s from aspect 0x%x into aspect 0x%x", what,
                              (unsigned)region->source.aspectMask,
                              (unsigned)region->destination.aspectMask);
      return false;
   }
   if (!ps5vk_image_copy_side(source, &region->source, source_texel_bytes, source_side) ||
       !ps5vk_image_copy_side(destination, &region->destination, &destination_texel_bytes,
                              destination_side)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "%s of a tiled image whose texel placement no probe has recorded; "
                              "C7's needs a single-level four-byte tiled image "
                              "(docs/M5_REFERENCE.md)", what);
      return false;
   }
   return true;
}

/* Records one scaled blit region: the queue resamples it texel by texel, since
 * the source and destination rectangles differ (ps5vk_queue.c,
 * ps5vk_blit_execute). A destination texel's sample point maps onto the source
 * the way Vulkan's blit does -- destination texel centres map linearly onto the
 * source rectangle's edges -- so a 2x upscale puts each source texel on a 2x2
 * block of destination texels exactly, and a mirrored rectangle's negative
 * source_end makes the step negative. */
static void
ps5vk_cmd_buffer_blit_image_region(struct ps5vk_cmd_buffer *cmd_buffer, uint32_t after_words,
                                   const struct ps5vk_image_copy_side *source,
                                   const struct ps5vk_image_copy_side *destination,
                                   const struct ps5vk_image_copy *region, VkFilter filter,
                                   VkFormat source_format, VkFormat destination_format,
                                   uint64_t source_span, uint64_t source_span_bytes,
                                   uint64_t destination_span, uint64_t destination_span_bytes)
{
   struct ps5vk_memory_copy *record =
      util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
   if (!record) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY, "no memory to record a blit");
      return;
   }
   *record = (struct ps5vk_memory_copy){
      .blit = true,
      .linear = filter == VK_FILTER_LINEAR,
      .source_span = source_span,
      .source_span_bytes = source_span_bytes,
      .destination_span = destination_span,
      .destination_span_bytes = destination_span_bytes,
      .source_side = *source,
      .destination_side = *destination,
      .source_x = region->source_offset.x,
      .source_y = region->source_offset.y,
      .destination_x = (uint32_t)region->destination_offset.x,
      .destination_y = (uint32_t)region->destination_offset.y,
      .width = region->extent.width,
      .height = region->extent.height,
      .source_texel_bytes = region->source_texel_bytes,
      .destination_texel_bytes = region->destination_texel_bytes,
      .source_format = source_format,
      .destination_format = destination_format,
      .source_step_x = (float)((double)(region->source_end.x - region->source_offset.x) /
                               (double)MAX2(region->extent.width, 1u)),
      .source_step_y = (float)((double)(region->source_end.y - region->source_offset.y) /
                               (double)MAX2(region->extent.height, 1u)),
      .after_words = after_words,
   };
}

/* The copies a vkCmdCopyImage or a one-to-one vkCmdBlitImage records. */
static void
ps5vk_cmd_buffer_copy_image(struct ps5vk_cmd_buffer *cmd_buffer, struct ps5vk_image *source,
                            struct ps5vk_image *destination, const struct ps5vk_image_copy *regions,
                            uint32_t region_count, bool blit)
{
   const char *const what = blit ? "a blit" : "a copy";
   for (uint32_t r = 0; ps5vk_census_enabled && r < region_count; r++)
      ps5vk_census("%s src fmt %d %ux%u storage %d samples %u aspect 0x%x -> dst fmt %d %ux%u storage %d "
                   "samples %u aspect 0x%x extent %ux%u same_image %d",
                   blit ? "blit" : "copy", (int)source->vk.format, source->vk.extent.width,
                   source->vk.extent.height, (int)source->storage, (unsigned)source->vk.samples,
                   regions[r].source.aspectMask, (int)destination->vk.format,
                   destination->vk.extent.width, destination->vk.extent.height,
                   (int)destination->storage, (unsigned)destination->vk.samples,
                   regions[r].destination.aspectMask, regions[r].extent.width,
                   regions[r].extent.height, source == destination);
   if (!ps5vk_image_transfer_check(cmd_buffer, source, destination, what, true))
      return;
   const uint32_t after_words =
      (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t);
   for (uint32_t r = 0; r < region_count; r++) {
      const struct ps5vk_image_copy *const region = &regions[r];
      uint32_t texel_bytes = 0;
      struct ps5vk_image_copy_side source_side = {0};
      struct ps5vk_image_copy_side destination_side = {0};
      if (!ps5vk_image_region_sides(cmd_buffer, source, destination, region, what, &texel_bytes,
                                    &source_side, &destination_side))
         return;
      ps5vk_cmd_buffer_copy_image_region(cmd_buffer, after_words, &source_side, &destination_side,
                                         (uint32_t)region->source_offset.x,
                                         (uint32_t)region->source_offset.y,
                                         (uint32_t)region->destination_offset.x,
                                         (uint32_t)region->destination_offset.y, region->extent,
                                         texel_bytes);
   }
}

/* vkCmdResolveImage and vkCmdResolveImage2KHR reach this one: a four-sample
 * image's texels into a one-sample destination's, which the queue averages
 * (ps5vk_queue.c, ps5vk_resolve_execute). What the driver records is one
 * region's two sides and the destination rectangle; the sample positions do not
 * matter to the arithmetic, since averaging four words is commutative, and the
 * probe that says where they sit is C8's measurement of the four-sample tile
 * (docs/M5_PHASE_C.md). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdResolveImage2KHR(VkCommandBuffer commandBuffer, const VkResolveImageInfo2 *pResolveImageInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_image, source, pResolveImageInfo->srcImage);
   VK_FROM_HANDLE(ps5vk_image, destination, pResolveImageInfo->dstImage);
   const VkResolveImageInfo2 *const info = pResolveImageInfo;
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || info->regionCount == 0 || source == NULL ||
       destination == NULL)
      return;
   /* R76: on the GPU when the images allow it (ps5vk_draw.c), any sample
    * count the driver renders (R78); the CPU walk below is C8's four. */
   if (destination->vk.samples == VK_SAMPLE_COUNT_1_BIT && ps5vk_meta_resolve(cmd_buffer, info))
      return;
   if (source->vk.samples != VK_SAMPLE_COUNT_4_BIT ||
       destination->vk.samples != VK_SAMPLE_COUNT_1_BIT) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a resolve from %u samples into %u that the GPU path does not take; "
                              "the CPU's is C8's four into one",
                              (unsigned)source->vk.samples, (unsigned)destination->vk.samples);
      return;
   }
   /* R5: the sentence names the usage bit that decides an image's storage rather
    * than tiling alone, because an application that follows the specification --
    * a resolve destination with TRANSFER_DST and SAMPLED, which is what the
    * resolve writes -- otherwise cannot tell from "two tiled images" that the
    * COLOR_ATTACHMENT bit it never performs is the missing thing
    * (PS5_VULKAN_REQUESTS.md, R5). Tiling a transfer destination instead would
    * move every sampled image's descriptor -- tiling is chosen from the usage
    * bits, and a texture upload declares TRANSFER_DST -- so the sentence is the
    * cheaper half of the request's two options. */
   /* R77: the destination may be stored in rows -- a TRANSFER_DST and SAMPLED
    * image, which is what the specification asks of a resolve's destination
    * (PS5_VULKAN_REQUESTS.md, R5) -- and the walk below writes it through its
    * row pitch; the GPU path above takes the ones whose rows are whole 256-byte
    * units. */
   if (!ps5vk_image_transfer_check(cmd_buffer, source, destination, "a resolve", true) ||
       source->storage != PS5VK_IMAGE_STORAGE_TILES ||
       vk_format_get_blocksize(source->vk.format) != 4) {
      ps5vk_cmd_buffer_refuse(
         cmd_buffer, VK_ERROR_UNKNOWN,
         "a resolve needs a tiled four-byte source of the destination's format (C8's "
         "four-sample tile is the one measured)");
      return;
   }
   const uint32_t after_words =
      (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t);
   for (uint32_t r = 0; r < info->regionCount; r++) {
      const VkImageResolve2 *const region = &info->pRegions[r];
      uint32_t source_texel_bytes = 0;
      uint32_t destination_texel_bytes = 0;
      struct ps5vk_image_copy_side source_side = {0};
      struct ps5vk_image_copy_side destination_side = {0};
      if (!ps5vk_image_copy_side(source, &region->srcSubresource, &source_texel_bytes, &source_side) ||
          !ps5vk_image_copy_side(destination, &region->dstSubresource, &destination_texel_bytes,
                                 &destination_side))
         break;
      if (region->srcSubresource.layerCount != 1 || region->srcSubresource.baseArrayLayer != 0 ||
          region->dstSubresource.layerCount != 1 || region->dstSubresource.baseArrayLayer != 0) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "a resolve of %u layers from layer %u; array layers are D1 "
                                 "(docs/M5_REFERENCE.md)", region->srcSubresource.layerCount,
                                 region->srcSubresource.baseArrayLayer);
         return;
      }
      struct ps5vk_memory_copy *const record =
         util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
      if (!record) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                 "no memory to record a resolve");
         return;
      }
      *record = (struct ps5vk_memory_copy){
         .resolve = true,
         .source_span = source->address,
         .source_span_bytes = source->size,
         .source_side = source_side,
         .destination_side = destination_side,
         .destination_x = (uint32_t)region->dstOffset.x,
         .destination_y = (uint32_t)region->dstOffset.y,
         .width = region->extent.width,
         .height = region->extent.height,
         .source_texel_bytes = source_texel_bytes,
         .destination_texel_bytes = destination_texel_bytes,
         .source_format = source->vk.format,
         .destination_format = destination->vk.format,
         .after_words = after_words,
      };
   }
}

/* vkCmdCopyImage (1.0) and vkCmdCopyImage2KHR both reach this one: the runtime
 * forwards the core command to the 2 form, and the two names share a slot in
 * the dispatch table, so only the KHR spelling is defined here
 * (vk_dispatch_table.c asserts that no slot is filled twice). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdCopyImage2KHR(VkCommandBuffer commandBuffer, const VkCopyImageInfo2 *pCopyImageInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_image, source, pCopyImageInfo->srcImage);
   VK_FROM_HANDLE(ps5vk_image, destination, pCopyImageInfo->dstImage);
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || pCopyImageInfo->regionCount == 0 ||
       source == NULL || destination == NULL)
      return;
   if (ps5vk_meta_copy(cmd_buffer, pCopyImageInfo))
      return;
   struct ps5vk_image_copy *const regions = malloc(pCopyImageInfo->regionCount * sizeof(*regions));
   if (!regions) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record an image copy");
      return;
   }
   for (uint32_t r = 0; r < pCopyImageInfo->regionCount; r++) {
      const VkImageCopy2 *const copy = &pCopyImageInfo->pRegions[r];
      regions[r] = (struct ps5vk_image_copy){
         .source = copy->srcSubresource,
         .destination = copy->dstSubresource,
         .source_offset = copy->srcOffset,
         .destination_offset = copy->dstOffset,
         .extent = copy->extent,
      };
   }
   ps5vk_cmd_buffer_copy_image(cmd_buffer, source, destination, regions,
                               pCopyImageInfo->regionCount, false);
   free(regions);
}

/* vkCmdCopyImageToBuffer (1.0) and its KHR 2 form. The runtime forwards the core
 * command to the 2 form, and the two names share a dispatch slot, so only the
 * KHR spelling is defined here, as for vkCmdCopyImage. A buffer is a linear side
 * with a row pitch of its own: bufferRowLength texels when the application names
 * one, the region's width otherwise. bufferImageHeight only pads between layers,
 * and a region names one, so it does not enter the pitch. The copy is the same
 * CPU work at a submission split point that every transfer here is
 * (driver/ps5vk_queue.c), which is why it needs no probe: what it reads is the
 * image's own texel map, the measured one for a tile and the padded rows for a
 * linear image. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdCopyImageToBuffer2KHR(VkCommandBuffer commandBuffer,
                               const VkCopyImageToBufferInfo2 *pCopyImageToBufferInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_image, source, pCopyImageToBufferInfo->srcImage);
   VK_FROM_HANDLE(ps5vk_buffer, buffer, pCopyImageToBufferInfo->dstBuffer);
   if (source && pCopyImageToBufferInfo->regionCount)
      ps5vk_census("readback fmt %d %ux%u storage %d aspect 0x%x region %ux%u",
                   (int)source->vk.format, source->vk.extent.width, source->vk.extent.height,
                   (int)source->storage,
                   pCopyImageToBufferInfo->pRegions[0].imageSubresource.aspectMask,
                   pCopyImageToBufferInfo->pRegions[0].imageExtent.width,
                   pCopyImageToBufferInfo->pRegions[0].imageExtent.height);
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || pCopyImageToBufferInfo->regionCount == 0 ||
       source == NULL || buffer == NULL)
      return;
   if (buffer->vk.device_address == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a readback names a buffer with no GPU address: it has to be bound "
                              "to memory");
      return;
   }
   /* A depth image reads back through its own map: one sample of D16 or D32 is
    * the shape the depth rows' transfer round trip proves (round 21), which is
    * what the maintenance1 note's TRANSFER_SRC asks of a format the driver
    * reports as sampled. A combined depth/stencil image reads back one aspect a
    * region, each from its own plane (R83, ps5vk_image_plane): four bytes a
    * texel of depth, one of stencil, as Vulkan's buffer layout has them.
    * Non-2D images still refuse by name. */
   if (source->vk.image_type != VK_IMAGE_TYPE_2D) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a readback of non-2D images; C7's are 2D ones, depth included for "
                              "a format whose map is measured (docs/M5_REFERENCE.md)");
      return;
   }
   const uint32_t after_words =
      (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t);
   for (uint32_t r = 0; r < pCopyImageToBufferInfo->regionCount; r++) {
      const VkBufferImageCopy2 *const region = &pCopyImageToBufferInfo->pRegions[r];
      if (region->imageSubresource.layerCount != 1 ||
          region->imageSubresource.baseArrayLayer != 0) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "a readback of %u layers from layer %u; array layers are D1 "
                                 "(docs/M5_REFERENCE.md)", region->imageSubresource.layerCount,
                                 region->imageSubresource.baseArrayLayer);
         return;
      }
      uint32_t texel_bytes = 0;
      struct ps5vk_image_copy_side source_side = {0};
      if (!ps5vk_image_copy_side(source, &region->imageSubresource, &texel_bytes, &source_side)) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "a readback of a tiled image whose texel placement no probe has "
                                 "recorded; C7's needs a single-level four-byte tiled image "
                                 "(docs/M5_REFERENCE.md)");
         return;
      }
      const struct ps5vk_image_copy_side destination_side = {
         .address = buffer->vk.device_address + region->bufferOffset,
         .row_pitch = (region->bufferRowLength != 0 ? region->bufferRowLength
                                                    : region->imageExtent.width) *
                      (uint64_t)texel_bytes,
      };
      ps5vk_cmd_buffer_copy_image_region(
         cmd_buffer, after_words, &source_side, &destination_side,
         (uint32_t)region->imageOffset.x, (uint32_t)region->imageOffset.y, 0, 0,
         region->imageExtent, texel_bytes);
      /* The record the call just appended: a readback out of a reversed image
       * swaps the four bytes of every texel on the way into the buffer
       * (ps5vk_format.storage_reversed). */
      if (ps5vk_image_storage_reversed(source) &&
          util_dynarray_num_elements(&cmd_buffer->copies, struct ps5vk_memory_copy) != 0)
         util_dynarray_element(&cmd_buffer->copies, struct ps5vk_memory_copy,
                               util_dynarray_num_elements(&cmd_buffer->copies,
                                                          struct ps5vk_memory_copy) - 1)
            ->reverse_texel_bytes = texel_bytes;
   }
}

/* vkCmdBlitImage and vkCmdBlitImage2KHR reach here the same way. A blit whose
 * rectangles match is the copy above -- any filter, since no sample lands
 * between texels -- and one whose rectangles differ is resampled on the CPU
 * (ps5vk_queue.c). One call's regions have to be all of one shape, which is why
 * the first region decides for the rest. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdBlitImage2KHR(VkCommandBuffer commandBuffer, const VkBlitImageInfo2 *pBlitImageInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_image, source, pBlitImageInfo->srcImage);
   VK_FROM_HANDLE(ps5vk_image, destination, pBlitImageInfo->dstImage);
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || pBlitImageInfo->regionCount == 0 ||
       source == NULL || destination == NULL)
      return;
   if (ps5vk_meta_blit(cmd_buffer, pBlitImageInfo))
      return;
   struct ps5vk_image_copy *const regions = malloc(pBlitImageInfo->regionCount * sizeof(*regions));
   if (!regions) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record a blit");
      return;
   }
   bool scaled = false;
   for (uint32_t r = 0; r < pBlitImageInfo->regionCount; r++) {
      const VkImageBlit2 *const blit = &pBlitImageInfo->pRegions[r];
      const int32_t source_width = blit->srcOffsets[1].x - blit->srcOffsets[0].x;
      const int32_t source_height = blit->srcOffsets[1].y - blit->srcOffsets[0].y;
      const int32_t destination_width = blit->dstOffsets[1].x - blit->dstOffsets[0].x;
      const int32_t destination_height = blit->dstOffsets[1].y - blit->dstOffsets[0].y;
      const bool region_scaled = source_width != destination_width ||
                                 source_height != destination_height;
      if (r == 0)
         scaled = region_scaled;
      else if (region_scaled != scaled) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "a blit whose regions mix one-to-one and scaled rectangles; one "
                                 "blit call is one shape here (docs/M5_REFERENCE.md, C7)");
         free(regions);
         return;
      }
      regions[r] = (struct ps5vk_image_copy){
         .source = blit->srcSubresource,
         .destination = blit->dstSubresource,
         .source_offset = {blit->srcOffsets[0].x, blit->srcOffsets[0].y, 0},
         .source_end = {blit->srcOffsets[1].x, blit->srcOffsets[1].y, 1},
         .destination_offset = {blit->dstOffsets[0].x, blit->dstOffsets[0].y, 0},
         .extent =
            {
               (uint32_t)destination_width,
               (uint32_t)destination_height,
               1,
            },
      };
   }
   if (!scaled) {
      ps5vk_cmd_buffer_copy_image(cmd_buffer, source, destination, regions,
                                  pBlitImageInfo->regionCount, true);
      free(regions);
      return;
   }
   if (!ps5vk_image_transfer_check(cmd_buffer, source, destination, "a blit", false)) {
      free(regions);
      return;
   }
   /* The resampler writes four-byte texels, and decodes the source's fetch into
    * them: any format ps5vk_image.c's table reports has a decode (the one
    * V0-formats measured), while a source format the table does not carry is
    * refused here rather than mis-sampled. A *filtered* blit needs
    * R8G8B8A8_UNORM on both sides: the blend is on the texel bytes, which are
    * the colour channels only there and ordinary UNORM values only there (an
    * sRGB or SNORM blend would need the decode Vulkan's filter names). */
   const uint32_t destination_texels = vk_format_get_blocksize(destination->vk.format);
   const struct ps5vk_format *const source_entry = ps5vk_find_format(source->vk.format);
   if (ps5vk_rgba8_texel_bytes(destination->vk.format) == 0 || source_entry == NULL ||
       source_entry->image_format == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a scaled blit into %u-byte texels from a format %u image with no "
                              "recorded decode or encode (docs/M5_REFERENCE.md, C7)",
                              destination_texels, (unsigned)source->vk.format);
      free(regions);
      return;
   }
   if (pBlitImageInfo->filter == VK_FILTER_LINEAR &&
       (source->vk.format != VK_FORMAT_R8G8B8A8_UNORM ||
        destination->vk.format != VK_FORMAT_R8G8B8A8_UNORM)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a filtered blit of format %u images; the linear blend is written "
                              "for R8G8B8A8_UNORM's texel bytes (docs/M5_REFERENCE.md, C7)",
                              (unsigned)source->vk.format);
      free(regions);
      return;
   }
   const uint32_t after_words =
      (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t);
   for (uint32_t r = 0; r < pBlitImageInfo->regionCount; r++) {
      /* The destination rectangle clips to the image, as Vulkan's blit does.
       * The transform stays the unclipped one, so a rectangle that reaches
       * outside keeps its scale and the sample point clamps at the source's
       * edge (ps5vk_blit_execute). */
      const int32_t x0 = MAX2(pBlitImageInfo->pRegions[r].dstOffsets[0].x, 0);
      const int32_t y0 = MAX2(pBlitImageInfo->pRegions[r].dstOffsets[0].y, 0);
      const int32_t x1 = MIN2(pBlitImageInfo->pRegions[r].dstOffsets[1].x,
                              (int32_t)destination->vk.extent.width);
      const int32_t y1 = MIN2(pBlitImageInfo->pRegions[r].dstOffsets[1].y,
                              (int32_t)destination->vk.extent.height);
      if (x1 <= x0 || y1 <= y0)
         continue;
      struct ps5vk_image_copy region = regions[r];
      region.destination_offset = (VkOffset3D){x0, y0, 0};
      region.extent = (VkExtent3D){(uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1};
      region.source_format = source->vk.format;
      region.destination_format = destination->vk.format;
      uint32_t source_texel_bytes = 0;
      struct ps5vk_image_copy_side source_side = {0};
      struct ps5vk_image_copy_side destination_side = {0};
      if (!ps5vk_image_region_sides(cmd_buffer, source, destination, &region, "a blit",
                                    &source_texel_bytes, &source_side, &destination_side))
         break;
      region.source_texel_bytes = source_texel_bytes;
      region.destination_texel_bytes = destination_texels;
      ps5vk_cmd_buffer_blit_image_region(cmd_buffer, after_words, &source_side, &destination_side,
                                         &region, pBlitImageInfo->filter, source->vk.format,
                                         destination->vk.format, source->address, source->size,
                                         destination->address, destination->size);
   }
   free(regions);
}

void *
ps5vk_debug_image_storage(VkImage _image, size_t *bytes)
{
   VK_FROM_HANDLE(ps5vk_image, image, _image);
   const bool stored = image && image->address != 0;
   *bytes = stored ? (size_t)image->size : 0;
   /* The caller reads what the GPU rendered through this pointer, and the queue
    * no longer evicts a target the application never mapped after each step
    * (ps5vk_queue_flush_targets), so the range is invalidated here, when it is
    * handed over to be read. */
   if (stored)
      ps5vk_flush_cpu_cache((const void *)(uintptr_t)image->address, (size_t)image->size);
   return stored ? (void *)(uintptr_t)image->address : NULL;
}

/* ---------------- clearing an image (1.0's image clears) ------------------ */

/* A float as the half its 16-bit format stores. */
static uint16_t
ps5vk_half_from_float(float value)
{
   uint32_t bits = 0;
   memcpy(&bits, &value, sizeof(bits));
   const uint32_t sign = (bits >> 16) & 0x8000u;
   const int32_t exponent = (int32_t)((bits >> 23) & 0xffu) - 127 + 15;
   const uint32_t mantissa = bits & 0x7fffffu;
   if (((bits >> 23) & 0xffu) == 0xffu)
      return (uint16_t)(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
   if (exponent >= 0x1f)
      return (uint16_t)(sign | 0x7c00u);
   if (exponent <= 0) {
      if (exponent < -10)
         return (uint16_t)sign;
      const uint32_t shifted = (mantissa | 0x800000u) >> (uint32_t)(1 - exponent);
      return (uint16_t)(sign | ((shifted + 0x1000u) >> 13));
   }
   return (uint16_t)(sign | ((uint32_t)exponent << 10) | ((mantissa + 0x1000u) >> 13));
}

/* The bytes a format's texel holds for a clear value: Vulkan's conversion from
 * VkClearColorValue to the format's components, which is the inverse of the
 * decode a blit does (ps5vk_queue.c, ps5vk_texel_to_rgba8). False for a format
 * whose encoding no probe has recorded, which the recording then refuses. */
static bool
ps5vk_format_encode_clear(VkFormat format, const VkClearColorValue *value, uint8_t texel[16],
                          uint32_t *texel_bytes)
{
   const float *const f = value->float32;
   memset(texel, 0, 16);
   switch (format) {
   case VK_FORMAT_D32_SFLOAT:
      /* A depth clear stores the float's own bits, which is what the console's
       * D32F target holds (M4 step 1, C5: the stored depth equals clip z). */
      memcpy(texel, &f[0], sizeof(float));
      *texel_bytes = sizeof(float);
      return true;
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_UNORM: {
      /* B8G8R8A8 holds blue first, which is what a swapchain image's bytes
       * are (Phase C1). */
      const bool blue_first = format == VK_FORMAT_B8G8R8A8_UNORM;
      texel[blue_first ? 2u : 0u] = (uint8_t)floorf(CLAMP(f[0], 0.0f, 1.0f) * 255.0f + 0.5f);
      texel[1] = (uint8_t)floorf(CLAMP(f[1], 0.0f, 1.0f) * 255.0f + 0.5f);
      texel[blue_first ? 0u : 2u] = (uint8_t)floorf(CLAMP(f[2], 0.0f, 1.0f) * 255.0f + 0.5f);
      texel[3] = (uint8_t)floorf(CLAMP(f[3], 0.0f, 1.0f) * 255.0f + 0.5f);
      *texel_bytes = 4;
      return true;
   }
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_B8G8R8A8_SRGB: {
      /* A clear value is linear and the texel is sRGB, so this is the encode
       * the decode's linearisation undoes; the byte-reversed B8G8R8A8 form
       * writes the three encoded channels in B, G, R order, and the packed
       * A8B8G8R8 one is written in the R, G, B, A order its storage holds
       * (ps5vk_format.storage_reversed). */
      const bool blue_first = format == VK_FORMAT_B8G8R8A8_SRGB;
      for (unsigned channel = 0; channel < 3; channel++) {
         const float linear = CLAMP(f[channel], 0.0f, 1.0f);
         const float encoded =
            linear <= 0.0031308f ? linear * 12.92f : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
         texel[blue_first ? 2u - channel : channel] =
            (uint8_t)floorf(CLAMP(encoded, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
      texel[3] = (uint8_t)floorf(CLAMP(f[3], 0.0f, 1.0f) * 255.0f + 0.5f);
      *texel_bytes = 4;
      return true;
   }
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_SNORM_PACK32: {
      /* The packed formats hold red last, alpha first, which is the order the
       * decode swaps (ps5vk_texel_to_rgba8). */
      const bool signed_format = format == VK_FORMAT_A8B8G8R8_SNORM_PACK32;
      const float scale = signed_format ? 127.0f : 255.0f;
      const float low = signed_format ? -1.0f : 0.0f;
      for (unsigned channel = 0; channel < 3; channel++) {
         const float component = CLAMP(f[channel], low, 1.0f);
         texel[3u - channel] = (uint8_t)(int8_t)CLAMP(
            (int32_t)lroundf(component * scale), signed_format ? -128 : 0, 127);
      }
      texel[0] = (uint8_t)(int8_t)CLAMP((int32_t)lroundf(CLAMP(f[3], low, 1.0f) * scale),
                                        signed_format ? -128 : 0, 127);
      *texel_bytes = 4;
      return true;
   }
   case VK_FORMAT_R8G8B8A8_SNORM: {
      for (unsigned channel = 0; channel < 4; channel++)
         texel[channel] = (uint8_t)(int8_t)CLAMP((int32_t)lroundf(CLAMP(f[channel], -1.0f, 1.0f) *
                                                                  127.0f),
                                                 -128, 127);
      *texel_bytes = 4;
      return true;
   }
   case VK_FORMAT_R8_UNORM:
      texel[0] = (uint8_t)floorf(CLAMP(f[0], 0.0f, 1.0f) * 255.0f + 0.5f);
      *texel_bytes = 1;
      return true;
   case VK_FORMAT_R8G8_UNORM:
      texel[0] = (uint8_t)floorf(CLAMP(f[0], 0.0f, 1.0f) * 255.0f + 0.5f);
      texel[1] = (uint8_t)floorf(CLAMP(f[1], 0.0f, 1.0f) * 255.0f + 0.5f);
      *texel_bytes = 2;
      return true;
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16B16A16_UNORM: {
      const unsigned channels =
         format == VK_FORMAT_R16G16B16A16_UNORM ? 4u : (format == VK_FORMAT_R16G16_UNORM ? 2u : 1u);
      uint16_t *const words = (uint16_t *)texel;
      for (unsigned channel = 0; channel < channels; channel++)
         words[channel] =
            (uint16_t)floorf(CLAMP(f[channel], 0.0f, 1.0f) * 65535.0f + 0.5f);
      *texel_bytes = channels * 2u;
      return true;
   }
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_SFLOAT: {
      const unsigned channels = format == VK_FORMAT_R16G16B16A16_SFLOAT
                                   ? 4u
                                   : (format == VK_FORMAT_R16G16_SFLOAT ? 2u : 1u);
      uint16_t *const words = (uint16_t *)texel;
      for (unsigned channel = 0; channel < channels; channel++)
         words[channel] = ps5vk_half_from_float(f[channel]);
      *texel_bytes = channels * 2u;
      return true;
   }
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_SFLOAT: {
      const unsigned channels = format == VK_FORMAT_R32G32B32A32_SFLOAT
                                   ? 4u
                                   : (format == VK_FORMAT_R32G32_SFLOAT ? 2u : 1u);
      for (unsigned channel = 0; channel < channels; channel++)
         memcpy(texel + channel * 4u, &f[channel], sizeof(float));
      *texel_bytes = channels * 4u;
      return true;
   }
   default:
      return false;
   }
}

/* vkCmdClearColorImage: the same CPU work at a submission split point the
 * copies are, writing one encoded texel over each texel of the ranges' regions
 * through the side's own map (ps5vk_queue.c, ps5vk_clear_execute). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdClearColorImage(VkCommandBuffer commandBuffer, VkImage _image, VkImageLayout imageLayout,
                         const VkClearColorValue *pColor, uint32_t rangeCount,
                         const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_image, image, _image);
   (void)imageLayout;
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || image == NULL || rangeCount == 0)
      return;
   ps5vk_census("clear colour image fmt %d %ux%u storage %d mips %u", (int)image->vk.format,
                image->vk.extent.width, image->vk.extent.height, (int)image->storage,
                image->vk.mip_levels);
   if (image->vk.image_type != VK_IMAGE_TYPE_2D || vk_format_has_depth(image->vk.format) ||
       vk_format_has_stencil(image->vk.format)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "clearing %s images; this driver's images are 2D colour ones "
                              "(docs/M5_REFERENCE.md)",
                              vk_format_has_depth(image->vk.format) ? "depth or stencil" : "non-2D");
      return;
   }
   uint8_t texel[16] = {0};
   uint32_t texel_bytes = 0;
   if (!ps5vk_format_encode_clear(image->vk.format, pColor, texel, &texel_bytes)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "clearing a format %d image; that format's clear encoding is not in "
                              "the table (docs/V0_FORMATS_AUDIT.md)", (int)image->vk.format);
      return;
   }
   const uint32_t after_words =
      (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t);
   for (uint32_t r = 0; r < rangeCount; r++) {
      const VkImageSubresourceRange *const range = &pRanges[r];
      if (range->baseArrayLayer != 0 || range->layerCount != 1) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "clearing %u array layers from layer %u; array layers are D1 "
                                 "(docs/M5_REFERENCE.md)", range->layerCount,
                                 range->baseArrayLayer);
         return;
      }
      for (uint32_t level = range->baseMipLevel; level < range->baseMipLevel + range->levelCount;
           level++) {
         uint32_t side_texel_bytes = 0;
         struct ps5vk_image_copy_side side = {0};
         const VkImageSubresourceLayers subresource = {range->aspectMask, level, 0, 1};
         if (!ps5vk_image_copy_side(image, &subresource, &side_texel_bytes, &side)) {
            ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                    "clearing a tiled image that is not a single-level four-byte "
                                    "one; the map a chain's levels sit at is C7's open piece "
                                    "(docs/HARDWARE_FINDINGS.md)");
            return;
         }
         struct ps5vk_memory_copy *const copy =
            util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
         if (!copy) {
            ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                    "no memory to record an image clear");
            return;
         }
         *copy = (struct ps5vk_memory_copy){
            .clear = true,
            .destination_side = side,
            .destination_texel_bytes = side_texel_bytes,
            .destination_x = 0,
            .destination_y = 0,
            .width = MAX2(image->vk.extent.width >> level, 1u),
            .height = MAX2(image->vk.extent.height >> level, 1u),
            .after_words = after_words,
         };
         memcpy(copy->clear_texel, texel, sizeof(texel));
      }
   }
}

/* vkCmdClearDepthStencilImage: the same CPU clear at a split point the colour
 * clear is, through the depth image's own map (ps5vk_tiled_depth_offset, the
 * map C5's readbacks proved). The driver's depth images are D32_SFLOAT with no
 * stencil (ps5vk_depth_registers disables stencil), so a stencil aspect is
 * refused by name and the depth value is the float's own bits. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage _image,
                                VkImageLayout imageLayout,
                                const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount,
                                const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_image, image, _image);
   (void)imageLayout;
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || rangeCount == 0)
      return;
   if (image)
      ps5vk_census("clear depth image fmt %d %ux%u aspect 0x%x", (int)image->vk.format,
                   image->vk.extent.width, image->vk.extent.height, pRanges[0].aspectMask);
   /* An image the driver does not have is a refusal, not silence: the B2 device
    * test records every 1.0 command with the handles it has (none), which is
    * how a missing entry point or a crash in one shows up there. */
   if (image == NULL || !vk_format_has_depth(image->vk.format)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "vkCmdClearDepthStencilImage names %s (docs/M5_REFERENCE.md)",
                              image == NULL ? "no image this driver created"
                                            : "an image whose format is not a depth one");
      return;
   }
   /* The depth plane's texel is the depth-only format's: a combined
    * D32_SFLOAT_S8_UINT image's depth plane is D32_SFLOAT's surface (R83,
    * ps5vk_image_plane), and its stencil plane's texel is the clear's byte. */
   const VkFormat depth_format =
      image->vk.format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_FORMAT_D32_SFLOAT : image->vk.format;
   uint8_t texel[16] = {0};
   uint32_t texel_bytes = 0;
   const VkClearColorValue value = {.float32 = {pDepthStencil->depth, 0.0f, 0.0f, 0.0f}};
   if (!ps5vk_format_encode_clear(depth_format, &value, texel, &texel_bytes)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "clearing a depth format %d image; that format's clear encoding is "
                              "not in the table (docs/V0_FORMATS_AUDIT.md)",
                              (int)image->vk.format);
      return;
   }
   uint8_t stencil_texel[16] = {(uint8_t)(pDepthStencil->stencil & 0xffu)};
   const uint32_t after_words =
      (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t);
   /* A four-sample depth image's storage is four words a texel -- one a sample
    * -- and the storage the driver allocated is exactly that image and nothing
    * else, which the console measured when the hardware's own clear wrote
    * 33,177,600 words of the 128 MiB target (docs/HARDWARE_FINDINGS.md,
    * unknowns-depth4x-map). Every one of those words takes the clear value, so
    * the command is one 32-bit fill of the image's storage, and no sample map
    * is needed. A range that does not cover the whole image would need one, and
    * is refused below. */
   if (image->vk.samples == 4) {
      const VkImageSubresourceRange *const range = &pRanges[0];
      if (rangeCount != 1 || range->aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT ||
          range->baseMipLevel != 0 || range->levelCount != 1 || range->baseArrayLayer != 0 ||
          range->layerCount != 1) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "clearing part of a four-sample depth image; the whole-image "
                                 "clear is the storage fill, a range needs the sample map "
                                 "(docs/V0_FORMATS_AUDIT.md)");
         return;
      }
      uint32_t word = 0;
      memcpy(&word, texel, sizeof(word));
      struct ps5vk_memory_copy *const fill =
         util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
      if (!fill) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                 "no memory to record a four-sample depth clear");
         return;
      }
      *fill = (struct ps5vk_memory_copy){
         .destination = image->address,
         .bytes = image->size,
         .fill = true,
         .fill_value = word,
         .after_words = after_words,
      };
      return;
   }
   for (uint32_t r = 0; r < rangeCount; r++) {
      const VkImageSubresourceRange *const range = &pRanges[r];
      /* Each aspect the range names is its own plane's clear (R83): the depth
       * surface takes the depth texel and a combined image's stencil plane the
       * stencil byte. An aspect the format lacks is invalid usage. */
      const VkImageAspectFlags known = VK_IMAGE_ASPECT_DEPTH_BIT |
                                       (vk_format_has_stencil(image->vk.format)
                                           ? VK_IMAGE_ASPECT_STENCIL_BIT
                                           : 0);
      if (range->aspectMask == 0 || (range->aspectMask & ~known) != 0) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "clearing aspect 0x%x of a format %d image",
                                 (unsigned)range->aspectMask, (int)image->vk.format);
         return;
      }
      if (range->baseArrayLayer != 0 || range->layerCount != 1) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "clearing %u array layers from layer %u; array layers are D1 "
                                 "(docs/M5_REFERENCE.md)", range->layerCount,
                                 range->baseArrayLayer);
         return;
      }
      for (uint32_t aspect_bit = VK_IMAGE_ASPECT_DEPTH_BIT;
           aspect_bit <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect_bit <<= 1) {
         if ((range->aspectMask & aspect_bit) == 0)
            continue;
         const uint8_t *const aspect_texel =
            aspect_bit == VK_IMAGE_ASPECT_STENCIL_BIT ? stencil_texel : texel;
         for (uint32_t level = range->baseMipLevel; level < range->baseMipLevel + range->levelCount;
              level++) {
            uint32_t side_texel_bytes = 0;
            struct ps5vk_image_copy_side side = {0};
            const VkImageSubresourceLayers subresource = {aspect_bit, level, 0, 1};
            if (!ps5vk_image_copy_side(image, &subresource, &side_texel_bytes, &side)) {
               ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                       "clearing aspect 0x%x of a depth image that is not a "
                                       "single-level one; anything else has no measured depth map "
                                       "(docs/HARDWARE_FINDINGS.md)", (unsigned)aspect_bit);
               return;
            }
            struct ps5vk_memory_copy *const copy =
               util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
            if (!copy) {
               ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                       "no memory to record a depth clear");
               return;
            }
            *copy = (struct ps5vk_memory_copy){
               .clear = true,
               .destination_side = side,
               .destination_texel_bytes = side_texel_bytes,
               .destination_x = 0,
               .destination_y = 0,
               .width = MAX2(image->vk.extent.width >> level, 1u),
               .height = MAX2(image->vk.extent.height >> level, 1u),
               .after_words = after_words,
            };
            memcpy(copy->clear_texel, aspect_texel, sizeof(texel));
         }
      }
   }
}
