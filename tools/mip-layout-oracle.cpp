// PS5 Vulkan compatibility probe - the mip chain layout, from AddrLib itself.
// Copyright (C) 2026 Mihawk-99
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console lays a tiled mip chain out by AddrLib's rule, and the probe's
// measured level bases are that rule's answer (docs/HARDWARE_FINDINGS.md).
// This tool asks the rule's own implementation instead of a transcription of
// it: it compiles the pinned AddrLib sources (.deps/native/mesa,
// tools/setup-native-dependencies.sh) for the host and prints, for the shapes
// the probe's chains use, where every level starts and how long the chain is.
//
//   tools/check-mip-layout.sh              builds it and checks the table below
//   build/host/mip-layout-oracle           the shape matrix
//   build/host/mip-layout-oracle 256 256 5 one shape, per level
//
// The ADDR2 API is what radv uses (src/amd/common/ac_surface.c): a 64 KiB
// swizzle (ADDR_SW_64KB_S, the PS5's tiled 4-byte-texel mode), a thin 2D
// resource, one slice. A tail level's base is the swizzle of its mip tail
// coordinate -- Addr2ComputeSurfaceInfo's pMipInfo[level].mipTailCoordX/Y --
// and every level outside the tail sits at its macroBlockOffset, which is what
// the measured bases agree with to the byte.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "addrinterface.h"
#include "addrtypes.h"
#include "amdgpu_asic_addr.h"

#define CIASICIDGFXENGINE_ARCTICISLAND 0x0000000D

/* AddrLib's own GB_ADDR_CONFIG field values for the console: NUM_PIPES = 16
 * (src/core/addrlib.h, ADDR_CONFIG_16_PIPE = 0x4) with the 256-byte pipe
 * interleave the address library requires and asserts (its value is 0), and
 * every other field zero. */
static constexpr unsigned kGbAddrConfig16Pipe = 0x4u;

/* The PS5's tiled colour format: 128x128 texels of four bytes in one 64 KiB
 * block, whose swizzle the driver's map (ps5vk_image.c,
 * ps5vk_tiled_texel_offset) is the measured form of. */
static constexpr unsigned kTileTexels = 128;
static constexpr unsigned kBlockBytes = 0x10000;

static void *
allocSysMem(const ADDR_ALLOCSYSMEM_INPUT *input)
{
    return std::malloc(input->sizeInBytes);
}

static ADDR_E_RETURNCODE
freeSysMem(const ADDR_FREESYSMEM_INPUT *input)
{
    std::free(input->pVirtAddr);
    return ADDR_OK;
}

/* The driver's measured map inside one 64 KiB block: where a tail level's mip
 * tail coordinate puts that level's (0, 0) texel. A tail coordinate is always
 * inside the block, so the block index the driver's own map carries is zero. */
static unsigned long long
blockOffset(unsigned x, unsigned y)
{
    const unsigned long long sx = x;
    const unsigned long long sy = y;
    return ((sy << 4) & 0x70u) ^ ((sy << 5) & 0xf00u) ^ ((sy << 9) & 0x1000u) ^
           ((sy << 8) & 0x4000u) ^ ((sx << 2) & 0x0cu) ^ ((sx << 5) & 0x380u) ^
           ((sx << 4) & 0x400u) ^ ((sx << 6) & 0x800u) ^ ((sx << 9) & 0xa000u);
}

/* AddrLib's own ceiling on a chain's levels (MaxMipLevels in its sources). */
static constexpr unsigned kMaxMipLevels = 16;

struct Chain {
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT info;
    ADDR2_MIP_INFO mip[kMaxMipLevels];
};

/* One shape's chain, from AddrLib. */
static bool
computeChain(ADDR_HANDLE lib, unsigned width, unsigned height, unsigned levels, unsigned elementBytes,
             Chain *chain)
{
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in;
    std::memset(&in, 0, sizeof(in));
    std::memset(chain, 0, sizeof(*chain));
    in.size = sizeof(in);
    in.swizzleMode = ADDR_SW_64KB_S;
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.flags.texture = 1;
    in.bpp = 32;
    in.width = width;
    in.height = height;
    in.numSlices = 1;
    in.numMipLevels = levels;
    in.bpp = elementBytes * 8u;
    chain->info.size = sizeof(chain->info);
    chain->info.pMipInfo = chain->mip;
    return Addr2ComputeSurfaceInfo(lib, &in, &chain->info) == ADDR_OK;
}

