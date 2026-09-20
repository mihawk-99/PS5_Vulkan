/*
 * ps5-native-app-boilerplate - PS5 diagnostic HTTP endpoint.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serves the probe status page on the console's local network address.
 */
#pragma once

class DiagnosticWebServer final
{
  public:
    bool start(unsigned short port) noexcept;
    void poll() noexcept;
    bool ready() const noexcept
    {
        return listener_ >= 0;
    }
    unsigned short port() const noexcept
    {
        return port_;
    }

  private:
    int listener_ = -1;
    unsigned short port_ = 0;
};
