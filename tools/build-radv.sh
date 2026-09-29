#!/usr/bin/env bash
# PS5 Vulkan - build RADV from my Mesa fork at its pinned revision.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The RADV port lives in my Mesa fork, ../PS5_Mesa (branch main: the Mesa
# 26.2.0 release with a PS5 winsys, -Dradv-winsys=ps5). The pinned revision is
# exported with git archive, so a build never depends on the fork's working
# tree, and built with meson for the console (tooling/radv/ps5-cross.ini):
#
#   .deps/native/radv/lib/libvulkan_radeon.ps5.a   RADV, ACO, NIR and Mesa's
#                                                  Vulkan runtime in one archive
#   .deps/native/radv/include/vulkan/              the headers it was built with
#   .deps/native/radv/PROVENANCE.txt
#
# That is the debug build, with Mesa's assertions and NIR validation, for the
# smoke test and the CTS. `tools/build-radv.sh release` builds the same
# revision without them into .deps/native/radv-release, for the titles: the
# checks cost 5 to 6 times the shader compile time (docs/HARDWARE_FINDINGS.md).
#
# Titles link the archive with tools/radv-link.sh. While a change to the fork
# is being worked on, RADV_ARCHIVE can name the fork's own build instead
# (../PS5_Mesa/build-ps5/src/amd/vulkan/libvulkan_radeon.a).

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mesa_fork="${PS5_MESA_FORK:-$root/../PS5_Mesa}"
mesa_revision=5d9f448ae6cb063ce5f12457abaab1505b9906c2
variant=${1:-debug}
sdk="$root/.deps/native/ps5-payload-sdk"
source_tree="$root/.deps/work/radv-src"
case $variant in
    debug)
        build="$root/.deps/work/radv-build-ps5"
        install="$root/.deps/native/radv"
        ndebug=false
        ;;
    release)
        build="$root/.deps/work/radv-build-ps5-release"
        install="$root/.deps/native/radv-release"
        ndebug=true
        ;;
    *) echo "usage: tools/build-radv.sh [debug|release]" >&2; exit 2 ;;
esac
meson=${MESON:-$(command -v meson || echo "$HOME/.local/bin/meson")}
ninja=${NINJA:-$(command -v ninja || echo "$HOME/.local/bin/ninja")}

[[ -x $meson && -x $ninja ]] || { echo "meson and ninja are needed (uv tool install meson ninja)" >&2; exit 2; }
command -v rsync > /dev/null || { echo "rsync is needed" >&2; exit 2; }
[[ -f $sdk/.ps5-sdk-revision ]] || { echo "run tools/setup-native-dependencies.sh first" >&2; exit 2; }
if [[ -f $install/PROVENANCE.txt ]] && grep -q "^revision: $mesa_revision$" "$install/PROVENANCE.txt" &&
    grep -q "^sdk: $(cat "$sdk/.ps5-sdk-revision")$" "$install/PROVENANCE.txt"; then
    echo "==> [radv] $install is RADV ($variant) at ${mesa_revision:0:12}"
    exit 0
fi
git -C "$mesa_fork" cat-file -e "$mesa_revision^{commit}" 2>/dev/null ||
    { echo "the Mesa fork at $mesa_fork does not have $mesa_revision" >&2; exit 2; }

if [[ ! -f $source_tree/.revision || $(<"$source_tree/.revision") != "$mesa_revision" ]]; then
    # The new revision's files replace the old ones by content: a file that
    # did not change keeps its time, so the build below recompiles only what
    # the revision touched. The subprojects meson fetched (zlib, by the hash
    # its wrap pins) stay.
    staging="$source_tree.new"
    rm -rf "$staging"
    mkdir -p "$staging" "$source_tree"
    git -C "$mesa_fork" archive "$mesa_revision" | tar -x -C "$staging"
    rsync -rlp --checksum --delete --exclude=/.revision --exclude=/subprojects/packagecache/ \
        --exclude=/subprojects/zlib-*/ "$staging/" "$source_tree/"
    rm -rf "$staging"
    printf '%s\n' "$mesa_revision" > "$source_tree/.revision"
fi

