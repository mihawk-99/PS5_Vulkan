/*
 * PS5 Vulkan compatibility probe - native diagnostic application.
 * Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Draws a small CPU-rendered scene and keeps the diagnostic JSONL available
 * for FTP retrieval.
 */

#include "demo_renderer.hpp"
#include "diagnostics.hpp"
#include "web_server.hpp"

#include <array>
#include <cstdio>
#include <span>

namespace
{
std::array<char, 48> banner{};

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;

    canvas.clear(Color::background);
    canvas.rectangle(120, 430, 500, 470, Color::panel);
    canvas.rectangle(710, 430, 500, 470, Color::panel);
    canvas.rectangle(1300, 430, 500, 470, Color::panel);
    canvas.rectangle(120, 375, 1680, 8, Color::white);

    canvas.text(120, 90, "PS5 DIAGNOSTIC HARNESS", 10, Color::white);
    canvas.text(120, 245, "SYSTEM + AGC PROBES", 6, Color::white);
    canvas.text(120, 315, banner.data(), 5, Color::cyan);

    canvas.circle(370, 665, 130, Color::cyan);
    canvas.rectangle(840, 535, 240, 240, Color::yellow);
    canvas.triangle(1550, 520, 170, 285, Color::magenta);

    canvas.text(250, 830, "CIRCLE", 5, Color::white);
    canvas.text(870, 830, "SQUARE", 5, Color::white);
    canvas.text(1420, 830, "TRIANGLE", 5, Color::white);
}
} // namespace

int main()
{
#if defined(AGC_DRIVER_CANARY)
    const DiagnosticSummary summary =
        run_linked_driver_canary("/download0/ps5-vulkan-driver-canary.jsonl");
#elif defined(AGC_LINKED_CANARY)
    const DiagnosticSummary summary =
        run_linked_agc_canary("/download0/ps5-vulkan-linked-canary.jsonl");
#else
    const DiagnosticSummary summary = run_diagnostics("/download0/ps5-vulkan-probe.jsonl");
#endif
    (void)summary;
    // A queued battery can ask the probe to leave the console once it is done
    // (the queue's "exit", diagnostics.hpp): the work is in the log either way,
    // and the next launch has to load the eboot the next deploy writes.
    if (runner_exit_requested())
        return 0;
    static DiagnosticWebServer web_server;
    web_server.start(8080);
    char local_ip[16]{};
    if (diagnostics_local_ip(local_ip, sizeof(local_ip)) && web_server.ready())
#if defined(AGC_DRIVER_CANARY)
        std::snprintf(banner.data(), banner.size(), "LINKED DRIVER CANARY");
#elif defined(AGC_TEST_RUNNER)
        std::snprintf(banner.data(), banner.size(), "VULKAN TEST RUNNER");
#elif defined(AGC_LINKED_CANARY)
        std::snprintf(banner.data(), banner.size(), "LINKED AGC CANARY");
#else
        std::snprintf(banner.data(), banner.size(), "WEB: http://%s:%u/log", local_ip,
                      static_cast<unsigned>(web_server.port()));
#endif
    else
        ps5::demo::read_asset_text("/app0/assets/banner.txt", std::span{banner},
                                   "WEB IP UNAVAILABLE");
    ps5::demo::run(draw_scene, banner.data(), []() noexcept { web_server.poll(); });
}
