#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - check the SDK fork's compiler against the
# patches this project's pinned compiler carries.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Blocker round 16 (docs/BLOCKERS.md, the ps5-opengl-sdk-0.3.0 section). The
# pinned compiler is the SDK 0.2.0-era work copy with this project's patches
# applied; ps5-opengl 0.3.0 ships the *fork*'s compiler as a patch over upstream
# `PS4-OpenGNM/opengnm-psbc`. Moving to it is the fix for the ACO divide fault
# round 15 identified, so this check answers the migration's first question
# offline: does the fork's patch apply to the base the SDK pins, is the assembled
# tree the one the SDK records, and which of this project's compiler patches
# still hold once it has?
#
# It writes nothing in the repository: the scratch checkout lives under
# build/sdk-fork. A checkout that is already there is reused, so a second run
# needs no network.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${PS5_OPENGL_SDK:-$root/../ps5-opengl-sdk-0.3.0}
dependencies="$sdk/dependencies.json"
[[ -f $dependencies ]] || { echo "missing $dependencies" >&2; exit 2; }

read -r revision patch tree expected <<<"$(
    python3 - "$dependencies" <<'JSON'
import json, sys
d = json.load(open(sys.argv[1]))
print(d["repositories"]["opengnm-psbc"]["revision"],
      d["psbc_patch"]["path"],
      d["psbc_patch"]["patched_tree"],
      d["psbc_patch"]["sha256"])
JSON
)"
patch="$sdk/$patch"
actual=$(sha256sum "$patch" | cut -d' ' -f1)
[[ $actual == "$expected" ]] || { echo "compiler patch hash is $actual, expected $expected" >&2; exit 2; }

work="$root/build/sdk-fork/opengnm-psbc"
if [[ ! -d $work/.git ]]; then
    mkdir -p "$(dirname -- "$work")"
    echo "== fetching $revision into $work"
    git clone --quiet https://github.com/PS4-OpenGNM/opengnm-psbc.git "$work"
fi
git -C "$work" checkout --quiet "$revision"
git -C "$work" reset --quiet --hard
git -C "$work" clean --quiet -fd
echo "== base: $revision, patch: ${expected:0:12}"

echo "== the fork's patch over the pinned base"
git -C "$work" apply "$patch"
echo "   applied cleanly"
# The SDK records the patched tree's own identity, so the assembly is checked
# against the tree the SDK built rather than trusted.
git -C "$work" add --all >/dev/null
assembled=$(git -C "$work" write-tree)
status=0
if [[ $assembled == "$tree" ]]; then
    echo "   tree $assembled is the SDK's own patched tree"
else
    echo "   tree $assembled differs from the SDK's $tree" >&2
    status=1
fi

echo "== this project's compiler patches against the fork's tree"
for script in patch-fragment-inputs patch-descriptor-types patch-vertex-formats \
              patch-compute-metadata; do
    if out=$(python3 "$root/tooling/psbc/$script.py" "$work" 2>&1); then
        printf '   %-24s holds: %s\n' "$script" "$(printf '%s' "$out" | tail -n 1)"
    else
        printf '   %-24s DOES NOT HOLD: %s\n' "$script" "$(printf '%s' "$out" | tail -n 1)"
        status=1
    fi
done

echo "== the fork's own schema"
grep -h "PSBC_SHADER_METADATA_VERSION" "$work/libpsbc/psbc_compile.h" | sed 's/^/   /'
grep -h "PSBC_MAX_DESCRIPTOR_BINDINGS" "$work/libpsbc/psbc_compile.h" | head -n 1 | sed 's/^/   /'
echo "== work list: a patch that does not hold needs its anchor moved, and one the"
echo "   fork's schema makes obsolete is dropped rather than moved. Metadata version 14"
echo "   means every probe package is rebuilt and every battery re-run before a claim."
exit "$status"
