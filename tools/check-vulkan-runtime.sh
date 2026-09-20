#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - check Mesa's Vulkan runtime builds.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase B1 (docs/M5_PHASE_B.md). Builds the runtime smoke test
# (tooling/vulkan-runtime/smoke/vk_runtime_smoke.c) against the archives of
# tools/build-vulkan-runtime.sh:
# 1. PC: links it with libvk_runtime.a and the SDK's host libpsbc.a (Mesa
#    util) and runs it: create and destroy a vk_instance, check the derived
#    application and driver versions.
# 2. PS5: links it the way a console title links (this repository's CRT and
#    C++ runtime objects, the shader compiler recipe of tools/psbc-link.sh for
#    Mesa util, the SDK's system library stubs) with libvk_runtime.ps5.a, and
#    runs the title converter. Every symbol must resolve and every import
#    must be accepted. The PS5 program is not run.
# The runtime archives link whole, as meson's link_whole does, so the
# runtime's weak entry points resolve against any driver overrides.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
sdk_root="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
runtime="$root/.deps/native/vulkan-runtime"
psbc_work=${PSBC_PS5_WORK:-$root/.deps/work/psbc-ps5}
tree="$psbc_work/third_party/opengnm-psbc"
smoke="$root/tooling/vulkan-runtime/smoke/vk_runtime_smoke.c"
work="$root/build/vulkan-runtime-check"
mesa_version=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["mesa"]["version"])' \
    "$sdk/dependencies.json")

for file in "$runtime/lib/libvk_runtime.a" "$runtime/lib/libvk_runtime.ps5.a"; do
    [[ -f $file ]] || { echo "missing $file; run tools/build-vulkan-runtime.sh" >&2; exit 2; }
done
[[ -f $sdk/third_party/opengnm-psbc/libpsbc.a ]] ||
    { echo "missing the SDK's host libpsbc.a" >&2; exit 2; }
# shellcheck source=tools/psbc-link.sh
source "$root/tools/psbc-link.sh"
psbc_link_recipe "$root" "$sdk_root" || exit 2
mkdir -p "$work"

# The compiler tree's flags per target, with the runtime's installed headers
# first; compiled from the tree, whose include paths are relative.
print_cflags() {
    make -s --no-print-directory -C "$tree" -f "$1" \
        -f <(printf 'print-cflags:\n\t@echo $(CFLAGS)\n') PS5_PAYLOAD_SDK="$sdk_root" print-cflags
}
host_cflags=$(print_cflags "$sdk/toolchain/opengnm-psbc-host.mak")
ps5_cflags=$(print_cflags "$psbc_work/toolchain/opengnm-psbc-ps5.mak")
ps5_cflags=${ps5_cflags//-D_XOPEN_SOURCE=700/}
includes=(-I "$runtime/include/vulkan/runtime" -I "$runtime/include/vulkan/util" -I "$runtime/include")
expected=(-DPS5VK_EXPECTED_MESA="\"$mesa_version\"")

echo "== PC"
# shellcheck disable=SC2086
(cd "$tree" && gcc "${includes[@]}" $host_cflags -Werror "${expected[@]}" -c "$smoke" -o "$work/smoke.host.o")
g++ -o "$work/vk_runtime_smoke" "$work/smoke.host.o" \
    -Wl,--whole-archive "$runtime/lib/libvk_runtime.a" -Wl,--no-whole-archive \
    "$sdk/third_party/opengnm-psbc/libpsbc.a" -pthread -lm
"$work/vk_runtime_smoke"

echo "== PS5"
export PS5_PAYLOAD_SDK="$sdk_root"
# shellcheck disable=SC2086
(cd "$tree" && "$sdk_root/bin/prospero-clang" "${includes[@]}" $ps5_cflags -Werror "${expected[@]}" \
    -c "$smoke" -o "$work/smoke.ps5.o")
compile=(sh "$root/tooling/prospero-clang18")
"${compile[@]}" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/app_crt.o"
"${compile[@]}" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/app_cpp_runtime.cpp" -o "$work/app_cpp_runtime.o"
log="$work/link.log"
# Mesa's generated entry-point tables name every Vulkan function through weak
# references, which read as NULL when nothing defines them; the dispatch
# helpers skip NULL entries. The title converter imports every entry of the
# dynamic symbol table and demands an SDK stub for each. Symbols SDK stubs
# define are shared symbols, not undefined weak ones, and are still imported.
# --no-dynamic-linker keeps the titles' interpreter-free layout. LLD 18 also
# left undefined weak symbols out of that table on its own, but the FreeBSD
# target defaults to -z dynamic-undefined-weak, which LLD 22 now honours, so
# the opposite is requested explicitly. LLD 18 warns and links byte-identically.
if ! "$sdk_root/bin/prospero-lld" "${psbc_linker_script[@]}" --eh-frame-hdr --error-limit=0 \
        --no-dynamic-linker \
        -z nodynamic-undefined-weak \
        --version-script "$native/app-symbols.map" --exclude-libs=ALL \
        -e _start -o "$work/vk_runtime_smoke.ps5.elf" \
        "$work/app_crt.o" "$work/app_cpp_runtime.o" "$work/smoke.ps5.o" \
        --whole-archive "$runtime/lib/libvk_runtime.ps5.a" --no-whole-archive \
        "${psbc_link_inputs[@]}" \
        --as-needed "$sdk_root"/target/lib/*.so > "$log" 2>&1; then
    undefined=$(grep -oE "undefined symbol: .*" "$log" | sed 's/^undefined symbol: /  /' | sort -u || true)
    duplicate=$(grep -oE "duplicate symbol: .*" "$log" | sed 's/^duplicate symbol: /  /' | sort -u || true)
    echo "PS5 link: FAIL"
    [[ -z $undefined ]] || printf '%s undefined symbol(s):\n%s\n' "$(grep -c . <<< "$undefined")" "$undefined"
    [[ -z $duplicate ]] || printf '%s duplicate symbol(s):\n%s\n' "$(grep -c . <<< "$duplicate")" "$duplicate"
    grep -vE "undefined symbol|duplicate symbol|>>> " "$log" | head -n 20
    exit 1
fi
echo "lld: every symbol resolved ($(stat -c %s "$work/vk_runtime_smoke.ps5.elf") byte ELF)"
tool="$root/build/host/ps5-native-tool"
[[ -x $tool ]] || { echo "missing $tool; run tools/build.sh once" >&2; exit 2; }
if ! "$tool" link --in "$work/vk_runtime_smoke.ps5.elf" --out "$work/vk_runtime_smoke.eboot.elf" \
        --stub-dir "$sdk_root/target/lib" --module-sdk 0x02000009 \
        --companion-sdk 0x08050001 --file-name eboot.elf > "$work/convert.log" 2>&1; then
    cat "$work/convert.log"
    echo "PS5 link: FAIL; the title converter rejected the imports" >&2
    exit 1
fi
imports=$("$sdk_root/bin/prospero-nm" -D --undefined-only "$work/vk_runtime_smoke.ps5.elf" | awk '{ print $NF }' | sort -u)
echo "converter: accepted $(grep -c . <<< "$imports") imported symbols"
echo "vulkan runtime check: PASS"
