#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - build the test runner for the PC.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiles the console runner's sources (src/diagnostics.cpp and
# src/probe_pack.cpp, with the runner's definitions) together with the Linux
# implementation of its PS5 system calls (host/ps5) and the AGC helper models
# (host/agc) into build/host/runner_host, which tools/golden.py rebuild runs.
# Set HOST_CXX to choose the compiler.
#
#   tools/build-host-runner.sh            the AGC-level runner above
#   tools/build-host-runner.sh --driver   build/host/runner_host_driver: the
#                                         same runner with AGC_VULKAN_DRIVER=1,
#                                         the driver's host archives and the B7
#                                         program driver/tests/ps5vk_triangle.c,
#                                         exactly as tools/build.sh links the
#                                         console title. A runner case that goes
#                                         through the driver is run and read
#                                         with this one before the console runs
#                                         it; run tools/build-driver.sh first.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=agc
for argument in "$@"; do
    case $argument in
        --driver) mode=driver ;;
        *)
            echo "usage: ${0##*/} [--driver]" >&2
            exit 2
            ;;
    esac
done
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

# The runner's compile mode uses the same compiler sources built for the PC:
# the SDK's host libpsbc.a and the C package writer object that
# tools/build-psbc-cli.sh compiles with the SDK's host flags. The driver-enabled
# copy links the driver's own copy of that compiler instead, as the console
# title does (tools/build.sh).
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
psbc="$sdk/third_party/opengnm-psbc"
writer="$root/build/psbc-cli/ps5_agc_package.o"
[[ -f $psbc/libpsbc.a && -f $writer ]] ||
    { echo "missing $psbc/libpsbc.a or $writer; run tools/build-psbc-cli.sh" >&2; exit 2; }
compiler_archive="$psbc/libpsbc.a"

definitions=(-DAGC_LINKED_CANARY=1 -DAGC_TEST_RUNNER=1 -DAGC_OUTPUT_4K=1 -DAGC_LIVE_SUBMIT_ARMED=1
    -DAGC_SHADER_COMPILER=1 -DPS5VK_HOST_BUILD=1)
includes=(-I "$root/src" -I "$root/host" -I "$psbc/libpsbc" -I "$sdk/src/platform")
objects=()
link_flags=()
link_inputs=()

if [[ $mode == driver ]]; then
    output="$root/build/host/runner_host_driver"
    driver="$root/build/driver"
    runtime="$root/.deps/native/vulkan-runtime"
    for file in "$driver/host/libps5vk.a" "$runtime/lib/libvk_runtime.a" \
        "$driver/host/libpsbc_driver.pic.a"; do
        [[ -f $file ]] || { echo "missing $file; run tools/build-driver.sh" >&2; exit 2; }
    done
    # The B7 program is C, compiled as tools/check-driver.sh compiles it.
    harness="$root/build/host/ps5vk_triangle_host.o"
    gcc -std=c11 -O2 -Wall -Wextra -Werror \
        -I "$sdk/third_party/Vulkan-Headers/include" -I "$root/driver/tests" \
        -c "$root/driver/tests/ps5vk_triangle.c" -o "$harness"
    objects+=("$harness")
    # Phase D2's compute program, the same way.
    compute="$root/build/host/ps5vk_compute_host.o"
    gcc -std=c11 -O2 -Wall -Wextra -Werror \
        -I "$sdk/third_party/Vulkan-Headers/include" -I "$root/driver/tests" \
        -c "$root/driver/tests/ps5vk_compute.c" -o "$compute"
    objects+=("$compute")
    definitions+=(-DAGC_VULKAN_DRIVER=1)
    includes+=(-I "$root/driver" -I "$root/driver/tests")
    # Mesa's weak entry points resolve at link time (tools/check-vulkan-runtime.sh).
    link_inputs=(-Wl,--whole-archive "$driver/host/libps5vk.a" "$runtime/lib/libvk_runtime.a"
        -Wl,--no-whole-archive)
    compiler_archive="$driver/host/libpsbc_driver.pic.a"
else
    output="$root/build/host/runner_host"
fi

mkdir -p "$(dirname -- "$output")"
# The console runner's definitions; frames are "submitted" to the host layer,
# which completes them without a GPU.
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    "${definitions[@]}" "${includes[@]}" -I "$sdk/third_party/Vulkan-Headers/include" \
    "$root/src/diagnostics.cpp" "$root/src/probe_pack.cpp" \
    "$root/host/ps5/ps5_host.cpp" "$root/host/agc/agc_host.cpp" \
    "$root/host/runner/runner_host_main.cpp" \
    "${objects[@]}" "${link_inputs[@]}" \
    "$writer" "$compiler_archive" "${link_flags[@]}" -pthread -lm \
    -o "$output"
echo "built $output with $cxx"