/* Where AddrLib puts one level: its macro block offset, or -- inside the tail,
 * where every level's macroBlockOffset is zero -- the swizzle of its tail
 * coordinate. */
static unsigned long long
levelBase(const Chain &chain, unsigned level)
{
    const ADDR2_MIP_INFO &mip = chain.mip[level];
    if (mip.mipTailOffset != 0)
        return blockOffset(mip.mipTailCoordX, mip.mipTailCoordY);
    return mip.macroBlockOffset;
}

/* One 128x128-texel image of each element size: the block AddrLib makes of it,
 * which is the tile extent the driver's ps5vk_tile_extent holds (64 KiB of
 * elements whatever the size) and the SDK's ps5_tiled_color_tile table names.
 * AddrLib refuses a multi-sample shape (ADDR_INVALIDPARAMS, every swizzle mode
 * and flag combination), so the four-sample row stays the SDK's
 * ps5_tiled_color_msaa4_tile, whose tiles are 64 KiB of four-sample elements as
 * well. */
static void
printTiles(ADDR_HANDLE lib)
{
    const unsigned sizes[] = {1, 2, 4, 8, 16};
    std::printf("/* element bytes  block texels  block bytes */\n");
    for (unsigned bytes : sizes) {
        ADDR2_COMPUTE_SURFACE_INFO_INPUT in;
        ADDR2_MIP_INFO mip[2];
        ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out;
        std::memset(&in, 0, sizeof(in));
        std::memset(mip, 0, sizeof(mip));
        std::memset(&out, 0, sizeof(out));
        in.size = sizeof(in);
        in.swizzleMode = ADDR_SW_64KB_S;
        in.resourceType = ADDR_RSRC_TEX_2D;
        in.flags.texture = 1;
        in.bpp = bytes * 8u;
        in.width = 128;
        in.height = 128;
        in.numSlices = 1;
        in.numMipLevels = 1;
        out.size = sizeof(out);
        out.pMipInfo = mip;
        if (Addr2ComputeSurfaceInfo(lib, &in, &out) != ADDR_OK) {
            std::printf("   %-13u refused\n", bytes);
            continue;
        }
        std::printf("   %-13u  %ux%-9u 0x%llx\n", bytes, out.blockWidth, out.blockHeight,
                    static_cast<unsigned long long>(out.blockWidth) * out.blockHeight * bytes);
    }
}

/* --- the within-tile swizzle, derived from AddrLib -------------------------
 *
 * The driver walks an image's texels itself (copies, readbacks, clears), and
 * the map it walks is `ps5vk_tiled_texel_offset`. Its 4-byte one-sample form is
 * the console's measurement; every other element size is a claim the console
 * has not made yet. This derives the map from AddrLib instead of transcribing
 * it, one element size at a time, and checks the derivation two ways before the
 * driver is allowed to hold it:
 *
 *   - every texel of a tile is recomputed from the derived terms and compared
 *     with AddrLib's own Addr2ComputeSurfaceAddrFromCoord answer, and
 *   - for the 4-byte one-sample colour tile, every offset is compared with
 *     blockOffset() above, which *is* the console's measured map, so the
 *     derivation is known to produce the map the console ran before it is
 *     trusted for a size the console has not run.
 *
 * A term ((coord << shift) & mask), combined by exclusive or, is the form
 * AddrLib's own generated equation tables use: one output bit per term bit, and
 * several terms may reach the same output bit from different coordinates. */

struct SwizzleTerm {
    unsigned char coord; /* 0 = x, 1 = y */
    int shift;
    unsigned mask;
};

struct SwizzleMap {
    unsigned bpe;
    unsigned samples;
    bool depth;
    unsigned blockWidth;  /* the tile's width in elements */
    unsigned blockHeight; /* the tile's height in elements */
    unsigned width;       /* the probed region's width: the tile times the tile count */
    unsigned height;      /* the probed region's height */
    unsigned pitch;  /* what AddrLib reports for a tile-sized surface */
    unsigned long long base;
    unsigned terms;
    SwizzleTerm term[32];
    unsigned runBytes; /* contiguous bytes along x from (0, 0) */
    unsigned long long verified;
    unsigned long long measuredMismatch; /* against blockOffset(), bpe 4 1x only */
    AddrSwizzleMode mode;
};

