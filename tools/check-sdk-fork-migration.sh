#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - check the SDK fork's compiler against the
# patches this project's compiler carries.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Blocker rounds 16-17 (docs/BLOCKERS.md, the ps5-opengl-sdk-0.3.0 section). The
# driver links a compiler built from the SDK's own pin and patch with this
# repository's tooling/psbc patches on top. Round 16 measured that migration
# offline; the migration itself has since landed, so what this checks now is that
# the assembly still matches the tree the SDK records and that every patch still
# finds its anchors:
#
#   - the SDK's patch applies to the revision dependencies.json pins and the
#     assembled tree is the psbc_patch.patched_tree the manifest records
#     (tooling/sdk/assemble-psbc-fork.sh does the work and fails loudly);
#   - patch-fragment-inputs.py, patch-descriptor-types.py,
#     patch-vertex-formats.py, patch-push-constant-location.py and
#     patch-descriptor-sets.py hold against that tree;
#   - patch-compute-metadata.py is gone: the fork's own schema carries the
#     compute fields, with compute_lds_bytes in place of compute_lds_size
#     (tools/build-psbc-ps5.sh records the reasoning).
#
# It writes nothing in the repository: the tree is assembled under build/sdk-fork.
# A checkout that is already there is reused, so a second run needs no network.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${PS5_OPENGL_SDK:-$root/../ps5-opengl-sdk-0.3.0}
work="$root/build/sdk-fork/assembled"

bash "$root/tooling/sdk/assemble-psbc-fork.sh" "$sdk" "$work"

echo "== this project's compiler patches against the fork's tree"
status=0
for script in patch-fragment-inputs patch-aco-min-waves patch-descriptor-types \
              patch-vertex-formats patch-push-constant-location \
              patch-descriptor-sets; do
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
echo "== metadata version 14 is why every probe package and battery was rebuilt"
echo "   with this compiler before the migration was committed"
exit "$status"
