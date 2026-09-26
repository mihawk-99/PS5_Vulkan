#!/usr/bin/env bash
# ps5-native-app-boilerplate - Build the V0-compute dispatch probe payload.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiles shaders/c0/dispatch.comp to the SPIR-V the runner compiles at run
# time, and writes it with a provenance receipt. Unlike the graphics probe sets
# this one has no AGC package: a compute dispatch programs its own
# COMPUTE_PGM_RSRC1/2 and user SGPRs, so the payload is the SPIR-V plus the
# metadata the compiler reports when the runner compiles it.
#
# Usage: bash tools/build-compute-probe.sh

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
set_name=${1:-c0}
case "$set_name" in c0|c0-images|r17-descriptor-array|r84-subgroup|r87-ceiling) ;; *) echo "unknown compute probe: $set_name" >&2; exit 2 ;; esac
# R84's subgroup probe needs SPIR-V 1.3, Vulkan 1.1's; the others stay 1.0.
target_env=vulkan1.0
[[ $set_name != r84-subgroup ]] || target_env=vulkan1.1
source_file="$root/shaders/$set_name/dispatch.comp"
output="$root/probes/$set_name"
work="$root/build/probes/$set_name"
glslang=${GLSLANG:-glslang}

[[ -f $source_file ]] || { echo "missing source: $source_file" >&2; exit 2; }
command -v "$glslang" >/dev/null 2>&1 || command -v "$glslang" >/dev/null ||
    { echo "missing glslang; install it or set GLSLANG" >&2; exit 2; }

mkdir -p "$work" "$output"
"$glslang" -V --target-env "$target_env" -S comp "$source_file" -o "$work/dispatch.spv" \
    > "$work/glslang.log" 2>&1 || { cat "$work/glslang.log" >&2; exit 1; }

cp "$work/dispatch.spv" "$output/dispatch.spv"
spirv_words=$(python3 - "$output/dispatch.spv" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
if len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
    raise SystemExit("not a SPIR-V module")
print(len(data) // 4)
PY
)

# The words a dispatch programs, recorded beside the payload so the PC runner
# compiles the same shader and programs the same registers
# (probes/c0/resources.txt). The recorder links the host archive
# tools/build-driver.sh builds -- the 0.3.0 fork's compiler plus this
# repository's patches -- and reads the words where that compiler reports them,
# in the shader register table.
if [[ $set_name == c0 ]]; then
psbc_archive="$root/build/driver/host/libpsbc_driver.pic.a"
psbc_include="$root/.deps/native/psbc/include"
[[ -f $psbc_archive && -f $psbc_include/psbc_compile.h ]] ||
    { echo "missing the patched host compiler; run tools/build-driver.sh first" >&2; exit 2; }
gcc -std=c11 -O1 -Wall -Wextra -Werror -I "$psbc_include" \
    -c "$root/tooling/psbc/compute-resources.c" -o "$work/compute-resources.o"
g++ "$work/compute-resources.o" "$psbc_archive" -pthread -lm -o "$work/compute-resources"
"$work/compute-resources" "$output/dispatch.spv" > "$work/resources.txt"
{
    echo "# PS5 Vulkan c0 compute dispatch probe: the words a compute dispatch"
    echo "# programs, read from the compiler's own register table (the 0.3.0 fork"
    echo "# reports R_00B848/COMPUTE_PGM_RSRC1, R_00B84C/RSRC2 and R_00B8A0/RSRC3"
    echo "# there, with the wave size and workgroup shape it compiled for). Written"
    echo "# by tools/build-compute-probe.sh, whose recorder links the host archive"
    echo "# tools/build-driver.sh builds, for the runner's PC builds."
    cat "$work/resources.txt"
} > "$output/resources.txt"

fi

{
    echo "PS5 Vulkan $set_name compute dispatch probe, built by tools/build-compute-probe.sh $set_name."
    echo "source: shaders/$set_name/dispatch.comp"
    echo "pipeline: GLSL -> SPIR-V (glslang, $target_env), compiled on the console by libpsbc"
    printf 'glslang: %s\n' "$("$glslang" --version | head -n 1)"
    printf 'dispatch.spv: %s words, sha256 %s\n' "$spirv_words" \
        "$(sha256sum "$output/dispatch.spv" | cut -d' ' -f1)"
    if [[ $set_name == c0 ]]; then
        printf 'resources.txt: sha256 %s\n' \
            "$(sha256sum "$output/resources.txt" | cut -d' ' -f1)"
    fi
} > "$output/PROVENANCE.txt"

cat "$output/PROVENANCE.txt"