# poly's kernels (geometry shaders run as compute) are OpenCL C that mesa_clc
# and vtn_bindgen2 compile at build time: host tools from the same revision,
# built natively against the host's LLVM, Clang, libclc and SPIR-V translator.
clc_build="$root/.deps/work/radv-clc-build"
clc_bin="$root/.deps/work/radv-clc-bin"
if [[ ! -f $clc_build/build.ninja ]]; then
    "$meson" setup "$clc_build" "$source_tree" -Dbuildtype=release -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
        -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
        -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled -Dshared-llvm=enabled \
        -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=disabled \
        -Dtools= > "$clc_build.setup.log" 2>&1 ||
        { tail -20 "$clc_build.setup.log" >&2; exit 1; }
fi
"$ninja" -C "$clc_build" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 > "$clc_build.log" 2>&1 ||
    { grep -E "error|FAILED" "$clc_build.log" | head -20 >&2; exit 1; }
mkdir -p "$clc_bin"
ln -sf "$clc_build/src/compiler/clc/mesa_clc" "$clc_bin/mesa_clc"
ln -sf "$clc_build/src/compiler/spirv/vtn_bindgen2" "$clc_bin/vtn_bindgen2"
export PATH="$clc_bin:$PATH"

constants="$root/.deps/work/radv-cross-constants.ini"
printf "[constants]\nsdk = '%s'\n" "$sdk" > "$constants"
# The shader cache compresses with zlib, from its subproject: the SDK has none
# a cross build can find.
options=(-Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dradv-winsys=ps5
    -Dllvm=disabled -Damd-use-llvm=false -Dvideo-codecs=
    -Dbuildtype=debugoptimized -Db_ndebug="$ndebug"
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
    -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dzlib=enabled --force-fallback-for=zlib
    -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=enabled -Dbuild-tests=false -Dvulkan-layers= -Dtools=
    -Dmesa-clc=system)
if [[ ! -f $build/build.ninja || $(cat "$build/.radv-options" 2>/dev/null) != "${options[*]}" ]]; then
    # A tree configured with other options starts again from them.
    wipe=()
    [[ -f $build/build.ninja ]] && wipe=(--wipe)
    "$meson" setup "${wipe[@]}" "$build" "$source_tree" \
        --cross-file "$constants" --cross-file "$root/tooling/radv/ps5-cross.ini" \
        "${options[@]}" -Dradv-build-id="$mesa_revision" > "$build.setup.log" 2>&1 ||
        { tail -20 "$build.setup.log" >&2; exit 1; }
elif [[ $(cat "$build/.radv-build-id" 2>/dev/null) != "$mesa_revision" ]]; then
    "$meson" configure "$build" -Dradv-build-id="$mesa_revision" > "$build.setup.log" 2>&1 ||
        { tail -20 "$build.setup.log" >&2; exit 1; }
fi
printf '%s\n' "${options[*]}" > "$build/.radv-options"
printf '%s\n' "$mesa_revision" > "$build/.radv-build-id"
"$ninja" -C "$build" src/amd/vulkan/libvulkan_radeon.a > "$build.log" 2>&1 ||
    { grep -E "error|FAILED" "$build.log" | head -20 >&2; exit 1; }

rm -rf "$install"
mkdir -p "$install/lib" "$install/include"
cp "$build/src/amd/vulkan/libvulkan_radeon.a" "$install/lib/libvulkan_radeon.ps5.a"
cp -r "$source_tree/include/vulkan" "$install/include/vulkan"
cp -r "$source_tree/include/vk_video" "$install/include/vk_video" 2>/dev/null || true
cat > "$install/PROVENANCE.txt" <<PROV
RADV for the PlayStation 5, built by tools/build-radv.sh $variant
fork: $mesa_fork
revision: $mesa_revision
assertions: $([[ $ndebug == true ]] && echo off || echo on)
sdk: $(cat "$sdk/.ps5-sdk-revision")
archive sha256: $(sha256sum "$install/lib/libvulkan_radeon.ps5.a" | cut -d' ' -f1)
PROV
echo "==> [radv] built RADV ($variant) at ${mesa_revision:0:12} into $install"