/* One element's byte offset from the surface base, from AddrLib, with the flags
 * the derivation uses. */
static bool
coordOffsetWith(ADDR_HANDLE lib, AddrSwizzleMode mode, unsigned bpe, unsigned samples, bool depth,
                unsigned width, unsigned height, unsigned pitch, unsigned x, unsigned y,
                unsigned sample, ADDR2_SURFACE_FLAGS flags, unsigned long long *offset)
{
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in;
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out;
    std::memset(&in, 0, sizeof(in));
    std::memset(&out, 0, sizeof(out));
    in.size = sizeof(in);
    in.x = x;
    in.y = y;
    in.slice = 0;
    in.sample = sample;
    in.mipId = 0;
    in.swizzleMode = mode;
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.bpp = bpe * 8u;
    in.unalignedWidth = width;
    in.unalignedHeight = height;
    in.numSlices = 1;
    in.numMipLevels = 1;
    in.numSamples = samples;
    in.numFrags = samples;
    in.pitchInElement = pitch;
    in.pipeBankXor = 0;
    in.flags = flags;
    out.size = sizeof(out);
    if (Addr2ComputeSurfaceAddrFromCoord(lib, &in, &out) != ADDR_OK)
        return false;
    *offset = out.addr;
    return true;
}

/* The flags the derivation uses: a colour or depth target that is sampled as a
 * texture, which needs the extended equation rather than the two-XOR legacy
 * one. */
static ADDR2_SURFACE_FLAGS
derivationFlags(bool depth)
{
    ADDR2_SURFACE_FLAGS flags;
    flags.value = 0;
    if (depth) {
        flags.depth = 1;
        flags.stencil = 0;
    } else {
        flags.color = 1;
    }
    flags.texture = 1;
    flags.needEquation = 1;
    flags.allowExtEquation = 1;
    return flags;
}

/* One element's byte offset from the surface base, from AddrLib. */
static bool
coordOffset(ADDR_HANDLE lib, AddrSwizzleMode mode, unsigned bpe, unsigned samples, bool depth,
            unsigned width, unsigned height, unsigned pitch, unsigned x, unsigned y,
            unsigned sample, unsigned long long *offset)
{
    return coordOffsetWith(lib, mode, bpe, samples, depth, width, height, pitch, x, y, sample,
                           derivationFlags(depth), offset);
}

/* The tile AddrLib builds for a mode and element size: its block extents and
 * the pitch of a surface one tile wide. False when AddrLib refuses the shape. */
static bool
tileShape(ADDR_HANDLE lib, AddrSwizzleMode mode, unsigned bpe, unsigned samples, bool depth,
          unsigned *width, unsigned *height, unsigned *pitch)
{
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in;
    ADDR2_MIP_INFO mip[2];
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out;
    std::memset(&in, 0, sizeof(in));
    std::memset(mip, 0, sizeof(mip));
    std::memset(&out, 0, sizeof(out));
    in.size = sizeof(in);
    in.swizzleMode = mode;
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.bpp = bpe * 8u;
    in.width = 4096;
    in.height = 4096;
    in.numSlices = 1;
    in.numMipLevels = 1;
    in.numSamples = samples;
    in.numFrags = samples;
    if (depth)
        in.flags.depth = 1;
    else
        in.flags.color = 1;
    in.flags.texture = 1;
    in.flags.needEquation = 1;
    in.flags.allowExtEquation = 1;
    out.size = sizeof(out);
    out.pMipInfo = mip;
    if (Addr2ComputeSurfaceInfo(lib, &in, &out) != ADDR_OK)
        return false;
    *width = out.blockWidth;
    *height = out.blockHeight;
    *pitch = out.pitch;
    return out.blockWidth != 0 && out.blockHeight != 0;
}

/* The map, derived by probing one coordinate bit at a time and then required to
 * reproduce every texel of the tile. */
