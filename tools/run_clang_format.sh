#!/usr/bin/env bash
# ps5-native-app-boilerplate - Clang formatter driver.
# Copyright (C) 2026 BlackBearReloaded
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Applies the formatting policy shared with the CPython PS5 project.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
formatter=${CLANG_FORMAT:-}
if [[ -z $formatter ]]; then
    formatter=$(command -v clang-format || command -v clang-format-18 || true)
fi
[[ -n $formatter ]] || { echo "clang-format is required" >&2; exit 2; }

mapfile -d '' sources < <(find "$root/src" "$root/tooling/native" "$root/tests" -type f \
    \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    -print0)

# The trees this policy does not cover keep the style they were derived from, and
# each carries a .clang-format with DisableFormat: true so that no tool -- including
# a hand-run `clang-format -i`, which is what a round was lost to once -- rewrites
# them (docs/M5_PHASE_C.md, CTS round 8). The marker is the mechanism; this list is
# what keeps the mechanism honest, because a tree without it is one command away
# from a whole-file rewrite.
unformatted=(
    driver
    host
    vendor
    payload
    tooling/psbc
    tooling/vulkan-runtime
)
for tree in "${unformatted[@]}"; do
    marker="$root/$tree/.clang-format"
    [[ -f $marker ]] ||
        { echo "missing $marker: $tree is outside the formatting policy and must say so" >&2; exit 2; }
    grep -q '^DisableFormat: true$' "$marker" ||
        { echo "$marker does not set DisableFormat: true" >&2; exit 2; }
done

if [[ ${1:-} == --check ]]; then
    "$formatter" --dry-run --Werror "${sources[@]}"
    echo "format policy: ${#sources[@]} files under src, tooling/native and tests; "\
"${#unformatted[@]} trees disabled by marker"
elif [[ $# -eq 0 ]]; then
    "$formatter" -i "${sources[@]}"
else
    echo "usage: tools/run_clang_format.sh [--check]" >&2
    exit 2
fi
