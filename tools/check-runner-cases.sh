#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - run the driver runner cases on the PC.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The runner's driver cases (run_vulkan_* in src/diagnostics.cpp, the ones the
# console runs from jobs/*/queue.txt) are run on the PC through a driver-enabled
# runner: bash tools/build-host-runner.sh --driver links AGC_VULKAN_DRIVER=1 with
# the driver and the B7 program, and --memory free keeps direct memory free,
# because the driver maps its own memory as it does on the console
# (docs/TESTING.md, "Adding tests").
#
# This script is that recipe as a gate: it builds the driver and the runner when
# they are missing, writes the queue and the replay, runs the cases and requires
# the runner's own summary to pass. --cases driver tells the runner its queue
# holds nothing but driver cases, which brings its own device and shaders, so the
# runner skips the AGC-level package staging: that staging's 64 KiB workspace
# otherwise takes the captured pipeline stage a driver case's own stage
# allocation needs, and a drawing case then refuses its pipeline
# (docs/M5_PHASE_B.md). The cases here only query or submit, because a *drawing*
# driver case's frame check is what the console run proves; its pipeline and its
# recording are what the PC run proves.
#
#   tools/check-runner-cases.sh            every case in the queue
#   tools/check-runner-cases.sh c7-clear   only the named ones

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work="$root/build/driver/runner-cases"
driver="$root/build/driver"
runner="$root/build/host/runner_host_driver"
golden="$root/golden/c7-mip-tiled/run-1.json"
# A drawing case's submission is compared with the console's b4-headless frame,
# so it replays that frame: the register defaults a replay carries are what the
# SH records it writes come from, and another run's would differ there.
drawing_golden="$root/golden/b5/b4-headless-1.json"
selected=("$@")