static bool
deriveSwizzle(ADDR_HANDLE lib, AddrSwizzleMode mode, unsigned bpe, unsigned samples, bool depth,
              unsigned tiles, SwizzleMap *map)
{
    std::memset(map, 0, sizeof(*map));
    map->bpe = bpe;
    map->samples = samples;
    map->depth = depth;
    map->mode = mode;
    unsigned tile_width = 0;
    unsigned tile_height = 0;
    if (!tileShape(lib, mode, bpe, samples, depth, &tile_width, &tile_height, &map->pitch))
        return false;
    map->blockWidth = tile_width;
    map->blockHeight = tile_height;
    /* More than one tile, so a map whose texel placement depends on the tile's
     * own place in the grid exposes the bits that carry it as terms of their
     * own; one tile's derivation cannot see them. */
    map->width = tile_width * (tiles > 0 ? tiles : 1u);
    map->height = tile_height * (tiles > 0 ? tiles : 1u);

    unsigned long long base = 0;
    if (!coordOffset(lib, mode, bpe, samples, depth, map->width, map->height, map->pitch, 0, 0, 0,
                     &base))
        return false;
    map->base = base;

    /* One input bit's contribution: the address difference it makes. */
    const auto delta = [&](unsigned coord, unsigned bit, unsigned long long *value) {
        const unsigned x = coord == 0 ? 1u << bit : 0u;
        const unsigned y = coord == 0 ? 0u : 1u << bit;
        unsigned long long at = 0;
        if (!coordOffset(lib, mode, bpe, samples, depth, map->width, map->height, map->pitch, x, y, 0,
                         &at))
            return false;
        *value = at ^ base;
        return true;
    };
    const unsigned coordBits[2] = {map->width, map->height};
    for (unsigned coord = 0; coord < 2; coord++) {
        for (unsigned bit = 0; (1u << bit) < coordBits[coord]; bit++) {
            unsigned long long changed = 0;
            if (!delta(coord, bit, &changed))
                return false;
            for (unsigned out = 0; out < 32; out++) {
                if ((changed & (1ull << out)) == 0)
                    continue;
                const int shift = static_cast<int>(out) - static_cast<int>(bit);
                bool merged = false;
                for (unsigned term = 0; term < map->terms && !merged; term++) {
                    if (map->term[term].coord == coord && map->term[term].shift == shift) {
                        map->term[term].mask |= 1u << out;
                        merged = true;
                    }
                }
                if (!merged) {
                    if (map->terms >= 32)
                        return false;
                    map->term[map->terms++] =
                        SwizzleTerm{static_cast<unsigned char>(coord), shift, 1u << out};
                }
            }
        }
    }
    /* The sample bits, where a four-sample texel's placement depends on them:
     * what separates a four-sample depth map (samples four bytes apart inside
     * one sixteen-byte texel) from a four-sample colour one (four sample planes
     * at one position, ps5vk_image.c's PS5VK_SAMPLE_PLANE_BYTES). */
    for (unsigned bit = 0; (1u << bit) < samples; bit++) {
        unsigned long long at = 0;
        if (!coordOffset(lib, mode, bpe, samples, depth, map->width, map->height, map->pitch, 0, 0,
                         1u << bit, &at))
            return false;
        const unsigned long long changed = at ^ base;
        for (unsigned out = 0; out < 32; out++) {
            if ((changed & (1ull << out)) == 0)
                continue;
            const int shift = static_cast<int>(out) - static_cast<int>(bit);
            bool merged = false;
            for (unsigned term = 0; term < map->terms && !merged; term++) {
                if (map->term[term].coord == 2 && map->term[term].shift == shift) {
                    map->term[term].mask |= 1u << out;
                    merged = true;
                }
            }
            if (!merged) {
                if (map->terms >= 32)
                    return false;
                map->term[map->terms++] = SwizzleTerm{2, shift, 1u << out};
            }
        }
    }

    /* Every texel of the tile, recomputed from the terms, and every sample of
     * it when there is more than one. */
    for (unsigned y = 0; y < map->height; y++) {
        for (unsigned x = 0; x < map->width; x++) {
            for (unsigned sample = 0; sample < (samples > 1 ? samples : 1); sample++) {
                unsigned long long predicted = base;
                unsigned long long actual = 0;
                for (unsigned term = 0; term < map->terms; term++) {
                    const unsigned value =
                        map->term[term].coord == 0 ? x : (map->term[term].coord == 1 ? y : sample);
                    const int shift = map->term[term].shift;
                    const unsigned long long moved =
                        shift >= 0 ? (static_cast<unsigned long long>(value) << shift)
                                   : (value >> -shift);
                    predicted ^= moved & map->term[term].mask;
                }
                if (!coordOffset(lib, mode, bpe, samples, depth, map->width, map->height,
                                 map->pitch, x, y, sample, &actual))
                    return false;
                if (predicted != actual)
                    return false;
                map->verified++;
                if (bpe == 4 && samples == 1 && !depth && tiles <= 1 &&
                    predicted != blockOffset(x, y))
                    map->measuredMismatch++;
            }
        }
    }

    /* How many bytes of a row stay contiguous from the tile's origin: what a
     * copy may move in one piece. */
    unsigned long long previous = 0;
    if (!coordOffset(lib, mode, bpe, samples, depth, map->width, map->height, map->pitch, 0, 0, 0,
                     &previous))
        return false;
    map->runBytes = bpe;
    for (unsigned x = 1; x < map->width; x++) {
        unsigned long long at = 0;
        if (!coordOffset(lib, mode, bpe, samples, depth, map->width, map->height, map->pitch, x, 0,
                         0, &at))
            return false;
        if (at != previous + bpe)
            break;
        previous = at;
        map->runBytes += bpe;
    }
    return true;
}

