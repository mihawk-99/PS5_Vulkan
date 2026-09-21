#!/usr/bin/env bash
# PS5 Vulkan - fetch the pinned VK-GL-CTS revision and record what it pins.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Phase E1 (docs/CTS.md). The CTS is not distributed by this repository: it is
# fetched into an ignored cache the way the pinned SDK and Mesa release are, and
# this script is the fetch. It refuses a checkout that is not the pinned commit,
# and it writes conformance_inventory/cts_pin.json -- the record a run's manifest
# is read against -- from the checkout itself.
#
# Two kinds of pin are in that record, and they are not the same thing:
#
#   * the CTS revision, which this script verifies byte for byte;
#   * the revisions the CTS's own external/fetch_sources.py *declares* for
#     glslang, SPIRV-Tools, SPIRV-Headers, amber and the rest. Declared is not
#     compiled: a build may end up with different checkouts in the tree, which
#     is why the reference project's manifest records the commits that were
#     really compiled. This record carries the declared ones now and gains the
#     compiled ones when the payload build can report them.
#
#   tools/fetch-vk-gl-cts.sh            fetch (or verify) the pinned checkout
#   tools/fetch-vk-gl-cts.sh --check    verify the record and the docs agree
#
# The default cache is .deps/work/vk-gl-cts; PS5VK_CTS_DIR moves it.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work="${PS5VK_CTS_DIR:-$root/.deps/work/vk-gl-cts}"
record="$root/conformance_inventory/cts_pin.json"
tag="vulkan-cts-1.3.8.4"
commit="a0270c1897597e6c77679870e10415398a13001c"
repo="https://github.com/KhronosGroup/VK-GL-CTS.git"

write_record() {
    python3 - "$work" "$record" "$tag" "$commit" "$repo" <<'PY'
import json
import pathlib
import re
import subprocess
import sys

work, record, tag, commit, repo = (pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]),
                                   sys.argv[3], sys.argv[4], sys.argv[5])
head = subprocess.run(["git", "-C", str(work), "rev-parse", "HEAD"], check=True,
                      capture_output=True, text=True).stdout.strip()
if head != commit:
    raise SystemExit(f"{work}: HEAD is {head}, not the pinned {commit}")

# The external revisions the CTS declares, read out of its own fetch script.
# GitRepo(url, sshUrl, revision, directory) and
# SourceFile(url, filename, sha256, directory) are the two shapes it uses.
sources = work / "external" / "fetch_sources.py"
git_pins, file_pins = {}, {}
text = sources.read_text()
for url, revision, directory in re.findall(
        r'GitRepo\(\s*"([^"]+)",\s*"[^"]+",\s*"([0-9a-f]{40})",\s*"([^"]+)"', text):
    git_pins[directory] = {"url": url, "revision": revision}
for url, name, digest, directory in re.findall(
        r'SourceFile\(\s*"([^"]+)",\s*"([^"]+)",\s*"([0-9a-f]{64})",\s*"([^"]+)"', text):
    file_pins.setdefault(directory, []).append({"url": url, "file": name, "sha256": digest})
record.parent.mkdir(parents=True, exist_ok=True)
record.write_text(json.dumps({
    "note": "Phase E1 pin (docs/CTS.md). The CTS revision is verified from the checkout; "
            "the external revisions are what the CTS's external/fetch_sources.py declares, "
            "not necessarily what a build compiles.",
    "cts": {"repo": repo, "tag": tag, "commit": commit, "verified_head": head},
    "external_declared": {"git": git_pins, "files": file_pins},
}, indent=2, sort_keys=True) + "\n")
print(f"   record            {record}")
print(f"   cts               {tag} {commit[:12]}")
print(f"   external declared {len(git_pins)} git repos, "
      f"{sum(len(v) for v in file_pins.values())} files")
PY
}

if [[ ${1:-} == --check ]]; then
    [[ -f $record ]] || { echo "missing $record; run tools/fetch-vk-gl-cts.sh" >&2; exit 2; }
    python3 - "$record" "$tag" "$commit" <<'PY'
import json
import pathlib
import sys

record, tag, commit = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
data = json.loads(record.read_text())
if data["cts"]["tag"] != tag or data["cts"]["commit"] != commit:
    raise SystemExit(f"{record}: the record pins {data['cts']['tag']} "
                     f"{data['cts']['commit']}, the script pins {tag} {commit}")
docs = (record.parent.parent / "docs" / "CTS.md").read_text()
missing = [name for name in (tag, commit) if name not in docs]
if missing:
    raise SystemExit(f"docs/CTS.md does not name {', '.join(missing)}")
print(f"   {record.name} and docs/CTS.md agree on {tag} {commit[:12]}, "
      f"{len(data['external_declared']['git'])} declared external repos")
PY
    echo "cts pin: PASS"
    exit 0
fi

if [[ -d $work/.git ]]; then
    have=$(git -C "$work" rev-parse HEAD 2>/dev/null || echo none)
    if [[ $have != "$commit" ]]; then
        echo "   $work is at ${have:0:12}; fetching the pinned revision" >&2
        git -C "$work" fetch --depth 1 origin "$commit" >/dev/null 2>&1 ||
            git -C "$work" fetch --depth 1 origin "refs/tags/$tag:refs/tags/$tag"
        git -C "$work" checkout --detach "$commit" >/dev/null 2>&1
    fi
else
    echo "   cloning $tag into $work"
    mkdir -p "$(dirname -- "$work")"
    git clone --depth 1 --branch "$tag" "$repo" "$work" >/dev/null 2>&1
fi

have=$(git -C "$work" rev-parse HEAD)
if [[ $have != "$commit" ]]; then
    echo "the checkout is at $have, not the pinned $commit" >&2
    exit 2
fi
echo "== the pinned CTS checkout"
echo "   checkout          $work"
echo "   size              $(du -sh "$work" | cut -f1)"
write_record