# The cases that draw nothing: the PC runs them whole, and each has to pass.
# v0-blit-dst is here because its work is the CPU resampler's: the bytes it
# writes into each destination format and reads back are the driver's own two
# tables, so the PC run proves the expectations the console run then has to
# confirm (round 5, docs/M5_PHASE_C.md).
cases=(
    v0-formats
    c2-transfers
    c7-clear
    v0-blit-dst
    b5-events
    device-report
)
# The cases that draw: their frame is what the console run proves, because
# nothing renders on the PC, so the PC run is asked for the submission they dump
# and that submission is compared with the console's own frame -- the comparison
# tools/check-driver.sh makes for the same code. A case's own record may fail
# here: its frame is empty.
drawing_cases=(
    c2-indirect
    b8-secondary
)
if (( ${#selected[@]} > 0 )); then
    wanted=()
    for name in "${selected[@]}"; do
        found=0
        for case in "${cases[@]}" "${drawing_cases[@]}"; do
            [[ $case == "$name" ]] && found=1
        done
        (( found )) || { echo "unknown runner case: $name" >&2; exit 2; }
        wanted+=("$name")
    done
    keep=()
    drawing=()
    for name in "${wanted[@]}"; do
        is_drawing=0
        for case in "${drawing_cases[@]}"; do
            [[ $case == "$name" ]] && is_drawing=1
        done
        if (( is_drawing )); then drawing+=("$name"); else keep+=("$name"); fi
    done
    cases=("${keep[@]}")
    drawing_cases=("${drawing[@]}")
fi

[[ -f $driver/host/libps5vk.a ]] || bash "$root/tools/build-driver.sh"
[[ -f $driver/host/libps5vk.a ]] || { echo "missing the driver archive" >&2; exit 2; }
bash "$root/tools/build-host-runner.sh" --driver
[[ -f $golden ]] || { echo "missing $golden" >&2; exit 2; }
[[ -f $drawing_golden ]] || { echo "missing $drawing_golden" >&2; exit 2; }
drawing_replay="$work/drawing.replay"

mkdir -p "$work"
queue="$work/queue.txt"
replay="$work/replay.txt"
log="$work/runner.log"
# The replay has to come from a golden a *driver* run captured: the driver's own
# 2 MiB queue buffer is mapped from it (its regions are what a PS5_HOST_REPLAY
# replay hands to matching allocations).
python3 "$root/tools/golden.py" replay "$golden" "$replay"
python3 "$root/tools/golden.py" replay "$drawing_golden" "$drawing_replay"
{
    printf 'capture\n'
    for case in "${cases[@]}"; do printf '%s\n' "$case"; done
} > "$queue"

status=0
echo "== PC, driver-enabled runner (${#cases[@]} case(s), ${#drawing_cases[@]} drawing)"
if (( ${#cases[@]} > 0 )) && ! "$runner" --replay "$replay" --memory free --cases driver \
        --app0 "$root" --download0 "$work" --queue "$queue" > "$log" 2>&1; then
    echo "the runner exited non-zero; see $log"
    status=1
fi
for case in "${cases[@]}"; do
    # The case's own record, and the number of checks it reported.
    line=$(grep -F "\"probe\":\"${case//-/_}\"" "$log" | tail -n 1 || true)
    if [[ -z $line ]]; then
        printf '%-20s %s\n' "$case" "NO RECORD"
        status=1
        continue
    fi
    if [[ $line == *'"status":"PASS"'* ]]; then
        printf '%-20s %s\n' "$case" "PASS"
    else
        printf '%-20s %s\n' "$case" "FAIL"
        printf '    %s\n' "$line"
        status=1
    fi
done
# The device's reporting is an artefact the CTS selection is made from, so a run
# that changes it has to change the committed inventory in the same commit:
# regenerate into a scratch file and compare (docs/CTS.md, Phase E1). The
# inventory's "source" names the log it was collected from -- the console's,
# the authoritative one -- so it is the one field not compared.
inventory="$root/conformance_inventory/device_report.json"
if [[ -f $inventory ]]; then
    python3 "$root/tools/collect-device-report.py" --out "$work/device-report.json" > /dev/null ||
        { echo "the device report could not be collected"; status=1; }
    if ! python3 - "$inventory" "$work/device-report.json" <<'PY'
import json
import sys
committed, collected = (json.load(open(path)) for path in sys.argv[1:3])
committed.pop("source", None)
collected.pop("source", None)
sys.exit(0 if committed == collected else 1)
PY
    then
        echo "conformance_inventory/device_report.json is stale: the device's reporting changed"
        diff "$inventory" "$work/device-report.json" | grep -v '"source"' | head -20
        status=1
    fi
fi
if (( ${#cases[@]} > 0 )) && ! grep -q '"probe":"runner_summary","status":"PASS"' "$log"; then
    echo "the runner's summary did not pass; see $log"
    grep -F '"probe":"runner_summary"' "$log" | tail -n 2 || true
    status=1
fi
for case in "${drawing_cases[@]}"; do
    case_queue="$work/$case.queue"
    case_log="$work/$case.log"
    dump="$work/$case.dump"
    printf 'capture\n%s\n' "$case" > "$case_queue"
    rm -f "$dump"
    # The case is expected to report its frame as failed: nothing renders on the
    # PC, and the frame is the console run's. What the PC proves is that the case
    # got as far as submitting, which the dump and its comparison show.
    PS5_HOST_SUBMISSION_DUMP="$dump" "$runner" --replay "$drawing_replay" --memory free \
        --cases driver --app0 "$root" --download0 "$work" --queue "$case_queue" \
        > "$case_log" 2>&1 || true
    if [[ ! -s $dump ]]; then
        printf '%-20s %s\n' "$case" "NO SUBMISSION"
        status=1
        continue
    fi
    # The registers the driver writes in every draw's table because context
    # registers keep their last value between draws -- the write mask, blend,
    # clip and rasterizer words (driver/ps5vk_draw.c; check-driver.sh restates
    # the same five): a console frame that left their defaults in place writes
    # none of them.
    if python3 "$root/tools/golden.py" compare-submission "$drawing_golden" "$dump" \
            --draws 1 --extra-sh-register 0x8c --extra-sh-register 0x0c \
            --expect-record 0x111=0x44870000 --restated-cx-register 0x08e \
            --restated-cx-register 0x1e0 --restated-cx-register 0x202 \
            --restated-cx-register 0x204 --restated-cx-register 0x205 \
            > "$work/$case.compare" 2>&1; then
        printf '%-20s %s\n' "$case" "PASS (submission identical)"
    else
        printf '%-20s %s\n' "$case" "FAIL (submission differs)"
        tail -n 3 "$work/$case.compare" | sed 's/^/    /'
        status=1
    fi
done
if (( status == 0 )); then
    echo "runner cases: PASS ($(grep -c '"status":"PASS"' "$log") PASS records)"
else
    echo "runner cases: FAIL (see $log)"
fi
exit $status