static void
printSwizzle(const SwizzleMap &map, const char *mode)
{
    std::printf("   %-7u %-7s %-8s tile %ux%-4u region %ux%-4u pitch %-5u run %-3u bytes  "
                "terms %-2u  verified %llu",
                map.bpe,
                map.samples == 1 ? "1x" : (map.samples == 4 ? "4x" : "?"), mode, map.blockWidth,
                map.blockHeight, map.width, map.height, map.pitch, map.runBytes, map.terms,
                map.verified);
    if (map.bpe == 4 && map.samples == 1 && !map.depth)
        std::printf("  measured %s", map.measuredMismatch == 0 ? "match" : "MISMATCH");
    std::printf("\n");
    for (unsigned term = 0; term < map.terms; term++)
        std::printf("      (%c, %d, 0x%x)%s",
                    map.term[term].coord == 0 ? 'x' : (map.term[term].coord == 1 ? 'y' : 's'),
                    map.term[term].shift, map.term[term].mask,
                    term + 1 == map.terms ? "\n" : ",\n");
}

/* Every texel's and sample's offset over a tile count, one line each: a check
 * evaluates the driver's own map -- its terms and its grid turn -- and compares
 * the two, so a map that is written down correctly but applied incorrectly is
 * caught as well. */
static void
printCoords(ADDR_HANDLE lib, AddrSwizzleMode mode, unsigned bpe, unsigned samples, bool depth,
            unsigned tiles)
{
    SwizzleMap map;
    if (!deriveSwizzle(lib, mode, bpe, samples, depth, tiles, &map)) {
        std::printf("refused\n");
        return;
    }
    for (unsigned y = 0; y < map.height; y++) {
        for (unsigned x = 0; x < map.width; x++) {
            for (unsigned sample = 0; sample < (samples > 1 ? samples : 1); sample++) {
                unsigned long long at = 0;
                if (!coordOffset(lib, mode, bpe, samples, depth, map.width, map.height,
                                 map.pitch, x, y, sample, &at))
                    return;
                std::printf("%u %u %u 0x%llx\n", x, y, sample, at);
            }
        }
    }
}

/* Every 64 KiB swizzle mode, against the console's measured 4-byte map: which
 * one is the mode the console's own storage was read as. In this library's
 * configuration exactly one matches -- the render-target swizzle -- and the
 * others are the record of why a map has to be derived per mode and per element
 * size rather than scaled from one. */
