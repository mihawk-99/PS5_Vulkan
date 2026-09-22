#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - build the shader compiler for the PS5.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase A1 (docs/M5_PHASE_A.md). Builds ps5-opengl's PS5 archive
# of the opengnm-psbc compiler (NIR, SPIR-V to NIR, ACO, Mesa util; MIT) with
# this repository's payload SDK, and installs it for the console titles:
#
#   .deps/native/psbc/lib/libpsbc.ps5.a
#   .deps/native/psbc/include/psbc_compile.h
#   .deps/native/psbc/PROVENANCE.txt
#
# The SDK's pinned, patched compiler tree is verified first, then copied into an
# ignored work directory and built there with the SDK's own PS5 makefile, so the
# SDK checkout stays untouched. The installed archives land in
# `.deps/native/psbc/`.
#
# PS5_OPENGL_SDK selects the SDK (default: beside this repository);
# PSBC_PS5_WORK selects the build directory (default .deps/work/psbc-ps5).

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
payload="$root/.deps/native/ps5-payload-sdk"
work=${PSBC_PS5_WORK:-$root/.deps/work/psbc-ps5}
install="$root/.deps/native/psbc"

# shellcheck source=tools/ccache.sh
source "$root/tools/ccache.sh"

[[ -x $payload/bin/prospero-clang ]] ||
    { echo "missing payload SDK compiler: $payload/bin/prospero-clang" >&2; exit 2; }
command -v rsync >/dev/null || { echo "rsync is required" >&2; exit 2; }

# The exact source tree ps5-opengl pins and patches (offline check).
(cd "$sdk" && python3 tools/fetch-sources.py --verify-psbc)

# Only what the PS5 makefile reads: the compiler tree without history or host
# build outputs, its include dependencies, the PS5 make configuration and the
# platform shim it compiles.
mkdir -p "$work/third_party/opengnm" "$work/third_party/Vulkan-Headers" \
    "$work/third_party/SPIRV-Headers" "$work/toolchain" "$work/src/platform"
tree="$work/third_party/opengnm-psbc"
cpu_detect=src/util/u_cpu_detect.c
rsync -a --delete --exclude=/.git --exclude='*.o' --exclude='*.a' \
    --exclude=/opengnm-psbc --exclude='*.identity' --exclude="/$cpu_detect" \
    "$sdk/third_party/opengnm-psbc/" "$tree/"
rsync -a --delete "$sdk/third_party/opengnm/include/" "$work/third_party/opengnm/include/"
rsync -a --delete "$sdk/third_party/Vulkan-Headers/include/" "$work/third_party/Vulkan-Headers/include/"
rsync -a --delete "$sdk/third_party/SPIRV-Headers/include/" "$work/third_party/SPIRV-Headers/include/"
# cp -p keeps timestamps, so unchanged inputs do not rebuild.
cp -p "$sdk/toolchain/Makefile.opengnm-psbc-ps5" "$sdk/toolchain/opengnm-psbc-ps5.mak" "$work/toolchain/"
# Note: dropping -g here was tried and rejected. The archives are installed
# with debug information stripped, so it looked free, but the stripped output
# is not byte-identical to the upstream flags' output (29722694 bytes against
# 29732190, different digest), and the recorded install digests are what the
# shader-compiler titles are validated against. The measured saving was 11%
# of a 53 s build, which does not justify moving those bytes.
cp -p "$sdk/src/platform/ps5_mesa_shims.c" "$sdk/src/platform/ps5_agc_package.c" \
    "$sdk/src/platform/ps5_agc_package.h" "$root/tooling/psbc/psbc_ps5_shims.c" "$work/src/platform/"

# Upstream bug on this target: the payload SDK defines neither
# _SC_NPROCESSORS_ONLN nor _SC_NPROCESSORS_CONF, so util_cpu_detect compiles
# its BSD HW_NCPU fallback, which passes an int length where sysctl reads and
# writes a size_t (8 bytes). The work copy gets the fixed copy; it is only
# replaced when its content changes.
#
# A downloaded SDK tree carries the unfixed file, and this script fixes it. The
# reconstructed tree of tools/adapt-opengl-sdk.sh carries the fixed one instead
# -- it is a copy of the work copy, which this script already fixed -- so the
# check accepts either: the bug where it is still present, or the fix, and
# nothing else. Its identity is in the tree's dependencies.json
# (tooling/sdk/fetch-sources.py).
bad_length='      int len = sizeof(ncpu);'
good_length='      size_t len = sizeof(ncpu);'
bad_count=$(grep -Fxc -- "$bad_length" "$sdk/third_party/opengnm-psbc/$cpu_detect" || true)
good_count=$(grep -Fxc -- "$good_length" "$sdk/third_party/opengnm-psbc/$cpu_detect" || true)
if [[ $bad_count == 2 && $good_count == 1 ]]; then
    sed "s/^$bad_length\$/      size_t len = sizeof(ncpu);/" \
        "$sdk/third_party/opengnm-psbc/$cpu_detect" > "$work/u_cpu_detect.patched.c"
