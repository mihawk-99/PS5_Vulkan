#!/usr/bin/env bash
# PS5 Vulkan - run a VK-GL-CTS group against this driver's host ICD.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Phase E1 (docs/CTS.md). The console payload is still to come; this runs the
# pinned CTS against the *host* build of the same driver through the Khronos
# loader, which is the same path tools/check-driver.sh already uses for its
# loader tests. The host driver is a model -- it records and replays AGC work
# rather than driving a console -- so the groups worth running here are the ones
# that read what the device *reports* and how it handles the API, not the ones
# that need rendering. That is where the selection is decided anyway.
#
# The CTS is not compiled here by accident: the pinned tag's Amber dependency
# does not build under this host's GCC 16 (a missing <cstdint> in 2024 code), so
# the build force-includes the headers libstdc++ no longer provides
# transitively. That is a compiler flag, not a source change: the pin stays
# exact, which is what makes a run comparable with the reference project's.
#
#   bash tools/run-cts-host.sh 'dEQP-VK.info.*'          run a group
#   bash tools/run-cts-host.sh --list                    export the case list
#   bash tools/run-cts-host.sh --build                   build or rebuild deqp-vk
#
# Runs land in build/cts-host/runs/<group>/ with stdout.log, results.qpa and
# summary.json; the script prints the totals and every failing case, and exits
# non-zero when a case failed.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cts=${PS5VK_CTS_DIR:-$root/.deps/work/vk-gl-cts}
build="$root/build/cts-host"
deqp="$build/external/vulkancts/modules/vulkan/deqp-vk"
icd="$root/build/driver/host/ps5vk_icd.x86_64.json"
driver_so="$root/build/driver/host/libvulkan_ps5vk.so"

build_cts() {
    [[ -d $cts ]] || { echo "no CTS checkout; run tools/fetch-vk-gl-cts.sh" >&2; exit 2; }
    cmake -S "$cts" -B "$build" -DDEQP_TARGET=vulkan_headless -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_CXX_FLAGS="-include cstdint -include cstddef -include cstring" > /dev/null
    cmake --build "$build" --target deqp-vk -j"$(nproc)"
}

case ${1:-} in
    --build) build_cts; exit 0 ;;
    --list)
        mkdir -p "$build/case-list"
        ( cd "$build/case-list" && "$deqp" --deqp-runmode=txt-caselist \
            --deqp-caselist-export-file=cts-cases.txt > /dev/null 2>&1 || true )
        list="$build/case-list/cts-cases.txt"
        echo "case list: $list ($(grep -c '^TEST:' "$list") cases)"
        exit 0 ;;
    "") echo "usage: tools/run-cts-host.sh <group> | --list | --build" >&2; exit 2 ;;
esac

group=$1
[[ -x $deqp ]] || build_cts
[[ -f $driver_so && -f $icd ]] || bash "$root/tools/build-driver.sh"
[[ -f $icd ]] || { echo "the host ICD is missing; run tools/build-driver.sh" >&2; exit 2; }

slug=$(printf '%s' "$group" | tr -c 'A-Za-z0-9._-' '_')
out="$build/runs/$slug"
mkdir -p "$out"
rm -f "$out/results.qpa"

echo "== CTS $group against $icd"
set +e
timeout "${PS5VK_CTS_TIMEOUT:-3600}" env \
    VK_DRIVER_FILES="$icd" VK_LOADER_LAYERS_DISABLE='~all~' \
    "$deqp" --deqp-case="$group" --deqp-log-filename="$out/results.qpa" > "$out/stdout.log" 2>&1
run_status=$?
set -e

python3 - "$out/stdout.log" "$out/summary.json" "$group" <<'PY'
import json
import re
import sys

log, summary, group = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(log, errors="replace").read()
totals = {}
for name in ("Passed", "Failed", "Not supported", "Warnings", "Waived"):
    match = re.search(rf"^\s+{name}:\s+(\d+)/(\d+)", text, re.M)
    totals[name.lower().replace(" ", "_")] = (
        {"count": int(match.group(1)), "total": int(match.group(2))} if match else None)

failures = []
for match in re.finditer(r"Test case '([^']+)'\.\.\n\s+(Fail|QualityWarning|InternalError) \((.*)\)",
                         text):
    failures.append({"case": match.group(1), "status": match.group(2), "reason": match.group(3)})

data = {"group": group, "totals": totals, "failures": failures}
open(summary, "w").write(json.dumps(data, indent=2, sort_keys=True) + "\n")

passed = totals.get("passed") or {"count": 0, "total": 0}
failed = totals.get("failed") or {"count": 0, "total": 0}
skipped = totals.get("not_supported") or {"count": 0, "total": 0}
print(f"   passed        {passed['count']}/{passed['total']}")
print(f"   failed        {failed['count']}/{failed['total']}")
print(f"   not supported {skipped['count']}/{skipped['total']}")
for failure in failures:
    print(f"   FAIL {failure['case']}")
    print(f"        {failure['reason']}")
print(f"   written       {summary}")
PY
grep -q '"failures": \[\]' "$out/summary.json" && { echo "cts host run: PASS"; exit 0; }
echo "cts host run: FAIL (see $out/summary.json)"
exit 1