static void
findMeasuredMode(ADDR_HANDLE lib)
{
    const struct {
        AddrSwizzleMode mode;
        const char *name;
    } modes[] = {
        {ADDR_SW_64KB_Z, "64kb_z"},     {ADDR_SW_64KB_S, "64kb_s"},
        {ADDR_SW_64KB_D, "64kb_d"},     {ADDR_SW_64KB_R, "64kb_r"},
        {ADDR_SW_64KB_Z_T, "64kb_z_t"}, {ADDR_SW_64KB_S_T, "64kb_s_t"},
        {ADDR_SW_64KB_D_T, "64kb_d_t"}, {ADDR_SW_64KB_R_T, "64kb_r_t"},
        {ADDR_SW_64KB_Z_X, "64kb_z_x"}, {ADDR_SW_64KB_S_X, "64kb_s_x"},
        {ADDR_SW_64KB_D_X, "64kb_d_x"}, {ADDR_SW_64KB_R_X, "64kb_r_x"},
    };
    std::printf("/* 4-byte 1x colour: which 64 KiB swizzle mode is the console's map */\n");
    for (const auto &entry : modes) {
        SwizzleMap map;
        if (!deriveSwizzle(lib, entry.mode, 4, 1, false, 1, &map)) {
            std::printf("   %-10s refused\n", entry.name);
            continue;
        }
        std::printf("   %-10s tile %ux%-5u terms %-2u  measured mismatch %llu of %llu\n",
                    entry.name, map.width, map.height, map.terms, map.measuredMismatch,
                    map.verified);
    }
}

/* The sizes the driver's map is asked for: the console-measured 4-byte colour
 * tile first, in every 64 KiB mode AddrLib offers, so the derivation is pinned
 * to the mode whose answer the console's own readbacks already agreed with;
 * then the 2-byte one this step is about, then the depth tile and the sizes the
 * driver still has no map for. */
static void
printSwizzles(ADDR_HANDLE lib)
{
    std::printf("/* element samples mode      tile           pitch     run          terms     check */\n");
    const struct {
        unsigned bpe;
        unsigned samples;
        bool depth;
        AddrSwizzleMode mode;
        const char *name;
    } rows[] = {
        {4, 1, false, ADDR_SW_64KB_S, "64kb_s"},
        {4, 1, false, ADDR_SW_64KB_R_X, "64kb_r_x"},
        {4, 1, true, ADDR_SW_64KB_Z_X, "64kb_z_x"},
        {4, 1, true, ADDR_SW_64KB_S, "64kb_s"},
        {2, 1, false, ADDR_SW_64KB_S, "64kb_s"},
        {2, 1, false, ADDR_SW_64KB_R_X, "64kb_r_x"},
        {2, 1, true, ADDR_SW_64KB_Z_X, "64kb_z_x"},
        {1, 1, false, ADDR_SW_64KB_R_X, "64kb_r_x"},
        {8, 1, false, ADDR_SW_64KB_R_X, "64kb_r_x"},
        {4, 4, false, ADDR_SW_64KB_R_X, "64kb_r_x"},
        {4, 4, true, ADDR_SW_64KB_Z_X, "64kb_z_x"},
    };
    for (const auto &row : rows) {
        SwizzleMap map;
        if (!deriveSwizzle(lib, row.mode, row.bpe, row.samples, row.depth, 1, &map))
            std::printf("   %-7u %-7s %-9s refused\n", row.bpe,
                        row.samples == 1 ? "1x" : (row.samples == 4 ? "4x" : "?"), row.name);
        else
            printSwizzle(map, row.name);
    }
}

/* An array or cube resource: AddrLib lays a slice out as a chain of its own, so
 * the layers are consecutive chains and the one number a caller needs is
 * sliceSize. This is the reference D1's array work is written from: the console
 * has measured no array yet, and the sampler's own layer field is Battery 1's
 * question (docs/M5_REFERENCE.md, V0-unknowns). AddrLib's ADDR2 API has no cube
 * flag at all: a cube is a six-slice array to it, which is why the cube row
 * below and the six-slice array row are the same numbers. */
static void
printArray(ADDR_HANDLE lib, unsigned width, unsigned height, unsigned levels, unsigned slices,
           bool cube)
{
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in;
    ADDR2_MIP_INFO mip[kMaxMipLevels];
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out;
    std::memset(&in, 0, sizeof(in));
    std::memset(mip, 0, sizeof(mip));
    std::memset(&out, 0, sizeof(out));
    in.size = sizeof(in);
    in.swizzleMode = ADDR_SW_64KB_S;
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.flags.texture = 1;
    in.bpp = 32;
    in.width = width;
    in.height = height;
    in.numSlices = slices;
    in.numMipLevels = levels;
    out.size = sizeof(out);
    out.pMipInfo = mip;
    if (Addr2ComputeSurfaceInfo(lib, &in, &out) != ADDR_OK) {
        std::printf("/* %ux%u, %u levels, %u slice(s)%s: AddrLib refused the shape */\n", width,
                    height, levels, slices, cube ? ", cube" : "");
        return;
    }
    std::printf("   {%u, %u, %u, %u, %s, 0x%llx, 0x%llx}, /* slice bases: ",
                width, height, levels, slices, cube ? "cube" : "array",
                static_cast<unsigned long long>(out.sliceSize),
                static_cast<unsigned long long>(out.surfSize));
    for (unsigned slice = 0; slice < slices; slice++)
        std::printf("%s0x%llx", slice ? ", " : "",
                    static_cast<unsigned long long>(out.sliceSize) * slice);
    std::printf(" */\n");
}