elif [[ $bad_count == 0 && $good_count == 3 ]]; then
    cp -p "$sdk/third_party/opengnm-psbc/$cpu_detect" "$work/u_cpu_detect.patched.c"
else
    echo "$cpu_detect is neither the file upstream has (two int sites and one size_t) nor the" \
        "fixed one (three size_t): $bad_count int, $good_count size_t" >&2
    exit 1
fi
cmp -s "$work/u_cpu_detect.patched.c" "$tree/$cpu_detect" ||
    cp "$work/u_cpu_detect.patched.c" "$tree/$cpu_detect"

# Compute metadata: the pinned 0.2.0-era compiler kept ACO's rsrc1/rsrc2/rsrc3,
# VGPR and SGPR counts and LDS size to itself and had to be patched to export
# them. The 0.3.0 fork carries those fields itself -- PsbcShaderMetadata version
# 14 has user_sgpr_count, the ngg_lds_layout trio and the compute_* group, and
# the compiler programs COMPUTE_PGM_RSRC1/2 -- so the patch that added them,
# tooling/psbc/patch-compute-metadata.py, was dropped rather than moved when this
# repository migrated to the fork (docs/BLOCKERS.md, the SDK section). The field
# is named compute_lds_bytes there, not compute_lds_size, and nothing in this
# repository ever read the old name.

# Match RADV before shader info: distinct fragment varyings need distinct slots.
python3 "$root/tooling/psbc/patch-fragment-inputs.py" "$tree"

# The wave arithmetic: the standalone path can leave workgroup_size 0, which
# calc_min_waves turns into 0 waves and get_addr_regs_from_waves divides by.
# The patch restores the field's documented "unknown is UINT_MAX" invariant and
# guards the divide with a witness line. It is not the console's SIGFPE -- that
# fault is the test runner's own zero divisor (docs/HARDWARE_FINDINGS.md,
# 2026-09-20) -- but the divide is real and unguarded upstream.
python3 "$root/tooling/psbc/patch-aco-min-waves.py" "$tree"

# The eight-bit vertex layouts: Vulkan requires VERTEX_BUFFER for every format
# the hardware can fetch, and the compiler's enum named none of them. The patch
# appends the enum values and their pipe formats; the descriptor word comes from
# Mesa's vertex-element table (docs/BLOCKERS.md, the vertex formats).
python3 "$root/tooling/psbc/patch-vertex-formats.py" "$tree"

# The texel-buffer descriptor types: the compiler builds a RADV descriptor set
# layout and maps each binding onto a Vulkan descriptor type, so a uniform or
# storage texel buffer is an enum value and its type (docs/BLOCKERS.md, the
# descriptor types).
python3 "$root/tooling/psbc/patch-descriptor-types.py" "$tree"

# Push constants: the standalone path hands the shader a pointer to the data in
# a user-data dword, and PsbcShaderMetadata reported every user-data location
# except that one, so an application's layout(push_constant) read an unwritten
# SGPR -- a silent zero. The patch reports the location (and the inline form, so
# the driver can refuse it by name); docs/M5_PHASE_C.md, R9.
python3 "$root/tooling/psbc/patch-push-constant-location.py" "$tree"

# Descriptor sets: the wrapper built one flat set-0 layout and handed the ABI
# num_sets = 1, so a program binding more than set 0 could not be compiled at
# all -- while RADV's ABI already declares one set pointer per set bit. The
# patch splits the table per set, sizes each from its own bindings, names every
# used set in desc_set_used_mask and reports one user-data dword per set;
# docs/M5_PHASE_C.md, R7.
python3 "$root/tooling/psbc/patch-descriptor-sets.py" "$tree"

# Specialization constants: the compiler is RADV's front end, which already
# applies a stage's VkSpecializationInfo in spirv_to_nir, but the standalone path
# had no option to carry one and stubbed the conversion to NULL. The patch adds the
# option, points the stage at it and restores Mesa's conversion; docs/M5_PHASE_C.md,
# R9 of the vkQuake port's requests.
python3 "$root/tooling/psbc/patch-specialization.py" "$tree"

# The subpass input read: the fork lowers an input attachment to the tile
# coordinate intrinsic, which its ACO has no case for, so a shader that reads
# one does not compile at all. The patch asks for the pass's other form, the
# texel fetch through the input attachment's own descriptor -- which is what
# this driver binds; docs/M5_PHASE_C.md, R10.
python3 "$root/tooling/psbc/patch-subpass-input.py" "$tree"

