/*
 * PS5 Vulkan compatibility probe - the test runner built for the PC.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs the console runner's own code (src/diagnostics.cpp, AGC_TEST_RUNNER)
 * on Linux, against host/ps5/ps5_host.cpp and the AGC helper models,
 * replaying the inputs one golden capture recorded. The [PS5VK] records it
 * writes to stdout have the console's klog format; tools/golden.py rebuild
 * runs it and compares its captures with the golden files.
 */

#include "agc/agc_host.hpp"
#include "diagnostics.hpp"
#include "ps5/ps5_host.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv)
{
    const char *replay = nullptr;
    const char *app0 = nullptr;
    const char *download0 = nullptr;
    const char *queue = nullptr;
    bool regions = true;
    bool driver_cases = false;
    bool usage = argc % 2 == 0;
    for (int index = 1; !usage && index + 1 < argc; index += 2)
    {
        const char *const option = argv[index];
        const char *const value = argv[index + 1];
        if (std::strcmp(option, "--replay") == 0)
            replay = value;
        else if (std::strcmp(option, "--app0") == 0)
            app0 = value;
        else if (std::strcmp(option, "--download0") == 0)
            download0 = value;
        else if (std::strcmp(option, "--queue") == 0)
            queue = value;
        // Whether the replay's captured regions are handed to allocations.
        // "replay" is what an AGC-level frame needs, because it has to land on
        // the addresses the capture names; "free" is for a queue whose cases go
        // through the Vulkan driver, which maps its own memory as it does on
        // the console (host/ps5/ps5_host.hpp).
        else if (std::strcmp(option, "--memory") == 0 && std::strcmp(value, "free") == 0)
            regions = false;
        else if (std::strcmp(option, "--memory") == 0 && std::strcmp(value, "replay") == 0)
            regions = true;
        // Whether every queued case goes through the Vulkan driver. Such a case
        // brings its own device and shaders, so the runner skips the AGC-level
        // package staging -- whose 64 KiB workspace would otherwise take the
        // captured pipeline stage the driver's own stage allocation needs
        // (src/diagnostics.cpp, docs/M5_PHASE_B.md).
        else if (std::strcmp(option, "--cases") == 0 && std::strcmp(value, "driver") == 0)
            driver_cases = true;
        else if (std::strcmp(option, "--cases") == 0 && std::strcmp(value, "agc") == 0)
            driver_cases = false;
        else
            usage = true;
    }
    if (usage || replay == nullptr || app0 == nullptr || download0 == nullptr)
    {
        std::fprintf(stderr,
                     "usage: %s --replay FILE --app0 DIR --download0 DIR [--queue FILE] "
                     "[--memory free|replay] [--cases driver|agc]\n",
                     argv[0]);
        return 2;
    }
#ifdef PS5VK_HOST_BUILD
    if (driver_cases)
        setenv("PS5VK_DRIVER_CASES", "1", 1);
#endif
    std::uint32_t frame = 0;
    std::uint32_t flips_before = 0;
    if (!ps5_host_configure(app0, download0, queue) ||
        !ps5_host_load_replay(replay, frame, flips_before, regions))
        return 2;
    // The flip encodes how many flips the console process made before it.
    agc_host_reset(flips_before);
    (void)run_linked_agc_canary("/download0/ps5-vulkan-linked-canary.jsonl");
    std::fflush(stdout);
    return 0;
}
