/*
 * PS5 Vulkan compatibility probe - PS5 system calls on Linux.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>

// Host directories that /app0 and /download0 map to, and an optional file
// that stands in for /app0/jobs/queue.txt.
bool ps5_host_configure(const char *app0, const char *download0, const char *queue) noexcept;

// Load a replay file written by tools/golden.py rebuild. frame receives the
// number of the first frame it replays within its console run, and
// flips_before how many flips the console process made before that frame.
// regions says whether the replay's captured regions are handed to
// allocations: true for the runner's AGC-level cases, whose frames have to land
// on the addresses the capture names, and false for a case that goes through
// the Vulkan driver, which maps its own memory exactly as it does on the
// console -- the replay then supplies the register defaults and the pipeline
// stage images alone (host/runner/runner_host_main.cpp, --memory).
bool ps5_host_load_replay(const char *path, std::uint32_t &frame,
                          std::uint32_t &flips_before, bool regions = true) noexcept;
