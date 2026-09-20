/*
 * ps5-native-app-boilerplate - Probe pack manifest read from the console.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mirrors the module and direct-memory probe list the runner reads back.
 */
#pragma once

#include <cstddef>

struct ProbeMemoryTest
{
    std::size_t bytes;
    std::size_t alignment;
};

struct ProbePack
{
    const char *source;
    char modules[3][64];
    std::size_t module_count;
    ProbeMemoryTest memory_tests[4];
    std::size_t memory_test_count;
    bool modules_loaded;
    bool memory_tests_loaded;
};

bool load_probe_pack(ProbePack &pack) noexcept;
