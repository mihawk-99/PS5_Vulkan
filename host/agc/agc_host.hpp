/*
 * PS5 Vulkan compatibility probe - controls of the PC AGC helper models.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

extern "C"
{
// Why the last model call wrote nothing; empty after a success.
const char *agc_host_last_error(void);
// Start a process: the next flip is flip flips_before + 1.
void agc_host_reset(std::uint32_t flips_before);
}
