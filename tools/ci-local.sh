#!/usr/bin/env bash
# ps5-native-app-boilerplate - Local reproduction of the CI build job.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the steps of the build job in .github/workflows/tooling.yml on this
# host, in the same order, so a local failure is the same failure the runner
# reports. Two steps are deliberately not reproduced: installing the runner's
# packages, and the release-tag rule that only applies to tagged pushes. The
# runner pins the LLVM 18 packages from the Ubuntu archive; this host uses the
# unversioned commands that `make doctor` reports, which the format policy
# covers across the 18 to 22 range the project verifies.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

step() {
    printf '\n==> [ci] %s\n' "$1"
}

step 'Toolchain the gates will use'
for tool in clang clang++ clang-format clang-tidy llvm-ar llvm-ranlib; do
    if resolved=$(command -v "$tool" 2>/dev/null); then
        printf '%-14s %s\n' "$tool" "$resolved"
    else
        printf '%-14s %s\n' "$tool" 'not found; a versioned fallback may serve'
    fi
done

step 'Read app identity and version'
metadata=$(python3 -c \
    'import json; p=json.load(open("sce_sys/param.json")); print(p["titleId"], p["contentVersion"])')
read -r title_id content_version <<< "$metadata"
[[ $title_id =~ ^PPSA[0-9]{5}$ ]] || {
    echo "param.json titleId is not PPSA plus five digits: $title_id" >&2
    exit 2
}
[[ $content_version =~ ^[0-9]{2}\.[0-9]{3}\.[0-9]{3}$ ]] || {
    echo "param.json contentVersion is not NN.NNN.NNN: $content_version" >&2
    exit 2
}
printf 'title id %s, content version %s\n' "$title_id" "$content_version"

step 'Lint source and metadata'
make lint

step 'Run host GoogleTest suite'
make test-unit

step 'Run host integration tests'
make test-integration

step 'Reproduce clean-room runtime shim'
make libc

step 'Verify generated runtime is unchanged'
(cd runtime && sha256sum -c libc.prx.sha256)

step 'Build compressed release image'
make ffpfsc

step 'Archive directory-style application'
(cd dist && python3 -m zipfile -c "$title_id.zip" "$title_id")

step 'Write release checksums'
(cd dist && sha256sum "$title_id.ffpfsc" "$title_id.zip" > SHA256SUMS)

printf '\n==> [ci] Every reproduced step passed. Artifacts: dist/%s.ffpfsc dist/%s.zip dist/SHA256SUMS\n' \
    "$title_id" "$title_id"
