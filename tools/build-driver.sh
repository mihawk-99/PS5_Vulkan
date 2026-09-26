#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - build the PS5 Vulkan driver.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase B2 (docs/M5_PHASE_B.md). Builds driver/ on Mesa's Vulkan
# runtime (tools/build-vulkan-runtime.sh) with the shader compiler tree's flags
# for both targets:
#
#   build/driver/ps5/libps5vk.ps5.a            PS5 archive; titles link it with the runtime
#   build/driver/ps5/libvulkan.so.1            PS5 shared object; a title dlopens it and
#                                              resolves vkGetInstanceProcAddr (E2)
#   build/driver/host/libps5vk.a               PC archive, for direct (loaderless) tests
#   build/driver/host/libvulkan_ps5vk.so       PC Vulkan loader driver over host/ps5/ps5_host.cpp
#   build/driver/host/ps5vk_icd.x86_64.json    its loader manifest, for VK_DRIVER_FILES
#   build/driver/{ps5/libpsbc_driver.ps5.a,host/libpsbc_driver.pic.a}
#                                              the shader compiler as the driver links it
#   build/driver/host/ps5_agc_package.o        the PC build of ps5-opengl's package writer
#
# The PC outputs link libpsbc.pic.a, a position-independent build of the
# compiler archive in the work copy (tooling/psbc/Makefile.opengnm-psbc-host-pic):
# the SDK's host libpsbc.a cannot go into a shared library.
#
# The entry points are generated from vk.xml with Mesa's vk_entrypoints_gen.py
# (--prefix ps5vk) and the manifest with vk_icd_gen.py, as Mesa's own drivers
# do. The PC library exports only the three loader entry points
# (driver/ps5vk_icd.map, -Bsymbolic); the build fails on any other export.
#
# Run tools/fetch-mesa.sh, tools/build-psbc-ps5.sh and
# tools/build-vulkan-runtime.sh first.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
payload="$root/.deps/native/ps5-payload-sdk"
psbc_work=${PSBC_PS5_WORK:-$root/.deps/work/psbc-ps5}
tree="$psbc_work/third_party/opengnm-psbc"
runtime="$root/.deps/native/vulkan-runtime"
psbc_include="$root/.deps/native/psbc/include"
host_psbc="$tree/libpsbc.pic.a"
work="$root/build/driver"
mesa_version=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["mesa"]["version"])' \
    "$sdk/dependencies.json")
mesa=${MESA_SOURCE_DIR:-$root/.deps/native/mesa/mesa-$mesa_version}

for file in "$runtime/lib/libvk_runtime.ps5.a" "$runtime/lib/libvk_runtime.a"; do
    [[ -f $file ]] || { echo "missing $file; run tools/build-vulkan-runtime.sh" >&2; exit 2; }
done
[[ -f $tree/src/compiler/nir/nir_opcodes.h ]] ||
    { echo "missing the compiler work copy $tree; run tools/build-psbc-ps5.sh" >&2; exit 2; }
[[ -f $mesa/src/vulkan/util/vk_entrypoints_gen.py ]] ||
    { echo "missing Mesa $mesa_version sources at $mesa; run tools/fetch-mesa.sh" >&2; exit 2; }
# shellcheck source=tools/mesa-python.sh
source "$root/tools/mesa-python.sh"

# shellcheck source=tools/ccache.sh
source "$root/tools/ccache.sh"
mesa_python_init || exit 2
mkdir -p "$work/generated" "$work/fresh" "$work/ps5" "$work/ps5-pic" "$work/host"

# --- generated entry points (replaced only when their content changes) ------
xml="$mesa/src/vulkan/registry/vk.xml"
python3 "$mesa/src/vulkan/util/vk_entrypoints_gen.py" --xml "$xml" --proto --weak \
    --out-h "$work/fresh/ps5vk_entrypoints.h" --out-c "$work/fresh/ps5vk_entrypoints.c" \
    --prefix ps5vk --beta false
for file in "$work/fresh/ps5vk_entrypoints.h" "$work/fresh/ps5vk_entrypoints.c"; do
    cmp -s "$file" "$work/generated/${file##*/}" || cp "$file" "$work/generated/"
done

