#!/usr/bin/env bash
# ps5-native-app-boilerplate - Native dependency bootstrapper.
# Copyright (C) 2026 BlackBearReloaded
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs the PS5 payload SDK (my fork, pinned below) and static zlib into the
# ignored cache.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.deps/native"
sdk="$cache/ps5-payload-sdk"
zlib_directory="$cache/zlib"
zlib_root="$zlib_directory/root"
zlib_version="1.3.2"
zlib_source="$zlib_directory/zlib-$zlib_version"
zlib_archive="$zlib_directory/zlib-$zlib_version.tar.gz"
zlib_stamp="$zlib_root/.source-version"
# The payload SDK is my fork, ../PS5_PayloadSDK: the upstream v0.42 release with
# the PS5 platform layer (libps5platform.a, include/ps5platform) and the
# console's machine-context layout installed over it (platform/README.md
# there). The pinned revision is exported with git archive, so a build never
# depends on the fork's working tree, and the SDK directory records it.
sdk_fork="${PS5_PAYLOAD_SDK_FORK:-$root/../PS5_PayloadSDK}"
sdk_revision=26fc54c3c3a3736411bfe0418527a473b4cdfca6
zlib_url="https://zlib.net/fossils/zlib-$zlib_version.tar.gz"
zlib_hash="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
skip_sdk=false

if [[ ${1:-} == "--skip-sdk" ]]; then
    skip_sdk=true
elif [[ $# -ne 0 ]]; then
    echo "usage: tools/setup-native-dependencies.sh [--skip-sdk]" >&2
    exit 2
fi

for command in git wget unzip sha256sum tar make; do
    command -v "$command" >/dev/null || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
compiler=$(command -v clang || command -v clang-18 || true)
archiver=$(command -v llvm-ar || command -v llvm-ar-18 || command -v ar || true)
ranlib=$(command -v llvm-ranlib || command -v llvm-ranlib-18 || command -v ranlib || true)
[[ -n $compiler && -n $archiver && -n $ranlib ]] || {
    echo "clang, an archive tool, and ranlib are required to build zlib" >&2
    exit 2
}

mkdir -p "$cache"

# The SDK ships its drivers and wrappers as scripts in bin/. A cache restored
# from an archive or copied from a filesystem without Unix permissions can lose
# the executable bit, and the build runs those scripts directly. Restore it
# rather than re-fetching.
restore_sdk_permissions() {
    [[ -d $sdk/bin ]] || return 0
    find "$sdk/bin" -maxdepth 1 -type f -exec chmod +x {} +
}

restore_sdk_permissions
if ! $skip_sdk && [[ ! -f $sdk/.ps5-sdk-revision || $(<"$sdk/.ps5-sdk-revision") != "$sdk_revision" ]]; then
    git -C "$sdk_fork" cat-file -e "$sdk_revision^{commit}" 2>/dev/null || {
        echo "the payload SDK fork at $sdk_fork does not have $sdk_revision" >&2
        exit 2
    }
    sdk_tree=$(mktemp -d)
    git -C "$sdk_fork" archive "$sdk_revision" | tar -x -C "$sdk_tree"
    bash "$sdk_tree/platform/tools/setup-sdk.sh" "$sdk" "$sdk_revision" "$cache" >&2
    rm -rf -- "$sdk_tree"
    restore_sdk_permissions
fi
if ! $skip_sdk && [[ ! -x "$sdk/bin/prospero-lld" || ! -d "$sdk/target/include" ]]; then
    echo "the pinned PS5 payload SDK is incomplete" >&2
    exit 2
fi

zlib_library=$(find "$zlib_root" -type f -name libz.a -print -quit 2>/dev/null || true)
if [[ -z $zlib_library || ! -f $zlib_root/usr/include/zlib.h ||
    ! -f $zlib_stamp || $(<"$zlib_stamp") != "$zlib_version" ]]; then
    echo "==> [deps] Building pinned zlib $zlib_version from source" >&2
    mkdir -p "$zlib_directory"
    if [[ -f $zlib_archive ]] &&
        ! printf '%s  %s\n' "$zlib_hash" "$zlib_archive" |
            sha256sum --check --strict >/dev/null 2>&1; then
        rm -f -- "$zlib_archive"
    fi
    if [[ ! -f $zlib_archive ]]; then
        wget -q "$zlib_url" -O "$zlib_archive.download"
        mv "$zlib_archive.download" "$zlib_archive"
    fi
    printf '%s  %s\n' "$zlib_hash" "$zlib_archive" | sha256sum --check --strict >/dev/null
    rm -rf -- "$zlib_source" "$zlib_root"
    tar -xzf "$zlib_archive" -C "$zlib_directory"
    mkdir -p "$zlib_root"
    jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || printf '2')}
    (
        cd "$zlib_source"
        CC="$compiler" AR="$archiver" RANLIB="$ranlib" ./configure --static --prefix=/usr
        make -j "$jobs" CC="$compiler" AR="$archiver" RANLIB="$ranlib"
        make DESTDIR="$zlib_root" install
    ) >"$zlib_directory/build.log"
    printf '%s\n' "$zlib_version" >"$zlib_stamp"
    zlib_library=$(find "$zlib_root" -type f -name libz.a -print -quit 2>/dev/null || true)
fi
if [[ -z $zlib_library ]]; then
    echo "the pinned native zlib archive was not found after compilation" >&2
    exit 2
fi

printf 'SDK_ROOT=%s\nZLIB_INCLUDE=%s\nZLIB_ARCHIVE=%s\n' \
    "$sdk" "$zlib_root/usr/include" "$zlib_library"
