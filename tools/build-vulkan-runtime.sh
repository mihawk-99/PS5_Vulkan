#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - build Mesa's common Vulkan runtime.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase B1 (docs/M5_PHASE_B.md). Generates Mesa's Vulkan dispatch,
# entry-point, extension, feature and property sources from vk.xml with
# Mesa's own generators, and builds the util library and lite runtime
# (src/vulkan/{util,runtime}, MIT) of the Mesa 26.2.0 release for the PS5 and
# for the PC, with the same flags as the shader compiler archives:
#
#   .deps/native/vulkan-runtime/lib/libvk_runtime.ps5.a
#   .deps/native/vulkan-runtime/lib/libvk_runtime.a        (PC)
#   .deps/native/vulkan-runtime/include/vulkan/{runtime,util,wsi}/*.h
#   .deps/native/vulkan-runtime/PROVENANCE.txt
#
# Sources: the opengnm-psbc tree is a modified Mesa 26.2.0 whose Vulkan
# runtime headers vk_device.h, vk_instance.h, vk_physical_device.h, vk_queue.h,
# vk_command_buffer.h and vk_command_pool.h are 3-line placeholders, beside
# placeholders for generated headers and edits to other runtime files. The
# runtime sources therefore come from the pinned Mesa release
# (tools/fetch-mesa.sh), while compilation runs against the work copy of the
# tree that tools/build-psbc-ps5.sh prepares, with the tree's include paths, so
# util and compiler headers match the shader compiler archive the driver
# links beside it. src/vulkan/util and vk.xml are identical in both.
#
# Run tools/fetch-mesa.sh and tools/build-psbc-ps5.sh first. Mesa's generators
# need Python's mako: install python-mako and python-markupsafe, or set
# PS5VK_MAKO_PATH to a directory that provides them.
#
# The runtime builds from a copy of src/vulkan/{runtime,util} with each
# generated file beside the sources that include it, as meson's build
# directory has it; files are only replaced when their content changes, so
# rebuilds stay incremental.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
payload="$root/.deps/native/ps5-payload-sdk"
psbc_work=${PSBC_PS5_WORK:-$root/.deps/work/psbc-ps5}
tree="$psbc_work/third_party/opengnm-psbc"
work=${VULKAN_RUNTIME_WORK:-$root/.deps/work/vulkan-runtime}
install="$root/.deps/native/vulkan-runtime"
stubs="$root/tooling/vulkan-runtime"

mesa_version=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["mesa"]["version"])' \
    "$sdk/dependencies.json")
mesa=${MESA_SOURCE_DIR:-$root/.deps/native/mesa/mesa-$mesa_version}
[[ -f $tree/src/compiler/nir/nir_opcodes.h ]] ||
    { echo "missing the compiler work copy $tree; run tools/build-psbc-ps5.sh first" >&2; exit 2; }
[[ -f $mesa/VERSION && $(tr -d '[:space:]' < "$mesa/VERSION") == "$mesa_version" ]] ||
    { echo "missing Mesa $mesa_version sources at $mesa; run tools/fetch-mesa.sh first" >&2; exit 2; }
[[ -x $payload/bin/prospero-clang ]] ||
    { echo "missing payload SDK compiler: $payload/bin/prospero-clang" >&2; exit 2; }

# shellcheck source=tools/mesa-python.sh
source "$root/tools/mesa-python.sh"
mesa_python_init || exit 2

# shellcheck source=tools/ccache.sh
source "$root/tools/ccache.sh"

# --- source stamp -------------------------------------------------------------
# rsync keeps the release's file times, which can be older than objects built
# from other sources, so make alone would not notice a source change. Objects
# are discarded whenever the sources, stubs or flags' origin change.
stamp="Mesa $mesa_version $(sha256sum "$root/.deps/native/mesa/mesa-$mesa_version.tar.xz" | cut -d' ' -f1)
stubs $(cat "$stubs/wsi/"* "$stubs/Makefile" | sha256sum | cut -d' ' -f1)
script $(sha256sum "$root/tools/build-vulkan-runtime.sh" | cut -d' ' -f1)
tree $(git -C "$sdk/third_party/opengnm-psbc" rev-parse HEAD)"
mkdir -p "$work"
if [[ ! -f $work/source-stamp || $(cat "$work/source-stamp") != "$stamp" ]]; then
    find "$work" -maxdepth 1 -type d -name 'out-*' -exec find {} -type f -delete \;
    printf '%s\n' "$stamp" > "$work/source-stamp"
fi

# --- copy of src/vulkan/{runtime,util} and the window-system stubs ----------
src="$work/src/vulkan"
generated_runtime=(vk_cmd_enqueue_entrypoints.c vk_cmd_enqueue_entrypoints.h vk_cmd_queue.c
    vk_cmd_queue.h vk_common_entrypoints.c vk_common_entrypoints.h vk_dispatch_trampolines.c
    vk_dispatch_trampolines.h vk_format_info.c vk_format_info.h vk_physical_device_features.c
    vk_physical_device_features.h vk_physical_device_properties.c vk_physical_device_properties.h
    vk_physical_device_spirv_caps.c vk_synchronization_helpers.c)