# --- position-independent PC compiler archive -----------------------------------
host_mak="$sdk/toolchain/opengnm-psbc-host.mak"
pic_log="$work/build-psbc-pic.log"
if ! make -C "$tree" -f "$root/tooling/psbc/Makefile.opengnm-psbc-host-pic" -j"$(nproc)" \
        CONFIG="$host_mak" libpsbc \
        CC="$(ps5vk_ccache gcc)" CXX="$(ps5vk_ccache g++)" > "$pic_log" 2>&1; then
    grep -E 'error:' "$pic_log" | head -n 40
    echo "PC compiler archive build failed; see $pic_log" >&2
    exit 1
fi
echo "libpsbc.pic.a: $(grep -cE ' -c [^ ]+\.(c|cpp) ' "$pic_log" || true) sources compiled, $(grep -c 'warning:' "$pic_log" || true) warnings, $(stat -c %s "$host_psbc") bytes"

# --- archives ------------------------------------------------------------------
print_cflags() {
    make -s --no-print-directory -C "$tree" -f "$1" \
        -f <(printf 'print-cflags:\n\t@echo $(CFLAGS)\n') PS5_PAYLOAD_SDK="$payload" print-cflags
}
ps5_cflags=$(print_cflags "$psbc_work/toolchain/opengnm-psbc-ps5.mak")
host_cflags="$(print_cflags "$host_mak") -fPIC"
# As for the runtime: FreeBSD's sys/cdefs.h hides C11 and BSD interfaces
# under _XOPEN_SOURCE=700.
ps5_cflags=${ps5_cflags//-D_XOPEN_SOURCE=700/}

# The headers this build consumes, hashed: the driver's own, the generated entry
# points, and the runtime's and the compiler's installed trees. The Makefile
# names the driver's as prerequisites (driver/Makefile), which covers an edit
# there; nothing covers a change to the *installed* headers, and an object built
# against an older copy of one is a mixed layout -- R7 round 2's symptom was not
# a build error but a table reported with a NULL pointer and zero bytes
# (docs/M5_PHASE_C.md). So the digest joins the flags below: any change discards
# every object for that target, which is what a header's layout means.
header_digest() {
    { find "$root/driver" -name '*.h' -print0
      find "$work/generated" -name '*.h' -print0 2>/dev/null
      find "$runtime/include" -name '*.h' -print0 2>/dev/null
      find "$psbc_include" -name '*.h' -print0 2>/dev/null
    } | sort -z | xargs -0 sha256sum 2>/dev/null | sha256sum | cut -c1-16
}
# Persistent shader outputs are valid only for these compiler/driver inputs.
# Hash content, not timestamps: unchanged builds retain the on-console cache.
python3 - "$root" "$host_psbc" "$ps5_cflags" "$host_cflags" <<'PY_CACHE'
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1])
common = [root / "driver" / name for name in
          ("ps5vk_pipeline.c", "ps5vk_compute.c", "ps5vk_nir.c", "ps5vk_shader_cache.c",
           "ps5vk_shader_cache.h", "ps5vk_private.h")]
common += [root / ".deps/native/psbc/include/psbc_compile.h"]
lines = []
for target, compiler, flags in (
    ("PS5", root / ".deps/native/psbc/lib/libpsbc.ps5.a", sys.argv[3]),
    ("HOST", pathlib.Path(sys.argv[2]), sys.argv[4])):
    digest = hashlib.sha256(b"ps5vk-shader-cache-v1" + flags.encode())
    for path in [*common, compiler]:
        data = path.read_bytes()
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
    lines.append(f'#define PS5VK_CACHE_{target}_BUILD "{digest.hexdigest()}"')
path = root / "build/driver/generated/ps5vk_cache_build.h"
text = "\n".join(lines) + "\n"
if not path.exists() or path.read_text() != text:
    path.write_text(text)
PY_CACHE
headers=$(header_digest)

