#!/usr/bin/env bash
# PS5 Vulkan - the ACO compile-order state probe's offline half.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The console compiler has faulted on the *next* compile after the unsigned
# texture case's frames, twice and differently: a SIGFPE (integer divide) after
# the case's last row when another case was queued, and a SIGSEGV (a write to an
# unmapped stack page) in the same case's setup when it ran alone
# (Klog_Logs/aco-state-run1.log, Klog_Logs/aco-state-unsigned-alone.log,
# docs/HARDWARE_FINDINGS.md). Both are in the compiler, not the driver, and both
# are the console build's: this script compiles the same shaders, with the same
# options and the same library the driver links, on the host, in one process.
#
# It builds tooling/psbc/compile-sequence.c against the work copy's host archive
# (libpsbc.pic.a, the one tools/build-driver.sh builds) and runs two sequences:
#
#   recorded  the unsigned texture case's pixel stage, then the probe sets the
#             batteries compiled after it, one line a compile
#   stress    every probe set's pixel stage three times over
#
# Both must finish without a fault for the console's fault to stay the console
# build's; a fault here names the pair of compiles that triggers it on the host
# and is the minimal reproducer the ACO round needs. The script exits 0 either
# way -- a fault that stops happening is good news -- and prints what it saw.
#
# Run from the repository root:  bash tools/check-aco-state.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${PSBC_PS5_WORK:-$root/.deps/work/psbc-ps5}
tree="$work/third_party/opengnm-psbc"
out="$root/build/aco-state"
mkdir -p "$out"

[[ -f $tree/libpsbc.pic.a ]] ||
    { echo "missing $tree/libpsbc.pic.a; run tools/build-driver.sh" >&2; exit 2; }
[[ -f $tree/libpsbc/psbc_compile.h ]] ||
    { echo "missing the compiler work copy $tree; run tools/build-psbc-ps5.sh" >&2; exit 2; }

gcc -std=c11 -O2 -Wall -Wextra -Werror -I "$tree/libpsbc" \
    -c "$root/tooling/psbc/compile-sequence.c" -o "$out/compile-sequence.o"
g++ "$out/compile-sequence.o" "$tree/libpsbc.pic.a" -pthread -lm -o "$out/compile-sequence"

# The recorded shape: the unsigned case's shader, then the sets whose compiles
# followed it in the batteries (v0-formats' own sampler2D, the texel buffers and
# the storage images).
cat > "$out/recorded.txt" <<'EOF'
# One compile a line: <spir-v> <descriptor>.
probes/v0-texture-uint/pixel.spv tex
probes/v0-texture-uint/pixel.spv tex
probes/m3-texture/pixel.spv tex
probes/v0-formats/pixel.spv tex
EOF
# A set's placeholder is not a shader: only lines whose file exists are compiled.
grep -v 'probes/v0-formats/pixel.spv' "$out/recorded.txt" > "$out/recorded.clean"
mv "$out/recorded.clean" "$out/recorded.txt"

# The stress shape: every probe set's pixel stage, three times over.
: > "$out/stress.txt"
for pass in 1 2 3; do
    for set in "$root"/probes/*/; do
        name=$(basename -- "$set")
        [[ -f $set/pixel.spv ]] || continue
        case $name in
        *texel-buffer*) kind=texel ;;
        *image-store*) kind=image ;;
        *texture*|m3-texture|c7-mip*|v0-array|v0-cube) kind=tex ;;
        *) kind=none ;;
        esac
        printf 'probes/%s/pixel.spv %s\n' "$name" "$kind" >> "$out/stress.txt"
    done
done

status=0
for sequence in recorded stress; do
    echo "== $sequence =="
    set +e
    (cd "$root" && "$out/compile-sequence" "$out/$sequence.txt")
    result=$?
    set -e
    if (( result == 0 )); then
        echo "$sequence: the host compiled every shader in one process, no fault"
    else
        echo "$sequence: the host faulted (exit $result); the console's fault has a host pair" >&2
        status=1
    fi
done
exit $status
