/*
 * ps5-native-app-boilerplate - Native PS5 dynamic-module writer interface.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Declares conversion of an ordinary LLVM-linked PIE into the PS5 application
 * ELF layout consumed by the FSELF wrapper.
 */

#pragma once

#include "elf_object.hpp"

#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <vector>

namespace ps5::module
{

struct Options
{
    std::string entry = "_start";
    std::string file_name = "eboot.elf";
    std::uint32_t module_sdk = 0x02000009;
    std::uint32_t companion_sdk = 0x08050001;
    std::vector<std::string> version_components;
    // Experimental: the flexible memory the process asks the kernel for, through the memory parameter block's
    // sceKernelFlexibleMemorySize field (the PS4 layout: a pointer at +0x10 to the size). 0 leaves the field empty,
    // as before, and the output unchanged.
    std::uint64_t flexible_memory = 0;
    // Experimental: also store each process-parameter pointer in the file, as this base plus its address, for a
    // kernel that reads them before the image's relocations are applied. Without it they are filled by relocation
    // only, as before.
    std::optional<std::uint64_t> parameter_pointer_base;
};

[[nodiscard]] elf::Bytes write_executable(const elf::Image &image, std::span<const elf::Stub> stubs,
                                          const Options &options = {});

} // namespace ps5::module
