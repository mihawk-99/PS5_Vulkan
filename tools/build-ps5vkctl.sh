#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - build the resident control payload.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds payload/ps5vkctl with the PS5 payload SDK (ps5-payload-dev/sdk):
#
#   build/ps5vkctl/ps5vkctl.elf    the agent tools/ps5_console.py talks to
#
# The SDK is found through PS5_PAYLOAD_SDK, or in ~/ps5-payload-sdk,
# /usr/local/ps5-payload-sdk or /opt/ps5-payload-sdk. Unlike the title builds
# this needs no Mesa tree and no shader compiler: the payload is standalone, and
# it is not linked into any title. Load it on the console once per boot; the loop
# it serves is described in docs/DEPLOYMENT.md.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${PS5_PAYLOAD_SDK:-}"
if [[ -z $sdk ]]; then
    for candidate in "$HOME/ps5-payload-sdk" /usr/local/ps5-payload-sdk /opt/ps5-payload-sdk; do
        if [[ -x "$candidate/bin/prospero-clang" ]]; then
            sdk="$candidate"
            break
        fi
    done
fi
if [[ -z $sdk || ! -x "$sdk/bin/prospero-clang" ]]; then
    echo "build-ps5vkctl: no PS5 payload SDK; set PS5_PAYLOAD_SDK to one, or take" >&2
    echo "                ps5-payload-sdk.zip from https://github.com/ps5-payload-dev/sdk/releases" >&2
    exit 1
fi
export PS5_PAYLOAD_SDK="$sdk"

make -C "$root/payload/ps5vkctl" "$@"
echo "==> [ps5vkctl] $root/build/ps5vkctl/ps5vkctl.elf"
