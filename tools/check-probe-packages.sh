#!/usr/bin/env bash
# PS5 Vulkan - every committed probe package still compiles to itself.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A committed package can drift from the compiler with no gate watching it.
# probes/v0-push is the case that happened: its package was written by a probe
# CLI built before R9's compiler fix, so it held push constants inlined into
# seven user SGPRs and 48 bytes of code where the driver and the console have the
# pointer form at four SGPRs and 68 bytes. Nothing noticed, because the runner's
# compile keyword -- the only thing that recompiles a package on the console --
# appears in one queue (jobs/compile), and here is why that is not enough:
#
#   - 92 of the 93 queues in jobs/ carry no compile keyword at all, so during any
#     per-case or per-round battery every package is taken as it stands;
#   - jobs/compile queues "all", which compiles the package set each *test* loads
#     -- 40 of the 43 committed sets. c8-sampleid, c7-diag and v0-multiset are
#     named by no test, so even that run does not cover them.
#
# So the check is here, on the host: rebuild every set tools/build-probe-shaders.sh
# can build into a scratch root (PS5VK_PROBE_OUTPUT_ROOT, so nothing in the tree is
# written) and compare the result with the committed files byte for byte.
#
# What it cannot cover is printed rather than skipped silently, and a committed set
# that is neither rebuilt nor a known exception fails the check: the arithmetic is
# asserted every run, so a new set cannot join probes/ without a builder.
#
# Run from the repository root: bash tools/check-probe-packages.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build="$root/tools/build-probe-shaders.sh"
cli="$root/build/host/opengnm-psbc-probe"
scratch="$root/build/probe-packages"

[[ -x $cli ]] ||
    { echo "missing probe compiler $cli; run tools/build-psbc-cli.sh" >&2; exit 2; }

# The sets this check cannot rebuild, with the reason. They are expected, and
# every one of them is printed in the summary; a committed set that is not
# rebuilt and not named here fails the check below.
known_unbuilt=(
    "c8-sampleid:recorded as not building in the build script (its label exits 2)"
    "shaders:imported Split AGC assets, no builder in this repository"
)

# Each case label of the build script, with the probes/ directory its output= names.
labels=()
for label in $(grep -oP '^\K[a-z0-9-]+(?=\))' "$build" | sort -u); do
    output=$(awk -v L="$label" '
        /^[a-z0-9-]+\)$/ { s = $0; sub(/\)$/, "", s); in_case = (s == L) }
        in_case && /^    output=probes\// { sub(/^    output=probes\//, ""); print; exit }' "$build")
    [[ -n $output ]] && labels+=("$label:$output")
done
(( ${#labels[@]} > 0 )) || { echo "no probe sets found in $build" >&2; exit 2; }

# Every set with committed packages: probes/<set>/<stage>.bin.
committed=$(git -C "$root" ls-files 'probes/*' | awk -F/ '$3 ~ /\.bin$/ { print $2 }' | sort -u)
(( $(grep -c . <<< "$committed") > 0 )) ||
    { echo "no committed probe packages under $root/probes" >&2; exit 2; }

# One label: build it into the scratch root, then compare what it wrote.
run_label() {
    local label=$1 set=$2
    if ! PS5VK_PROBE_OUTPUT_ROOT="$scratch/out" bash "$build" "$label" \
            > "$scratch/$label.log" 2>&1; then
        # The two labels the build script records as not building fail here too,
        # by name; any other failure is a failure of this check.
        case $set in
            c8-sampleid|v0-robust)
                printf '   %-28s not built (recorded as not building)\n' "$set"
                return 3 ;;
            *)  printf '   %-28s BUILD FAILED (%s)\n' "$set" "$scratch/$label.log"
                grep -E "failed|error|Error" "$scratch/$label.log" | head -3 | sed 's/^/      /'
                return 2 ;;
        esac
    fi
    if [[ ! -d $root/probes/$set ]]; then
        printf '   %-28s built, but nothing is committed for it\n' "$set"
        return 0
    fi
    if diff -r "$scratch/out/probes/$set" "$root/probes/$set" > "$scratch/$set.diff" 2>&1; then
        printf '   %-28s identical\n' "$set"
        return 0
    fi
    # PROVENANCE.txt records the probe compiler's hash, which moves whenever the
    # CLI is rebuilt; that is provenance drift, reported at the end, not a package
    # difference. Everything else here is a package difference and fails.
    if diff -r --exclude=PROVENANCE.txt "$scratch/out/probes/$set" "$root/probes/$set" \
            > "$scratch/$set.diff" 2>&1; then
        printf '   %-28s identical, PROVENANCE names an older compiler\n' "$set"
        provenance_drift+=("$set")
        return 0
    fi
    printf '   %-28s DIFFERS from the committed package\n' "$set"
    head -10 "$scratch/$set.diff" | sed 's/^/      /'
    return 1
}

echo "== rebuilding every probe set with the current compiler and comparing"
rm -rf "$scratch"
mkdir -p "$scratch/out/probes"
status=0
rebuilt=()
provenance_drift=()
for entry in "${labels[@]}"; do
    label=${entry%%:*}
    set=${entry#*:}
    run_label "$label" "$set" && result=0 || result=$?
    # 0 and 1 both mean the set was rebuilt and compared -- they differ only in
    # whether it matched. 2 is a build that failed, 3 a set recorded as not
    # building, and neither counts as covered.
    if (( result <= 1 )); then
        rebuilt+=("$set")
    fi
    (( result == 0 || result == 3 )) || status=1
done

echo
echo "== coverage"
echo "   $(grep -c . <<< "$committed") committed package sets; ${#rebuilt[@]} of them rebuilt and compared"
# The two halves have to account for every committed set: a set with no builder
# and no exception is exactly the drift this check exists to prevent.
uncovered=()
for set in $committed; do
    if [[ " ${rebuilt[*]} " == *" $set "* ]]; then
        continue
    fi
    for entry in "${known_unbuilt[@]}"; do
        if [[ ${entry%%:*} == "$set" ]]; then
            continue 2
        fi
    done
    uncovered+=("$set")
done
for entry in "${known_unbuilt[@]}"; do
    set=${entry%%:*}
    printf '   %-28s not rebuildable here: %s\n' "$set" "${entry#*:}"
done
if (( ${#uncovered[@]} > 0 )); then
    for set in "${uncovered[@]}"; do
        printf '   %-28s committed but neither rebuilt nor excepted\n' "$set"
    done
    status=1
fi
if (( ${#provenance_drift[@]} > 0 )); then
    echo "   PROVENANCE.txt names an older probe compiler for: ${provenance_drift[*]}"
    echo "   (their packages are byte-identical; re-run the set to refresh the record)"
fi
if (( status == 0 )); then
    echo "probe packages: PASS"
else
    echo "probe packages: FAIL"
fi
exit $status
