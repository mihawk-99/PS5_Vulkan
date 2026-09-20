#!/usr/bin/env bash
# ps5-native-app-boilerplate - Native Linux application build.
# Copyright (C) 2026 BlackBearReloaded
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiles, links, signs, validates, and assembles the root skeleton app.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
format=${1:-Folder}
format=${format,,}
case "$format" in folder|ffpkg|ffpfsc|all) ;; *)
    echo "usage: tools/build.sh [Folder|Ffpkg|Ffpfsc|All]" >&2
    exit 2
esac

for command in python3 sha256sum; do
    command -v "$command" >/dev/null || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
bash "$root/tools/setup-native-dependencies.sh" >/dev/null

param_input=${PARAM_PATH:-sce_sys/param.json}
if [[ $param_input = /* ]]; then param="$param_input"; else param="$root/$param_input"; fi
[[ -f $param ]] || { echo "param.json was not found: $param" >&2; exit 2; }
title_id=$(python3 - "$param" <<'PY'
import json, re, sys

with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)

title_id = value.get("titleId", "")
concept_id = value.get("conceptId", "")
content_id = value.get("contentId", "")
if not re.fullmatch(r"PPSA\d{5}", title_id):
    raise SystemExit("param.json titleId must use PPSA followed by five digits")
if not re.fullmatch(r"\d{5}", concept_id):
    raise SystemExit("param.json conceptId must contain five digits")
if (not re.fullmatch(r"[A-Z]{2}\d{4}-PPSA\d{5}_00-[A-Z0-9]{16}", content_id)
        or title_id not in content_id):
    raise SystemExit("param.json contentId must be valid and contain titleId")
if not re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", value.get("contentVersion", "")):
    raise SystemExit("param.json contentVersion must use NN.NNN.NNN")
if not re.fullmatch(r"\d{2}\.\d{2}", value.get("masterVersion", "")):
    raise SystemExit("param.json masterVersion must use NN.NN")
size = value.get("downloadDataSize")
if isinstance(size, bool) or not isinstance(size, int) or size < 0:
    raise SystemExit("param.json downloadDataSize must be a non-negative integer")

category = value.get("applicationCategoryType")
badge = value.get("contentBadgeType")
if (category, badge) not in {(0, 1), (65536, 2)}:
    raise SystemExit("param.json category and badge must describe a game or media app")
if category == 0:
    intents = value.get("gameIntent", {}).get("permittedIntents", [])
    if not any(item.get("intentType") == "launchActivity" for item in intents):
        raise SystemExit("game param.json must permit the launchActivity intent")
elif "gameIntent" in value:
    raise SystemExit("media param.json must not contain gameIntent")

localized = value.get("localizedParameters", {})
language = localized.get("defaultLanguage", "")
title = localized.get(language, {}).get("titleName", "")
if not isinstance(title, str) or not title.strip():
    raise SystemExit("param.json default-language titleName cannot be empty")
print(title_id)
PY
)

# Loader/container constants validated on firmware 6.02 and 12.70. These are
# deliberately separate from the public application version in param.json.
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

bash "$root/tools/validate-assets.sh" "$root/sce_sys"

sdk_root="$root/.deps/native/ps5-payload-sdk"
zlib_root="$root/.deps/native/zlib/root"
zlib_archive=$(find "$zlib_root" -type f -name libz.a -print -quit)
cxx=${CXX:-}
if [[ -z $cxx ]]; then
    cxx=$(command -v clang++ || command -v clang++-18)
fi
[[ -n $cxx ]] || { echo "Clang++ was not found" >&2; exit 2; }

build="$root/build"
dist="$root/dist"
native="$root/tooling/native"
tool="$build/host/ps5-native-tool"
mkdir -p "$build/host" "$build/obj" "$dist"
# The host tool the whole build runs through is worth about five seconds of the
# run, so it is rebuilt only when one of its own inputs is newer, the same
# `find -newer` test the driver-archive guard below uses.
stale=
if [[ ! -x $tool ]]; then
    stale=$tool
else
    for input in "$zlib_archive" "$native/native_app_builder.cpp" \
        "$native/self_container.cpp" "$native/elf_object.cpp" \
        "$native/sce_module_writer.cpp" "$native"/*.hpp "$native"/*.h; do
        [[ -e $input && $input -nt $tool ]] || continue
        stale=$input
        break
    done
fi
if [[ -n $stale ]]; then
    "$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
        -I "$zlib_root/usr/include" \
        "$native/native_app_builder.cpp" "$native/self_container.cpp" \
        "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        "$zlib_archive" -o "$tool"
fi

mapfile -d '' -t source_paths < <(
    find "$root/src" -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' \) \
        -print0 | sort -z
)
sources=()
for source in "${source_paths[@]}"; do
    sources+=("${source#"$root/"}")
done
(( ${#sources[@]} > 0 )) || { echo "src/ has no C or C++ sources" >&2; exit 2; }

definitions=()
includes=()
archives=()
pacbrew_packages=()
pacbrew_includes=()
pacbrew_archives=()
[[ -z ${APP_DEFINITIONS:-} ]] || read -r -a definitions <<< "$APP_DEFINITIONS"
[[ -z ${APP_INCLUDE_PATHS:-} ]] || read -r -a includes <<< "$APP_INCLUDE_PATHS"
[[ -z ${APP_STATIC_ARCHIVES:-} ]] || read -r -a archives <<< "$APP_STATIC_ARCHIVES"
[[ -z ${PACBREW_PACKAGES:-} ]] || read -r -a pacbrew_packages <<< "$PACBREW_PACKAGES"
[[ -z ${PACBREW_INCLUDE_PATHS:-} ]] || read -r -a pacbrew_includes <<< "$PACBREW_INCLUDE_PATHS"
[[ -z ${PACBREW_STATIC_ARCHIVES:-} ]] || read -r -a pacbrew_archives <<< "$PACBREW_STATIC_ARCHIVES"

pacbrew_cflags=()
pacbrew_libs=()
if (( ${#pacbrew_packages[@]} > 0 || ${#pacbrew_includes[@]} > 0 || ${#pacbrew_archives[@]} > 0 )); then
    pacbrew_resolution=$(bash "$root/tools/setup-pacbrew-dependencies.sh" \
        --resolve "${pacbrew_packages[@]}")
    mapfile -d '' -t pacbrew_cflags < <(python3 -c \
        'import json,sys; [print(v, end="\0") for v in json.loads(sys.argv[1])["cflags"]]' \
        "$pacbrew_resolution")
    mapfile -d '' -t pacbrew_libs < <(python3 -c \
        'import json,sys; [print(v, end="\0") for v in json.loads(sys.argv[1])["libs"]]' \
        "$pacbrew_resolution")
    pacbrew_root=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["root"])' \
        "$pacbrew_resolution")
    for include in "${pacbrew_includes[@]}"; do
        [[ $include =~ ^[A-Za-z0-9_.+-]+(/[A-Za-z0-9_.+-]+)*$ &&
            -d $pacbrew_root/user/homebrew/$include ]] || {
            echo "invalid PacBrew include path: $include" >&2; exit 2;
        }
        pacbrew_cflags+=("-I$pacbrew_root/user/homebrew/$include")
    done
    for archive in "${pacbrew_archives[@]}"; do
        [[ $archive =~ ^[A-Za-z0-9_.+-]+(/[A-Za-z0-9_.+-]+)*\.a$ &&
            -f $pacbrew_root/user/homebrew/$archive ]] || {
            echo "invalid PacBrew static archive: $archive" >&2; exit 2;
        }
        pacbrew_libs+=("$pacbrew_root/user/homebrew/$archive")
    done
    printf 'PacBrew dependencies: %s\n' "${pacbrew_packages[*]:-(manual archives)}"
fi

# AGC_SHADER_COMPILER=1 links the PS5 shader compiler (docs/M5_PHASE_A.md)
# with the recipe tools/check-psbc-link.sh proves.
shader_compiler=0
for definition in "${definitions[@]}"; do
    [[ $definition == AGC_SHADER_COMPILER=1 ]] && shader_compiler=1
done
linker_script=(-T "$native/ps5-pie.ld")
compiler_include=()
compiler_link_inputs=()
if (( shader_compiler )); then
    # shellcheck source=tools/psbc-link.sh
    source "$root/tools/psbc-link.sh"
    psbc_link_recipe "$root" "$sdk_root" || exit 2
    linker_script=("${psbc_linker_script[@]}")
    compiler_include=("${psbc_include[@]}")
    compiler_link_inputs=("${psbc_link_inputs[@]}")
fi

# AGC_VULKAN_DRIVER=1 links the PS5 Vulkan driver into the test runner
# (docs/M5_PHASE_B.md, B7) as tools/check-driver.sh links it: the driver and
# Mesa's runtime whole, the driver's copy of the shader compiler in place of
# libpsbc.ps5.a, and the B7 program driver/tests/ps5vk_triangle.c. The runner
# reads swapchain images back through driver/ps5vk_debug.h (C1). Run
# tools/build-driver.sh first.
vulkan_driver=0
for definition in "${definitions[@]}"; do
    [[ $definition == AGC_VULKAN_DRIVER=1 ]] && vulkan_driver=1
done
driver_include=()
driver_link_flags=()
driver_link_inputs=()
if (( vulkan_driver )); then
    (( shader_compiler )) || { echo "AGC_VULKAN_DRIVER=1 needs AGC_SHADER_COMPILER=1" >&2; exit 2; }
    # shellcheck source=tools/sdk-root.sh
    source "$root/tools/sdk-root.sh"
    opengl_sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
    driver_archives=("$root/build/driver/ps5/libps5vk.ps5.a"
        "$root/.deps/native/vulkan-runtime/lib/libvk_runtime.ps5.a")
    driver_compiler="$root/build/driver/ps5/libpsbc_driver.ps5.a"
    for file in "${driver_archives[@]}" "$driver_compiler"; do
        [[ -f $file ]] || { echo "missing $file; run tools/build-driver.sh" >&2; exit 2; }
    done
    # The archives are prebuilt: a driver source edited after the last driver
    # build would be linked stale, which is round 5's first console run (the
    # archive predated driver/ps5vk_queue.c, so the blit it ran was not the one
    # the source decided; docs/M5_PHASE_C.md). Refuse to link a stale archive.
    # Only the sources the driver archive is built from count (driver/Makefile's
    # DRIVER_SRCS): driver/tests is compiled into the runner instead, and Mesa's
    # runtime beside it by its own script.
    for file in "$root/build/driver/ps5/libps5vk.ps5.a" "$driver_compiler"; do
        newer=$(find "$root/driver" -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' \) \
            -newer "$file" -print -quit)
        [[ -z $newer ]] || {
            echo "$file is older than $newer; run tools/build-driver.sh" >&2; exit 2;
        }
    done
    driver_include=(-I "$opengl_sdk/third_party/Vulkan-Headers/include" -I "$root/driver/tests"
        -I "$root/driver")
    # Mesa's weak entry points resolve at link time (tools/check-vulkan-runtime.sh).
    driver_link_flags=(--no-dynamic-linker -z nodynamic-undefined-weak)
    driver_link_inputs=(--whole-archive "${driver_archives[@]}" --no-whole-archive)
    compiler_link_inputs=(-L "$sdk_root/target/lib" --start-group "$driver_compiler"
        "$root/.deps/native/psbc/lib/libpsbc_support.ps5.a" "${psbc_runtime_archives[@]}"
        --end-group)
fi

# The sources are independent compilation units, so they compile concurrently.
# Validation stays ahead of the fan-out: a rejected source, definition or
# include directory is reported before any compiler starts. Diagnoses are
# captured per source so concurrent compilers cannot interleave their errors,
# and the first failure in submission order is the one reported.
compile_dir="$build/obj/logs"
mkdir -p "$compile_dir"
trap 'rm -rf -- "$compile_dir"' EXIT
# An interrupt reaches both the shell and the compiler subshells, which share
# its process group; killing the group makes sure none is left compiling.
trap 'kill 0' INT TERM
compile_sources=()
compile_objects=()
compile_logs=()
for source in "${sources[@]}"; do
    [[ $source =~ ^src/[A-Za-z0-9_./-]+\.(c|cc|cpp)$ && -f $root/$source ]] || {
        echo "invalid source: $source" >&2; exit 2;
    }
    if [[ $source == *.c ]]; then standard=-std=c11; else standard=-std=c++20; fi
    args=("$standard" -O2 -Wall -Wextra -ffunction-sections -fdata-sections)
    [[ $source == *.c ]] || args+=(-fno-exceptions -fno-rtti)
    for definition in "${definitions[@]}"; do
        [[ $definition =~ ^[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_]+)?$ ]] || {
            echo "invalid compile definition: $definition" >&2; exit 2;
        }
        args+=("-D$definition")
    done
    for include in "${includes[@]}"; do
        [[ $include =~ ^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$ && -d $root/$include ]] || {
            echo "invalid include path: $include" >&2; exit 2;
        }
        args+=("-I$root/$include")
    done
    args+=("${pacbrew_cflags[@]}" "${compiler_include[@]}" "${driver_include[@]}")
    compile_sources+=("$source")
    compile_objects+=("$build/obj/${source//\//_}.o")
    log="$compile_dir/${source//\//_}.txt"
    compile_logs+=("$log")
    PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" \
        "${args[@]}" -c "$root/$source" -o "${compile_objects[-1]}" > "$log" 2>&1 &
done
compile_failed=0
for compile_job in $(jobs -p); do
    wait "$compile_job" || compile_failed=1
done
if (( compile_failed )); then
    for index in "${!compile_logs[@]}"; do
        [[ -s ${compile_logs[index]} ]] || continue
        echo "compilation failed: ${compile_sources[index]}" >&2
        cat -- "${compile_logs[index]}" >&2
        exit 1
    done
    echo "a source failed to compile" >&2
    exit 1
fi
objects=("${compile_objects[@]}")
if (( vulkan_driver )); then
    object="$build/obj/driver_tests_ps5vk_triangle.c.o"
    PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" \
        -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections "${driver_include[@]}" \
        -c "$root/driver/tests/ps5vk_triangle.c" -o "$object"
    objects+=("$object")
    # Phase D2's compute program: the case that dispatches through the driver
    # (src/diagnostics.cpp, d2-compute) shares it with the PC test.
    object="$build/obj/driver_tests_ps5vk_compute.c.o"
    PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" \
        -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections "${driver_include[@]}" \
        -c "$root/driver/tests/ps5vk_compute.c" -o "$object"
    objects+=("$object")
fi

PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" \
    -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections \
    -c "$native/app_crt.cpp" -o "$build/obj/app_crt.o"

PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" \
    -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections \
    -c "$native/app_cpp_runtime.cpp" -o "$build/obj/app_cpp_runtime.o"

# AGC is provided by PS5 system modules rather than the public libc stubs.
# These tiny host-link objects let the native converter record the imports;
# their bodies are never packaged or executed on the console.
build_system_link_stub() {
    local library=$1
    local source=$2
    local object="$build/obj/${library}_link_stub.o"
    local output="$build/stubs/${library}.so"

    mkdir -p "${output%/*}"
    PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" \
        -std=c11 -O2 -fPIC -ffunction-sections -fdata-sections \
        -c "$root/$source" -o "$object"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$output" "$object"
    printf '%s\n' "$output"
}

canary_profile=0
driver_canary_profile=0
for definition in "${definitions[@]}"; do
    [[ $definition == AGC_LINKED_CANARY=1 ]] && canary_profile=1
    [[ $definition == AGC_DRIVER_CANARY=1 ]] && driver_canary_profile=1
done
if (( driver_canary_profile )); then
    agc_source=vendor/ps5/sdk/stubs/agc_suspend_import_canary_link_stub.c
    agc_driver_source=vendor/ps5/sdk/stubs/agc_driver_import_canary_link_stub.c
elif (( canary_profile )); then
    agc_source=vendor/ps5/sdk/stubs/agc_canary_link_stub.c
    agc_driver_source=vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
else
    agc_source=vendor/ps5/sdk/stubs/agc_link_stub.c
    agc_driver_source=vendor/ps5/sdk/stubs/agc_driver_link_stub.c
fi
agc_stub=$(build_system_link_stub libSceAgc "$agc_source")
agc_driver_stub=$(build_system_link_stub libSceAgcDriver "$agc_driver_source")

link_inputs=("$build/obj/app_crt.o" "$build/obj/app_cpp_runtime.o" "${objects[@]}")
link_inputs+=("$agc_stub" "$agc_driver_stub")
for archive in "${archives[@]}"; do
    [[ $archive =~ ^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*\.a$ && -f $root/$archive ]] || {
        echo "invalid static archive: $archive" >&2; exit 2;
    }
    link_inputs+=("$root/$archive")
done
if (( ${#pacbrew_libs[@]} > 0 )); then
    link_inputs+=(--start-group "${pacbrew_libs[@]}" --end-group)
fi
link_inputs+=("${driver_link_inputs[@]}" "${compiler_link_inputs[@]}")
"$sdk_root/bin/prospero-lld" "${linker_script[@]}" --eh-frame-hdr "${driver_link_flags[@]}" \
    --version-script "$native/app-symbols.map" \
    --exclude-libs=ALL -L "$build/obj" \
    -e _start -o "$build/llvm-pie.elf" "${link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
"$tool" link --in "$build/llvm-pie.elf" --out "$build/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$agc_stub" \
    --stub "$agc_driver_stub" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf

app="$dist/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$build/eboot.elf" --out "$app/eboot.bin" \
    --magic "$fself_magic"

cp "$param" "$app/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    [[ -f $root/sce_sys/$asset ]] && cp "$root/sce_sys/$asset" "$app/sce_sys/$asset"
done
[[ ! -d $root/assets ]] || cp -a "$root/assets" "$app/assets"
[[ ! -d $root/probes ]] || cp -a "$root/probes" "$app/probes"
# The loaderless delivery (docs/M5_REFERENCE.md, E2): the shared object a
# frontend dlopens, beside eboot.bin under the name RetroArch's Vulkan driver
# asks for first. tools/build-driver.sh builds and signs it; only a title that
# links the driver carries it.
if (( vulkan_driver )); then
    ps5_vulkan_so="$root/build/driver/ps5/libvulkan.so.1"
    [[ -f $ps5_vulkan_so ]] || { echo "missing $ps5_vulkan_so; run tools/build-driver.sh" >&2; exit 2; }
    cp "$ps5_vulkan_so" "$app/libvulkan.so.1"
    # The title's module directory is the other path the rtld may search; the
    # smoke-test case tries both.
    cp "$ps5_vulkan_so" "$app/sce_module/libvulkan.so.1"
    # The same library under the soname-as-path shape the payload SDK's own
    # hello_so sample uses: there the file's absolute path is the soname and the
    # load name is that path. tools/build-driver.sh links this copy.
    [[ -f $root/build/driver/ps5/libvulkan-abs.so ]] &&
        cp "$root/build/driver/ps5/libvulkan-abs.so" "$app/libvulkan-abs.so"
    # The e2-module-load case's controls: a trivial shared object built the way
    # the payload SDK's hello_so sample builds its libraries, so a failed
    # dlopen of the Vulkan module can be told apart from a title that cannot
    # dlopen a file from its own folder at all. It ships in the three shapes the
    # loader could insist on: bare, signed into a SELF container the way
    # eboot.bin is, and with the absolute path in the soname.
    PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" -std=c11 -O2 -fPIC \
        -c "$root/driver/tests/dlfcn_control.c" -o "$build/obj/dlfcn_control.o"
    "$sdk_root/bin/prospero-lld" --shared -soname libps5vk-control.so \
        -o "$app/libps5vk-control.so" "$build/obj/dlfcn_control.o"
    "$tool" self --sign --in "$app/libps5vk-control.so" \
        --out "$app/libps5vk-control-signed.so" --magic "$fself_magic"
    "$sdk_root/bin/prospero-lld" --shared -soname /app0/libps5vk-control-abs.so \
        -o "$app/libps5vk-control-abs.so" "$build/obj/dlfcn_control.o"
fi

[[ -f $root/runtime/libc.prx ]] || bash "$root/tools/rebuild-libc.sh"
(cd "$root/runtime" && sha256sum --check --strict libc.prx.sha256)
runtime_modules=("$root/runtime/libc.prx")
additional_runtime=()
[[ -z ${APP_RUNTIME_MODULES:-} ]] || read -r -a additional_runtime <<< "$APP_RUNTIME_MODULES"
for source in "${additional_runtime[@]}"; do
    [[ $source =~ ^\.local/runtime/[A-Za-z0-9._-]+\.prx$ && -f $root/$source ]] || {
        echo "invalid runtime module: $source" >&2; exit 2;
    }
    runtime_modules+=("$root/$source")
done
declare -A runtime_names=()
for input in "${runtime_modules[@]}"; do
    name=${input##*/}
    [[ $name =~ ^[A-Za-z0-9._-]+\.prx$ && -z ${runtime_names[$name]+present} ]] || {
        echo "invalid or duplicate runtime module name: $name" >&2; exit 2;
    }
    runtime_names[$name]=present
    magic=$(python3 - "$input" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as stream:
    print(f"{struct.unpack('<I', stream.read(4))[0]:08x}")
PY
)
    if [[ $magic == 1d3d154f || $magic == eef51454 ]]; then
        cp "$input" "$app/sce_module/$name"
    else
        "$tool" self --sign --in "$input" --out "$app/sce_module/$name"
    fi
    "$tool" self --inspect --file "$app/sce_module/$name"
done
"$tool" self --inspect --file "$app/eboot.bin"

if [[ $format == ffpkg || $format == all ]]; then
    ufs2tool=$(bash "$root/tools/setup-packaging-dependencies.sh" ffpkg)
    rm -f -- "$dist/$title_id.ffpkg"
    "$ufs2tool" makefs -S 4096 -b 20% -t ffs \
        -o version=2,bsize=32768,fsize=4096,minfree=0,softupdates=0,optimization=space \
        "$dist/$title_id.ffpkg" "$app"
    python3 - "$dist/$title_id.ffpkg" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as stream:
    stream.seek(0x1055c)
    if struct.unpack("<I", stream.read(4))[0] != 0x19540119:
        raise SystemExit("FFPKG is missing the UFS2 superblock magic")
PY
fi
if [[ $format == ffpfsc || $format == all ]]; then
    mkpfs=$(bash "$root/tools/setup-packaging-dependencies.sh" ffpfsc)
    rm -f -- "$dist/$title_id.ffpfsc"
    "$mkpfs" pack folder --no-adjust-output-file-extension \
        --version PS5 --verify "$app" "$dist/$title_id.ffpfsc"
fi

printf 'Build complete.\nApp folder: %s\n' "$app"
[[ $format != ffpkg && $format != all ]] || printf 'FFPKG:     %s\n' "$dist/$title_id.ffpkg"
[[ $format != ffpfsc && $format != all ]] || printf 'FFPFSC:    %s\n' "$dist/$title_id.ffpfsc"
