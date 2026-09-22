#!/usr/bin/env bash
# PS5 Vulkan compatibility probe - check the PS5 Vulkan driver.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase B (docs/M5_PHASE_B.md). Runs every driver test
# (driver/tests/vk_*_test.c, see ps5vk_test.h) against the outputs of
# tools/build-driver.sh:
# 1. PC, through the Khronos Vulkan loader: VK_DRIVER_FILES names only
#    libvulkan_ps5vk.so, and implicit layers are disabled.
# 2. PC, directly through the driver's vk_icdGetInstanceProcAddr, linked with
#    the driver and runtime archives, as a console title uses them.
# 3. PS5: each direct build linked the way a console title links, then passed
#    through the title converter. Not run on the console.
# Negative tests run only in the direct PC build, with the host layer made to
# break a hardware rule (PS5_HOST_* variables, host/ps5/ps5_host.cpp), set here
# or by the test itself around the calls it checks.
# The B7 and B8 draws run against a replay of the console's b4-headless frame
# (PS5_HOST_REPLAY), and every submission the host layer records for them
# (PS5_HOST_SUBMISSION_DUMP) must equal that frame, its draw repeated as often
# as the submission draws (tools/golden.py compare-submission). The C1
# presentation runs against a replay of the console's own driver run, and its
# frames and flips must equal that run's streams (tools/golden.py compare-run,
# golden/c1-triangle).
# With test names as arguments only those tests' three arms run, without the
# negative tests and without building the other tests' replays, so a driver
# change costs one test's build and comparison instead of every test's:
#   tools/check-driver.sh c4_rtt

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
sdk_root="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
runtime="$root/.deps/native/vulkan-runtime"
driver="$root/build/driver"
headers="$sdk/third_party/Vulkan-Headers/include"
# The shader compiler as the driver links it (tools/build-driver.sh).
host_psbc="$driver/host/libpsbc_driver.pic.a"
host_writer="$driver/host/ps5_agc_package.o"
tests_dir="$root/driver/tests"
# One test per line: a parked patch that adds a test adds a line here, and a
# line addition does not collide with another one the way a single long line
# does (the guard in tests/test_tools.py catches it when it does).
tests=(
    b2_device
    b2_commands
    b3_memory
    b3_buffer
    b3_image
    b5_submit
    b6_pipeline
    b7_draw
    b8_groups
    c1_present
    c2_indexed
    c2_staging
    c2_instancing
    c3_uniform
    c3_quad
    c4_texture
    c4_rtt
    c5_depth
    c5_stencil
    c5_depth_bias
    v0_push_constant
    c7_tiled_mip
    c7_copy
    c7_blit_formats
    c8_msaa
    c8_resolve
    d2_compute
    v0_query_full
    v0_robust
    v0_formats
    d1_dynamic_ubo
    c2_transfers
    c7_readback
    c7_clear_image
    c2_indirect
    b8_secondary
    b5_events
    b6_pipeline_cache
    v0_timestamp
    v0_vertex_sint
    v0_vertex_uint
    c7_mip_upload
    b2_buffer_view
    v0_array
    v0_cube
    v0_texel_buffer
    psbc_multiset
    v0_multiset_draw
    v0_multiset_quake
)
# Negative tests: name, and the host variable that breaks the rule it checks
# unless the test sets it itself (b3_window).
negatives=("b3_window"
           "b5_lost PS5_HOST_DROP_COMPLETION_MARKERS=1")