generated_util=(vk_dispatch_table.c vk_dispatch_table.h vk_enum_to_str.c vk_enum_to_str.h
    vk_enum_defines.h vk_extensions.c vk_extensions.h vk_struct_type_cast.h)
# Generated files are excluded from the copies, so rsync --delete keeps them.
runtime_excludes=(--exclude='*.o' --exclude=__pycache__)
for name in "${generated_runtime[@]}" "${generated_util[@]}"; do
    runtime_excludes+=(--exclude="/$name")
done
util_excludes=(--exclude='*.o' --exclude=__pycache__)
for name in "${generated_util[@]}"; do
    util_excludes+=(--exclude="/$name")
done
mkdir -p "$src/runtime" "$src/util" "$src/wsi"
rsync -a --delete "${runtime_excludes[@]}" "$mesa/src/vulkan/runtime/" "$src/runtime/"
rsync -a --delete "${util_excludes[@]}" "$mesa/src/vulkan/util/" "$src/util/"
# Only the drm-uapi headers of Mesa's include directory (see the Makefile).
mkdir -p "$work/include/drm-uapi"
rsync -a --delete "$mesa/include/drm-uapi/" "$work/include/drm-uapi/"
cp -p "$stubs/wsi/wsi_common.h" "$stubs/wsi/wsi_common_private.h" "$stubs/wsi/vk_wsi_stubs.c" "$src/wsi/"

# --- generated sources (src/vulkan/{util,runtime}/meson.build) -------------
fresh="$work/generated"
rm -rf "$fresh"
mkdir -p "$fresh/util" "$fresh/runtime"
xml="$mesa/src/vulkan/registry/vk.xml"
gen_util="$mesa/src/vulkan/util"
generate() {
    local log="$fresh/$1.log"
    shift
    python3 "$@" > "$log" 2>&1 || { cat "$log" >&2; echo "generator failed: $*" >&2; exit 1; }
}
u="$fresh/util"
r="$fresh/runtime"
generate dispatch_table "$gen_util/vk_dispatch_table_gen.py" --xml "$xml" \
    --out-c "$u/vk_dispatch_table.c" --out-h "$u/vk_dispatch_table.h" --beta false
generate enum_to_str "$gen_util/gen_enum_to_str.py" --xml "$xml" --out-c "$u/vk_enum_to_str.c" \
    --out-h "$u/vk_enum_to_str.h" --out-d "$u/vk_enum_defines.h" --beta false
generate struct_type_cast "$gen_util/vk_struct_type_cast_gen.py" --xml "$xml" \
    --out "$u/vk_struct_type_cast.h" --beta false
generate extensions "$gen_util/vk_extensions_gen.py" --xml "$xml" \
    --out-c "$u/vk_extensions.c" --out-h "$u/vk_extensions.h"
generate common_entrypoints "$gen_util/vk_entrypoints_gen.py" --xml "$xml" --proto --weak \
    --out-h "$r/vk_common_entrypoints.h" --out-c "$r/vk_common_entrypoints.c" \
    --prefix vk_common --beta false
generate cmd_queue "$gen_util/vk_cmd_queue_gen.py" --xml "$xml" \
    --out-c "$r/vk_cmd_queue.c" --out-h "$r/vk_cmd_queue.h" --beta false
generate cmd_enqueue_entrypoints "$gen_util/vk_entrypoints_gen.py" --xml "$xml" --proto --weak \
    --out-h "$r/vk_cmd_enqueue_entrypoints.h" --out-c "$r/vk_cmd_enqueue_entrypoints.c" \
    --prefix vk_cmd_enqueue --prefix vk_cmd_enqueue_unless_primary --beta false
generate dispatch_trampolines "$gen_util/vk_dispatch_trampolines_gen.py" --xml "$xml" \
    --out-c "$r/vk_dispatch_trampolines.c" --out-h "$r/vk_dispatch_trampolines.h" --beta false
generate physical_device_features "$gen_util/vk_physical_device_features_gen.py" --xml "$xml" \
    --out-c "$r/vk_physical_device_features.c" --out-h "$r/vk_physical_device_features.h" --beta false
generate physical_device_properties "$gen_util/vk_physical_device_properties_gen.py" --xml "$xml" \
    --out-c "$r/vk_physical_device_properties.c" --out-h "$r/vk_physical_device_properties.h" \
    --beta false
generate physical_device_spirv_caps "$gen_util/vk_physical_device_spirv_caps_gen.py" --xml "$xml" \
    --out-c "$r/vk_physical_device_spirv_caps.c" --beta false
generate synchronization_helpers "$gen_util/vk_synchronization_helpers_gen.py" --xml "$xml" \
    --out-c "$r/vk_synchronization_helpers.c" --beta false
generate format_info "$mesa/src/vulkan/runtime/vk_format_info_gen.py" --xml "$xml" \
    --out-c "$r/vk_format_info.c" --out-h "$r/vk_format_info.h"
for part in util runtime; do
    for file in "$fresh/$part"/*.[ch]; do
        cmp -s "$file" "$src/$part/${file##*/}" || cp "$file" "$src/$part/"
    done