# libpsbc_support.ps5.a completes the archive for titles that link only the
# compiler: ps5-opengl's C package writer (identical to the Python writer for
# every probe package, A2), S3TC (ps5-opengl leaves it out because its Mesa
# build supplies it, but the format table still references every DXT pack,
# unpack and fetch function) and the PS5 shims. Two more util sources the PS5
# archive leaves out are needed by Mesa's Vulkan runtime (Phase B1): os_time.c
# (fence and sync waits) and u_process.c (process name, through getprogname).
support_objects=(../../src/platform/ps5_agc_package.ps5.o src/util/format/u_format_s3tc.ps5.o
    src/util/os_time.ps5.o src/util/u_process.ps5.o ../../src/platform/psbc_ps5_shims.ps5.o)

log="$work/build.log"
started=$(date +%s)
# tooling/psbc/support.mk adjusts the flags of the util support objects.
if ! make -C "$tree" -f "$work/toolchain/Makefile.opengnm-psbc-ps5" -f "$root/tooling/psbc/support.mk" \
        -j"$(nproc)" PS5_PAYLOAD_SDK="$payload" \
        CC="$(ps5vk_ccache "$payload/bin/prospero-clang")" \
        CXX="$(ps5vk_ccache "$payload/bin/prospero-clang++")" \
        libpsbc "${support_objects[@]}" > "$log" 2>&1; then
    grep -E 'error' "$log" | head -n 40
    echo "PS5 compiler build failed; see $log" >&2
    exit 1
fi
seconds=$(( $(date +%s) - started ))
archive="$tree/libpsbc.ps5.a"
support="$work/libpsbc_support.ps5.a"
rm -f "$support"
"$payload/bin/prospero-ar" rcs "$support" "${support_objects[@]/#/$tree/}"
# make only recompiles changed sources, so both counts describe this run.
compiled=$(grep -cE ' -c [^ ]+\.(c|cpp) ' "$log" || true)
warnings=$(grep -c 'warning:' "$log" || true)

mkdir -p "$install/lib" "$install/include"
# The SDK compiles with -g. The installed archives drop debug information so
# titles that link them stay small; the full archives remain in the build
# directory for debugging.
cp "$archive" "$install/lib/libpsbc.ps5.a"
cp "$support" "$install/lib/libpsbc_support.ps5.a"
"$payload/bin/prospero-strip" --strip-debug "$install/lib/libpsbc.ps5.a" "$install/lib/libpsbc_support.ps5.a"
cp "$tree/libpsbc/psbc_compile.h" "$work/src/platform/ps5_agc_package.h" "$install/include/"
{
    echo "libpsbc.ps5.a: opengnm-psbc (MIT) built for the PS5 by tools/build-psbc-ps5.sh"
    echo "sdk: $sdk"
    echo "sdk commit: $(git -C "$sdk" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "compiler tree: $(git -C "$sdk/third_party/opengnm-psbc" rev-parse HEAD) + toolchain/opengnm-psbc-ps5.patch (verified)"
    echo "makefile sha256: $(sha256sum "$sdk/toolchain/Makefile.opengnm-psbc-ps5" | cut -d' ' -f1)"
    echo "make configuration sha256: $(sha256sum "$sdk/toolchain/opengnm-psbc-ps5.mak" | cut -d' ' -f1)"
    echo "shims sha256: $(sha256sum "$sdk/src/platform/ps5_mesa_shims.c" | cut -d' ' -f1)"
    echo "patch: $cpu_detect sysctl length int -> size_t (2 sites)"
    echo "patch: vertex formats appended to PsbcVertexFormat (26 layouts)"
    echo "patch: texel-buffer descriptor types appended to PsbcDescriptorType (2 values)"
    echo "compiler: $("$payload/bin/prospero-clang" --version | head -n 1)"
    echo "objects: $("$payload/bin/prospero-ar" t "$archive" | wc -l)"
    echo "this run: $compiled sources compiled, $warnings compiler warnings"
    echo "build archive sha256: $(sha256sum "$archive" | cut -d' ' -f1) ($(stat -c %s "$archive") bytes, with debug information)"
    echo "installed archive sha256: $(sha256sum "$install/lib/libpsbc.ps5.a" | cut -d' ' -f1) ($(stat -c %s "$install/lib/libpsbc.ps5.a") bytes, debug information stripped)"
    echo "libpsbc_support.ps5.a: ps5_agc_package.c (sha256 $(sha256sum "$sdk/src/platform/ps5_agc_package.c" | cut -d' ' -f1)) + u_format_s3tc.c + tooling/psbc/psbc_ps5_shims.c (sha256 $(sha256sum "$root/tooling/psbc/psbc_ps5_shims.c" | cut -d' ' -f1))"
    echo "support util objects: os_time.c + u_process.c, flags adjusted by tooling/psbc/support.mk (sha256 $(sha256sum "$root/tooling/psbc/support.mk" | cut -d' ' -f1))"
    echo "installed support archive sha256: $(sha256sum "$install/lib/libpsbc_support.ps5.a" | cut -d' ' -f1) ($(stat -c %s "$install/lib/libpsbc_support.ps5.a") bytes)"
} > "$install/PROVENANCE.txt"
cat "$install/PROVENANCE.txt"
echo "built in ${seconds} s; installed to $install"
