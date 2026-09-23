/*
 * PS5 Vulkan compatibility probe - PC models of the AGC command helpers.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each model writes the PM4 words the console's helper wrote for the same
 * arguments, as recorded in golden/runner; tools/golden.py check-helpers
 * replays every recorded call against these models. Where the captures do
 * not show how an argument is encoded, the model refuses it instead of
 * inventing an encoding: it writes nothing, fails, and names the reason in
 * agc_host_last_error(). Build with tools/build-agc-host.sh.
 */

#include "agc/agc_host.hpp"
#include "agc_abi.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>

namespace
{
const char *g_last_error = "";
std::uint32_t g_flips = 0;

constexpr std::uint32_t kWaitPacketWords = 32;
constexpr std::uint32_t kFlipPacketWords = 64;

// PM4 type-3 header for a packet of count + 1 payload words.
constexpr std::uint32_t pkt3(std::uint32_t opcode, std::uint32_t count) noexcept
{
    return 0xc0000000u | (count << 16) | (opcode << 8);
}

constexpr std::uint32_t low_word(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t high_word(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(value >> 32);
}

std::uint64_t address_of(const void *pointer) noexcept
{
    return reinterpret_cast<std::uintptr_t>(pointer);
}

std::uint32_t *refuse(const char *reason) noexcept
{
    g_last_error = reason;
    return nullptr;
}

// Reserve words at the stream end of command; nullptr when they do not fit.
std::uint32_t *reserve(void *buffer, std::size_t words) noexcept
{
    auto *const command = static_cast<AgcCommandBuffer *>(buffer);
    if (command == nullptr || command->up == nullptr || command->top == nullptr ||
        command->up > command->top || static_cast<std::size_t>(command->top - command->up) < words)
        return nullptr;
    std::uint32_t *const start = command->up;
    command->up += words;
    g_last_error = "";
    return start;
}

std::uint32_t *emit(void *buffer, std::initializer_list<std::uint32_t> words) noexcept
{
    std::uint32_t *const start = reserve(buffer, words.size());
    if (start == nullptr)
        return refuse("command buffer full or invalid");
    std::copy(words.begin(), words.end(), start);
    return start;
}

// Indexed register-table load: table address low and high, 0x80000000 and
// the record count.
std::uint32_t *table_load(void *buffer, std::uint32_t opcode, const void *table,
                          std::uint32_t count) noexcept
{
    const std::uint64_t address = address_of(table);
    return emit(buffer, {pkt3(opcode, 3), low_word(address), high_word(address), 0x80000000u, count});
}
} // namespace

extern "C"
{
const char *agc_host_last_error(void)
{
    return g_last_error;
}

void agc_host_reset(std::uint32_t flips_before)
{
    g_flips = flips_before;
    g_last_error = "";
}

std::uint32_t *sceAgcDcbSetCxRegistersIndirect(void *buffer, const void *table, std::uint32_t count)
{
    return table_load(buffer, 0x9f, table, count);
}

std::uint32_t *sceAgcDcbSetUcRegistersIndirect(void *buffer, const void *table, std::uint32_t count)
{
    return table_load(buffer, 0x64, table, count);
}

std::uint32_t *sceAgcDcbSetShRegistersIndirect(void *buffer, const void *table, std::uint32_t count)
{
    return table_load(buffer, 0x63, table, count);
}

// SET_SH_REG: the first SH register offset, then count values.
std::uint32_t *sceAgcCbSetShRegisterRangeDirect(void *buffer, std::uint32_t offset,
                                                const std::uint32_t *values, std::uint32_t count)
{
    if (values == nullptr || count == 0 || count > 0x3ffe || offset > 0xffff)
        return refuse("sceAgcCbSetShRegisterRangeDirect: needs 1-16382 values at an offset below 0x10000");
    std::uint32_t *const start = reserve(buffer, std::size_t{count} + 2);
    if (start == nullptr)
        return refuse("command buffer full or invalid");
    start[0] = pkt3(0x76, count);
    start[1] = offset;
    std::copy(values, values + count, start + 2);
    return start;
}

// SET_UCONFIG_REG_INDEX of VGT_INDEX_TYPE: 0 for UINT16, 1 for UINT32.
std::uint32_t *sceAgcDcbSetIndexSize(void *buffer, std::uint8_t size, std::uint8_t reserved)
{
    if (size > 1 || reserved != 0)
        return refuse("sceAgcDcbSetIndexSize: expected UINT16 (0) or UINT32 (1), reserved zero");
    return emit(buffer, {pkt3(0x7a, 1), 0x20000243u, 0x00000400u | size});
}

// INDEX_BASE: index buffer address low and high.
std::uint32_t *sceAgcDcbSetIndexBuffer(void *buffer, void *indices)
{
    const std::uint64_t address = address_of(indices);
    return emit(buffer, {pkt3(0x26, 1), low_word(address), high_word(address)});
}

// INDEX_BUFFER_SIZE: the index count.
std::uint32_t *sceAgcDcbSetIndexCount(void *buffer, std::uint32_t count)
{
    return emit(buffer, {pkt3(0x13, 0), count});
}

// DRAW_INDEX_2: buffer size and index count (both the draw's count), address
// low and high, and the draw modifier.
std::uint32_t *sceAgcDcbDrawIndex(void *buffer, std::uint32_t count, void *indices,
                                  std::uint64_t modifier)
{
    const std::uint64_t address = address_of(indices);
    return emit(buffer, {pkt3(0x27, 4), count, low_word(address), high_word(address), count,
                         low_word(modifier)});
}

// DRAW_INDEX_AUTO: vertex count and draw modifier.
std::uint32_t *sceAgcDcbDrawIndexAuto(void *buffer, std::uint32_t count, std::uint64_t modifier)
{
    return emit(buffer, {pkt3(0x2d, 1), count, low_word(modifier)});
}

// NUM_INSTANCES: how many instances the draw that follows runs, one body word.
// The driver programs it immediately before a multi-instance draw and puts it
// back to one after, the order ps5-opengl's runtime uses
// (sceAgcDcbSetNumInstances; docs/M5_PHASE_C.md, C2's instancing probe).
std::uint32_t *sceAgcDcbSetNumInstances(void *buffer, std::uint32_t count)
{
    return emit(buffer, {pkt3(0x2f, 0), count});
}

// RELEASE_MEM. Two forms are captured. ps5-opengl's render-to-texture barrier
// (event 45, control 12) and its completion marker (event 40, control 0x30c;
// golden/b4), which has the GPU write a 32-bit value to an address. In both,
// the first payload word is control << 12 | 0x500 | event. The marker then
// carries 0x20000000, the address's low word, 2, and the value; only address
// high word 2 is captured, so any other is refused.
std::uint32_t *sceAgcCbReleaseMem(void *buffer, std::uint8_t event, std::int16_t control,
                                  std::uint64_t a, std::int8_t b, void *c, std::uint32_t d,
                                  std::uint64_t e, std::uint16_t f, std::uint16_t g, std::int8_t h,
                                  std::int32_t i)
{
    if (event == 45 && control == 12 && a == 1 && b == 0 && c == nullptr && d == 0 && e == 0 &&
        f == 0 && g == 1 && h == 0 && i == 0)
        return emit(buffer, {pkt3(0x49, 6), 0x0000c52du, 0x00010000u, 0, 0, 0, 0, 0});
    const std::uint64_t address = address_of(c);
    if (event == 40 && control == 0x30c && a == 0 && b == 0 && d == 1 && f == 0 && g == 0 &&
        h == 0 && i == 0 && high_word(address) == 2 && e <= UINT32_MAX)
        return emit(buffer, {pkt3(0x49, 6), 0x0030c528u, 0x20000000u, low_word(address), 2u,
                             low_word(e), 0, 0});
    return refuse("sceAgcCbReleaseMem: only the barrier (45, 12, 1, 0, NULL, 0, 0, 0, 1, 0, 0) "
                  "and the completion marker (40, 0x30c, 0, 0, address with high word 2, 1, "
                  "32-bit value, 0, 0, 0, 0) are captured");
}

// The flip: trace markers, WRITE_DATA of the marker, RELEASE_MEM carrying the
// process's flip count, and the 0x10 packet that follows them -- 19 words
// written, with the command buffer advanced the 64 words the console's helper
// reserves. The 45 words after the helper's own are the caller's: runner
// pid 109's probe found the pattern it had put there still in place, and the
// driver's flips hold the frame's own tail because a frame and its flip are
// written at the same address (run pid 113). Zeroing them here would be a
// buffer the console does not have. The VideoOut handle is not encoded. Only
// mode 1, buffers 0-1 and the first 255 flips of a process are captured.
std::uint32_t *sceAgcDcbSetFlip(void *buffer, std::uint32_t video, int buffer_index,
                                std::uint32_t mode, std::int64_t marker)
{
    (void)video;
    if (mode != 1 || buffer_index < 0 || buffer_index > 1 || g_flips >= 0xff)
        return refuse("sceAgcDcbSetFlip: only mode 1, buffers 0-1 and flips 1-255 are captured");
    std::uint32_t *const start = reserve(buffer, kFlipPacketWords);
    if (start == nullptr)
        return refuse("command buffer full or invalid");
    const auto slot = static_cast<std::uint32_t>(buffer_index);
    const auto value = static_cast<std::uint64_t>(marker);
    const std::uint32_t head[] = {
        pkt3(0x79, 2) | 0x04u, 0x00000342u, 0xc7010101u | (slot << 3), 0,
        pkt3(0x37, 4) | 0x04u, 0x06010000u, 0x0000c343u, 0, low_word(value), high_word(value),
        pkt3(0x49, 6), 0x06200504u, 0x42010000u, 0x800040a0u | (slot << 3), 0x0000000cu,
        0x00000001u, 0, 0x08000100u | (g_flips + 1),
        pkt3(0x10, kFlipPacketWords - 20)};
    std::copy(std::begin(head), std::end(head), start);
    ++g_flips;
    return start;
}

std::uint32_t sceAgcDriverGetWaitRenderingPacketSizeInDwords(void)
{
    return kWaitPacketWords;
}

// The wait-until-safe packet: trace markers around the wait (opcode 0x93) for
// the target buffer, then the 0x10 packet that follows them -- 16 words
// written, with the caller's pointer advanced the 32 words the console's
// helper reserves. The 16 words after the helper's own are the caller's, as
// the flip's are (runner pid 109, run pid 113): a frame's wait carries the
// previous submission's words there. The VideoOut handle is not encoded. Only
// buffers 0-1 are captured. Returns 0 on success.
std::uint32_t sceAgcDriverWaitUntilSafeForRendering(std::uint32_t **up, std::uint32_t words,
                                                    std::uint32_t reserved, std::uint32_t video,
                                                    int buffer_index)
{
    (void)video;
    if (up == nullptr || *up == nullptr || words != kWaitPacketWords || reserved != 0 ||
        buffer_index < 0 || buffer_index > 1)
    {
        refuse("sceAgcDriverWaitUntilSafeForRendering: needs a 32-word packet, reserved 0 and buffer 0-1");
        return 1;
    }
    const auto slot = static_cast<std::uint32_t>(buffer_index);
    const std::uint32_t head[] = {
        pkt3(0x79, 1) | 0x04u, 0x00000342u, 0xcb000000u | slot,
        pkt3(0x93, 7), 0x06000113u, 0x800040a0u | (slot << 3), 0x0000000cu, 0, 0, 0xffffffffu,
        0xffffffffu, 0x00000040u,
        pkt3(0x79, 1) | 0x04u, 0x00000342u, 0xcb000020u | slot,
        pkt3(0x10, kWaitPacketWords - 17)};
    std::copy(std::begin(head), std::end(head), *up);
    *up += kWaitPacketWords;
    g_last_error = "";
    return 0;
}
}
