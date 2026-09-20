#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - fetch the pinned Mesa source release.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase B1 (docs/M5_PHASE_B.md). The opengnm-psbc tree carries
# Mesa's Vulkan runtime with its core headers (vk_device.h, vk_instance.h,
# vk_queue.h and more) reduced to placeholders, so the runtime cannot build
# from it. This downloads the Mesa release the ps5-opengl SDK pins in its
# dependencies.json (same version, URL and SHA-256 as its
# tools/fetch-sources.py), verifies it, and extracts it into the ignored cache:
#
#   .deps/native/mesa/mesa-<version>.tar.xz   (kept; verified on every run)
#   .deps/native/mesa/mesa-<version>/         (MESA_SOURCE_PARENT overrides)
#
# A download is only promoted after its checksum matches.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
read -r version url sha256 < <(python3 -c '
import json, sys
mesa = json.load(open(sys.argv[1]))["mesa"]
print(mesa["version"], mesa["url"], mesa["sha256"])' "$sdk/dependencies.json")

cache="$root/.deps/native/mesa"
archive="$cache/mesa-$version.tar.xz"
mkdir -p "$cache"
if [[ ! -f $archive ]]; then
    echo "downloading $url"
    python3 -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' \
        "$url" "$archive.download"
    printf '%s  %s\n' "$sha256" "$archive.download" | sha256sum --check --status ||
        { echo "Mesa download checksum mismatch; not promoted: $archive.download" >&2; exit 1; }
    mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$sha256" "$archive" | sha256sum --check --status ||
    { echo "existing Mesa archive checksum mismatch; file preserved: $archive" >&2; exit 1; }

parent=${MESA_SOURCE_PARENT:-$root/.deps/native/mesa}
source_dir="$parent/mesa-$version"
if [[ ! -f $source_dir/VERSION ]]; then
    mkdir -p "$parent"
    tar -xJf "$archive" -C "$parent"
fi
[[ $(tr -d '[:space:]' < "$source_dir/VERSION") == "$version" ]] ||
    { echo "unexpected Mesa source version in $source_dir" >&2; exit 1; }
echo "Mesa $version: $archive ($(stat -c %s "$archive") bytes, sha256 $sha256)"
echo "sources: $source_dir"
