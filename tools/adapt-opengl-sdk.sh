#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - reconstruct the SDK tree the tools read.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ps5-opengl-sdk-0.3.0 ships a release layout -- sdk/ (the OpenGL payload),
# sources/*.tar (the pinned upstream trees) and ps5-opengl/ (the source tree and
# its toolchain patch) -- while every tool here reads the older SDK layout:
#
#   third_party/opengnm-psbc/          the compiler fork the driver links
#   third_party/{opengnm,Vulkan-Headers,SPIRV-Headers}/include/
#   toolchain/{Makefile.opengnm-psbc-ps5,opengnm-psbc-ps5.mak,opengnm-psbc-host.mak}
#   src/platform/{ps5_mesa_shims.c,ps5_agc_package.c,ps5_agc_package.h}
#   tools/{agc_shader_package_writer.py,fetch-sources.py}
#
# This script writes that layout to .deps/native/opengl-sdk (tools/sdk-root.sh
# resolves it; PS5_OPENGL_SDK overrides). The compiler half is **frozen**: it
# comes from the work copy tools/build-psbc-ps5.sh last built, so the tree is the
# 0.2.0-era opengnm-psbc fork with tooling/psbc/patch-*.py applied -- the exact
# compiler every probe package and console claim in this repository was built
# with. Nothing a proof depends on moves because the old SDK directory is gone
# (docs/BLOCKERS.md, the SDK section). Moving to the 0.3.0 fork's own compiler is
# a deliberate later step, because a compiler swap re-proves every row.
#
# What comes from the release is what the work copy does not hold: the AGC
# package writer, the host build configuration, and dependencies.json (Mesa
# 26.2.0, the same version the runtime is built from). The AGC package *sources*
# and the PS5 makefiles come from the work copy too, so the bytes the C writer
# produces stay the ones the committed probe packages were built with.
#
# Run from the repository root:  bash tools/adapt-opengl-sdk.sh [RELEASE-DIR]
# The release directory defaults to ../ps5-opengl-sdk-0.3.0 and may be given as
# PS5_OPENGL_RELEASE; the written tree as PS5_OPENGL_SDK_TREE.
#
# The host compiler (libpsbc.a and the opengnm-psbc CLI) is built into the tree,
# because tools/build-probe-shaders.sh and tools/build-host-runner.sh read it
# there. It is the same source the work copy holds, so its objects are the only
# thing this script compiles.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
input=$(cd -- "${1:-${PS5_OPENGL_RELEASE:-$root/../ps5-opengl-sdk-0.3.0}}" && pwd)
out=$(cd -- "$(dirname -- "${PS5_OPENGL_SDK_TREE:-$root/.deps/native/opengl-sdk}")" 2>/dev/null \
    && pwd)/$(basename -- "${PS5_OPENGL_SDK_TREE:-$root/.deps/native/opengl-sdk}")
work=${PSBC_PS5_WORK:-$root/.deps/work/psbc-ps5}

# The input is either the ps5-opengl source checkout (a git clone of the
# repository, which is what a release bundle was built from and what carries a
# revision to record) or a release bundle, whose sources/ps5-opengl.tar holds the
# same tree. A checkout of the repository is the better input: the receipt below
# names its commit.
sources=$input
shape=checkout
if [[ ! -f $input/toolchain/opengnm-psbc-host.mak ]]; then
    shape=release
    sources=${PS5_OPENGL_RELEASE_WORK:-$root/.deps/work/opengl-sdk-release}/ps5-opengl
    mkdir -p "$(dirname -- "$sources")"
    if [[ ! -f $sources/toolchain/opengnm-psbc-host.mak ]]; then
        [[ -f $input/sources/ps5-opengl.tar ]] ||
            { echo "$input is neither a ps5-opengl checkout nor a release bundle" >&2; exit 2; }
        tar xf "$input/sources/ps5-opengl.tar" -C "$(dirname -- "$sources")"
    fi
fi
for file in "$input/dependencies.json" "$sources/toolchain/opengnm-psbc-ps5.patch" \
        "$sources/toolchain/opengnm-psbc-host.mak" \
        "$sources/tools/agc_shader_package_writer.py"; do
    [[ -e $file ]] ||
        { echo "missing $file; pass the ps5-opengl checkout or release directory" >&2; exit 2; }
done
revision=$(git -C "$sources" rev-parse HEAD 2>/dev/null || true)
[[ -d $work/third_party/opengnm-psbc/libpsbc ]] ||
    { echo "missing the compiler work copy $work; run tools/build-psbc-ps5.sh once" >&2; exit 2; }
for part in third_party/opengnm/include third_party/Vulkan-Headers/include \
        third_party/SPIRV-Headers/include toolchain/Makefile.opengnm-psbc-ps5 \
        toolchain/opengnm-psbc-ps5.mak src/platform/ps5_mesa_shims.c \
        src/platform/ps5_agc_package.c src/platform/ps5_agc_package.h; do
    [[ -e $work/$part ]] || { echo "the work copy holds no $part" >&2; exit 2; }
done

