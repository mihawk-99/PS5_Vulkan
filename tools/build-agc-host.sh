#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - build the PC models of the AGC helpers.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiles host/agc/agc_host.cpp against src/agc_abi.hpp into
# build/host/libagc_host.so, which tools/golden.py check-helpers loads. Set
# HOST_CXX to choose the compiler.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=${HOST_CXX:-}
if [[ -z $cxx ]]; then
    for candidate in clang++ clang++-18 g++; do
        if command -v "$candidate" >/dev/null; then
            cxx=$candidate
            break
        fi
    done
fi
[[ -n $cxx ]] || { echo "no host C++ compiler found; set HOST_CXX" >&2; exit 2; }

output="$root/build/host/libagc_host.so"
mkdir -p "$(dirname -- "$output")"
"$cxx" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -fPIC -shared \
    -I "$root/src" -I "$root/host" "$root/host/agc/agc_host.cpp" -o "$output"
echo "built $output with $cxx"
