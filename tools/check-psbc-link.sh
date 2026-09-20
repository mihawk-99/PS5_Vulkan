#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - link check of the PS5 shader compiler.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase A1 (docs/M5_PHASE_A.md). Links a small program that calls
# psbc_init, psbc_compile_shader, ps5_agc_package_build, psbc_free_output and
# psbc_shutdown the way the console titles link: this repository's CRT and C++
# runtime objects, version script and the payload SDK's system library stubs,
# plus the compiler recipe of tools/psbc-link.sh that tools/build.sh uses for
# AGC_SHADER_COMPILER=1. Every symbol must resolve, and the title converter
# must accept every import. The program is never packaged or run.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk_root="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
work="$root/build/psbc-link"
compile=(sh "$root/tooling/prospero-clang18")
# shellcheck source=tools/psbc-link.sh
source "$root/tools/psbc-link.sh"
psbc_link_recipe "$root" "$sdk_root" || exit 2
mkdir -p "$work"

cat > "$work/link_probe.c" <<'C'
#include <stdlib.h>

#include "ps5_agc_package.h"
#include "psbc_compile.h"

int main(void)
{
    psbc_init();
    PsbcCompileOptions options = {0};
    PsbcShaderOutput output = {0};
    uint8_t *package = NULL;
    size_t package_size = 0;
    const PsbcResult result = psbc_compile_shader(NULL, 0, &options, &output);
    if (result == PSBC_RESULT_OK && ps5_agc_package_build(&output, 1, &package, &package_size) == 0)
        free(package);
    psbc_free_output(&output);
    psbc_shutdown();
    return (int)result;
}
C

export PS5_PAYLOAD_SDK="$sdk_root"
"${compile[@]}" -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    "${psbc_include[@]}" -c "$work/link_probe.c" -o "$work/link_probe.o"
"${compile[@]}" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/app_crt.o"
"${compile[@]}" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/app_cpp_runtime.cpp" -o "$work/app_cpp_runtime.o"

log="$work/link.log"
# --error-limit=0: lld otherwise stops after 20 errors and hides the rest.
# --why-extract records which archive members the link pulls in.
if ! "$sdk_root/bin/prospero-lld" "${psbc_linker_script[@]}" --eh-frame-hdr --error-limit=0 \
        --version-script "$native/app-symbols.map" --exclude-libs=ALL \
        --why-extract="$work/why-extract.txt" \
        -e _start -o "$work/link-probe.elf" \
        "$work/app_crt.o" "$work/app_cpp_runtime.o" "$work/link_probe.o" \
        "${psbc_link_inputs[@]}" \
        --as-needed "$sdk_root"/target/lib/*.so > "$log" 2>&1; then
    undefined=$(grep -oE "undefined symbol: .*" "$log" | sed 's/^undefined symbol: /  /' | sort -u || true)
    echo "link check: FAIL"
    [[ -z $undefined ]] || printf '%s undefined symbol(s):\n%s\n' "$(grep -c . <<< "$undefined")" "$undefined"
    grep -vE "undefined symbol|>>> " "$log" | head -n 20
    exit 1
fi
echo "lld: every compiler symbol resolved ($(stat -c %s "$work/link-probe.elf") byte ELF)"
echo "runtime archive members linked:"
for archive in "${psbc_runtime_archives[@]}"; do
    members=$(awk -F'\t' -v archive="$archive" 'index($2, archive "(") == 1 { print $2 }' \
        "$work/why-extract.txt" | sed -E 's/^.*\((.*)\)$/\1/' | sort -u | tr '\n' ' ')
    echo "  ${archive##*/}: ${members:-none}"
done

# The title converter turns the ELF's dynamic imports into console module
# imports; it fails on any import the SDK's system library stubs do not name.
tool="$root/build/host/ps5-native-tool"
[[ -x $tool ]] || { echo "missing $tool; run tools/build.sh once" >&2; exit 2; }
if ! "$tool" link --in "$work/link-probe.elf" --out "$work/link-probe.eboot.elf" \
        --stub-dir "$sdk_root/target/lib" --module-sdk 0x02000009 \
        --companion-sdk 0x08050001 --file-name eboot.elf > "$work/convert.log" 2>&1; then
    cat "$work/convert.log"
    echo "link check: FAIL; the title converter rejected the imports" >&2
    exit 1
fi
imports=$("$sdk_root/bin/prospero-nm" -D --undefined-only "$work/link-probe.elf" | awk '{ print $NF }' | sort -u)
echo "converter: accepted $(printf '%s\n' "$imports" | grep -c .) imported symbols"
echo "link check: PASS"
