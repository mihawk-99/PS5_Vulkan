#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - link recipe for titles with the shader compiler.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by tools/build.sh (definition AGC_SHADER_COMPILER=1) and
# tools/check-psbc-link.sh, so both link the compiler the same way
# (docs/M5_PHASE_A.md, A1):
# - .deps/native/psbc (tools/build-psbc-ps5.sh): libpsbc.ps5.a, and
#   libpsbc_support.ps5.a with ps5-opengl's C package writer, S3TC and the
#   PS5 shims
# - the payload SDK's static libc++, libc++abi and libunwind for ACO's C++
#   runtime, found through -L target/lib because they name pthread as a
#   dependent library
# - Clang's builtins, for __emutls_get_address
# - tooling/psbc/ps5-pie-unwind.ld: the titles' layout plus libunwind's
#   __eh_frame_* boundary symbols
#
# psbc_link_recipe ROOT SDK_ROOT sets psbc_include (compile flags),
# psbc_linker_script (replaces -T ps5-pie.ld), psbc_runtime_archives and
# psbc_link_inputs. It returns 2 when an input is missing.

psbc_link_recipe() {
    local root=$1 sdk_root=$2
    local psbc="$root/.deps/native/psbc"
    local compiler=${PS5_CLANG:-$(command -v clang || command -v clang-18 || true)}
    [[ -n $compiler ]] || { echo "clang was not found; set PS5_CLANG" >&2; return 2; }
    local builtins
    builtins="$("$compiler" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"

    psbc_include=(-I "$psbc/include")
    psbc_linker_script=(-T "$root/tooling/psbc/ps5-pie-unwind.ld" -L "$root/tooling/native")
    psbc_runtime_archives=("$sdk_root/target/lib/libc++.a" "$sdk_root/target/lib/libc++abi.a"
        "$sdk_root/target/lib/libunwind.a" "$builtins")
    psbc_link_inputs=(-L "$sdk_root/target/lib" --start-group
        "$psbc/lib/libpsbc.ps5.a" "$psbc/lib/libpsbc_support.ps5.a"
        "${psbc_runtime_archives[@]}" --end-group)

    local file
    for file in "$psbc/include/psbc_compile.h" "$psbc/include/ps5_agc_package.h" \
            "$psbc/lib/libpsbc.ps5.a" "$psbc/lib/libpsbc_support.ps5.a" \
            "${psbc_runtime_archives[@]}"; do
        [[ -f $file ]] || { echo "missing $file; run tools/build-psbc-ps5.sh" >&2; return 2; }
    done
}