static void
printShape(ADDR_HANDLE lib, unsigned width, unsigned height, unsigned levels, bool detail)
{
    Chain chain;
    if (!computeChain(lib, width, height, levels, 4, &chain)) {
        std::printf("/* %ux%u, %u levels: AddrLib refused the shape */\n", width, height, levels);
        return;
    }
    std::printf("   {%u, %u, %u, 0x%llx, {", width, height, levels,
                static_cast<unsigned long long>(chain.info.surfSize));
    for (unsigned level = 0; level < levels; level++)
        std::printf("%s0x%llx", level ? ", " : "", levelBase(chain, level));
    std::printf("}}, /* firstMipInTail %u */\n", chain.info.firstMipIdInTail);
    if (!detail)
        return;
    std::printf("/* level  base        macroBlk    tailOff     tailCoord   pitch height block %ux%u */\n",
                chain.info.blockWidth, chain.info.blockHeight);
    for (unsigned level = 0; level < levels; level++) {
        const ADDR2_MIP_INFO &mip = chain.mip[level];
        std::printf("   %-6u 0x%08llx  0x%08llx  0x%08x  (%3u,%3u)   %-5u %-6u 0x%x\n", level,
                    levelBase(chain, level), static_cast<unsigned long long>(mip.macroBlockOffset),
                    mip.mipTailOffset, mip.mipTailCoordX, mip.mipTailCoordY, mip.pitch, mip.height,
                    mip.equationIndex);
    }
    std::printf("/* chain 0x%llx bytes, tail block 0x%x, tile %ux%u texels */\n\n",
                static_cast<unsigned long long>(chain.info.surfSize), kBlockBytes, kTileTexels,
                kTileTexels);
}

