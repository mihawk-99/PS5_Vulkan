#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - build every title and check its segments.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds each console title in turn and verifies that every PT_LOAD segment of
# its extracted ELF is page aligned the way the PS5 loader (rtld scan_phdr)
# requires: offset % align == vaddr % align. Fails on any build error, compiler
# warning or misaligned segment.
#
# The titles share build/obj but compile with different definitions, so each
# is a full compile. Run it from the repository root:
#
#   bash tools/build-all-titles.sh

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

titles=(
    "PPSA99999|sce_sys/param.json|"
    "PPSA99997|sce_sys/param-driver-canary.json|AGC_DRIVER_CANARY=1"
    "PPSA99998|sce_sys/param-canary.json|AGC_LINKED_CANARY=1 AGC_COMPLETE_STATE_CANARY=1"
    "PPSA99996|sce_sys/param-submit-canary.json|AGC_LINKED_CANARY=1 AGC_LIVE_SUBMISSION_CANARY=1 AGC_LIVE_SUBMIT_ARMED=1"
    "PPSA99988|sce_sys/param-runner.json|AGC_LINKED_CANARY=1 AGC_TEST_RUNNER=1 AGC_OUTPUT_4K=1 AGC_LIVE_SUBMIT_ARMED=1 AGC_SHADER_COMPILER=1 AGC_VULKAN_DRIVER=1"
)

mkdir -p build/selfcheck
failed=0
total_started=$(date +%s)
for entry in "${titles[@]}"; do
    IFS='|' read -r title param definitions <<< "$entry"
    log="build/$title.build.log"
    started=$(date +%s)
    if ! PARAM_PATH="$param" APP_DEFINITIONS="$definitions" bash tools/build.sh Folder > "$log" 2>&1; then
        tail -n 40 "$log"
        echo "$title: BUILD FAILED (see $log)"
        exit 1
    fi
    seconds=$(( $(date +%s) - started ))
    warnings=$(grep -ciE 'warning|error' "$log" || true)
    digest=$(grep -E '^digest:' "$log" | tail -n 1 | cut -d' ' -f2)
    build/host/ps5-native-tool self --extract --file "dist/$title/eboot.bin" \
        --out "build/selfcheck/$title.elf" > /dev/null
    segments=$(python3 - "build/selfcheck/$title.elf" <<'PY'
import struct
import sys

data = open(sys.argv[1], "rb").read()
phoff = struct.unpack_from("<Q", data, 32)[0]
phentsize, phnum = struct.unpack_from("<HH", data, 54)
loads = misaligned = 0
for index in range(phnum):
    kind, _, offset, vaddr, _, _, _, align = struct.unpack_from("<IIQQQQQQ", data,
                                                               phoff + index * phentsize)
    if kind == 1:
        loads += 1
        misaligned += 0 if align <= 1 or offset % align == vaddr % align else 1
print(f"{loads} {misaligned}")
PY
)
    read -r loads misaligned <<< "$segments"
    echo "$title: built in ${seconds} s, ${warnings} warning/error line(s), digest ${digest:0:16}, ${loads} PT_LOAD, ${misaligned} misaligned"
    if [[ $warnings != 0 || $misaligned != 0 ]]; then
        grep -iE 'warning|error' "$log" | head -n 20
        failed=1
    fi
done
echo "all titles: $(( $(date +%s) - total_started )) s, $([[ $failed == 0 ]] && echo PASS || echo FAIL)"
exit "$failed"