build_archive() {
    local target=$1 archive=$2 cc=$3 ar=$4 cflags=$5
    local log="$work/build-$target.log"
    # make does not track flags, and only the Makefile's own prerequisites track
    # a header: objects built with other flags or another header set are
    # discarded.
    local flags="$work/$target/flags"
    if [[ ! -f $flags || $(cat "$flags") != "$cc $cflags headers $headers" ]]; then
        rm -f "$work/$target"/*.o "$work/$target"/*.a
        printf '%s\n' "$cc $cflags headers $headers" > "$flags"
    fi
    if ! make -k -C "$tree" -f "$root/driver/Makefile" -j"$(nproc)" DRIVER="$root/driver" \
            GENERATED="$work/generated" RUNTIME="$runtime" PSBC_INCLUDE="$psbc_include" \
            OUT="$work/$target" ARCHIVE="$archive" \
            CC="$cc" AR="$ar" CFLAGS="$cflags" > "$log" 2>&1; then
        grep -E 'error:|warning:' "$log" | head -n 40
        echo "$target driver build failed; see $log" >&2
        exit 1
    fi
    echo "$target: $(grep -cE ' -c [^ ]+\.c ' "$log" || true) sources compiled, $(grep -c 'warning:' "$log" || true) warnings"
}
# As in tools/build-vulkan-runtime.sh: the two archives share their inputs and
# nothing else, and either one alone leaves cores idle.
build_archive ps5 libps5vk.ps5.a "$(ps5vk_ccache "$payload/bin/prospero-clang")" \
    "$payload/bin/prospero-ar" "$ps5_cflags" &
ps5_pid=$!
# The shared object needs position-independent objects: a second archive of the
# same sources, its own object directory (build_archive keys on the flags).
build_archive ps5-pic libps5vk.ps5.pic.a "$(ps5vk_ccache "$payload/bin/prospero-clang")" \
    "$payload/bin/prospero-ar" "$ps5_cflags -fPIC" &
ps5_pic_pid=$!
build_archive host libps5vk.a "$(ps5vk_ccache gcc)" ar "$host_cflags" &
host_pid=$!
failed=0
wait "$ps5_pid" || failed=1
wait "$ps5_pic_pid" || failed=1
wait "$host_pid" || failed=1
((failed == 0)) || {
    echo "a driver archive failed to build; see $work/build-ps5.log and $work/build-host.log" >&2
    exit 1
}

# --- the shader compiler as the driver links it ---------------------------------
# The compiler archive's psbc_stubs member stubs five functions of Mesa's
# Vulkan runtime, which the driver links for real: a duplicate-symbol link
# error. Mesa's vk_debug_report also dereferences the report the compiler
# passes as NULL. A copy of each compiler archive renames the five, in the
# stubs and in the compiler's references alike, so the compiler keeps the
# behaviour Phase A3 measured and the runtime keeps its own functions.
psbc_renamed=(vk_debug_report vk_format_get_ycbcr_info vk_format_to_pipe_format
              vk_sampler_state_init vk_spec_info_to_nir_spirv)
rename_arguments=()
for symbol in "${psbc_renamed[@]}"; do
    rename_arguments+=(--redefine-sym "$symbol=psbc_$symbol")
done
derive_compiler_archive() {
    local objcopy=$1 nm=$2 source=$3 output=$4
    "$objcopy" "${rename_arguments[@]}" "$source" "$output"
    local defined
    defined=$("$nm" --defined-only "$output" 2>/dev/null | awk 'NF == 3 { print $3 }')
    for symbol in "${psbc_renamed[@]}"; do
        ! grep -qx "$symbol" <<< "$defined" || { echo "$output still defines $symbol" >&2; exit 1; }
        grep -qx "psbc_$symbol" <<< "$defined" || { echo "$output lacks psbc_$symbol" >&2; exit 1; }
    done
}
derive_compiler_archive "$payload/bin/prospero-objcopy" "$payload/bin/prospero-nm" \
    "$root/.deps/native/psbc/lib/libpsbc.ps5.a" "$work/ps5/libpsbc_driver.ps5.a"
derive_compiler_archive objcopy nm "$host_psbc" "$work/host/libpsbc_driver.pic.a"
# ps5-opengl's C package writer; the PS5 links take it from libpsbc_support.ps5.a.
(cd "$tree" && gcc $host_cflags -I "$psbc_include" -c "$sdk/src/platform/ps5_agc_package.c" \
    -o "$work/host/ps5_agc_package.o")
echo "compiler archives: ${#psbc_renamed[@]} runtime symbols renamed in both copies"

# --- PC loader driver ----------------------------------------------------------
# The PS5 kernel and AGC calls the driver makes are, on the PC, the host
# layer's and the AGC helper models'.
for source in ps5/ps5_host agc/agc_host; do
    g++ -std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -fPIC \
        -I "$root/src" -I "$root/host" -c "$root/host/$source.cpp" -o "$work/host/${source#*/}.o"
