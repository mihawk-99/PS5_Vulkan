/*
 * ps5-native-app-boilerplate - Probe pack loader.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reads the module and direct-memory probe list from /app0 or /download0.
 */
#include "probe_pack.hpp"

#include <cstdio>
#include <cstring>

extern "C"
{
    int sceKernelOpen(const char *, int, unsigned short);
    int sceKernelClose(int);
    long long sceKernelRead(int, void *, std::size_t);
}

namespace
{
constexpr const char *kPackagedRoot = "/app0/probes/";
constexpr const char *kRuntimeRoot = "/download0/probes/";
constexpr std::size_t kFileLimit = 8192;

void defaults(ProbePack &pack) noexcept
{
    std::strncpy(pack.modules[0], "libSceAgc.sprx", sizeof(pack.modules[0]) - 1);
    std::strncpy(pack.modules[1], "libSceAgcDriver.sprx", sizeof(pack.modules[1]) - 1);
    std::strncpy(pack.modules[2], "libSceVideoOut.sprx", sizeof(pack.modules[2]) - 1);
    pack.module_count = 3;
    // The proven direct-memory path requires allocations in 64 KiB units.
    pack.memory_tests[0] = {0x10000, 0x4000};
    pack.memory_test_count = 1;
}

bool read_file(const char *path, char *buffer, std::size_t capacity) noexcept
{
    const int descriptor = sceKernelOpen(path, 0, 0);
    if (descriptor < 0)
        return false;
    std::size_t total = 0;
    while (total < capacity - 1)
    {
        const long long count = sceKernelRead(descriptor, buffer + total, capacity - 1 - total);
        if (count <= 0)
            break;
        total += static_cast<std::size_t>(count);
    }
    buffer[total] = '\0';
    sceKernelClose(descriptor);
    return total != 0;
}

bool string_at(const char *text, const char *key, unsigned index, char *destination,
               std::size_t capacity) noexcept
{
    const char *cursor = text;
    while ((cursor = std::strstr(cursor, key)) != nullptr)
    {
        if (index == 0)
            break;
        --index;
        ++cursor;
    }
    if (!cursor)
        return false;
    const char *end = std::strchr(cursor, '"');
    if (!end)
        return false;
    std::size_t length = static_cast<std::size_t>(end - cursor);
    if (length >= capacity)
        length = capacity - 1;
    std::memcpy(destination, cursor, length);
    destination[length] = '\0';
    return length != 0;
}

bool number_at(const char *text, const char *key, const char *from, std::size_t &value) noexcept
{
    const char *cursor = std::strstr(from ? from : text, key);
    if (!cursor || !(cursor = std::strchr(cursor, ':')))
        return false;
    unsigned long parsed = 0;
    if (std::sscanf(cursor + 1, "%lu", &parsed) != 1 || parsed == 0)
        return false;
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool load_modules(ProbePack &pack, const char *root) noexcept
{
    char text[kFileLimit]{};
    char path[128]{};
    std::snprintf(path, sizeof(path), "%smodules.json", root);
    if (!read_file(path, text, sizeof(text)))
        return false;
    std::size_t count = 0;
    for (unsigned index = 0; index < 3; ++index)
    {
        if (!string_at(text, "libSce", index, pack.modules[count], sizeof(pack.modules[count])))
            break;
        ++count;
    }
    if (count == 0)
        return false;
    pack.module_count = count;
    pack.modules_loaded = true;
    return true;
}

bool load_memory_tests(ProbePack &pack, const char *root) noexcept
{
    char text[kFileLimit]{};
    char path[128]{};
    std::snprintf(path, sizeof(path), "%smemory-tests.json", root);
    if (!read_file(path, text, sizeof(text)))
        return false;
    const char *cursor = text;
    std::size_t count = 0;
    while (count < 4)
    {
        std::size_t bytes = 0;
        std::size_t alignment = 0;
        if (!number_at(text, "\"bytes\"", cursor, bytes) ||
            !number_at(text, "\"alignment\"", cursor, alignment))
            break;
        pack.memory_tests[count++] = {bytes, alignment};
        cursor = std::strstr(cursor, "\"alignment\"");
        if (!cursor)
            break;
        ++cursor;
    }
    if (count == 0)
        return false;
    pack.memory_test_count = count;
    pack.memory_tests_loaded = true;
    return true;
}
} // namespace

bool load_probe_pack(ProbePack &pack) noexcept
{
    pack = {};
    defaults(pack);
    const bool packaged_modules = load_modules(pack, kPackagedRoot);
    const bool packaged_memory = load_memory_tests(pack, kPackagedRoot);
    const bool runtime_modules = load_modules(pack, kRuntimeRoot);
    const bool runtime_memory = load_memory_tests(pack, kRuntimeRoot);
    if (runtime_modules || runtime_memory)
    {
        pack.source = kRuntimeRoot;
        return true;
    }
    if (packaged_modules || packaged_memory)
    {
        pack.source = kPackagedRoot;
        return true;
    }
    pack.source = "built-in defaults";
    return false;
}