mkdir -p "$out/third_party" "$out/toolchain" "$out/src/platform" "$out/tools"
# 1. The compiler tree, frozen at the work copy's contents.
rsync -a --delete --exclude='*.o' --exclude='*.a' --exclude='*.log' \
    --exclude='__pycache__' "$work/third_party/opengnm-psbc/" "$out/third_party/opengnm-psbc/"
# 2. The include dependencies, and the PS5 platform sources and makefiles.
for header in opengnm Vulkan-Headers SPIRV-Headers; do
    mkdir -p "$out/third_party/$header"
    rsync -a --delete "$work/third_party/$header/include/" "$out/third_party/$header/include/"
done
cp -p "$work/toolchain/Makefile.opengnm-psbc-ps5" "$work/toolchain/opengnm-psbc-ps5.mak" \
    "$out/toolchain/"
cp -p "$sources/toolchain/opengnm-psbc-host.mak" "$out/toolchain/"
cp -p "$work/src/platform/ps5_mesa_shims.c" "$work/src/platform/ps5_agc_package.c" \
    "$work/src/platform/ps5_agc_package.h" "$out/src/platform/"
# 3. The tools: the source's writer (its CLI is the one tools/build-probe-shaders.sh
#    invokes, and it imports agc_shader_package beside it), and this repository's
#    verifier, which replaces the upstream fetch-sources.py the build calls (see
#    tooling/sdk/fetch-sources.py).
cp -p "$sources/tools/agc_shader_package_writer.py" "$sources/tools/agc_shader_package.py" \
    "$out/tools/"
cp -p "$root/tooling/sdk/fetch-sources.py" "$out/tools/fetch-sources.py"

# 4. The host compiler: libpsbc.a and the CLI, built from the frozen tree with
#    the source's host configuration, so the tools that compile shaders on the
#    host read them where the pinned layout keeps them.
tree="$out/third_party/opengnm-psbc"
log="$out/build-host.log"
if ! make -C "$tree" -f Makefile CONFIG="$out/toolchain/opengnm-psbc-host.mak" \
        -j"$(nproc)" > "$log" 2>&1; then
    grep -E 'error:' "$log" | head -n 20
    echo "the host compiler failed to build; see $log" >&2
    exit 1
fi
for file in "$tree/libpsbc.a" "$tree/opengnm-psbc"; do
    [[ -e $file ]] || { echo "the host build wrote no $file; see $log" >&2; exit 1; }
done

# 5. dependencies.json: the source's Mesa pin, plus the compiler tree's identity.
python3 - "$input/dependencies.json" "$out" "$root" <<'PY'
import hashlib
import json
import pathlib
import sys

release_manifest, out, root = map(pathlib.Path, sys.argv[1:4])
document = json.loads(release_manifest.read_text(encoding="utf-8"))
tree = out / "third_party" / "opengnm-psbc"
sys.path.insert(0, str(root / "tooling" / "sdk"))
import importlib.util

spec = importlib.util.spec_from_file_location("fetch_sources", root / "tooling/sdk/fetch-sources.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
document["psbc_compat"] = {
    "source": "the work copy tools/build-psbc-ps5.sh last built (0.2.0-era opengnm-psbc "
              "fork + tooling/psbc/patch-*.py)",
    "written_by": "tools/adapt-opengl-sdk.sh",
    "mesa": document.get("mesa", {}).get("version"),
    "tree_sha256": module.tree_digest(tree),
}
(out / "dependencies.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
print(f"compiler tree identity: {document['psbc_compat']['tree_sha256'][:16]}")
PY

# 6. The receipt: what was read, from where, and what came out.
{
    echo "PS5 Vulkan SDK tree, reconstructed by tools/adapt-opengl-sdk.sh"
    echo "source: $input ($shape)"
    if [[ -n $revision ]]; then
        echo "source revision: $revision ($(git -C "$sources" log -1 --format=%s))"
    fi
    echo "compiler patch the source carries: toolchain/opengnm-psbc-ps5.patch (not applied: the compiler is frozen)"
    echo "sha256 of that patch: $(sha256sum "$sources/toolchain/opengnm-psbc-ps5.patch" | cut -d' ' -f1)"
    echo "compiler: the frozen work copy $work/third_party/opengnm-psbc"
    echo "          (0.2.0-era opengnm-psbc fork + tooling/psbc/patch-*.py; identity in dependencies.json)"
    echo "host compiler: $tree/libpsbc.a + $tree/opengnm-psbc, built with $out/toolchain/opengnm-psbc-host.mak"
    echo "platform sources and PS5 makefiles: the same work copy"
    echo "package writer: $sources/tools/agc_shader_package_writer.py"
    echo "verifier: tooling/sdk/fetch-sources.py"
    echo "mesa: $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["mesa"]["version"])' "$out/dependencies.json")"
} > "$out/PROVENANCE.txt"
(cd "$out" && find third_party/opengnm-psbc -type f ! -name '*.o' ! -name '*.a' ! -path '*__pycache__*' \
    -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
echo "SDK tree: $out"
cat "$out/PROVENANCE.txt"
