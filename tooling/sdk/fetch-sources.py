#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - verify the reconstructed SDK tree's compiler.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""The verification step of tools/build-psbc-ps5.sh, for a tree this repository
reconstructed rather than downloaded.

ps5-opengl's own tools/fetch-sources.py checks the pinned upstream revision and
the patch it applies; a tree built by tools/adapt-opengl-sdk.sh has a different
identity on purpose -- it freezes the compiler this repository's probes were
built from (the 0.2.0-era opengnm-psbc fork with tooling/psbc/patch-*.py applied
on top) -- so the same call has to verify that identity instead. The adapter
writes it into dependencies.json's `psbc_compat` block:

    "tree_sha256": the hash of the compiler tree's files and their contents

and this script recomputes it. A tree whose contents moved -- a patch script
that stopped applying, a rebuild that wrote into the tree, a stale copy -- fails
here loudly, exactly as the upstream check does for the pinned revision.

    python3 tools/fetch-sources.py --verify-psbc      # from the SDK tree's root
"""
import hashlib
import json
import sys
from pathlib import Path

# Build outputs the compiler tree may acquire: they are not part of its identity.
EXCLUDED_DIRS = {".git", "__pycache__"}
EXCLUDED_SUFFIXES = (".o", ".a", ".so", ".log")


def tree_digest(root: Path) -> str:
    """A deterministic hash of every file below root, path and contents."""
    digest = hashlib.sha256()
    entries = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if any(part in EXCLUDED_DIRS for part in path.parts):
            continue
        if path.name.endswith(EXCLUDED_SUFFIXES):
            continue
        entries.append(path)
    for path in entries:
        digest.update(path.relative_to(root).as_posix().encode("utf-8"))
        digest.update(b"\0")
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1 << 20), b""):
                digest.update(block)
        digest.update(b"\0")
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    if argv[1:] != ["--verify-psbc"]:
        sys.exit("usage: fetch-sources.py --verify-psbc")
    tree = Path(__file__).resolve().parent.parent / "third_party" / "opengnm-psbc"
    manifest = Path(__file__).resolve().parent.parent / "dependencies.json"
    if not tree.is_dir():
        sys.exit(f"missing the compiler tree {tree}")
    if not manifest.is_file():
        sys.exit(f"missing {manifest}, which records the tree's identity")
    recorded = json.loads(manifest.read_text(encoding="utf-8")).get("psbc_compat", {})
    expected = recorded.get("tree_sha256")
    if not isinstance(expected, str) or len(expected) != 64:
        sys.exit(f"{manifest} records no psbc_compat.tree_sha256")
    actual = tree_digest(tree)
    if actual != expected:
        sys.exit(
            "the compiler tree's contents do not match the identity "
            f"dependencies.json records:\n  recorded {expected}\n  actual   {actual}\n"
            "Rebuild the tree with tools/adapt-opengl-sdk.sh if the change was intended; "
            "a driver or probe built against it must be re-proved (docs/BLOCKERS.md)."
        )
    print(f"compiler tree matches its recorded identity ({actual[:16]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