done
library="$work/host/libvulkan_ps5vk.so"
g++ -shared -o "$library" -Wl,-soname,libvulkan_ps5vk.so -Wl,-Bsymbolic -Wl,--no-undefined \
    -Wl,--version-script="$root/driver/ps5vk_icd.map" \
    -Wl,--whole-archive "$work/host/libps5vk.a" "$runtime/lib/libvk_runtime.a" -Wl,--no-whole-archive \
    "$work/host/ps5_host.o" "$work/host/agc_host.o" "$work/host/ps5_agc_package.o" \
    "$work/host/libpsbc_driver.pic.a" -pthread -lm

expected="vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion"
exports=$(nm -D --defined-only "$library" | awk '{ print $NF }' | sort | tr '\n' ' ' | sed 's/ $//')
if [[ $exports != "$expected" ]]; then
    echo "libvulkan_ps5vk.so exports: $exports" >&2
    echo "expected exactly: $expected" >&2
    exit 1
fi

manifest="$work/host/ps5vk_icd.x86_64.json"
# The driver's own version (R84: 1.1): the loader decides by this number which
# core device commands to hand an application, so a 1.0 manifest hid 1.1's.
python3 "$mesa/src/vulkan/util/vk_icd_gen.py" --api-version 1.1 --xml "$xml" --sizeof-pointer 8 \
    --icd-lib-path "$work/host" --icd-filename libvulkan_ps5vk.so --out "$manifest"

# --- PS5 shared object ----------------------------------------------------------
# The loaderless delivery (docs/M5_REFERENCE.md, E2): one file a title places
# beside eboot.bin and dlopens, holding the driver, Mesa's Vulkan runtime, the
# shader compiler and the AGC import stubs the driver-enabled title links (their
# bodies never run; they are what lets the converter record the imports). vkGetInstanceProcAddr is the entry
# point a frontend that loads the library itself resolves (driver/ps5vk_vulkan.map);
# the module is converted and signed the way eboot.bin is, so the console's
# loader accepts it.
so_elf="$work/ps5/libvulkan.so.1.elf"
so_mod="$work/ps5/libvulkan.so.1.mod"
so="$work/ps5/libvulkan.so.1"
so_abs_elf="$work/ps5/libvulkan-abs.so.elf"
so_abs="$work/ps5/libvulkan-abs.so"
# shellcheck source=tools/psbc-link.sh
source "$root/tools/psbc-link.sh"
psbc_link_recipe "$root" "$payload" || exit 2
agc_so_stub="$work/ps5/libSceAgc.so"
agc_driver_so_stub="$work/ps5/libSceAgcDriver.so"
for entry in "libSceAgc $root/vendor/ps5/sdk/stubs/agc_canary_link_stub.c $agc_so_stub" \
             "libSceAgcDriver $root/vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c $agc_driver_so_stub"; do
    read -r name stub_source stub_output <<< "$entry"
    [[ -f $stub_source ]] || { echo "missing $stub_source" >&2; exit 2; }
    PS5_PAYLOAD_SDK="$payload" sh "$root/tooling/prospero-clang18" \
        -std=c11 -O2 -fPIC -ffunction-sections -fdata-sections -c "$stub_source" \
        -o "$work/ps5/${name}_link_stub.o"
    "$payload/bin/prospero-lld" --shared -soname "${name}.prx" -o "$stub_output" \
        "$work/ps5/${name}_link_stub.o"
