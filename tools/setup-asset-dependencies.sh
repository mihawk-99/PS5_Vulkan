#!/usr/bin/env bash
# ps5-native-app-boilerplate - Native BC7 encoder bootstrapper.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The launcher backgrounds must ship as 3840x2160 single-surface DX10
# BC7_UNORM DDS files. Microsoft's texconv produces that container but builds
# its command-line tools only under WIN32, so it cannot run here without Wine.
# richgel999/bc7enc_rdo is plain C++ and writes the same DX10 BC7 container, so
# tools/prepare-assets.sh uses it instead and the conversion stays native.
#
# The pinned revision is cloned into the ignored .deps cache and built with the
# host compiler; nothing is installed globally. When Intel's ispc compiler is
# on PATH the encoder is built with SUPPORT_BC7E, which selects the higher
# quality all-mode BC7 encoder; otherwise the bundled bc7enc.cpp encoder runs.
# Install ispc (Arch: `sudo pacman -S ispc`, 275 MiB) to opt in.
#
# Usage: bash tools/setup-asset-dependencies.sh

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
revision=b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
cache="$root/.deps/native/bc7enc_rdo"
binary="$cache/build/bc7enc"
stamp="$cache/.boilerplate-build"

for command in git cmake; do
    command -v "$command" >/dev/null || {
        case $command in
            cmake) hint='sudo pacman -S cmake' ;;
            *) hint="install $command" ;;
        esac
        echo "$command is required to build the BC7 encoder; install it with: $hint" >&2
        exit 2
    }
done

if [[ ! -d $cache/.git ]]; then
    rm -rf -- "$cache"
    git clone --quiet https://github.com/richgel999/bc7enc_rdo "$cache"
fi
git -C "$cache" fetch --quiet origin "$revision" || true
git -C "$cache" checkout --quiet --detach "$revision"

checked_out=$(git -C "$cache" rev-parse HEAD)
[[ $checked_out == "$revision" ]] || {
    echo "bc7enc_rdo is at $checked_out, expected $revision" >&2
    exit 2
}

bc7e=OFF
command -v ispc >/dev/null && bc7e=ON
wanted="$revision bc7e=$bc7e"

if [[ ! -x $binary || ! -f $stamp || $(<"$stamp") != "$wanted" ]]; then
    # The upstream CMake project predates CMake 3.5, and its headers rely on
    # <cstdint> arriving transitively, which modern libstdc++ no longer does.
    # Its ispc objects are also position dependent, which a default PIE link
    # rejects with "relocation R_X86_64_32S against .bss can not be used".
    # None of the three is ours to change upstream, so they ride on the
    # configure line instead of a source patch.
    cmake -S "$cache" -B "$cache/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_CXX_FLAGS="-include cstdint" \
        -DCMAKE_EXE_LINKER_FLAGS="-no-pie" \
        -DSUPPORT_BC7E="$bc7e" >/dev/null
    cmake --build "$cache/build" --parallel >/dev/null
    printf '%s\n' "$wanted" >"$stamp"
fi

[[ -x $binary ]] || {
    echo "the BC7 encoder did not build: $binary" >&2
    exit 2
}

printf 'BC7ENC=%s\n' "$binary"
printf 'BC7 encoder: %s, bc7e %s\n' "$revision" "$([[ $bc7e == ON ]] && echo enabled || echo disabled)"