int
main(int argc, char **argv)
{
    ADDR_CREATE_INPUT create_in;
    ADDR_CREATE_OUTPUT create_out;
    std::memset(&create_in, 0, sizeof(create_in));
    std::memset(&create_out, 0, sizeof(create_out));
    create_in.size = sizeof(create_in);
    create_in.chipEngine = CIASICIDGFXENGINE_ARCTICISLAND;
    create_in.chipFamily = FAMILY_NV;
    /* The console's own configuration, and the one thing this tool has to be
     * built with: AddrLib picks its swizzle pattern by the pipe count, so a
     * default (one-pipe) library answers with a different map than the console
     * holds. Sixteen pipes, a 256-byte pipe interleave and a non-RbPlus
     * revision (Navi10) are what the console's measured 4-byte colour map and
     * depth mask agree with, texel for texel, in both 64 KiB modes
     * (docs/HARDWARE_FINDINGS.md). The register's own field values are
     * AddrLib's (src/core/addrlib.h, ADDR_CONFIG_16_PIPE = 0x4 and
     * ADDR_CONFIG_PIPE_INTERLEAVE_256B = 0), which addrinterface.h does not
     * carry: unset fields read as zero, so the value is the pipe count alone. */
    create_in.regValue.gbAddrConfig = kGbAddrConfig16Pipe;
    create_in.chipRevision = 0x01; /* AMDGPU_NAVI10_RANGE starts at 1: supportRbPlus stays 0. */
    create_in.callbacks.allocSysMem = allocSysMem;
    create_in.callbacks.freeSysMem = freeSysMem;
    create_in.createFlags.value = 0;
    if (AddrCreate(&create_in, &create_out) != ADDR_OK) {
        std::fprintf(stderr, "AddrCreate failed\n");
        return 2;
    }
    if (argc >= 2 && std::strcmp(argv[1], "swizzle") == 0) {
        if (argc <= 2) {
            printSwizzles(create_out.hLib);
        } else {
            const unsigned bpe = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0));
            const unsigned samples =
                argc >= 4 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 0)) : 1u;
            const char *const name = argc >= 5 ? argv[4] : "64kb_s";
            AddrSwizzleMode mode = ADDR_SW_64KB_S;
            bool depth = false;
            if (std::strcmp(name, "64kb_r_x") == 0 || std::strcmp(name, "target") == 0) {
                mode = ADDR_SW_64KB_R_X;
            } else if (std::strcmp(name, "64kb_z_x") == 0 || std::strcmp(name, "depth") == 0) {
                mode = ADDR_SW_64KB_Z_X;
                depth = true;
            } else if (std::strcmp(name, "64kb_s") != 0 && std::strcmp(name, "colour") != 0) {
                std::fprintf(stderr, "unknown mode: %s\n", name);
                AddrDestroy(create_out.hLib);
                return 2;
            }
            SwizzleMap map;
            const unsigned tiles =
                argc >= 6 ? static_cast<unsigned>(std::strtoul(argv[5], nullptr, 0)) : 1u;
            if (!deriveSwizzle(create_out.hLib, mode, bpe, samples, depth, tiles, &map))
                std::printf("/* %u-byte %ux %s: AddrLib refused the shape */\n", bpe, samples, name);
            else
                printSwizzle(map, name);
        }
        AddrDestroy(create_out.hLib);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "tiles") == 0) {
        printTiles(create_out.hLib);
        AddrDestroy(create_out.hLib);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "coords") == 0 && argc >= 5) {
        const unsigned bpe = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0));
        const unsigned samples = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 0));
        const char *const name = argv[4];
        const unsigned tiles =
            argc >= 6 ? static_cast<unsigned>(std::strtoul(argv[5], nullptr, 0)) : 1u;
        AddrSwizzleMode mode = ADDR_SW_64KB_S;
        bool depth = false;
        if (std::strcmp(name, "64kb_r_x") == 0 || std::strcmp(name, "target") == 0)
            mode = ADDR_SW_64KB_R_X;
        else if (std::strcmp(name, "64kb_z_x") == 0 || std::strcmp(name, "depth") == 0) {
            mode = ADDR_SW_64KB_Z_X;
            depth = true;
        }
        printCoords(create_out.hLib, mode, bpe, samples, depth, tiles);
        AddrDestroy(create_out.hLib);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "modes") == 0) {
        findMeasuredMode(create_out.hLib);
        AddrDestroy(create_out.hLib);
        return 0;
    }
    if (argc >= 4) {
        printShape(create_out.hLib, static_cast<unsigned>(std::strtoul(argv[1], nullptr, 0)),
                   static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0)),
                   static_cast<unsigned>(std::strtoul(argv[3], nullptr, 0)), true);
        AddrDestroy(create_out.hLib);
        return 0;
    }
    // The shapes probe chains use: the measured five-level 256x256 chain first,
    // then the same chain with a level more, then chains whose whole mip range
    // is inside the tail and chains with several levels outside it.
    printShape(create_out.hLib, 256, 256, 5, false);
    printShape(create_out.hLib, 256, 256, 6, false);
    printShape(create_out.hLib, 128, 128, 4, false);
    printShape(create_out.hLib, 512, 512, 7, false);
    printShape(create_out.hLib, 1024, 1024, 8, false);
    printShape(create_out.hLib, 256, 128, 4, false);
    printShape(create_out.hLib, 64, 64, 3, false);
    printShape(create_out.hLib, 32, 32, 2, false);
    // The single-level shapes the array rows below are slices of, so the check
    // can hold each slice size against its own chain.
    printShape(create_out.hLib, 256, 256, 1, false);
    printShape(create_out.hLib, 64, 64, 1, false);
    // D1's arrays and cubemaps: the two-layer chain the array probe fills, and
    // the six-layer square a cube is.
    printArray(create_out.hLib, 256, 256, 1, 2, false);
    printArray(create_out.hLib, 256, 256, 5, 2, false);
    printArray(create_out.hLib, 64, 64, 1, 6, false);
    printArray(create_out.hLib, 64, 64, 1, 6, true);
    printArray(create_out.hLib, 256, 256, 5, 6, false);
    AddrDestroy(create_out.hLib);
    return 0;
}
