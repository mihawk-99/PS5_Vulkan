#!/usr/bin/env bash
# ps5-native-app-boilerplate - Portable host prerequisite checker.
# Copyright (C) 2026 BlackBearReloaded
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Reports required and optional host tools without downloading or changing them.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
failed=0

required() {
    local command=$1
    local purpose=$2
    if command -v "$command" >/dev/null; then
        printf '[OK] %-18s %s\n' "$command" "$purpose"
    else
        printf '[MISSING] %-13s %s\n' "$command" "$purpose"
        failed=1
    fi
}

optional() {
    local command=$1
    local purpose=$2
    if command -v "$command" >/dev/null; then
        printf '[OK] %-18s %s\n' "$command" "$purpose"
    else
        printf '[OPTIONAL] %-12s %s\n' "$command" "$purpose"
    fi
}

required_one_of() {
    local label=$1
    local purpose=$2
    shift 2
    local candidate
    for candidate in "$@"; do
        if command -v "$candidate" >/dev/null; then
            printf '[OK] %-18s %s (%s)\n' "$label" "$purpose" "$candidate"
            return
        fi
    done
    printf '[MISSING] %-13s %s\n' "$label" "$purpose"
    failed=1
}

python_modules() {
    local modules=$1
    local purpose=$2
    if python3 -c "import $modules" >/dev/null 2>&1; then
        printf '[OK] %-18s %s\n' "$modules" "$purpose"
    else
        printf '[OPTIONAL] %-12s %s\n' "$modules" "$purpose"
    fi
}

printf 'Host: %s\n' "$(uname -srm)"
if [[ -r /etc/os-release ]]; then
    printf 'Distribution: %s\n' "$(
        # shellcheck disable=SC1091
        . /etc/os-release && printf '%s' "${PRETTY_NAME:-${ID:-unknown}}"
    )"
fi

required bash 'build orchestration'
required make 'primary build entry point'
required python3 'metadata, tests, and packaging helpers'
required_one_of clang 'native host and target compilation' clang clang-18
required_one_of clang++ 'C++ host and target compilation' clang++ clang++-18
required_one_of clang-format 'format validation' clang-format clang-format-18
required_one_of clang-tidy 'static analysis' clang-tidy clang-tidy-18
required_one_of llvm-ar 'static archive creation' llvm-ar llvm-ar-18 ar
required_one_of llvm-ranlib 'static archive indexing' llvm-ranlib llvm-ranlib-18 ranlib
required wget 'pinned dependency downloads'
required unzip 'public SDK extraction'
required tar 'zlib source extraction'
required sha256sum 'download and runtime verification'
required git 'source and optional dependency checkout'
optional curl 'FTP deployment'
optional pkg-config 'PacBrew dependency resolution'
optional dotnet 'optional .ffpkg generation'
optional ffmpeg 'presentation-audio preparation'
optional rsync 'PS5 compiler and Vulkan runtime work copies'
optional ispc 'optional all-mode BC7 encoder for presentation assets'
optional cmake 'building the pinned BC7 encoder (make assets-deps)'
optional glslang 'probe shader compilation (GLSL to SPIR-V)'
python_modules 'mako, markupsafe' 'Mesa Vulkan generators (install python-mako)'

if [[ -f $root/sce_sys/param.json ]]; then
    printf '[OK] %-18s %s\n' 'param.json' 'application metadata found'
else
    printf '[MISSING] %-13s %s\n' 'param.json' 'sce_sys/param.json was not found'
    failed=1
fi

if [[ -x $root/.deps/native/ps5-payload-sdk/bin/prospero-lld ]]; then
    printf '[CACHED] %-14s %s\n' 'PS5 SDK' '.deps/native/ps5-payload-sdk'
else
    printf '[GENERATED] %-11s %s\n' 'PS5 SDK' 'make will fetch the pinned public SDK'
fi
if [[ -f $root/runtime/libc.prx ]]; then
    printf '[CACHED] %-14s %s\n' 'libc.prx' 'generated runtime found'
else
    printf '[GENERATED] %-11s %s\n' 'libc.prx' 'make will generate and verify it'
fi

if ((failed)); then
    echo 'One or more required tools are missing. See docs/GETTING_STARTED.md.' >&2
    exit 1
fi
echo 'Required native build prerequisites are available.'