done
# No --exclude-libs here: it hides the archive members' symbols before the
# version script can export the four entry points (the PC library's link does
# the same); the script's `local: *` keeps every other symbol local.
link_shared_object() {
    local soname=$1 output=$2 log=$3
    if ! "$payload/bin/prospero-lld" --shared --eh-frame-hdr --error-limit=0 \
            --version-script "$root/driver/ps5vk_vulkan.map" \
            -L "$payload/target/lib" -soname "$soname" -o "$output" \
            --whole-archive "$work/ps5-pic/libps5vk.ps5.pic.a" "$runtime/lib/libvk_runtime.ps5.a" \
            --no-whole-archive \
            "$work/ps5/libpsbc_driver.ps5.a" "$root/.deps/native/psbc/lib/libpsbc_support.ps5.a" \
            "${psbc_runtime_archives[@]}" "$agc_so_stub" "$agc_driver_so_stub" \
            --as-needed "$payload"/target/lib/*.so > "$log" 2>&1; then
        grep -oE "(undefined|duplicate) symbol: .*" "$log" | sort -u | head -n 40 || true
        grep -vE "undefined symbol|duplicate symbol|>>> " "$log" | head -n 20 || true
        echo "the PS5 shared object failed to link; see $log" >&2
        exit 1
    fi
}
link_shared_object libvulkan.so.1 "$so_elf" "$work/build-ps5-so.log"
# The payload SDK's own library recipe (samples/hello_so) puts the file's
# absolute path in the soname and loads the module from that path; the smoke
# test tries this shape too, because the title's loader may match a module by
# the name it was linked under and not by the path it was handed.
link_shared_object /app0/libvulkan-abs.so "$so_abs_elf" "$work/build-ps5-so-abs.log"
# The signer wraps the shared ELF in the FSELF container, as runtime/libc.prx is
# wrapped. The module converter is not in this path: ps5-native-tool link
# refuses to publish application exports ("native converter does not yet publish
# application exports"), and this library's whole point is its export table, so
# the signed container carries the ELF's own .dynsym. Making the converter write
# a PRX export table is what a title that links against this library statically
# would need; a title that dlopens and dlsyms it does not (docs/M5_REFERENCE.md,
# E2).
"$root/build/host/ps5-native-tool" self --sign --in "$so_elf" --out "$so" \
    --magic 0x1D3D154F
"$root/build/host/ps5-native-tool" self --extract --file "$so" --out "$work/ps5/libvulkan.so.1.signed" >/dev/null
# The signed container's payload drops the section header table, so its .dynsym
# is not readable with nm; the check is that the extraction succeeds and the
# payload still names the soname and every entry point.
signed_ok=$(python3 "$root/tools/check-so-exports.py" "$work/ps5/libvulkan.so.1.signed")
so_exports=$("$payload/bin/prospero-nm" -D --defined-only "$so_elf" | awk '{ print $NF }' | sort | tr '\n' ' ' | sed 's/ $//')
expected_so="vkGetInstanceProcAddr vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion"
if [[ $so_exports != "$expected_so" ]]; then
    echo "libvulkan.so.1 exports: $so_exports" >&2
    echo "expected exactly: $expected_so" >&2
    exit 1
fi
[[ $signed_ok == ok ]] || { echo "the signed libvulkan.so.1 payload: $signed_ok" >&2; exit 1; }
echo "libvulkan.so.1: $(stat -c %s "$so") bytes (module), exports: $so_exports"

# The same treatment for the soname-as-path copy; its payload has to name that
# path rather than the plain soname.
"$root/build/host/ps5-native-tool" self --sign --in "$so_abs_elf" --out "$so_abs" \
    --magic 0x1D3D154F
"$root/build/host/ps5-native-tool" self --extract --file "$so_abs" \
    --out "$work/ps5/libvulkan-abs.so.signed" >/dev/null
abs_exports=$("$payload/bin/prospero-nm" -D --defined-only "$so_abs_elf" | awk '{ print $NF }' | sort | tr '\n' ' ' | sed 's/ $//')
[[ $abs_exports == "$expected_so" ]] || {
    echo "libvulkan-abs.so exports: $abs_exports" >&2
    echo "expected exactly: $expected_so" >&2
    exit 1
}
abs_ok=$(python3 "$root/tools/check-so-exports.py" "$work/ps5/libvulkan-abs.so.signed" /app0/libvulkan-abs.so)
[[ $abs_ok == ok ]] || { echo "the signed libvulkan-abs.so payload: $abs_ok" >&2; exit 1; }
echo "libvulkan-abs.so: $(stat -c %s "$so_abs") bytes (module), soname /app0/libvulkan-abs.so"

echo "libps5vk.ps5.a: $(stat -c %s "$work/ps5/libps5vk.ps5.a") bytes"
echo "libvulkan_ps5vk.so: $(stat -c %s "$library") bytes, exports: $exports"
echo "manifest: $manifest"
