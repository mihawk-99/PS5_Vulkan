/*
 * PS5 Vulkan compatibility probe - AGC command-buffer ABI.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The AGC types and command helpers that build a frame's command stream. The
 * console titles link the real helpers (libSceAgc and libSceAgcDriver); the
 * PC models in host/agc/agc_host.cpp implement the same declarations.
 */

#pragma once

#include <cstdint>

// One register-table record: AGC register offset and value.
struct AgcRegister
{
    std::uint16_t offset;
    std::uint16_t padding;
    std::uint32_t value;
};

// A command buffer the Dcb/Cb helpers append to: packets are written at up,
// and top bounds the stream.
struct AgcCommandBuffer
{
    std::uint32_t *bottom;
    std::uint32_t *top;
    std::uint32_t *up;
    std::uint32_t *down;
    std::uintptr_t callback;
    void *user_data;
    std::uint32_t reserved_dwords;
    std::uint32_t padding;
};

extern "C"
{
    // Each Dcb/Cb helper takes an AgcCommandBuffer, appends its packet at up and
    // returns the start of that packet; up is then the end of the stream.
    std::uint32_t *sceAgcDcbSetCxRegistersIndirect(void *, const void *, std::uint32_t);
    std::uint32_t *sceAgcDcbSetUcRegistersIndirect(void *, const void *, std::uint32_t);
    std::uint32_t *sceAgcDcbSetShRegistersIndirect(void *, const void *, std::uint32_t);
    std::uint32_t *sceAgcCbSetShRegisterRangeDirect(void *, std::uint32_t, const std::uint32_t *,
                                                    std::uint32_t);
    std::uint32_t *sceAgcDcbSetIndexSize(void *, std::uint8_t, std::uint8_t);
    std::uint32_t *sceAgcDcbSetIndexBuffer(void *, void *);
    std::uint32_t *sceAgcDcbSetIndexCount(void *, std::uint32_t);
    std::uint32_t *sceAgcDcbDrawIndex(void *, std::uint32_t, void *, std::uint64_t);
    std::uint32_t *sceAgcDcbDrawIndexAuto(void *, std::uint32_t, std::uint64_t);
    std::uint32_t *sceAgcCbReleaseMem(void *, std::uint8_t, std::int16_t, std::uint64_t,
                                      std::int8_t, void *, std::uint32_t, std::uint64_t,
                                      std::uint16_t, std::uint16_t, std::int8_t, std::int32_t);
    std::uint32_t *sceAgcDcbSetFlip(void *, std::uint32_t, int, std::uint32_t, std::int64_t);
    // The wait-until-safe packet is written through a pointer to the stream end,
    // which it advances by the packet size.
    std::uint32_t sceAgcDriverGetWaitRenderingPacketSizeInDwords(void);
    std::uint32_t sceAgcDriverWaitUntilSafeForRendering(std::uint32_t **, std::uint32_t,
                                                        std::uint32_t, std::uint32_t, int);
}
