/*
 * PS5 Vulkan compatibility probe - PS5 system calls on Linux.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PS5 system functions the test runner calls, implemented on Linux so the
 * runner's own code runs on the PC (host/runner). Paths under /app0 and
 * /download0 map to host directories; direct-memory allocations map at the
 * addresses a golden capture recorded, so every address in a command stream
 * matches the console's; klog lines go to stdout.
 *
 * Without a replay (the Vulkan driver's PC tests, which load none) direct
 * memory follows the console's observed behaviour instead: see free_allocate.
 * A driver test can still name a replay in PS5_HOST_REPLAY: allocations then
 * follow the same behaviour, except that each replayed region goes to the
 * first allocation of its size, at its captured address, and shader
 * creation, linking and the register defaults replay from it
 * (load_environment_replay). PS5_HOST_SUBMISSION_DUMP records every
 * submission with the register tables it points at (dump_submission).
 *
 * What the PC cannot compute is replayed from the capture: the AGC register
 * defaults, the relocated shader headers sceAgcCreateShader leaves in the
 * stage workspace, the link outputs sceAgcLinkShaders writes, and the
 * VideoOut handle. Nothing is submitted: submission, completion and flips
 * report success without doing anything.
 */

#include "ps5/ps5_host.hpp"
#include "agc_abi.hpp"
#include "agc/agc_host.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace
{
constexpr int kErrorNotFound = static_cast<int>(0x80020002u);
constexpr int kErrorNoMemory = static_cast<int>(0x8002000cu);
/* The regions a replay names: a frame's own allocations plus one per captured
 * pipeline stage. A probe that draws through a device per frame captures one
 * stage each, so the count follows the probe (the format probe's seventeen),
 * not the eight a single frame's allocation list used to need. */
/* The regions a replay lists: a driver run's stage images, the table
 * allocations its submissions name and the runner's own workspace. One a
 * pipeline, and V0-formats' sampled test now builds twenty-two of them (one
 * a packed format), so the cap has to sit above the largest capture rather
 * than at the previous one.
 *
 * A capture run's replay lists every test's pipelines, not one test's, because
 * the runner runs a queue in one process and takes one replay for all of it
 * (tools/golden.py, driver_replay_text, whose `test` argument is what narrows
 * it). The re-capture of 2026-09-20 covered fifteen cases in one run, so
 * golden/c7-mip-tiled's replay carries 77 stage lines and 50 regions, and
 * golden/v0-formats-sampled's 130 and 12: 142 of the 64 the previous cap
 * allowed, which made parse_replay reject a valid replay and every case that
 * needed it report NO RECORD. */
constexpr std::size_t kMaxRegions = 256;
constexpr std::size_t kMaxDefaults = 64;
// The runner's stage workspace, one 64 KiB half per linked package set
// (src/diagnostics.cpp kStageBytes and kMaxLinkedSets).
constexpr std::size_t kStageHalfBytes = 0x10000;
constexpr std::size_t kMaxWorkspaceBytes = 2 * kStageHalfBytes;
constexpr std::size_t kChunkWords = 64;
constexpr std::size_t kHeaderMinimumBytes = 96;
// sceAgcLinkShaders writes its context at +0x5000 and uniforms at +0x6000;
// the frame's own SH table starts at +0x6800.
constexpr std::size_t kLinkBegin = 0x5000;
constexpr std::size_t kLinkUniforms = 0x6000;
constexpr std::size_t kLinkEnd = 0x6800;
constexpr std::int64_t kDirectStartUnit = 0x200000;

// A direct-memory region of the replayed run, at its captured address.
struct Region
{
    /* Long enough for "tables-<test>-<address>", the name a replay writes for
     * a submission's own region (tools/golden.py, driver_run_document): a
     * shorter buffer truncates the name into the address that follows it and
     * the region parses as address 0. */
    char name[64];
    std::uintptr_t address;
    std::size_t bytes;
    bool allocated;
    bool mapped;
    /* The captured image of this region, allocated the first time a chunk of
     * it arrives, or null when the capture carries none: AGC's output for a
     * pipeline the console ran, which a PC rebuild replays
     * (ps5vk_debug.h, ps5vk_debug_pipeline_stages). */
    std::uint8_t *image;
    /* The pipeline index a "stage" region was declared with, or -1. */
    long stage_index;
};

// The layout the runner reads from sceAgcGetRegisterDefaults: a block-pointer
// array at offset 0 and the record count at offset 0x20.
struct RegisterDefaults
{
    AgcRegister **blocks;
    std::uint8_t reserved[0x18];
    std::uint32_t count;
};

char g_app0[512];
char g_download0[512];
char g_queue[512];
Region g_regions[kMaxRegions]{};
std::size_t g_region_count = 0;
AgcRegister g_default_records[kMaxDefaults]{};
AgcRegister *g_default_blocks[1] = {g_default_records};
RegisterDefaults g_defaults{g_default_blocks, {}, 0};
int g_video = 1;
// Whether the runner loaded a replay (ps5_host_load_replay): every allocation
// then comes from it. A PS5_HOST_REPLAY replay leaves direct memory free.
bool g_strict_replay = false;

const Region *find_region(const char *name) noexcept
{
    for (std::size_t index = 0; index < g_region_count; ++index)
        if (std::strcmp(g_regions[index].name, name) == 0)
            return &g_regions[index];
    return nullptr;
}

bool copy_text(char *destination, std::size_t capacity, const char *text) noexcept
{
    const int length = std::snprintf(destination, capacity, "%s", text ? text : "");
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

// The host path of a console path under /app0 or /download0.
bool host_path(const char *path, char *resolved, std::size_t capacity) noexcept
{
    if (path == nullptr || std::strstr(path, "..") != nullptr)
        return false;
    if (g_queue[0] != '\0' && std::strcmp(path, "/app0/jobs/queue.txt") == 0)
        return copy_text(resolved, capacity, g_queue);
    const struct
    {
        const char *prefix;
        const char *root;
    } mounts[] = {{"/app0/", g_app0}, {"/download0/", g_download0}};
    for (const auto &mount : mounts)
    {
        const std::size_t length = std::strlen(mount.prefix);
        if (std::strncmp(path, mount.prefix, length) == 0)
        {
            const int written =
                std::snprintf(resolved, capacity, "%s/%s", mount.root, path + length);
            return written >= 0 && static_cast<std::size_t>(written) < capacity;
        }
    }
    return false;
}

// FreeBSD open flags, as the PS5 kernel takes them, to Linux flags.
int host_open_flags(int flags) noexcept
{
    const int access = flags & 3;
    int result = access == 1 ? O_WRONLY : access == 2 ? O_RDWR : O_RDONLY;
    if (flags & 0x8)
        result |= O_APPEND;
    if (flags & 0x200)
        result |= O_CREAT;
    if (flags & 0x400)
        result |= O_TRUNC;
    if (flags & 0x800)
        result |= O_EXCL;
    return result | O_CLOEXEC;
}

// The submission description the runner passes to sceAgcDriverSubmitDcb.
struct SubmitDescription
{
    const std::uint32_t *words;
    std::uint32_t word_count;
    std::uint8_t flag;
    std::uint8_t padding[3];
};

// Whether [address, address + bytes) lies inside one mapped replay region.
bool mapped_range(std::uintptr_t address, std::size_t bytes) noexcept
{
    for (std::size_t index = 0; index < g_region_count; ++index)
    {
        const Region &region = g_regions[index];
        if (region.mapped && address >= region.address &&
            address - region.address <= region.bytes &&
            bytes <= region.bytes - (address - region.address))
            return true;
    }
    return false;
}

Region *region_of_start(std::int64_t start) noexcept
{
    if (start <= 0 || start % kDirectStartUnit != 0)
        return nullptr;
    const auto index = static_cast<std::size_t>(start / kDirectStartUnit) - 1;
    return index < g_region_count ? &g_regions[index] : nullptr;
}

// The image of a region, allocated zeroed the first time a chunk arrives.
std::uint8_t *region_image(Region &region) noexcept
{
    if (region.image == nullptr)
        region.image = static_cast<std::uint8_t *>(std::calloc(1, region.bytes));
    return region.image;
}

// The region [address, address + bytes) lies in, or null: the captured pipeline
// a header, a linked area or a table belongs to.
const Region *find_region_containing(std::uintptr_t address) noexcept
{
    for (std::size_t index = 0; index < g_region_count; ++index) {
        const Region &region = g_regions[index];
        /* A region the replay handed to an allocation: the same condition the
         * old single-stage lookup used, since a region's `mapped` flag is not
         * kept for the replayed ones. */
        if (!region.allocated)
            continue;
        if (address >= region.address && address - region.address < region.bytes)
            return &region;
    }
    return nullptr;
}

// One replay "workspace <offset> <hex>" or "stageimage <index> <offset> <hex>"
// chunk into a region's image.
bool load_chunk(Region &region, unsigned long long offset, const char *hex) noexcept
{
    if (std::strlen(hex) != kChunkWords * 8 || offset + kChunkWords * 4 > region.bytes)
        return false;
    std::uint8_t *const image = region_image(region);
    if (image == nullptr)
        return false;
    for (std::size_t word = 0; word < kChunkWords; ++word)
    {
        char digits[9]{};
        std::memcpy(digits, hex + word * 8, 8);
        char *end = nullptr;
        const unsigned long value = std::strtoul(digits, &end, 16);
        if (end != digits + 8)
            return false;
        const auto bits = static_cast<std::uint32_t>(value);
        std::memcpy(image + offset + word * 4, &bits, sizeof(bits));
    }
    return true;
}

// Direct memory without a replay. Allocations are first-fit ranges of the
// reported direct memory, aligned as requested (16 KiB at least, as the
// console's page size requires); a range mapped whole lands at
// kFreeMappingBase plus its start, so mappings keep address high word 2, where
// every console mapping of the golden captures lies (0x20001c000, 0x200200000).
constexpr int kErrorInvalid = static_cast<int>(0x80020016u);
constexpr std::size_t kMaxFreeAllocations = 4096;
constexpr std::size_t kDirectPageBytes = 0x4000;
constexpr std::int64_t kFreeDirectMemoryBytes = INT64_C(0x100000000);
constexpr std::uintptr_t kFreeMappingBase = UINT64_C(0x200000000);

struct FreeAllocation
{
    std::int64_t start;
    std::size_t bytes;
    bool mapped;
};

FreeAllocation g_free[kMaxFreeAllocations]{};
std::size_t g_free_count = 0;

bool free_mode() noexcept
{
    return !g_strict_replay;
}

std::uintptr_t free_mapping_base() noexcept;

std::int32_t free_allocate(std::int64_t search_start, std::int64_t search_end, std::size_t bytes,
                           std::size_t alignment, std::int64_t *start) noexcept
{
    if (alignment == 0)
        alignment = kDirectPageBytes;
    if (start == nullptr || bytes == 0 || bytes % kDirectPageBytes != 0 ||
        alignment < kDirectPageBytes || (alignment & (alignment - 1)) != 0 || search_start < 0 ||
        search_end <= search_start)
        return kErrorInvalid;
    const std::int64_t end = search_end < kFreeDirectMemoryBytes ? search_end : kFreeDirectMemoryBytes;
    const auto align = [alignment](std::int64_t value) {
        const auto mask = static_cast<std::int64_t>(alignment - 1);
        return (value + mask) & ~mask;
    };
    if (g_free_count == kMaxFreeAllocations || static_cast<std::int64_t>(bytes) > end)
        return kErrorNoMemory;
    const std::uintptr_t base = free_mapping_base();
    const auto overlaps = [bytes](std::int64_t candidate, const FreeAllocation &other) {
        return candidate < other.start + static_cast<std::int64_t>(other.bytes) &&
               other.start < candidate + static_cast<std::int64_t>(bytes);
    };
    // A region of a PS5_HOST_REPLAY replay goes to the first allocation of its
    // size, at the start that maps it at its captured address.
    for (std::size_t index = 0; index < g_region_count; ++index)
    {
        Region &region = g_regions[index];
        if (region.allocated || region.bytes != bytes || region.address < base)
            continue;
        const auto captured = static_cast<std::int64_t>(region.address - base);
        bool available = captured % static_cast<std::int64_t>(alignment) == 0 &&
                         captured <= end - static_cast<std::int64_t>(bytes);
        for (std::size_t other = 0; available && other < g_free_count; ++other)
            available = !overlaps(captured, g_free[other]);
        if (!available)
            continue;
        region.allocated = true;
        g_free[g_free_count++] = FreeAllocation{captured, bytes, false};
        *start = captured;
        return 0;
    }
    // First fit, around earlier allocations and regions not yet handed out.
    std::int64_t candidate = align(search_start);
    for (bool moved = true; moved && candidate <= end - static_cast<std::int64_t>(bytes);)
    {
        moved = false;
        for (std::size_t index = 0; index < g_free_count; ++index)
        {
            const FreeAllocation &other = g_free[index];
            if (overlaps(candidate, other))
            {
                candidate = align(other.start + static_cast<std::int64_t>(other.bytes));
                moved = true;
            }
        }
        for (std::size_t index = 0; index < g_region_count; ++index)
        {
            const Region &region = g_regions[index];
            if (region.allocated || region.address < base)
                continue;
            const FreeAllocation reserved{static_cast<std::int64_t>(region.address - base),
                                          region.bytes, false};
            if (overlaps(candidate, reserved))
            {
                candidate = align(reserved.start + static_cast<std::int64_t>(reserved.bytes));
                moved = true;
            }
        }
    }
    if (candidate > end - static_cast<std::int64_t>(bytes))
        return kErrorNoMemory;
    g_free[g_free_count++] = FreeAllocation{candidate, bytes, false};
    *start = candidate;
    return 0;
}

// PS5_HOST_DIRECT_MAPPING_BASE moves the mappings, so a check can prove that
// the driver refuses memory outside the GPU address window.
std::uintptr_t free_mapping_base() noexcept
{
    const char *const text = std::getenv("PS5_HOST_DIRECT_MAPPING_BASE");
    if (text == nullptr)
        return kFreeMappingBase;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    return end != text && *end == '\0' ? static_cast<std::uintptr_t>(value) : kFreeMappingBase;
}

FreeAllocation *free_allocation(std::int64_t start, std::size_t bytes) noexcept
{
    for (std::size_t index = 0; index < g_free_count; ++index)
        if (g_free[index].start == start && g_free[index].bytes == bytes)
            return &g_free[index];
    return nullptr;
}

std::int32_t free_map(void **address, std::size_t bytes, std::int64_t start) noexcept
{
    FreeAllocation *const allocation = free_allocation(start, bytes);
    if (address == nullptr || allocation == nullptr || allocation->mapped)
        return kErrorInvalid;
    const auto wanted =
        reinterpret_cast<void *>(free_mapping_base() + static_cast<std::uintptr_t>(start));
    void *const mapping = mmap(wanted, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapping == MAP_FAILED)
        return kErrorNoMemory;
    if (mapping != wanted)
    {
        munmap(mapping, bytes);
        return kErrorNoMemory;
    }
    allocation->mapped = true;
    *address = mapping;
    return 0;
}

std::int32_t free_unmap(void *address, std::size_t bytes) noexcept
{
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t base = free_mapping_base();
    if (at < base)
        return kErrorInvalid;
    FreeAllocation *const allocation = free_allocation(static_cast<std::int64_t>(at - base), bytes);
    if (allocation == nullptr || !allocation->mapped)
        return kErrorInvalid;
    munmap(address, bytes);
    allocation->mapped = false;
    return 0;
}

std::int32_t free_release(std::int64_t start, std::size_t bytes) noexcept
{
    FreeAllocation *const allocation = free_allocation(start, bytes);
    if (allocation == nullptr || allocation->mapped)
        return kErrorInvalid;
    /* A region handed out for this allocation is free again, as the console's
     * own allocator frees its range: a program that allocates the same sizes
     * again -- a device per frame, seventeen of them -- has to land on the
     * addresses the capture holds, and the capture holds each preallocated
     * region once. */
    const std::uintptr_t base = free_mapping_base();
    for (std::size_t index = 0; index < g_region_count; ++index)
    {
        Region &region = g_regions[index];
        if (region.allocated && region.bytes == bytes &&
            region.address == base + static_cast<std::uintptr_t>(start))
        {
            region.allocated = false;
            break;
        }
    }
    *allocation = g_free[--g_free_count];
    return 0;
}

// Whether [address, address + bytes) lies inside one mapped allocation made
// without a replay.
bool free_mapped_range(std::uintptr_t address, std::size_t bytes) noexcept
{
    const std::uintptr_t base = free_mapping_base();
    for (std::size_t index = 0; index < g_free_count; ++index)
    {
        const FreeAllocation &allocation = g_free[index];
        const std::uintptr_t begin = base + static_cast<std::uintptr_t>(allocation.start);
        if (allocation.mapped && address >= begin && address - begin <= allocation.bytes &&
            bytes <= allocation.bytes - (address - begin))
            return true;
    }
    return false;
}
} // namespace

bool ps5_host_configure(const char *app0, const char *download0, const char *queue) noexcept
{
    return copy_text(g_app0, sizeof(g_app0), app0) &&
           copy_text(g_download0, sizeof(g_download0), download0) &&
           copy_text(g_queue, sizeof(g_queue), queue);
}

namespace
{
// Reads a replay's frame, flips, VideoOut handle, regions, register defaults
// and workspace image; false when it is invalid or incomplete.
bool parse_replay(const char *path, std::uint32_t &frame, std::uint32_t &flips_before) noexcept
{
    frame = 0;
    flips_before = 0;
    bool have_flips = false;
    FILE *const file = std::fopen(path, "r");
    if (file == nullptr)
    {
        std::fprintf(stderr, "cannot open replay %s\n", path);
        return false;
    }
    char line[1024];
    unsigned long line_number = 0;
    bool valid = true;
    while (valid && std::fgets(line, sizeof(line), file) != nullptr)
    {
        ++line_number;
        char name[64]{};
        char hex[kChunkWords * 8 + 2]{};
        unsigned long long first = 0;
        unsigned long long second = 0;
        unsigned long long third = 0;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (std::sscanf(line, "frame %llu", &first) == 1)
            frame = static_cast<std::uint32_t>(first);
        else if (std::sscanf(line, "flips %llu", &first) == 1)
        {
            flips_before = static_cast<std::uint32_t>(first);
            have_flips = true;
        }
        else if (std::sscanf(line, "video %llx", &first) == 1)
            g_video = static_cast<int>(first);
        else if (std::sscanf(line, "region %63s %llx %llx", name, &first, &second) == 3 &&
                 g_region_count < kMaxRegions)
        {
            Region &region = g_regions[g_region_count++];
            region = Region{};
            copy_text(region.name, sizeof(region.name), name);
            region.address = static_cast<std::uintptr_t>(first);
            region.bytes = static_cast<std::size_t>(second);
            region.stage_index = -1;
        }
        /* A pipeline the console ran, as the runner captured it: its own
         * region, listed before the other regions so the replay hands a
         * driver's stage allocations their own captured addresses
         * (tools/golden.py, replay_text). */
        else if (std::sscanf(line, "stage %llu %llx %llx", &first, &second, &third) == 3 &&
                 g_region_count < kMaxRegions)
        {
            Region &region = g_regions[g_region_count++];
            region = Region{};
            std::snprintf(region.name, sizeof(region.name), "pipeline%llu", first);
            region.address = static_cast<std::uintptr_t>(second);
            region.bytes = static_cast<std::size_t>(third);
            region.stage_index = static_cast<long>(first);
        }
        /* A stage's image follows its own "stage" line. The index restarts with
         * every device a test creates (R64 captures twelve frames, a device each),
         * so the image belongs to the latest stage of that index: the first one
         * would take every later frame's chunks over its own relocated headers. */
        else if (std::sscanf(line, "stageimage %llu %llx %513s", &first, &second, hex) == 3)
        {
            const Region *found = nullptr;
            for (std::size_t index = g_region_count; index-- > 0 && !found;) {
                if (g_regions[index].stage_index == static_cast<long>(first))
                    found = &g_regions[index];
            }
            valid = found != nullptr && load_chunk(const_cast<Region &>(*found), second, hex);
        }
        else if (std::sscanf(line, "default %llx %llx", &first, &second) == 2 &&
                 g_defaults.count < kMaxDefaults)
            g_default_records[g_defaults.count++] = AgcRegister{
                static_cast<std::uint16_t>(first), 0, static_cast<std::uint32_t>(second)};
        else if (std::sscanf(line, "workspace %llx %513s", &first, hex) == 2)
        {
            /* The runner's own stage workspace, which every replay carries. */
            const Region *const stage = find_region("stage");
            valid = stage != nullptr && load_chunk(const_cast<Region &>(*stage), first, hex);
        }
        else
            valid = false;
    }
    std::fclose(file);
    // Replays written before B4 have no flips line; every earlier frame flipped.
    if (!have_flips && frame > 0)
        flips_before = frame - 1;
    const Region *const stage = find_region("stage");
    /* A replay of a test that drove the driver has no runner workspace: the
     * stage images it carries are the pipelines the driver built. Either is
     * enough, and a replay with neither has nothing to replay from. */
    bool has_pipeline = false;
    for (std::size_t index = 0; index < g_region_count && !has_pipeline; ++index)
        has_pipeline = g_regions[index].stage_index >= 0;
    const bool has_workspace =
        stage != nullptr && stage->bytes <= kMaxWorkspaceBytes && stage->bytes >= kLinkEnd;
    valid = valid && frame > 0 && g_defaults.count > 0 && (has_workspace || has_pipeline);
    if (!valid)
        std::fprintf(stderr, "%s:%lu: invalid or incomplete replay\n", path, line_number);
    return valid;
}
} // namespace

bool ps5_host_load_replay(const char *path, std::uint32_t &frame,
                          std::uint32_t &flips_before, bool regions) noexcept
{
    if (!parse_replay(path, frame, flips_before))
        return false;
    // g_strict_replay makes every allocation come from the replay's regions,
    // which is what an AGC-level frame needs. A case that goes through the
    // Vulkan driver maps its own memory on the console, so it keeps direct
    // memory free (free_mode) and takes the register defaults and the stage
    // images of its pipelines from the same replay.
    g_strict_replay = regions;
    return true;
}

namespace
{
// PS5_HOST_REPLAY names a replay for the Vulkan driver's PC tests, which run
// without the runner; it is read on first use. A replay that cannot be used
// ends the process, so a test never runs against half of one.
void load_environment_replay() noexcept
{
    static bool checked = false;
    if (checked || g_strict_replay)
        return;
    checked = true;
    const char *const path = std::getenv("PS5_HOST_REPLAY");
    std::uint32_t frame = 0;
    std::uint32_t flips_before = 0;
    if (path != nullptr && !parse_replay(path, frame, flips_before))
    {
        std::fprintf(stderr, "PS5_HOST_REPLAY: cannot use %s\n", path);
        std::abort();
    }
    // The flip helper numbers flips from the process's own count, which a
    // driver run's replay carries when earlier tests of its capture flipped.
    if (path != nullptr)
        agc_host_reset(flips_before);
}

bool host_mapped(std::uintptr_t address, std::size_t bytes) noexcept
{
    return mapped_range(address, bytes) || (free_mode() && free_mapped_range(address, bytes));
}

// Writes the value of every completion marker in a stream (RELEASE_MEM event
// 40, control 0x30c, address high word 2) to its address when that lies in
// mapped memory. PM4 INDIRECT_BUFFER packets (0x3F) are followed into mapped
// memory as Mesa's IB parser follows them (src/amd/common/ac_parse_ib.c): a
// call returns to the stream after the buffer, a chain (IB_CONTROL bit 20)
// ends it. Whether AGC runs such buffers only the console shows (Phase B8).
constexpr unsigned kMaxIndirectDepth = 4;

void complete_markers(const std::uint32_t *words, std::uint32_t count, unsigned depth) noexcept
{
    for (std::uint32_t at = 0; at < count;)
    {
        const std::uint32_t header = words[at];
        const std::uint32_t payload = ((header >> 16) & 0x3fff) + 1;
        // The AGC helpers write only type-3 packets.
        if (header >> 30 != 3 || payload > count - at - 1)
            return;
        const std::uint32_t opcode = (header >> 8) & 0xff;
        const std::uint32_t *const body = words + at + 1;
        if (opcode == 0x49 && payload == 7 && body[0] == 0x0030c528u && body[1] == 0x20000000u &&
            body[3] == 2u)
        {
            const std::uintptr_t address = (std::uintptr_t{2} << 32) | body[2];
            if (host_mapped(address, sizeof(std::uint32_t)))
                std::memcpy(reinterpret_cast<void *>(address), &body[4], sizeof(std::uint32_t));
        }
        else if (opcode == 0x3f && payload == 3 && depth < kMaxIndirectDepth)
        {
            const std::uintptr_t address =
                std::uintptr_t{body[0]} | (std::uintptr_t{body[1]} << 32);
            const std::uint32_t size = body[2] & 0xfffffu;
            if (size != 0 && host_mapped(address, std::size_t{size} * sizeof(std::uint32_t)))
                complete_markers(reinterpret_cast<const std::uint32_t *>(address), size, depth + 1);
            if (body[2] & (1u << 20))
                return;
        }
        at += 1 + payload;
    }
}

// PS5_HOST_SUBMISSION_DUMP names a file that receives one JSON line per
// submission: its words, and the records of every register table a table load
// in it points at in mapped memory, as 8-digit hex words keyed by address
// (tools/golden.py compare-submission).
void dump_submission(const SubmitDescription &submit) noexcept
{
    const char *const path = std::getenv("PS5_HOST_SUBMISSION_DUMP");
    if (path == nullptr || submit.words == nullptr)
        return;
    FILE *const file = std::fopen(path, "a");
    if (file == nullptr)
        return;
    std::fputs("{\"words\": [", file);
    for (std::uint32_t at = 0; at < submit.word_count; ++at)
        std::fprintf(file, "%s\"0x%08x\"", at == 0 ? "" : ", ", submit.words[at]);
    std::fputs("], \"tables\": {", file);
    bool first = true;
    for (std::uint32_t at = 0; at < submit.word_count;)
    {
        const std::uint32_t header = submit.words[at];
        const std::uint32_t payload = ((header >> 16) & 0x3fff) + 1;
        if (header >> 30 != 3 || payload > submit.word_count - at - 1)
            break;
        const std::uint32_t opcode = (header >> 8) & 0xff;
        if ((opcode == 0x63 || opcode == 0x64 || opcode == 0x9f) && payload >= 4)
        {
            const std::uintptr_t address = std::uintptr_t{submit.words[at + 1]} |
                                           (std::uintptr_t{submit.words[at + 2]} << 32);
            const std::size_t bytes = std::size_t{submit.words[at + 4]} * sizeof(AgcRegister);
            if (bytes != 0 &&
                (mapped_range(address, bytes) || (free_mode() && free_mapped_range(address, bytes))))
            {
                std::fprintf(file, "%s\"0x%llx\": \"", first ? "" : ", ",
                             static_cast<unsigned long long>(address));
                const auto *const table = reinterpret_cast<const std::uint32_t *>(address);
                for (std::size_t word = 0; word < bytes / sizeof(std::uint32_t); ++word)
                    std::fprintf(file, "%08x", table[word]);
                std::fputc('"', file);
                first = false;
            }
        }
        at += 1 + payload;
    }
    std::fputs("}}\n", file);
    std::fclose(file);
}
} // namespace

extern "C"
{
int sceKernelOpen(const char *path, int flags, std::uint16_t mode)
{
    char resolved[1100];
    if (!host_path(path, resolved, sizeof(resolved)))
        return kErrorNotFound;
    const int descriptor = open(resolved, host_open_flags(flags), mode != 0 ? mode : 0644);
    return descriptor >= 0 ? descriptor : kErrorNotFound;
}

int sceKernelClose(int descriptor)
{
    return close(descriptor) == 0 ? 0 : kErrorNotFound;
}

std::int64_t sceKernelRead(int descriptor, void *data, std::size_t bytes)
{
    const ssize_t result = read(descriptor, data, bytes);
    return result >= 0 ? result : kErrorNotFound;
}

std::int64_t sceKernelWrite(int descriptor, const void *data, std::size_t bytes)
{
    const ssize_t result = write(descriptor, data, bytes);
    return result >= 0 ? result : kErrorNotFound;
}

int sceKernelDebugOutText(int, const char *text)
{
    std::fputs(text, stdout);
    return 0;
}

std::int64_t sceKernelGetDirectMemorySize(void)
{
    return INT64_C(0x100000000);
}

// Each replayed region is one allocation of its size, handed out in order.
std::int32_t sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                           std::size_t bytes, std::size_t alignment, int,
                                           std::int64_t *start)
{
    load_environment_replay();
    if (free_mode())
        return free_allocate(search_start, search_end, bytes, alignment, start);
    for (std::size_t index = 0; index < g_region_count; ++index)
    {
        Region &region = g_regions[index];
        if (!region.allocated && region.bytes == bytes)
        {
            region.allocated = true;
            *start = kDirectStartUnit * static_cast<std::int64_t>(index + 1);
            return 0;
        }
    }
    return kErrorNoMemory;
}

std::int32_t sceKernelMapDirectMemory(void **address, std::size_t bytes, int, int,
                                      std::int64_t start, std::size_t)
{
    if (free_mode())
        return free_map(address, bytes, start);
    Region *const region = region_of_start(start);
    if (address == nullptr || region == nullptr || !region->allocated || region->mapped ||
        region->bytes != bytes)
        return kErrorNoMemory;
    const auto wanted = reinterpret_cast<void *>(region->address);
    void *const mapping = mmap(wanted, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapping == MAP_FAILED)
        return kErrorNoMemory;
    if (mapping != wanted)
    {
        munmap(mapping, bytes);
        return kErrorNoMemory;
    }
    region->mapped = true;
    *address = mapping;
    return 0;
}

std::int32_t sceKernelMunmap(void *address, std::size_t bytes)
{
    if (free_mode())
        return free_unmap(address, bytes);
    for (std::size_t index = 0; index < g_region_count; ++index)
    {
        Region &region = g_regions[index];
        if (region.mapped && reinterpret_cast<void *>(region.address) == address &&
            region.bytes == bytes)
        {
            munmap(address, bytes);
            region.mapped = false;
            return 0;
        }
    }
    return kErrorNoMemory;
}

std::int32_t sceKernelReleaseDirectMemory(std::int64_t start, std::size_t bytes)
{
    if (free_mode())
        return free_release(start, bytes);
    Region *const region = region_of_start(start);
    if (region == nullptr || !region->allocated || region->mapped)
        return kErrorNoMemory;
    region->allocated = false;
    return 0;
}

std::int32_t sceAgcInit(std::uint32_t)
{
    return 0;
}

void *sceAgcGetRegisterDefaults(void)
{
    load_environment_replay();
    return &g_defaults;
}

// Replay: the header as sceAgcCreateShader left it in the captured pipeline it
// belongs to. The capture carries one image per pipeline the console ran
// (tools/golden.py, "stages"), and the region holding the header says which
// one. A pipeline the capture never had has no header to replay: the
// compiler's own stands, which is the header AGC would have relocated.
std::int32_t sceAgcCreateShader(void **shader, void *header, void *)
{
    load_environment_replay();
    const auto at = reinterpret_cast<std::uintptr_t>(header);
    if (shader == nullptr || header == nullptr)
        return -1;
    const Region *const region = find_region_containing(at);
    if (region == nullptr || region->image == nullptr) {
        *shader = header;
        return 0;
    }
    const std::size_t offset = at - region->address;
    std::uint32_t header_bytes = 0;
    std::memcpy(&header_bytes, static_cast<const std::uint8_t *>(header) + 64, sizeof(header_bytes));
    if (header_bytes < kHeaderMinimumBytes || offset + header_bytes > region->bytes)
        return -1;
    std::memcpy(header, region->image + offset, header_bytes);
    *shader = header;
    return 0;
}

// Replay: the context and uniform areas as sceAgcLinkShaders left them in the
// captured pipeline they belong to. A test with two package sets links each in
// its own half of a stage, so the areas are at kLinkBegin and kLinkUniforms of
// any half. A pipeline the capture never had keeps what the driver's own
// linked areas hold, as sceAgcCreateShader does for its header.
std::int32_t sceAgcLinkShaders(void *context, void *uniforms, void *, void *, void *, std::uint32_t)
{
    load_environment_replay();
    if (context == nullptr || uniforms == nullptr)
        return -1;
    const Region *const region = find_region_containing(reinterpret_cast<std::uintptr_t>(context));
    if (region == nullptr || region->image == nullptr)
        return 0;
    const std::size_t offset = reinterpret_cast<std::uintptr_t>(context) - region->address;
    // R60: a stage whose shaders outgrew the fixed layout links its areas after
    // its code, on any 4 KiB boundary (driver/ps5vk_pipeline.c); the capture
    // holds them there, so they are replayed from wherever the driver linked.
    if ((offset % kStageHalfBytes != kLinkBegin && offset % 0x1000 != 0) ||
        offset + kLinkEnd - kLinkBegin > region->bytes ||
        reinterpret_cast<std::uintptr_t>(uniforms) !=
            reinterpret_cast<std::uintptr_t>(context) + kLinkUniforms - kLinkBegin)
        return -1;
    std::memcpy(context, region->image + offset, kLinkEnd - kLinkBegin);
    return 0;
}

// Nothing runs on a GPU, but completion markers complete at submission: on
// the console the GPU had written them by the time sceAgcSuspendPoint returned
// (Phase B4, golden/b4). Every completion marker in the stream and in the
// indirect buffers it calls or chains to writes its value to its address when
// that lies in mapped memory (complete_markers). PS5_HOST_DROP_COMPLETION_MARKERS leaves
// every marker unwritten, so a check can prove what a driver does when the GPU
// never completes a submission.
std::int32_t sceAgcDriverSubmitDcb(void *description)
{
    const auto *const submit = static_cast<const SubmitDescription *>(description);
    if (submit != nullptr)
        dump_submission(*submit);
    if (std::getenv("PS5_HOST_DROP_COMPLETION_MARKERS") != nullptr)
        return 0;
    if (submit != nullptr && submit->words != nullptr)
        complete_markers(submit->words, submit->word_count, 0);
    return 0;
}

std::int32_t sceAgcSuspendPoint(void)
{
    return 0;
}

// The PC runner does not wait: nothing on the PC completes asynchronously.
int sceKernelUsleep(std::uint32_t)
{
    return 0;
}

// The profile's time-source cost probe only; the PC's answers measure nothing.
std::uint64_t sceKernelReadTsc(void)
{
    return 0;
}

std::uint64_t sceKernelGetTscFrequency(void)
{
    return 1;
}

std::uint64_t sceKernelGetProcessTimeCounter(void)
{
    return 0;
}

int sceVideoOutOpen(std::int32_t, std::int32_t, std::int32_t, const void *)
{
    return g_video;
}

int sceVideoOutClose(std::int32_t)
{
    return 0;
}

int sceVideoOutSetFlipRate(std::int32_t, std::int32_t)
{
    return 0;
}

void sceVideoOutSetBufferAttribute2(void *, std::uint64_t, std::uint32_t, std::uint32_t,
                                    std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t)
{
}

int sceVideoOutRegisterBuffers2(std::int32_t, std::int32_t, std::int32_t, void *, std::int32_t,
                                void *, std::int32_t, void *)
{
    return 0;
}

int sceVideoOutUnregisterBuffers(std::int32_t, std::int32_t)
{
    return 0;
}

// Every flip marker has been reached.
int sceVideoOutGetFlipStatus(std::int32_t, void *status)
{
    std::uint64_t words[16]{};
    words[3] = static_cast<std::uint64_t>(INT64_MAX);
    std::memcpy(status, words, sizeof(words));
    return 0;
}

int sceVideoOutWaitVblank(std::int32_t)
{
    return 0;
}

int sceVideoOutIsFlipPending(std::int32_t)
{
    return 0;
}

// The output-mode selector. There is no panel behind the PC host, so the
// truthful answers are that no such mode is offered and that configuring one is
// refused; the R31 probe then reports that and measures nothing, which is what
// it should do here. The console's own answers are what the console run records.
int sceVideoOutIsOutputSupported(std::int32_t, std::uint32_t, const void *, const void *,
                                 const void *)
{
    return 0;
}

int sceVideoOutConfigureOutput(std::int32_t, std::uint32_t, const void *, const void *, const void *)
{
    return -1;
}

int sceSystemServiceHideSplashScreen(void)
{
    return 0;
}

// The console's module loader, modelled from what the console was measured to
// answer (docs/HARDWARE_FINDINGS.md, the E2 module entry): a path to a file that
// is not a PS5 module is refused with ENOEXEC, a bare name that is not in the
// title's own import list is not found with ENOENT, and the two modules the
// title already holds answer with the handle the loader gave them. The runner's
// e2-module-load case measures that loader on the console; these answers keep
// the case deterministic when it is run here.
int sceKernelLoadStartModule(const char *path, std::size_t, const void *, std::uint32_t,
                             const void *, int *result)
{
    if (result != nullptr)
        *result = 0;
    if (path == nullptr)
        return static_cast<int>(0x80020002u);
    if (std::strcmp(path, "/app0/sce_module/libc.prx") == 0)
        return 20;
    if (std::strcmp(path, "libSceVideoOut.sprx") == 0)
        return 42;
    return path[0] == '/' ? static_cast<int>(0x80020008u) : static_cast<int>(0x80020002u);
}

int sceKernelStopUnloadModule(int, std::size_t, const void *, std::uint32_t, const void *, int *)
{
    return 0;
}

// Every name the console was asked for came back ESRCH, system modules included.
int sceKernelDlsym(int, const char *, void **address)
{
    if (address != nullptr)
        *address = nullptr;
    return static_cast<int>(0x80020003u);
}
}