# The tests named on the command line, if any: only they are built and run.
selected=("$@")
if (( ${#selected[@]} > 0 )); then
    for test in "${selected[@]}"; do
        [[ " ${tests[*]} " == *" $test "* ]] ||
            { echo "unknown test $test; known: ${tests[*]}" >&2; exit 2; }
    done
    tests=("${selected[@]}")
    negatives=()
fi
# Whether a test is being run: everything is, unless some were named.
want() {
    (( ${#selected[@]} == 0 )) || [[ " ${selected[*]} " == *"$1"* ]]
}
work="$driver/check"

for file in "$driver/host/libvulkan_ps5vk.so" "$driver/host/ps5vk_icd.x86_64.json" \
        "$driver/host/libps5vk.a" "$driver/host/ps5_host.o" "$driver/host/agc_host.o" \
        "$driver/ps5/libps5vk.ps5.a" "$driver/ps5/libpsbc_driver.ps5.a" "$host_psbc" "$host_writer" \
        "$runtime/lib/libvk_runtime.a" "$runtime/lib/libvk_runtime.ps5.a"; do
    [[ -f $file ]] || { echo "missing $file; run tools/build-driver.sh" >&2; exit 2; }
done
loader=$(ldconfig -p | awk '/libvulkan\.so\.1 .*x86-64/ && !found { print $NF; found = 1 }')
[[ -n $loader && -f $loader ]] || { echo "the Khronos Vulkan loader (libvulkan.so.1) is not installed" >&2; exit 2; }
tool="$root/build/host/ps5-native-tool"
[[ -x $tool ]] || { echo "missing $tool; run tools/build.sh once" >&2; exit 2; }
# shellcheck source=tools/psbc-link.sh
source "$root/tools/psbc-link.sh"
psbc_link_recipe "$root" "$sdk_root" || exit 2
# The PS5 links take the driver's compiler copy instead of libpsbc.ps5.a.
driver_compiler_inputs=(-L "$sdk_root/target/lib" --start-group
    "$driver/ps5/libpsbc_driver.ps5.a" "$root/.deps/native/psbc/lib/libpsbc_support.ps5.a"
    "${psbc_runtime_archives[@]}" --end-group)
mkdir -p "$work"
# Regressions for the compiler archive used by both driver builds: distinct
# fragment inputs, and the compile-order state the console has faulted in (the
# host half of the ACO round; docs/M5_PHASE_C.md, blocker round 8).
bash "$root/tools/check-fragment-inputs.sh"
bash "$root/tools/check-aco-state.sh"
# And the same archive's committed output: every probe package still compiles to
# itself. This is the only check that catches a package drifting from the
# compiler -- the runner's compile keyword covers 40 of the 43 committed sets and
# appears in one queue out of 93 (tools/check-probe-packages.sh).
bash "$root/tools/check-probe-packages.sh"
# The B6 test compiles the probe sets' SPIR-V and compares their packages.
export PS5VK_PROBES="$root/probes"
draw_golden="$root/golden/b5/b4-headless-1.json"
present_run="$root/golden/c1-triangle/run-1.json"
# The C2 indexed test is compared with the console's own driver run, like C1's
# present test: the runner's m3-vertex frame cannot be that reference, because
# it keeps its vertex and index data inside its 64 KiB stage workspace while the
# driver allocates buffers of its own, and compare-run has no address tolerance.
# The capture carries the regions the driver allocated -- its stage, its table
# chunks and its live bound buffers -- so the replay pins a PC run's allocations
# to the console's addresses (src/diagnostics.cpp, log_driver_regions).
indexed_run="$root/golden/c2-indexed/run-1.json"
# The staging test copies its geometry into the buffers the draw binds, so its
# own run allocated a staging buffer too and the addresses only line up against
# that run's capture.
staging_run="$root/golden/c2-staging/run-1.json"
# The instancing test draws its two frames against the console's own
# c2-instancing run, whose capture holds the count packet the driver programs
# with sceAgcDcbSetNumInstances: immediately before the draw, and one after it.
instancing_run="$root/golden/c2-instancing/run-1.json"
# The tiled mip chain draws against its own console run: its image is a tiled
# attachment the probe fills itself, so only a capture of that run holds the
# addresses (golden/c7-mip-tiled, the runner's c7-mip-tiled test).
tiled_mip_run="$root/golden/c7-mip-tiled/run-1.json"
if want b7_draw || want b8_groups || want c2_indirect || want v0_multiset_draw; then
    python3 "$root/tools/golden.py" replay "$draw_golden" "$work/b4-headless.replay"
fi
# R7 round 3's two-set frame runs against the console capture it is the host
# half of: golden/v0-multiset-quake holds the case's own driver run (its two
# pipeline stages and its submission), and the replay built from it is where the
# driver's AGC register defaults come from. It needs the capture's own
# runner-built sibling for that table -- a driver case's capture carries none --
# which is why jobs/v0-multiset-quake/queue.txt names m2-solid beside the case.
# R7 step 1b's MRT frames draw against the console capture of their own case: the
# probe's shader writes four outputs, and the register tables a shader needs are
# part of what a replay carries -- another frame's replay is sized for another
# shader and refuses this one ("a created shader's register tables are out of
# bounds", measured with b4-headless).
# R7 step 1b's MRT frames draw against the replay of their own console capture:
# the probe's shader writes four outputs, and the register tables a shader needs
# are part of what a replay carries, so another frame's replay refuses it.
# The capture must hold a runner-built frame too (it carries the context register
# table a replay takes its defaults from), which is why jobs/v0-mrt's queue names
# m2-solid *before* the probe: the probe's own multi-attachment frame wedges the
# title after it has drawn.
mrt_run="$root/golden/v0-mrt/run-1.json"
want v0_mrt &&
    python3 "$root/tools/golden.py" replay "$mrt_run" "$work/v0-mrt.replay"
multiset_quake_run="$root/golden/v0-multiset-quake/run-1.json"
want v0_multiset_quake &&
    python3 "$root/tools/golden.py" replay "$multiset_quake_run" "$work/v0-multiset-quake.replay"
# The C5 depth test runs against the console's own c5-depth run, like every
# other driver-path test: its depth attachment means the submission names an
# address no runner frame has, so only a capture of itself provides the stage
# mapping and the allocations its replay hands out. The golden's own probe
# recorded the tables that carry the depth registers, and the test asserts their
# values against the M4 canary's (docs/M5_REFERENCE.md, C5).
depth_run="$root/golden/c5-depth/run-1.json"
want c5_depth &&
    python3 "$root/tools/golden.py" replay "$depth_run" "$work/c5-depth.replay" --test c5-depth
# Round 12's stencil frame is the same shape: its combined depth/stencil
# attachment is memory the driver mapped itself, so the capture of its own run
# (golden/v0-stencil, four submissions and twelve pipelines) is what provides
# the stage mapping and the allocations its replay hands out.
stencil_run="$root/golden/v0-stencil/run-1.json"
want c5_stencil &&
    python3 "$root/tools/golden.py" replay "$stencil_run" "$work/v0-stencil.replay" \
        --test v0-stencil
# R1's depth bias: its own case's capture, because the six PA_SU_POLY_OFFSET_*
# words are the pipeline's and the attachment's state together and the words are
# the gate, not a stream comparison (the case's own golden, golden/v0-depth-bias,
# is where the console run shows the unbiased frame recording none of them).
bias_run="$root/golden/v0-depth-bias/run-1.json"
want c5_depth_bias &&
    python3 "$root/tools/golden.py" replay "$bias_run" "$work/v0-depth-bias.replay" \
        --test v0-depth-bias
# R9's push constants: the case's own capture, because the words this test reads
# are the descriptor chain the draw recorded -- the pixel stage's user data, the
# table it names and the 16 bytes the reserved binding points at -- and not a
# stream comparison.
push_run="$root/golden/v0-push-constant/run-1.json"
want v0_push_constant &&
    python3 "$root/tools/golden.py" replay "$push_run" "$work/v0-push-constant.replay" \
        --test v0-push-constant
# The V0-query test too: its query counters live in memory the driver mapped
# itself, which no runner frame's capture holds, so only a capture of the test
# provides the addresses its replay hands out (as the C5 depth attachment is).
# The case's own golden directory since the compiler migration re-captured it:
# the v0-query-driver capture beside it is a pre-migration run, and the driver
# now writes one more user-data word than that run recorded
# (docs/M5_PHASE_C.md, 2026-09-20).
query_run="$root/golden/v0-query-full/run-1.json"
want v0_query_full &&
    python3 "$root/tools/golden.py" replay "$query_run" "$work/v0-query-full.replay" \
        --test v0-query-full
want c1_present &&
    python3 "$root/tools/golden.py" replay "$present_run" "$work/c1-triangle.replay" --test c1-triangle
want c2_indexed &&
    python3 "$root/tools/golden.py" replay "$indexed_run" "$work/c2-indexed.replay" --test c2-indexed
want c2_staging &&
    python3 "$root/tools/golden.py" replay "$staging_run" "$work/c2-staging.replay" --test c2-staging
want c2_instancing &&
    python3 "$root/tools/golden.py" replay "$instancing_run" "$work/c2-instancing.replay" \
        --test c2-instancing
want c7_tiled_mip &&
    python3 "$root/tools/golden.py" replay "$tiled_mip_run" "$work/c7-mip-tiled.replay" \
        --test c7-mip-tiled
# V0-robust draws against its own console run: the second frame's stream is
# where the clamped index count shows (golden/v0-robust, the runner's v0-robust
# test).
robust_run="$root/golden/v0-robust/run-1.json"
want v0_robust &&
    python3 "$root/tools/golden.py" replay "$robust_run" "$work/v0-robust.replay" \
        --test v0-robust
# V0-query's timestamps draw against their own console run: the two writes name
# the timestamp pool's counters, which the driver mapped itself, so only a
# capture of the test hands out the addresses its replay needs (golden/
# v0-timestamp-driver, the runner's v0-timestamp-driver test).
timestamp_run="$root/golden/v0-timestamp-driver/run-1.json"
want v0_timestamp &&
    python3 "$root/tools/golden.py" replay "$timestamp_run" "$work/v0-timestamp.replay" \
        --test v0-timestamp-driver
# D1's array layers draw against their own console run: the layers' places are
# the measured slices, so only a capture of that run gives the replay the
# addresses the two uploads write and the descriptor names (golden/v0-array-layers).
array_run="$root/golden/v0-array-layers/run-1.json"
want v0_array &&
    python3 "$root/tools/golden.py" replay "$array_run" "$work/v0-array.replay" \
        --test v0-array-layers
# C8's four-sample depth attachment is here once its console measurement exists:
# driver/tests/vk_unknowns_depth4x_test.c draws, so it needs a replay built from
# golden/unknowns-depth4x's register defaults, and its measurement run is the
# case unknowns-depth4x-map (jobs/unknowns-depth4x/queue.txt). Until then the
# test is outside the gate rather than failing it.
# Blocker round 5's uniform texel buffer draws against its own console run: what
# the case checks is the table the draw writes, and a PC run can only build that
# table with the register defaults and allocations a capture of the console run
# holds (golden/v0-texel-buffer, the runner's v0-formats-texel-buffer test).
texel_buffer_run="$root/golden/v0-texel-buffer/run-1.json"
want v0_texel_buffer &&
    python3 "$root/tools/golden.py" replay "$texel_buffer_run" "$work/v0-texel-buffer.replay" \
        --test v0-formats-texel-buffer
# D1's cube faces draw against their own console run: the six slices' places
# come from the oracle's array rows, so only a capture of that run hands the
# replay the addresses the six uploads write (golden/v0-cube-faces).
cube_run="$root/golden/v0-cube-faces/run-1.json"
want v0_cube &&
    python3 "$root/tools/golden.py" replay "$cube_run" "$work/v0-cube.replay" \
        --test v0-cube-faces
# C7's tiled upload draws against its own console run: the levels' places in the
# chain are the measured bases, so only a capture of that run gives the replay
# the addresses the copies write (golden/c7-mip-upload).
upload_run="$root/golden/c7-mip-upload/run-1.json"
want c7_mip_upload &&
    python3 "$root/tools/golden.py" replay "$upload_run" "$work/c7-mip-upload.replay" \
        --test c7-mip-upload
# V0-formats' two integer vertex formats draw against their own console runs:
# the vertex buffer descriptors carry the compiler's format word for
# R32G32B32_SINT and R32G32B32_UINT, which only a capture of those runs hands
# the replay (golden/v0-vertex-sint, golden/v0-vertex-uint).
sint_run="$root/golden/v0-vertex-sint/run-1.json"
want v0_vertex_sint &&
    python3 "$root/tools/golden.py" replay "$sint_run" "$work/v0-vertex-sint.replay" \
        --test v0-vertex-sint
uint_run="$root/golden/v0-vertex-uint/run-1.json"
want v0_vertex_uint &&
    python3 "$root/tools/golden.py" replay "$uint_run" "$work/v0-vertex-uint.replay" \
        --test v0-vertex-uint
# D1's dynamic uniform buffer draws against its own console run: the two frames'
# descriptors differ by the dynamic offset alone (golden/d1-dynamic-ubo).
dynamic_run="$root/golden/d1-dynamic-ubo/run-1.json"
want d1_dynamic_ubo &&
    python3 "$root/tools/golden.py" replay "$dynamic_run" "$work/d1-dynamic-ubo.replay" \
        --test d1-dynamic-ubo
# V0-formats draws its seventeen frames against its own console run: each
# format's combined image sampler descriptor carries that format's
# IMG_DATA_FORMAT and DST_SEL word, and the capture is where the console's own
# words are (golden/v0-formats-sampled, the runner's v0-formats-sampled test).
formats_run="$root/golden/v0-formats-sampled/run-1.json"
want v0_formats &&
    python3 "$root/tools/golden.py" replay "$formats_run" "$work/v0-formats.replay" \
        --test v0-formats-sampled
# C7's copies draw against their own console run: the tiled destination the
# frames sample is an image the copy wrote, so only a capture of that run holds
# its addresses and the tile placement the frames' readback proves
# (golden/c7-copy, the runner's c7-copy test).
copy_run="$root/golden/c7-copy/run-1.json"
want c7_copy &&
    python3 "$root/tools/golden.py" replay "$copy_run" "$work/c7-copy.replay" --test c7-copy
# C7's per-format blits draw against their own console run too: each frame's
# solid texture of one format is decoded into a tiled destination by the
# resampler, so the run's addresses and stage images are what a PC rebuild
# replays (golden/c7-blit-formats, the runner's c7-blit-formats test).
blit_formats_run="$root/golden/c7-blit-formats/run-1.json"
want c7_blit_formats &&
    python3 "$root/tools/golden.py" replay "$blit_formats_run" "$work/c7-blit-formats.replay" \
        --test c7-blit-formats
# C8's four-sample frame too: its target is a four-sample image the driver
# sized, so only a capture of that run holds its addresses (golden/c8-msaa).
msaa_run="$root/golden/c8-msaa/run-1.json"
# C8's resolve: the four-sample image it reads is the frame's own, so only a
# capture of that run holds its addresses (golden/c8-resolve).
resolve_run="$root/golden/c8-resolve/run-1.json"
# D2's compute dispatch: the shader code, the descriptor table and the storage
# buffer are all the driver's own allocations, so only a capture of that run
# holds their addresses (golden/d2-compute).
compute_run="$root/golden/d2-compute/run-1.json"
want c8_msaa &&
    python3 "$root/tools/golden.py" replay "$msaa_run" "$work/c8-msaa.replay" --test c8-msaa
want c8_resolve &&
    python3 "$root/tools/golden.py" replay "$resolve_run" "$work/c8-resolve.replay" \
        --test c8-resolve
want d2_compute &&
    python3 "$root/tools/golden.py" replay "$compute_run" "$work/d2-compute.replay" \
        --test d2-compute
# The C3 uniform test is compared with its own console run, like the C2 tests:
# the application's buffer is one of the addresses its submission names.
uniform_run="$root/golden/c3-uniform/run-1.json"
quad_run="$root/golden/c3-quad/run-1.json"
texture_run="$root/golden/c4-texture/run-1.json"
# The render-to-texture test's own run too: its submission names the colour
# image the frame renders into and samples, which the c4-texture capture does
# not hold, so its replay has to be its own.
rtt_run="$root/golden/c4-rtt/run-1.json"
want c3_uniform &&
    python3 "$root/tools/golden.py" replay "$uniform_run" "$work/c3-uniform.replay" --test c3-uniform
want c3_quad &&
    python3 "$root/tools/golden.py" replay "$quad_run" "$work/c3-quad.replay" --test c3-quad
want c4_texture &&
    python3 "$root/tools/golden.py" replay "$texture_run" "$work/c4-texture.replay" --test c4-texture
want c4_rtt &&
    python3 "$root/tools/golden.py" replay "$rtt_run" "$work/c4-rtt.replay" --test c4-rtt
declare -A results
# psbc_include (the compiler's public header, which driver/ps5vk_private.h
# includes by name and driver/tests/vk_psbc_multiset_test.c includes to read the
# metadata struct) comes from tools/psbc-link.sh, sourced above.
flags=(-std=c11 -O2 -Wall -Wextra -Werror -I "$headers" -I "$tests_dir" "${psbc_include[@]}")
# The B7 draw program links into every PC test; only b7_draw calls it. The
# compute program is Phase D2's harness: the D2 test and the runner's d2-compute
# case share it (driver/tests/ps5vk_compute.c).
gcc "${flags[@]}" -c "$tests_dir/ps5vk_triangle.c" -o "$work/ps5vk_triangle.o"
gcc "${flags[@]}" -c "$tests_dir/ps5vk_compute.c" -o "$work/ps5vk_compute.o"

record() {
    local key=$1
    shift
    if "$@"; then results[$key]=PASS; else results[$key]=FAIL; fi
}

# Runs one PC build of a test. The B7 and B8 draws run against the
# b4-headless replay, and their submissions must equal that frame with its
# draw repeated (B7: one submission of one draw; B8: two, two, two, one, one,
# as Mesa's queue merges the two VkSubmitInfos of one vkQueueSubmit into one
# submission, vk_queue_submits_merge). The C1 presentation runs against the
# console's own driver run, golden/c1-triangle: the same code path on both
# sides, so its four frames and four flips must equal that run's eight streams
# word for word. The b7 and b8 draws compare against runner-built frames
# instead, and every one of their draws differs from the console in
# PA_CL_VPORT_YSCALE (0x111): the driver programs Vulkan's orientation,
# +1080.0, where the runner programs ProsperoLight's -1080.0
# (driver/ps5vk_draw.c).
run_test() {
    local test=$1 mode=$2 replay
    # An empty array, not an unset one: ${#compare[@]} is read below under set -u.
    local -a compare=()
    shift 2
    local dump="$work/${test}_$mode.dump"
    case $test in
        b7_draw) replay=b4-headless
            compare=(compare-submission "$draw_golden" "$dump" --draws 1) ;;
        c2_indirect) replay=b4-headless
            compare=(compare-submission "$draw_golden" "$dump" --draws 1) ;;
        b8_secondary) replay=b4-headless
            compare=(compare-submission "$draw_golden" "$dump" --draws 1) ;;
        b8_groups) replay=b4-headless
            compare=(compare-submission "$draw_golden" "$dump" --draws 2,2,2,1,1) ;;
        c1_present) replay=c1-triangle
            compare=(compare-run "$present_run" "$dump" --test c1-triangle) ;;
        c2_indexed) replay=c2-indexed
            compare=(compare-run "$indexed_run" "$dump" --test c2-indexed) ;;
        c2_staging) replay=c2-staging
            compare=(compare-run "$staging_run" "$dump" --test c2-staging) ;;
        c2_instancing) replay=c2-instancing
            compare=(compare-run "$instancing_run" "$dump" --test c2-instancing) ;;
        c7_tiled_mip) replay=c7-mip-tiled
            compare=(compare-run "$tiled_mip_run" "$dump" --test c7-mip-tiled) ;;
        v0_robust) replay=v0-robust
            compare=(compare-run "$robust_run" "$dump" --test v0-robust) ;;
        d1_dynamic_ubo) replay=d1-dynamic-ubo
            compare=(compare-run "$dynamic_run" "$dump" --test d1-dynamic-ubo) ;;
        v0_formats) replay=v0-formats
            compare=(compare-run "$formats_run" "$dump" --test v0-formats-sampled) ;;
        c7_copy) replay=c7-copy
            compare=(compare-run "$copy_run" "$dump" --test c7-copy) ;;
        c7_blit_formats) replay=c7-blit-formats
            compare=(compare-run "$blit_formats_run" "$dump" --test c7-blit-formats) ;;
        c8_msaa) replay=c8-msaa
            compare=(compare-run "$msaa_run" "$dump" --test c8-msaa) ;;
        c8_resolve) replay=c8-resolve
            compare=(compare-run "$resolve_run" "$dump" --test c8-resolve) ;;
        d2_compute) replay=d2-compute
            compare=(compare-run "$compute_run" "$dump" --test d2-compute) ;;
        v0_timestamp) replay=v0-timestamp
            compare=(compare-run "$timestamp_run" "$dump" --test v0-timestamp-driver) ;;
        v0_vertex_sint) replay=v0-vertex-sint
            compare=(compare-run "$sint_run" "$dump" --test v0-vertex-sint) ;;
        v0_vertex_uint) replay=v0-vertex-uint
            compare=(compare-run "$uint_run" "$dump" --test v0-vertex-uint) ;;
        c7_mip_upload) replay=c7-mip-upload
            compare=(compare-run "$upload_run" "$dump" --test c7-mip-upload) ;;
        v0_array) replay=v0-array
            compare=(compare-run "$array_run" "$dump" --test v0-array-layers) ;;
        v0_cube) replay=v0-cube
            compare=(compare-run "$cube_run" "$dump" --test v0-cube-faces) ;;
        c3_uniform) replay=c3-uniform
            compare=(compare-run "$uniform_run" "$dump" --test c3-uniform) ;;
        c3_quad) replay=c3-quad
            compare=(compare-run "$quad_run" "$dump" --test c3-quad) ;;
        c4_texture) replay=c4-texture
            compare=(compare-run "$texture_run" "$dump" --test c4-texture) ;;
        c4_rtt) replay=c4-rtt
            compare=(compare-run "$rtt_run" "$dump" --test c4-rtt) ;;
        c5_depth) replay=c5-depth
            compare=(compare-run "$depth_run" "$dump" --test c5-depth) ;;
        # The stencil test's own words are the gate, not a stream comparison:
        # the replay places the console run's pipelines in creation order and
        # this program draws the first of its four frames.
        c5_stencil) replay=v0-stencil
            compare=() ;;
        # R1's depth bias: the six PA_SU_POLY_OFFSET_* words are the gate, not a
        # stream comparison, and the replay of the case's own capture is what
        # gives this program the stage mapping it draws through.
        c5_depth_bias) replay=v0-depth-bias
            compare=() ;;
        v0_push_constant) replay=v0-push-constant
            compare=() ;;
        # The Round 2 frame draws like the plain colour-target frame the B7 draw
        # does; what it needs from a replay is AGC's register defaults, and its
        # own pipelines and tables are what the test asserts.
        v0_multiset_draw) replay=b4-headless
            compare=() ;;
        # R7 round 3's frame is the host half of the runner case v0-multiset-quake
        # and draws exactly its one frame, so its recording is compared with that
        # case's own console capture word for word.
        v0_multiset_quake) replay=v0-multiset-quake
            compare=(compare-run "$multiset_quake_run" "$dump" --test v0-multiset-quake) ;;
        v0_query_full) replay=v0-query-full
            compare=(compare-run "$query_run" "$dump" --test v0-query-full) ;;
        v0_texel_buffer) replay=v0-texel-buffer
            compare=() ;;
        *) "$@"; return ;;
    esac
    # C1 and C2 compare against the console's own driver runs, which wrote the
    # user data and the viewport the driver writes; the runner-built frames the
    # other draws compare with never did.
    if [[ $test != c1_present && $test != c2_indexed && $test != c2_staging &&
        $test != c2_instancing && $test != c7_tiled_mip && $test != v0_robust &&
        $test != v0_formats && $test != c7_copy && $test != c7_blit_formats &&
        $test != c8_msaa && $test != c8_resolve && $test != d2_compute &&
        $test != d1_dynamic_ubo &&
        $test != c3_uniform &&
        $test != c3_quad && $test != c4_texture &&
        $test != c4_rtt && $test != c5_depth && $test != c5_stencil &&
        $test != c5_depth_bias && $test != v0_push_constant &&
        $test != v0_query_full &&
        $test != v0_timestamp && $test != v0_vertex_sint && $test != v0_vertex_uint &&
        $test != v0_multiset_quake &&
        $test != c7_mip_upload && $test != v0_array && $test != v0_cube &&
        ${#compare[@]} -gt 0 ]]; then
        # Every draw the driver records writes both stages' user data, which the
        # runner frames this replay is compared with never wrote: an AGC shader
        # object's register tables hold no user-data registers, so the driver
        # has to (HARDWARE_FINDINGS.md, pid 117). Both writes are declared here,
        # as is the viewport the driver's orientation turns over.
        compare+=(--extra-sh-register 0x8c --extra-sh-register 0x0c
                  --expect-record 0x111=0x44870000)
    fi
    rm -f "$dump"
    # A test whose golden is not in the tree yet runs against the replay alone:
    # the console capture is what its submission is compared with, and until
    # that exists the replay is what gives the run AGC's register defaults.
    if [[ ${#compare[@]} -eq 0 ]]; then
        PS5_HOST_REPLAY="$work/$replay.replay" PS5_HOST_SUBMISSION_DUMP="$dump" "$@"
    else
        PS5_HOST_REPLAY="$work/$replay.replay" PS5_HOST_SUBMISSION_DUMP="$dump" "$@" &&
            python3 "$root/tools/golden.py" "${compare[@]}"
    fi
}

# The direct PC build of one test.
build_direct() {
    local test=$1
    gcc "${flags[@]}" -DPS5VK_TEST_DIRECT -c "$tests_dir/vk_${test}_test.c" -o "$work/${test}_direct.o"
    g++ -o "$work/${test}_direct" "$work/${test}_direct.o" "$work/ps5vk_triangle.o" \
        "$work/ps5vk_compute.o" \
        -Wl,--whole-archive "$driver/host/libps5vk.a" "$runtime/lib/libvk_runtime.a" -Wl,--no-whole-archive \
        "$driver/host/ps5_host.o" "$driver/host/agc_host.o" "$host_writer" "$host_psbc" -pthread -lm
}

echo "== PC, through the Vulkan loader ($loader)"
for test in "${tests[@]}"; do
    gcc "${flags[@]}" "$tests_dir/vk_${test}_test.c" "$work/ps5vk_triangle.o" \
        "$work/ps5vk_compute.o" -o "$work/${test}_loader" "$loader"
    record "$test loader" run_test "$test" loader env \
        VK_DRIVER_FILES="$driver/host/ps5vk_icd.x86_64.json" VK_LOADER_LAYERS_DISABLE='~all~' \
        "$work/${test}_loader"
done

echo "== PC, direct"
for test in "${tests[@]}"; do
    build_direct "$test"
    record "$test direct" run_test "$test" direct "$work/${test}_direct"
done

echo "== PC, direct, negative"
for negative in "${negatives[@]}"; do
    read -r test variable <<< "$negative"
    echo "-- $test${variable:+ with $variable}"
    build_direct "$test"
    # shellcheck disable=SC2086
    record "$test negative" env ${variable:+"$variable"} "$work/${test}_direct"
done

echo "== PS5, direct link and title converter"
export PS5_PAYLOAD_SDK="$sdk_root"
compile=(sh "$root/tooling/prospero-clang18")
cxx_flags=(-std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
"${compile[@]}" "${cxx_flags[@]}" -c "$native/app_crt.cpp" -o "$work/app_crt.o"
"${compile[@]}" "${cxx_flags[@]}" -c "$native/app_cpp_runtime.cpp" -o "$work/app_cpp_runtime.o"
"${compile[@]}" "${flags[@]}" -ffunction-sections -fdata-sections \
    -c "$tests_dir/ps5vk_triangle.c" -o "$work/ps5vk_triangle.ps5.o"
"${compile[@]}" "${flags[@]}" -ffunction-sections -fdata-sections \
    -c "$tests_dir/ps5vk_compute.c" -o "$work/ps5vk_compute.ps5.o"

# AGC comes from PS5 system modules, not the payload SDK's stubs. As
# tools/build.sh does, link-only stub libraries let the linker and the title
# converter record the imports; the driver uses the linked-canary profile the
# test runner links (AGC_LINKED_CANARY=1), which declares sceAgcInit,
# sceAgcCbReleaseMem, sceAgcSuspendPoint and sceAgcDriverSubmitDcb.
build_system_link_stub() {
    local library=$1 source=$2
    "${compile[@]}" -std=c11 -O2 -fPIC -ffunction-sections -fdata-sections \
        -c "$root/$source" -o "$work/${library}_link_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/${library}.so" "$work/${library}_link_stub.o"
}
build_system_link_stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
build_system_link_stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
agc_stubs=("$work/libSceAgc.so" "$work/libSceAgcDriver.so")

# Links one test as a console title (runtime and driver whole, with Mesa's weak
# entry points resolved at link time; see tools/check-vulkan-runtime.sh) and
# converts it.
ps5_link() {
    local test=$1
    local elf="$work/${test}.ps5.elf" log="$work/${test}.link.log"
    # record runs this in an if, where set -e does not apply.
    "${compile[@]}" "${flags[@]}" -DPS5VK_TEST_DIRECT -ffunction-sections -fdata-sections \
        -c "$tests_dir/vk_${test}_test.c" -o "$work/${test}.ps5.o" ||
        { echo "$test PS5 compile: FAIL"; return 1; }
    if ! "$sdk_root/bin/prospero-lld" "${psbc_linker_script[@]}" --eh-frame-hdr --error-limit=0 \
            --no-dynamic-linker \
            -z nodynamic-undefined-weak \
            --version-script "$native/app-symbols.map" --exclude-libs=ALL \
            -e _start -o "$elf" \
            "$work/app_crt.o" "$work/app_cpp_runtime.o" "$work/${test}.ps5.o" \
            "$work/ps5vk_triangle.ps5.o" "$work/ps5vk_compute.ps5.o" \
            --whole-archive "$driver/ps5/libps5vk.ps5.a" "$runtime/lib/libvk_runtime.ps5.a" --no-whole-archive \
            "${driver_compiler_inputs[@]}" "${agc_stubs[@]}" \
            --as-needed "$sdk_root"/target/lib/*.so > "$log" 2>&1; then
        echo "$test PS5 link: FAIL"
        grep -oE "(undefined|duplicate) symbol: .*" "$log" | sort -u | head -n 40
        grep -vE "undefined symbol|duplicate symbol|>>> " "$log" | head -n 20
        return 1
    fi
    if ! "$tool" link --in "$elf" --out "$work/${test}.eboot.elf" \
            --stub-dir "$sdk_root/target/lib" --stub "${agc_stubs[0]}" --stub "${agc_stubs[1]}" \
            --module-sdk 0x02000009 \
            --companion-sdk 0x08050001 --file-name eboot.elf > "$work/${test}.convert.log" 2>&1; then
        cat "$work/${test}.convert.log"
        echo "$test PS5 link: FAIL; the title converter rejected the imports"
        return 1
    fi
    local imports
    imports=$("$sdk_root/bin/prospero-nm" -D --undefined-only "$elf" | awk '{ print $NF }' | sort -u)
    echo "$test: $(stat -c %s "$elf") byte ELF, converter accepted $(grep -c . <<< "$imports") imports"
}
for test in "${tests[@]}"; do
    record "$test PS5 link" ps5_link "$test"
done

echo
status=0
for test in "${tests[@]}"; do
    for mode in loader direct "PS5 link"; do
        printf '%-28s %s\n' "$test $mode" "${results[$test $mode]}"
        [[ ${results[$test $mode]} == PASS ]] || status=1
    done
done
for negative in "${negatives[@]}"; do
    read -r test _ <<< "$negative"
    printf '%-28s %s\n' "$test negative" "${results[$test negative]}"
    [[ ${results[$test negative]} == PASS ]] || status=1
done
echo "driver check: $([[ $status == 0 ]] && echo PASS || echo FAIL)"
exit $status
