/*
 * PS5 AGC/system diagnostic harness.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>

struct DiagnosticSummary
{
    int agc_init_result;
    int direct_memory_result;
    int command_encoding_result;
};

DiagnosticSummary run_diagnostics(const char *log_path) noexcept;

// Whether the job queue the runner last loaded asked the process to leave the
// console when its batch ended (the queue's "exit" directive). A headless batch
// has nothing left to draw, and the next launch has to load the eboot that
// batch's deploy wrote, so main returns instead of entering the render loop.
bool runner_exit_requested() noexcept;

// Minimal linked-import experiment. It intentionally performs no shader,
// command-buffer, or GPU submission work.
DiagnosticSummary run_linked_agc_canary(const char *log_path) noexcept;
DiagnosticSummary run_linked_driver_canary(const char *log_path) noexcept;

std::size_t diagnostics_log_size(const char *log_path) noexcept;

const char *diagnostics_log_data() noexcept;
std::size_t diagnostics_log_data_size() noexcept;

bool diagnostics_local_ip(char *destination, std::size_t capacity) noexcept;