done

# --- both targets ------------------------------------------------------------
print_cflags() {
    make -s --no-print-directory -C "$tree" -f "$1" \
        -f <(printf 'print-cflags:\n\t@echo $(CFLAGS)\n') PS5_PAYLOAD_SDK="$payload" print-cflags
}
ps5_cflags=$(print_cflags "$psbc_work/toolchain/opengnm-psbc-ps5.mak")
# The PC archive also goes into the driver's shared library for the Vulkan
# loader (tools/build-driver.sh), so it is position-independent.
host_cflags="$(print_cflags "$sdk/toolchain/opengnm-psbc-host.mak") -fPIC"
# The payload SDK's FreeBSD sys/cdefs.h caps ISO C visibility at C99 whenever
# _XOPEN_SOURCE is set, which hides C11's max_align_t that vk_alloc.c uses.
# Without it the default environment already exposes POSIX 2008, XSI 700,
# BSD and C11 interfaces.
ps5_cflags=${ps5_cflags//-D_XOPEN_SOURCE=700/}

build_target() {
    local target=$1 archive=$2 cc=$3 ar=$4 cflags=$5
    local log="$work/build-$target.log"
    # -k: a failing build still compiles every other object, so one run
    # reports every broken source.
    if ! make -k -C "$tree" -f "$stubs/Makefile" -j"$(nproc)" SRC="$work/src" OUT="$work/out-$target" \
            ARCHIVE="$archive" CC="$cc" AR="$ar" CFLAGS="$cflags" MESA_VERSION="$mesa_version" \
            > "$log" 2>&1; then
        return 1
    fi
    # Written to a file because each target runs in its own subshell.
    printf '%s %s\n' "$(grep -cE ' -c [^ ]+\.c ' "$log" || true)" \
        "$(grep -c 'warning:' "$log" || true)" > "$work/counts-$target"
}
declare -A compiled warnings
started=$(date +%s)
# The two targets share their inputs and nothing else, and each one alone
# leaves cores idle: the PS5 target averages five busy cores and the host
# target six. Building them at once fills the machine instead.
build_target ps5 libvk_runtime.ps5.a "$(ps5vk_ccache "$payload/bin/prospero-clang")" \
    "$payload/bin/prospero-ar" "$ps5_cflags" &
ps5_pid=$!
build_target host libvk_runtime.a "$(ps5vk_ccache gcc)" ar "$host_cflags" &
host_pid=$!
failed=0
wait "$ps5_pid" || failed=1
wait "$host_pid" || failed=1
seconds=$(( $(date +%s) - started ))
if ((failed)); then
    for target in ps5 host; do
        grep -E 'error' "$work/build-$target.log" 2>/dev/null | head -n 40
    done
    echo "the Vulkan runtime build failed; see $work/build-ps5.log and $work/build-host.log" >&2
    exit 1
fi
for target in ps5 host; do
    counts=$(cat "$work/counts-$target")
    compiled[$target]=${counts%% *}
    warnings[$target]=${counts##* }
done

# --- install -----------------------------------------------------------------
rm -rf "$install"
mkdir -p "$install/lib" "$install/include/vulkan"
cp "$work/out-ps5/libvk_runtime.ps5.a" "$work/out-host/libvk_runtime.a" "$install/lib/"
"$payload/bin/prospero-strip" --strip-debug "$install/lib/libvk_runtime.ps5.a"
strip --strip-debug "$install/lib/libvk_runtime.a"
for part in runtime util wsi; do
    mkdir -p "$install/include/vulkan/$part"
    (cd "$src/$part" && find . -name '*.h' -exec cp --parents {} "$install/include/vulkan/$part/" \;)
done
{
    echo "Mesa Vulkan util library and lite runtime (MIT) built by tools/build-vulkan-runtime.sh"
    echo "sources: Mesa $mesa_version release ($(sha256sum "$root/.deps/native/mesa/mesa-$mesa_version.tar.xz" | cut -d' ' -f1)), src/vulkan/{runtime,util}"
    echo "include paths: opengnm-psbc $(git -C "$sdk/third_party/opengnm-psbc" rev-parse HEAD) (ps5-opengl SDK $(git -C "$sdk" rev-parse HEAD 2>/dev/null || echo unknown))"
    echo "vk.xml: $(sha256sum "$xml" | cut -d' ' -f1)"
    echo "mako: $mako_version"
    echo "window-system stubs: $(cat "$stubs/wsi/"* | sha256sum | cut -d' ' -f1)"
    echo "Makefile: $(sha256sum "$stubs/Makefile" | cut -d' ' -f1)"
    for target in ps5 host; do
        echo "$target this run: ${compiled[$target]} sources compiled, ${warnings[$target]} compiler warnings"
    done
    for archive in "$install/lib/"*.a; do
        echo "$(basename "$archive"): sha256 $(sha256sum "$archive" | cut -d' ' -f1), $(stat -c %s "$archive") bytes, debug information stripped"
    done
} > "$install/PROVENANCE.txt"
cat "$install/PROVENANCE.txt"
echo "built in ${seconds} s; installed to $install"
