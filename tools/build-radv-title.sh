#!/usr/bin/env bash
# PS5 Vulkan - build the RADV test title (PPSA99014).
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The title that runs RADV on the console (docs/VULKAN_1_4_PLAN.md, Phase 1):
# radv/radv_smoke.c linked with the RADV archive my Mesa fork builds
# (../PS5_Mesa, -Dradv-winsys=ps5), into dist/PPSA99014 beside the runner's
# dist/PPSA99988, which it leaves alone. The link follows tools/build.sh: the
# repository's CRT and C++ runtime, the AGC link stubs, and the payload SDK's
# libc++ for ACO, with tooling/psbc/ps5-pie-unwind.ld as psbc-link.sh uses it.
#
# RADV_ARCHIVE names the archive (default: the fork's build-ps5 tree). The link
# is tools/radv-link.sh's.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mesa=${PS5_MESA_FORK:-$root/../PS5_Mesa}
archive=${RADV_ARCHIVE:-$mesa/build-ps5/src/amd/vulkan/libvulkan_radeon.a}
sdk_root="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
tool="$root/build/host/ps5-native-tool"
work="$root/build/radv"
param=${PARAM_PATH:-"$root/sce_sys/param-radv.json"}

for file in "$archive" "$tool" "$param" "$sdk_root/bin/prospero-lld"; do
    [[ -e $file ]] || { echo "missing $file" >&2; exit 2; }
done
command -v glslangValidator >/dev/null || { echo "glslangValidator was not found" >&2; exit 2; }

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

mkdir -p "$work/obj" "$work/stubs" "$work/gen"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" "$@"; }

# The smoke test's shaders, as SPIR-V arrays.
: > "$work/gen/radv_smoke_shaders.h.tmp"
for stage in comp vert frag; do
    glslangValidator -V --target-env vulkan1.0 --vn "radv_smoke_$stage" \
        "$root/radv/shaders/smoke.$stage" -o "$work/gen/smoke_$stage.h" > /dev/null
    cat "$work/gen/smoke_$stage.h" >> "$work/gen/radv_smoke_shaders.h.tmp"
done
for shader in tess.vert tess.tesc tess_coord.tese tess_patch.tese tess_varying.vert tess_varying.tesc \
        tess_varying.tese colour.frag tess_level.tesc gs_points.vert gs_16.geom gs_32.geom gs_64.geom \
        gs_100.geom gs_128.geom gs_primid.geom gs_colour.vert gs_colour.geom gs_primid_fixed.geom gs_record.geom \
        scratch.frag bary.vert bary.frag bary6.vert bary_survey.vert bary_coord.frag bary_line.frag full.vert rate.frag prim_id.vert prim_id.frag prim_id_raw.frag \
        gs_quads.vert gs_triangles.geom ptr_write.comp; do
    name=${shader//./_}
    glslangValidator -V --target-env vulkan1.0 --vn "radv_smoke_$name" \
        "$root/radv/shaders/$shader" -o "$work/gen/smoke_$name.h" > /dev/null
    cat "$work/gen/smoke_$name.h" >> "$work/gen/radv_smoke_shaders.h.tmp"
done
# Mesh shaders need SPIR-V 1.4.
for shader in mesh_ticket.mesh mesh_colour.frag task_payload.task mesh_payload.mesh; do
    name=${shader//./_}
    glslangValidator -V --target-env vulkan1.2 --vn "radv_smoke_$name" \
        "$root/radv/shaders/$shader" -o "$work/gen/smoke_$name.h" > /dev/null
    cat "$work/gen/smoke_$name.h" >> "$work/gen/radv_smoke_shaders.h.tmp"
done
mv "$work/gen/radv_smoke_shaders.h.tmp" "$work/gen/radv_smoke_shaders.h"

cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
    -I "$mesa/include" -I "$work/gen" -c "$root/radv/radv_smoke.c" -o "$work/obj/radv_smoke.o"
cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"

# AGC comes from system modules; these host-link stubs only name the imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$root/$source" -o "$work/obj/${library}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=tools/radv-link.sh
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk_root" "$archive" || exit 2
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/radv_smoke.o" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf

app="$root/dist/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic "$fself_magic"
cp "$param" "$app/sce_sys/param.json"
if [[ -n ${RADV_SMOKE_ENV:-} ]]; then
    cp "$RADV_SMOKE_ENV" "$app/radv-smoke-env.txt"
fi
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    [[ -f $root/sce_sys/$asset ]] && cp "$root/sce_sys/$asset" "$app/sce_sys/$asset"
done
[[ -f $root/runtime/libc.prx ]] || bash "$root/tools/rebuild-libc.sh"
(cd "$root/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$root/runtime/libc.prx" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/sce_module/libc.prx" > /dev/null
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
printf 'RADV title: %s (%s bytes; archive %s)\n' "$app" "$(stat -c %s "$app/eboot.bin")" \
    "$(git -C "$mesa" describe --always --dirty --abbrev=11 --exclude='*' 2>/dev/null || echo unknown)"
