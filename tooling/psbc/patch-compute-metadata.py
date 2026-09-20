#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - export the compute resource metadata.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The pinned compiler reports descriptor tables, user SGPR counts and the
# graphics stage's register writes, but a compute dispatch programs
# COMPUTE_PGM_RSRC1/2 itself and has no such table to read. The values exist
# inside the compiler -- ACO's ac_shader_config carries rsrc1, rsrc2, rsrc3,
# the VGPR and SGPR counts and the LDS size, and psbc_compile.c already reads
# that struct for the graphics stages -- they are simply not exported.
#
# This adds them to PsbcShaderMetadata for PSBC_STAGE_COMPUTE. It runs on the
# work copy that tools/build-psbc-ps5.sh makes, so the SDK checkout keeps
# upstream's sources. The work copy is refreshed from the SDK tree before each
# build, but that tree may itself already carry these fields -- the reconstructed
# tree of tools/adapt-opengl-sdk.sh is a copy of a work copy this script has run
# on -- so every insertion is skipped when its marker is already there, and the
# anchors are only required to be findable when the insertion is still due.
#
# The metadata version stays 8 on purpose. The new fields are appended, so
# every existing offset and the existing contract are unchanged, and the SDK's
# own package writer validates the version against its header
# (ps5_agc_package.c). Bumping it would make that writer refuse every package
# the patched compiler produces, which is exactly the regression this comment
# exists to prevent. Callers that need the new fields therefore test the
# marker this patch defines (PSBC_SHADER_METADATA_COMPUTE_RSRC) instead of a
# version number: the version cannot move, and a version test would silently
# select the unpatched path on the one build that has the fields.
#
# Usage: tooling/psbc/patch-compute-metadata.py <opengnm-psbc tree>

import sys
from pathlib import Path

HEADER_MARKER = """/* Set by tooling/psbc/patch-compute-metadata.py: this header carries the
 * compute resource fields appended to PsbcShaderMetadata. The version above
 * stays 8 because the SDK's package writer compares it against its own copy
 * of this header, which this patch never touches. */
#define PSBC_SHADER_METADATA_COMPUTE_RSRC 1u
"""

HEADER_FIELDS = """    /* Compute resource usage. A graphics stage reports the same words among
     * context_registers; a compute dispatch programs COMPUTE_PGM_RSRC1/2
     * itself and has no table to read them from, so they are reported here.
     * Filled for PSBC_STAGE_COMPUTE only: a dispatch that ignores these is a
     * dispatch that guessed its register values. */
    bool                 compute_config_valid;
    uint32_t             compute_rsrc1;
    uint32_t             compute_rsrc2;
    uint32_t             compute_rsrc3;
    uint32_t             compute_num_vgprs;
    uint32_t             compute_num_sgprs;
    uint32_t             compute_lds_size;
"""

SOURCE_BLOCK = """    if (ctx->psbc_stage == PSBC_STAGE_COMPUTE) {
        metadata->compute_config_valid = true;
        metadata->compute_rsrc1 = ctx->config->rsrc1;
        metadata->compute_rsrc2 = ctx->config->rsrc2;
        metadata->compute_rsrc3 = ctx->config->rsrc3;
        metadata->compute_num_vgprs = ctx->config->num_vgprs;
        metadata->compute_num_sgprs = ctx->config->num_sgprs;
        metadata->compute_lds_size = ctx->config->lds_size;
    }
"""


def patch(path: Path, replacements: list[tuple[str, str, str]]) -> None:
    """Apply (anchor, replacement, marker) triples once each.

    A marker already present means an earlier run inserted it: the insertion is
    skipped, and the anchor it would replace need not be there (an insertion
    anchored to a line the previous run already consumed is exactly that case).
    """
    text = path.read_text(encoding="utf-8")
    applied = 0
    for anchor, replacement, marker in replacements:
        if marker in text:
            continue
        count = text.count(anchor)
        if count != 1:
            raise SystemExit(
                f"{path}: expected exactly one anchor, found {count}: {anchor.splitlines()[0]}"
            )
        text = text.replace(anchor, replacement)
        applied += 1
    if applied:
        path.write_text(text, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-compute-metadata.py <opengnm-psbc tree>")
    tree = Path(sys.argv[1])
    header = tree / "libpsbc" / "psbc_compile.h"
    source = tree / "libpsbc" / "psbc_compile.c"
    for path in (header, source):
        if not path.is_file():
            raise SystemExit(f"not an opengnm-psbc tree: {path} is missing")

    patch(
        header,
        [
            (
                "#define PSBC_SHADER_METADATA_VERSION 8u\n",
                "#define PSBC_SHADER_METADATA_VERSION 8u\n\n" + HEADER_MARKER,
                "PSBC_SHADER_METADATA_COMPUTE_RSRC",
            ),
            (
                "    uint32_t             ngg_lds_layout; /* GS output base in bytes, after ES inputs. */\n",
                "    uint32_t             ngg_lds_layout; /* GS output base in bytes, after ES inputs. */\n"
                + HEADER_FIELDS,
                "compute_config_valid",
            ),
        ],
    )
    patch(
        source,
        [
            (
                "    metadata->user_sgpr_count = ctx->rargs->num_user_sgprs;\n",
                "    metadata->user_sgpr_count = ctx->rargs->num_user_sgprs;\n" + SOURCE_BLOCK,
                "metadata->compute_num_sgprs",
            ),
        ],
    )
    print(f"compute metadata exported in {tree}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
