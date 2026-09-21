#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - assemble the SDK's compiler fork tree.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ps5-opengl 0.3.0 does not vendor opengnm-psbc. Its dependencies.json pins the
# upstream revision and ships toolchain/opengnm-psbc-ps5.patch, and records the
# patch's result as psbc_patch.patched_tree. The compiler this repository links
# is that patch applied to that revision -- the fork's own compiler, whose
# psbc_compile.c guards the ACO divide fault (docs/BLOCKERS.md round 15) -- so
# both the SDK view (tools/adapt-opengl-sdk.sh) and the migration check
# (tools/check-sdk-fork-migration.sh) build the tree here instead of each
# keeping its own copy of the steps.
#
# The checkout is cached under build/sdk-fork/opengnm-psbc: the first run clones
# it, later runs only check the pin out, so a second run needs no network.
#
# usage: assemble-psbc-fork.sh <sdk-dir> <destination>
#   <sdk-dir>      a ps5-opengl checkout or release directory, holding
#                  dependencies.json and the patch it names
#   <destination>  where the assembled tree is written, rsynced without .git;
#                  "-" leaves it only in the cached checkout
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ $# -ne 2 ]]; then
    echo "usage: assemble-psbc-fork.sh <sdk-dir> <destination>" >&2
    exit 2
fi
sdk=$(cd -- "$1" && pwd)
destination=$2
dependencies="$sdk/dependencies.json"
[[ -f $dependencies ]] || { echo "missing $dependencies" >&2; exit 2; }

read -r url revision patch tree expected <<<"$(
    python3 - "$dependencies" <<'JSON'
import json, sys
d = json.load(open(sys.argv[1]))
repo = d["repositories"]["opengnm-psbc"]
print(repo["url"], repo["revision"], d["psbc_patch"]["path"],
      d["psbc_patch"]["patched_tree"], d["psbc_patch"]["sha256"])
JSON
)"
patch="$sdk/$patch"
[[ -f $patch ]] || { echo "missing $patch" >&2; exit 2; }
actual=$(sha256sum "$patch" | cut -d' ' -f1)
[[ $actual == "$expected" ]] ||
    { echo "compiler patch hash is $actual, expected $expected" >&2; exit 2; }

clone=${PSBC_FORK_CHECKOUT:-$root/build/sdk-fork/opengnm-psbc}
if [[ ! -d $clone/.git ]]; then
    mkdir -p "$(dirname -- "$clone")"
    echo "== fetching $url"
    git clone --quiet "$url" "$clone"
fi
git -C "$clone" checkout --quiet "$revision"
git -C "$clone" reset --quiet --hard
git -C "$clone" clean --quiet -fd
echo "== base: $revision, patch: ${expected:0:12}"
git -C "$clone" apply "$patch"
# The SDK records the patched tree's own identity, so the assembly is checked
# against the tree the SDK built rather than trusted.
git -C "$clone" add --all >/dev/null
assembled=$(git -C "$clone" write-tree)
if [[ $assembled != "$tree" ]]; then
    echo "the assembled tree $assembled is not the SDK's own patched tree $tree" >&2
    exit 1
fi
echo "   tree $assembled is the SDK's own patched tree"

if [[ $destination != - ]]; then
    mkdir -p "$destination"
    rsync -a --delete --exclude=/.git "$clone/" "$destination/"

    # The vendored Mesa sources need generated files that the standalone
    # Makefile omits from its GENERATED list, and it enumerates C sources with
    # wildcard at parse time, so they have to exist before the first make. The
    # list and the order are the SDK's own (toolchain/build-opengnm-psbc.sh,
    # "Materialize generated sources before that enumeration on a fresh
    # checkout"); the 0.2.0-era work copy carried them, a fresh checkout does not.
    (
        cd "$destination"
        python3 src/util/format/u_format_table.py src/util/format/u_format.yaml --enums > src/util/format/u_format_gen.h
        python3 src/util/format/u_format_table.py src/util/format/u_format.yaml --header > src/util/format/u_format_pack.h
        python3 src/util/format/u_format_table.py src/util/format/u_format.yaml > src/util/format/u_format_table.c
        python3 src/util/format_srgb.py > src/util/format_srgb.c
        python3 src/compiler/builtin_types_h.py src/compiler/builtin_types.h
        python3 src/compiler/builtin_types_c.py src/compiler/builtin_types.c
        python3 src/util/process_shader_stats.py src/util/shader_stats.rnc src/util/shader_stats.xml > src/util/shader_stats.h
        python3 src/vulkan/util/vk_struct_type_cast_gen.py \
            --xml src/vulkan/registry/vk.xml \
            --out src/vulkan/util/vk_struct_type_cast.h \
            --beta false
        python3 src/amd/packets/parse_cp_pm4_table_data_json.py \
            src/amd/packets/cp_pm4_table_data_gfx11.json \
            src/amd/packets/pm4_it_opcodes_gfx11.h \
            src/amd/packets/cp_pm4_table_data_gfx12.json \
            src/amd/packets/pm4_it_opcodes_gfx12.h \
            gfx11 packets_h > src/amd/common/amd_cp_packets_gfx11.h
        python3 src/amd/packets/parse_cp_pm4_table_data_json.py \
            src/amd/packets/cp_pm4_table_data_gfx11.json \
            src/amd/packets/pm4_it_opcodes_gfx11.h \
            src/amd/packets/cp_pm4_table_data_gfx12.json \
            src/amd/packets/pm4_it_opcodes_gfx12.h \
            gfx12 packets_h > src/amd/common/amd_cp_packets_gfx12.h
        python3 src/amd/common/gfx10_format_table.py \
            src/util/format/u_format.yaml \
            src/amd/registers/gfx10-rsrc.json \
            src/amd/registers/gfx11-rsrc.json > src/amd/common/gfx10_format_table.c
    )
    echo "== tree: $destination (generated sources materialized)"
fi
