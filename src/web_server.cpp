/*
 * Minimal read-only HTTP server for retrieving diagnostics over the LAN.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "web_server.hpp"

#include "diagnostics.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
void send_all(int client, const char *data, std::size_t size) noexcept
{
    while (size != 0)
    {
        const ssize_t sent = send(client, data, size, 0);
        if (sent <= 0)
            return;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
}

void reply(int client, const char *status, const char *type, const char *body,
           std::size_t size) noexcept
{
    char header[256]{};
    const int length = std::snprintf(header, sizeof(header),
                                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                                     "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                                     status, type, size);
    if (length > 0)
        send_all(client, header, static_cast<std::size_t>(length));
    send_all(client, body, size);
}
} // namespace

bool DiagnosticWebServer::start(unsigned short port) noexcept
{
    constexpr unsigned short kFallbackCount = 20;
    for (unsigned short candidate = port;
         candidate < static_cast<unsigned short>(port + kFallbackCount); ++candidate)
    {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            continue;

        int reuse = 1;
        (void)setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_port = htons(candidate);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listener_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 &&
            listen(listener_, 2) == 0)
        {
            port_ = candidate;
            break;
        }

        close(listener_);
        listener_ = -1;
    }
    if (listener_ < 0)
        return false;

    const int flags = fcntl(listener_, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(listener_, F_SETFL, flags | O_NONBLOCK);
    return true;
}

void DiagnosticWebServer::poll() noexcept
{
    if (listener_ < 0)
        return;

    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    const int client = accept(listener_, reinterpret_cast<sockaddr *>(&peer), &length);
    if (client < 0)
        return;

    char request[512]{};
    (void)recv(client, request, sizeof(request) - 1, 0);
    // The literal prefixes are nine and eleven bytes respectively, including
    // the trailing space before the HTTP version.
    const bool wants_log = std::strncmp(request, "GET /log ", 9) == 0;
    const bool wants_health = std::strncmp(request, "GET /health ", 11) == 0;
    const char *health = "ok\n";
    if (wants_log)
        reply(client, "200 OK", "application/x-ndjson; charset=utf-8", diagnostics_log_data(),
              diagnostics_log_data_size());
    else if (wants_health)
        reply(client, "200 OK", "text/plain; charset=utf-8", health, 3);
    else
    {
        const char *index = "PS5 Vulkan probe\n/log - diagnostics JSONL\n/health - status\n";
        reply(client, "200 OK", "text/plain; charset=utf-8", index, std::strlen(index));
    }
    close(client);
}
