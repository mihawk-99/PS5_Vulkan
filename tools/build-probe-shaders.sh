#!/usr/bin/env bash
# PS5 Vulkan - Build the AGC shader packages of one probe set.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# GLSL -> SPIR-V (glslang) -> NIR/ACO machine code and typed metadata
# (opengnm-psbc) -> AGC shader ELF (ps5-opengl agc_shader_package_writer.py).
#
# Sets:
#   m2          full-target triangle, constant colour          -> probes/m2
#   m3-uniform  full-target triangle, colour from a uniform
#               buffer at set 0, binding 0                     -> probes/m3
#   m3-vertex   geometry and colour from a vertex buffer,
#               drawn through an index buffer                  -> probes/m3-vertex
#   m3-texture  indexed square sampling a combined image
#               sampler at set 0, binding 0                    -> probes/m3-texture
#   m4-depth    clear and depth-tested rectangles from a
#               vertex buffer with z                           -> probes/m4-depth
#   m4-blend    the m4-depth shaders, pixel stage compiled for
#               FP16_ABGR colour exports, so hardware blending
#               into the RGBA8 target is correct               -> probes/m4-blend
#   b7-corner   top-left half triangle, constant colour, which
#               shows the viewport orientation (Phase B7)     -> probes/b7-corner
#   b8-corner   the b7-corner triangle in a second colour, a
#               frame's second draw (Phase B8)                -> probes/b8-corner
#   c1-clear    Mesa's vk_meta clear: a rectangle from a
#               vertex buffer, its colour from the clear
#               block at set 0, binding 0 (Phase C1)          -> probes/c1-clear
#   c3-quad     a vertex buffer's position scaled and
#               offset by a uniform buffer at set 0,
#               binding 0: the first vertex stage with
#               attributes and a descriptor (Phase C3)        -> probes/c3-quad
#   c7-mip      a banded quad sampling a mip chain at an
#               integer LOD per band, nearest mip filter
#               (Phase C7)                                     -> probes/c7-mip
#   c7-mip-linear  the same bands half a level further, so a
#               linear mip filter blends two levels
#               (Phase C7)                                     -> probes/c7-mip-linear
#   v0-array    two bands sampling two layers of one
#               2D array through a sampler2DArray, the
#               fetch D1's DEPTH/BASE_ARRAY fields are
#               for (D1)                                     -> probes/v0-array
#   v0-cube     six bands sampling six faces of one cube
#               through a samplerCube, the fetch D1's TYPE
#               11 and its six slices are for (D1)        -> probes/v0-cube
#   v0-vertex-sint  the m3-vertex square whose position is a
#               three-component signed integer, the third
#               component carried out as the fragment alpha
#               (V0-formats)                                   -> probes/v0-vertex-sint
#   v0-vertex-uint  the same with R32G32B32_UINT               -> probes/v0-vertex-uint
#
# Pixel stages compile for the legacy 32_ABGR export unless a set passes
# --color-format. That needs the probe CLI from tools/build-psbc-cli.sh, which
# is the SDK compiler plus that one option. For an 8-bit UNORM target, Mesa's
# ac_choose_spi_color_formats picks FP16_ABGR (4) for every variant, including
# blending; ps5-opengl's gallium runtime passes 0x99999994 for one RGBA8 target.
#
# Vertex packages use ESGS ring item size 1, as ps5-opengl's runtime packager
# (src/platform/ps5_agc_package.c) forces for every vertex-source NGG shader.
# Shaders that dereference GPU pointers are compiled for 32-bit addresses
# whose upper half is --address32-hi. Program-checksum registers stay
# unresolved, as in ps5-opengl's runtime packages.
#
# Each set writes vertex.bin, pixel.bin, checksums.txt (FNV-1a-64), the
# bindings.txt layout the title needs when the shaders read resources,
# SHA256SUMS and PROVENANCE.txt, after checking the compiler metadata.
#
# Run from the repository root:  bash tools/build-probe-shaders.sh m2|m3-uniform|v0-robust|m3-vertex|m3-texture|m4-depth|m4-blend|b7-corner|b8-corner|c1-clear|c3-quad|c7-mip|c7-mip-linear|c7-diag|c2-instance|c8-sampleid|v0-vertex-sint|v0-vertex-uint|v0-vertex-bytes-float|v0-vertex-bytes-uint|v0-vertex-bytes-sint|v0-array|v0-cube|v0-texture-uint|v0-texture-sint|v0-target-uint|v0-target-sint
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
compiler="$sdk/third_party/opengnm-psbc/opengnm-psbc"
# GLSLANG overrides the PATH lookup, which is where CachyOS's glslang package
# is found. The retired Windows drop held only glslang.exe, so it never served
# this branch.
if [[ -n ${GLSLANG:-} ]]; then
    glslang=$GLSLANG
else
    glslang=glslang
fi
set_name=${1:-}
pixel_compiler=$compiler
expected_col_format=

case $set_name in
m2)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/m2/solid.frag
    output=probes/m2
    vertex_flags=()
    pixel_flags=()
    ;;
m3-uniform)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/m3/uniform_colour.frag
    output=probes/m3
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:uniform_buffer:1:0:16)
    ;;
c8-sampleid)
    # Kept for the record, and it does NOT build: gl_SamplePosition and
    # gl_SampleID need SpvCapabilitySampleRateShading, which the compiler's
    # SPIR-V front end rejects ("Unsupported SPIR-V capability:
    # SpvCapabilitySampleRateShading (35)"). So a frame cannot name its samples
    # from inside the shader, and where the four sample words of a texel sit has
    # to be measured by coverage instead: a geometry edge covers some samples of
    # an edge pixel and not others (docs/M5_PHASE_C.md, C8).
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/c8/sample_position.frag
    output=probes/c8-sampleid
    vertex_flags=()
    pixel_flags=()
    exit 2
    ;;
v0-robust)
    # Kept for the record, and it does NOT build: the AGC compiler refuses any
    # uniform block larger than 16 bytes ("Shader compilation failed: internal
    # error"), so a shader-side out-of-bounds uniform read is not expressible
    # through it and V0-robust's reachable path is the driver's index-count
    # clamp (docs/M5_PHASE_C.md, V0-robust). The probe uses the m3-vertex set.
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/robust_colour.frag
    output=probes/v0-robust
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:uniform_buffer:1:0:32)
    ;;
m3-vertex)
    vertex_source=shaders/m3/vertex_colour.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/m3-vertex
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:24:4
        --vertex-attribute 1:r32g32b32a32_float:0:8:24:4)
    pixel_flags=(--address32-hi 2)
    ;;
m3-texture)
    vertex_source=shaders/m3/texture.vert
    pixel_source=shaders/m3/texture.frag
    output=probes/m3-texture
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:16:4
        --vertex-attribute 1:r32g32_float:0:8:16:4)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:combined_image_sampler:1:0:48)
    ;;
v0-texture-uint)
    # V0-formats' integer sampled formats: the m3 texture shaders with a
    # usampler, so the fetch is the texel's integer value (shaders/v0).
    vertex_source=shaders/m3/texture.vert
    pixel_source=shaders/v0/texture_uint.frag
    output=probes/v0-texture-uint
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:16:4
        --vertex-attribute 1:r32g32_float:0:8:16:4)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:combined_image_sampler:1:0:48)
    ;;
v0-texture-sint)
    # V0-formats' signed integer formats: the unsigned probe's twin with an
    # isampler, probing the two-channel words one candidate a battery
    # (shaders/v0).
    vertex_source=shaders/m3/texture.vert
    pixel_source=shaders/v0/texture_sint.frag
    output=probes/v0-texture-sint
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:16:4
        --vertex-attribute 1:r32g32_float:0:8:16:4)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:combined_image_sampler:1:0:48)
    ;;
v0-target-uint)
    # V0-formats' unsigned integer colour targets: the m2 fullscreen triangle
    # with a constant uvec4, which the driver compiles for the export Mesa picks
    # for a UINT target (SPI_SHADER_UINT16_ABGR 7) (shaders/v0).
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/target_uint.frag
    output=probes/v0-target-uint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --color-format 0x99999997)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    expected_col_format=7
    ;;
v0-target-sint)
    # Their signed twin (SPI_SHADER_SINT16_ABGR 8).
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/target_sint.frag
    output=probes/v0-target-sint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --color-format 0x99999998)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    expected_col_format=8
    ;;
v0-texel-buffer)
    # V0-formats' uniform texel buffers: the full-target triangle fetching a
    # samplerBuffer at set 0, binding 0, whose descriptor type is
    # VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER -- a 16-byte buffer descriptor
    # entry the driver writes as a hardware V#, which is what makes a
    # UNIFORM_TEXEL_BUFFER feature reachable (docs/BLOCKERS.md). The CLI names
    # the type through tooling/psbc/patch-descriptor-types.py, so the compiler
    # builds the set layout's entry from it.
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/texel_buffer.frag
    output=probes/v0-texel-buffer
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:uniform_texel_buffer:1:0:16)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-image-atomic)
    # V0-formats' storage image atomics: the same triangle adding to a r32ui
    # storage image at set 0, binding 0, whose descriptor is the 32-byte storage
    # image entry round 7 proved (docs/BLOCKERS.md).
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/image_atomic_uint.frag
    output=probes/v0-image-atomic
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:storage_image:1:0:32)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-image-atomic-sint)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/image_atomic_sint.frag
    output=probes/v0-image-atomic-sint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:storage_image:1:0:32)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    c_writer=1
    ;;
v0-stencil-setup)
    # Round 12's stencil path: the pass that writes the stencil plane. The
    # geometry is the m4-depth vertex layout (position and colour, 28-byte
    # records) so the same vertex buffer serves both pipelines, and the fragment
    # shader is the setup pass's constant red -- its pipeline state is what makes
    # it a stencil write (FUNC ALWAYS, PASS REPLACE), not the shader.
    vertex_source=shaders/m4/depth_colour.vert
    pixel_source=shaders/v0/stencil_setup.frag
    output=probes/v0-stencil-setup
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32_float:0:0:28:4
        --vertex-attribute 1:r32g32b32a32_float:0:12:28:4)
    pixel_flags=(--address32-hi 2)
    ;;
v0-stencil-test)
    # Round 12's stencil path: the pass that tests the plane the setup pass
    # wrote, with a constant green -- the frame's signal.
    vertex_source=shaders/m4/depth_colour.vert
    pixel_source=shaders/v0/stencil_test.frag
    output=probes/v0-stencil-test
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32_float:0:0:28:4
        --vertex-attribute 1:r32g32b32a32_float:0:12:28:4)
    pixel_flags=(--address32-hi 2)
    ;;
v0-stencil-deep)
    # Round 12's depth control for the stencil frames: the test pass's shader
    # with gl_FragDepth behind the setup pass's 0.75, so its fragments pass the
    # stencil test and fail the depth one.
    vertex_source=shaders/m4/depth_colour.vert
    pixel_source=shaders/v0/stencil_deep.frag
    output=probes/v0-stencil-deep
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32_float:0:0:28:4
        --vertex-attribute 1:r32g32b32a32_float:0:12:28:4)
    pixel_flags=(--address32-hi 2)
    ;;
v0-image-store)
    # V0-formats' storage images: the full-target triangle storing into a
    # writeonly image2D at set 0, binding 0, whose descriptor type is
    # VK_DESCRIPTOR_TYPE_STORAGE_IMAGE -- a 32-byte image descriptor, the
    # combined sampler's entry without its 16-byte sampler (docs/BLOCKERS.md).
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/image_store.frag
    output=probes/v0-image-store
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:storage_image:1:0:32)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-image-store-uint)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/image_store_uint.frag
    output=probes/v0-image-store-uint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:storage_image:1:0:32)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-image-store-sint)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/image_store_sint.frag
    output=probes/v0-image-store-sint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:storage_image:1:0:32)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-texel-buffer-store)
    # V0-formats' storage texel buffers: the same triangle storing into a
    # writeonly imageBuffer at set 0, binding 0, whose descriptor type is
    # VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER -- the same 16-byte entry and V# as
    # the uniform probe's, written rather than read (docs/BLOCKERS.md).
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/texel_buffer_store.frag
    output=probes/v0-texel-buffer-store
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:storage_texel_buffer:1:0:16)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-texel-buffer-store-uint)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/texel_buffer_store_uint.frag
    output=probes/v0-texel-buffer-store-uint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:storage_texel_buffer:1:0:16)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-texel-buffer-store-sint)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/texel_buffer_store_sint.frag
    output=probes/v0-texel-buffer-store-sint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:storage_texel_buffer:1:0:16)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-texel-buffer-uint)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/texel_buffer_uint.frag
    output=probes/v0-texel-buffer-uint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:uniform_texel_buffer:1:0:16)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
v0-texel-buffer-sint)
    vertex_source=shaders/m2/fullscreen.vert
    pixel_source=shaders/v0/texel_buffer_sint.frag
    output=probes/v0-texel-buffer-sint
    vertex_flags=(--address32-hi 2)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:uniform_texel_buffer:1:0:16)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    ;;
c7-mip)
    vertex_source=shaders/c7/band.vert
    pixel_source=shaders/c7/nearest.frag
    output=probes/c7-mip
    # The band quad's 16-byte records: position and texture coordinate, whose
    # v names the band and so the level; and the combined image sampler.
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:16:4
        --vertex-attribute 1:r32g32_float:0:8:16:4)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:combined_image_sampler:1:0:48)
    ;;
v0-cube)
    vertex_source=shaders/v0/cube.vert
    pixel_source=shaders/v0/cube.frag
    output=probes/v0-cube
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:20:4
        --vertex-attribute 1:r32g32b32_float:0:8:20:4)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:combined_image_sampler:1:0:48)
    ;;
v0-array)
    vertex_source=shaders/v0/array.vert
    pixel_source=shaders/v0/array.frag
    output=probes/v0-array
    # Two bands of four vertices: position and a vec3 array coordinate, 20-byte
    # records. The pixel stage samples a 2D array, so the binding is declared the
    # way the m3-texture set declares its sampler, with the same 48-byte entry.
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:20:4
        --vertex-attribute 1:r32g32b32_float:0:8:20:4)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:combined_image_sampler:1:0:48)
    ;;
v0-vertex-sint)
    vertex_source=shaders/v0/vertex_sint.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/v0-vertex-sint
    # The m3-vertex square's records widened for the integer position: ivec3 at
    # offset 0, the colour at 16, 32-byte stride. The compiler emits the vertex
    # buffer descriptor's format word for r32g32b32_sint, which is the point of
    # the probe: no probe had drawn with a three-component integer attribute.
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32_sint:0:0:32:4
        --vertex-attribute 1:r32g32b32a32_float:0:16:32:4)
    pixel_flags=(--address32-hi 2)
    ;;
v0-vertex-uint)
    vertex_source=shaders/v0/vertex_uint.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/v0-vertex-uint
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32_uint:0:0:32:4
        --vertex-attribute 1:r32g32b32a32_float:0:16:32:4)
    pixel_flags=(--address32-hi 2)
    ;;
v0-vertex-bytes-float)
    vertex_source=shaders/v0/vertex_bytes_float.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/v0-vertex-bytes-float
    # One attribute, the float class, and a full-screen triangle from
    # gl_VertexIndex: the attribute bytes are the whole vertex buffer, so a row
    # binds its own format at location 0 with the format's own size as the stride
    # and the frame is the colour that format decodes to.
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32a32_float:0:0:16:4)
    pixel_flags=(--address32-hi 2)
    ;;
v0-vertex-bytes-uint)
    vertex_source=shaders/v0/vertex_bytes_uint.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/v0-vertex-bytes-uint
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32a32_uint:0:0:16:4)
    pixel_flags=(--address32-hi 2)
    ;;
v0-vertex-bytes-sint)
    vertex_source=shaders/v0/vertex_bytes_sint.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/v0-vertex-bytes-sint
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32a32_sint:0:0:16:4)
    pixel_flags=(--address32-hi 2)
    ;;
c2-instance)
    vertex_source=shaders/c2/instance.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/c2-instance
    # The m3-vertex square's own layout -- position and colour, 24-byte records
    # -- so the frame draws the geometry the console proved; the shader colours
    # by instance index and leaves the position alone.
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:24:4
        --vertex-attribute 1:r32g32b32a32_float:0:8:24:4)
    pixel_flags=(--address32-hi 2)
    ;;
c7-diag)
    vertex_source=shaders/c7/band.vert
    pixel_source=shaders/c7/const.frag
    output=probes/c7-diag
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:16:4
        --vertex-attribute 1:r32g32_float:0:8:16:4)
    pixel_flags=(--address32-hi 2)
    ;;
c7-mip-linear)
    vertex_source=shaders/c7/band.vert
    pixel_source=shaders/c7/linear.frag
    output=probes/c7-mip-linear
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:16:4
        --vertex-attribute 1:r32g32_float:0:8:16:4)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:combined_image_sampler:1:0:48)
    ;;
m4-depth)
    vertex_source=shaders/m4/depth_colour.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/m4-depth
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32_float:0:0:28:4
        --vertex-attribute 1:r32g32b32a32_float:0:12:28:4)
    pixel_flags=(--address32-hi 2)
    ;;
m4-blend)
    vertex_source=shaders/m4/depth_colour.vert
    pixel_source=shaders/m3/vertex_colour.frag
    output=probes/m4-blend
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32_float:0:0:28:4
        --vertex-attribute 1:r32g32b32a32_float:0:12:28:4)
    pixel_flags=(--address32-hi 2 --color-format 0x99999994)
    pixel_compiler="$root/build/host/opengnm-psbc-probe"
    # SPI_SHADER_COL_FORMAT after the compiler packs the one written MRT.
    expected_col_format=4
    ;;
b7-corner)
    vertex_source=shaders/b7/corner.vert
    pixel_source=shaders/m2/solid.frag
    output=probes/b7-corner
    vertex_flags=()
    pixel_flags=()
    ;;
b8-corner)
    vertex_source=shaders/b7/corner.vert
    pixel_source=shaders/b8/second.frag
    output=probes/b8-corner
    vertex_flags=()
    pixel_flags=()
    ;;
c1-clear)
    vertex_source=shaders/c1/rect.vert
    pixel_source=shaders/c1/clear_colour.frag
    output=probes/c1-clear
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32b32a32_uint:0:0:16:4)
    pixel_flags=(--address32-hi 2 --descriptor-binding 0:0:uniform_buffer:1:0:16)
    ;;
c3-quad)
    vertex_source=shaders/c3/quad.vert
    pixel_source=shaders/c3/quad.frag
    output=probes/c3-quad
    vertex_flags=(--address32-hi 2
        --vertex-attribute 0:r32g32_float:0:0:8:4
        --descriptor-binding 0:0:uniform_buffer:1:0:16)
    pixel_flags=(--address32-hi 2)
    ;;
*)
    echo "usage: $0 m2|m3-uniform|v0-robust|m3-vertex|m3-texture|m4-depth|m4-blend|b7-corner|b8-corner|c1-clear|c3-quad|c7-mip|c7-mip-linear|c7-diag|c2-instance" >&2
    exit 2
    ;;
esac

[[ -x $compiler ]] || { echo "missing opengnm-psbc: $compiler" >&2; exit 2; }
[[ -x $pixel_compiler ]] ||
    { echo "missing pixel compiler: $pixel_compiler (run tools/build-psbc-cli.sh)" >&2; exit 2; }
probe="$root/build/host/opengnm-psbc-probe"
[[ -x $probe ]] ||
    { echo "missing probe compiler: $probe (run tools/build-psbc-cli.sh)" >&2; exit 2; }
[[ -f $glslang ]] || command -v "$glslang" > /dev/null ||
    { echo "missing glslang: $glslang" >&2; exit 2; }

work="$root/build/shaders/$set_name"
mkdir -p "$work" "$root/$output"

# A failed compile must stop the build: glslang reports its errors and leaves
# the output file alone, so a stale SPIR-V from an earlier build would be
# packaged as if it were this one. That is exactly what happened to Phase C2's
# instancing probe -- a shader that used gl_InstanceID, which Vulkan GLSL does
# not declare, was packaged from the previous build and drew its colour for
# five console runs (docs/M5_PHASE_C.md).
compile_spirv() {
    local stage=$1 input=$2 spirv=$3
    rm -f "$spirv"
    if ! "$glslang" -V --target-env vulkan1.0 -S "$stage" \
            "$input" -o "$spirv" > "$work/$stage.glslang.log" 2>&1; then
        echo "$set_name: glslang failed for the $stage stage:" >&2
        sed -n '1,20p' "$work/$stage.glslang.log" >&2
        exit 1
    fi
    [[ -s $spirv ]] || { echo "$set_name: no SPIR-V came out of the $stage stage" >&2; exit 1; }
}

compile_spirv vert "$root/$vertex_source" "$work/vertex.spv"
compile_spirv frag "$root/$pixel_source" "$work/pixel.spv"

"$compiler" -g -s vertex --ngg "${vertex_flags[@]}" \
    -f "$work/vertex.spv" -o "$work/vertex.ngg.bin" --metadata "$work/vertex.hw.json"
"$pixel_compiler" -g -s fragment --raw "${pixel_flags[@]}" \
    -f "$work/pixel.spv" -o "$work/pixel.raw.bin" --metadata "$work/pixel.hw.json"

# ps5-opengl's C writer (src/platform/ps5_agc_package.c, gcc-compiled into the
# probe CLI) is the writer this repository uses: it is the writer the titles
# link, it validates the metadata version against the compiler's own header, and
# 0.3.0 packages through it too. The SDK's Python writer is not used any more:
# it requires its own metadata schema version in the CLI's JSON, and neither the
# pinned compiler nor the 0.3.0 fork the migration moved to emits that
# (tools/adapt-opengl-sdk.sh; docs/BLOCKERS.md, the SDK section). What used to be
# a Python-against-C byte comparison is now the console's own compile-mode check
# below: the runner compiles this set's shipped SPIR-V with the shipped options
# and must reproduce both packages byte for byte.
"$probe" -g -s vertex --ngg "${vertex_flags[@]}" -f "$work/vertex.spv" \
    -o "$root/$output/vertex.bin" --agc-package "$root/$output/vertex.bin"
"$probe" -g -s fragment --raw "${pixel_flags[@]}" -f "$work/pixel.spv" \
    -o "$root/$output/pixel.bin" --agc-package "$root/$output/pixel.bin"
for stage in vertex pixel; do
    [[ -s $root/$output/$stage.bin ]] || {
        echo "$set_name: the C package writer wrote no $stage package" >&2
        exit 1
    }
done

# The runner's "compile" mode (docs/M5_PHASE_A.md, A3) compiles the same SPIR-V
# on the console and must reproduce both packages. Ship the SPIR-V and the
# exact compiler options, in the CLI's own syntax.
cp "$work/vertex.spv" "$work/pixel.spv" "$root/$output/"
{
    echo "# Compiler options of tools/build-probe-shaders.sh $set_name, in opengnm-psbc CLI syntax."
    echo "# The runner's compile mode compiles <stage>.spv with them and packages the result"
    echo "# with ps5-opengl's C writer (ESGS ring item size 1)."
    echo "vertex -g -s vertex --ngg${vertex_flags[*]:+ ${vertex_flags[*]}}"
    echo "pixel -g -s fragment --raw${pixel_flags[*]:+ ${pixel_flags[*]}}"
} > "$root/$output/compile.txt"

python3 - "$set_name" "$root" "$output" "$work" "$glslang" "$compiler" \
    "$vertex_source" "$pixel_source" "${vertex_flags[*]:-}" "${pixel_flags[*]:-}" \
    "$pixel_compiler" "$expected_col_format" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys

set_name = sys.argv[1]
root = pathlib.Path(sys.argv[2])
output = root / sys.argv[3]
work, glslang, compiler = map(pathlib.Path, sys.argv[4:7])
vertex_source, pixel_source = root / sys.argv[7], root / sys.argv[8]
vertex_flags, pixel_flags = sys.argv[9], sys.argv[10]
pixel_compiler = pathlib.Path(sys.argv[11])
expected_col_format = int(sys.argv[12]) if sys.argv[12] else None

def fail(message: str) -> None:
    sys.exit(f"{set_name} metadata check failed: {message}")

def fnv1a64(data: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"

def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def dword(meta: dict, key: str, label: str) -> int:
    count = meta.get("user_sgpr_count")
    value = meta.get(key)
    if not isinstance(count, int) or not isinstance(value, int) or not 0 <= value < count:
        fail(f"{label} {key} {value!r} outside user SGPR count {count!r}")
    return value

vertex = json.loads((work / "vertex.hw.json").read_text())
pixel = json.loads((work / "pixel.hw.json").read_text())
expected_hi = 0 if set_name in ("m2", "b7-corner", "b8-corner") else 2
for label, meta in (("vertex", vertex), ("pixel", pixel)):
    if meta.get("address32_hi") != expected_hi:
        fail(f"{label} address32_hi is {meta.get('address32_hi')!r}, expected {expected_hi}")

bindings = []
notes = []

if expected_col_format is not None:
    # SPI_SHADER_COL_FORMAT is context register 0x1c5.
    formats = [record["value"] for record in pixel.get("context_registers", [])
               if record.get("offset") == 0x1C5]
    if formats != [expected_col_format]:
        fail(f"pixel SPI_SHADER_COL_FORMAT {formats!r}, expected [{expected_col_format}]")
    notes.append(f"pixel SPI_SHADER_COL_FORMAT: {expected_col_format}")

def pixel_descriptor(stride: int) -> list:
    # Exactly one pixel binding: set 0, binding 0 at table offset 0.
    declared = pixel.get("descriptor_bindings") or []
    if len(declared) != 1:
        fail(f"pixel stage declares {len(declared)} descriptor bindings, expected 1")
    binding = declared[0]
    if (binding.get("set"), binding.get("binding"), binding.get("offset"),
            binding.get("stride")) != (0, 0, 0, stride):
        fail(f"unexpected pixel binding {binding!r}")
    notes.append(f"pixel binding metadata: {json.dumps(binding, sort_keys=True)}")
    return [
        ("pixel_user_sgpr_count", pixel["user_sgpr_count"]),
        ("pixel_descriptor_set0_dword",
         dword(pixel, "descriptor_set0_user_data_dword", "pixel")),
        ("pixel_set0_binding0_offset", binding["offset"]),
        ("pixel_set0_binding0_stride", binding["stride"]),
    ]

def vertex_ngg() -> list:
    # RADV's NGG ABI takes the base vertex and the LDS layout (where the pixel
    # stage finds vertex outputs) through vertex user data even when the stage
    # reads nothing else. A frame that draws with two pipelines must write them
    # per draw, because the tables sceAgcCreateShader leaves in a shader object
    # hold no user-data registers.
    lds = vertex.get("ngg_lds_layout")
    if (not isinstance(lds, dict) or not isinstance(lds.get("value"), int)
            or not 0 <= lds["value"] <= 0xFFFF):
        fail(f"vertex stage has no usable NGG LDS layout: {lds!r}")
    base = dword(vertex, "base_vertex_user_data_dword", "vertex")
    lds_dword = dword({"user_sgpr_count": vertex["user_sgpr_count"],
                       "ngg_lds_layout.user_data_dword": lds.get("user_data_dword")},
                      "ngg_lds_layout.user_data_dword", "vertex")
    if base == lds_dword:
        fail(f"vertex user-data dwords overlap: base vertex {base}, LDS layout {lds_dword}")
    return [
        ("vertex_user_sgpr_count", vertex["user_sgpr_count"]),
        ("vertex_base_vertex_dword", base),
        ("vertex_ngg_lds_dword", lds_dword),
        ("vertex_ngg_lds_layout", lds["value"]),
    ]

def vertex_input(attributes: str) -> list:
    # The vertex stage fetches attributes through a vertex-buffer table named
    # by one user-data dword and reads no descriptors. RADV's NGG ABI also
    # takes the base vertex and the LDS layout (where the pixel stage finds
    # vertex outputs) through vertex user data; ps5-opengl's gallium draw path
    # fills both for every NGG draw.
    if vertex.get("descriptor_bindings"):
        fail("vertex stage unexpectedly declares descriptor bindings")
    lds = vertex.get("ngg_lds_layout")
    if (not isinstance(lds, dict) or not isinstance(lds.get("value"), int)
            or not 0 <= lds["value"] <= 0xFFFF):
        fail(f"vertex stage has no usable NGG LDS layout: {lds!r}")
    table = dword(vertex, "vertex_buffer_table_user_data_dword", "vertex")
    base = dword(vertex, "base_vertex_user_data_dword", "vertex")
    lds_dword = dword({"user_sgpr_count": vertex["user_sgpr_count"],
                       "ngg_lds_layout.user_data_dword": lds.get("user_data_dword")},
                      "ngg_lds_layout.user_data_dword", "vertex")
    if len({table, base, lds_dword}) != 3:
        fail(f"vertex user-data dwords overlap: table {table}, base vertex {base}, "
             f"LDS layout {lds_dword}")
    notes.append(attributes)
    return [
        ("vertex_user_sgpr_count", vertex["user_sgpr_count"]),
        ("vertex_buffer_table_dword", table),
        ("vertex_base_vertex_dword", base),
        ("vertex_ngg_lds_dword", lds_dword),
        ("vertex_ngg_lds_layout", lds["value"]),
    ]

def vertex_input_descriptor(attributes: str, stride: int) -> list:
    # A vertex stage that fetches attributes through a vertex-buffer table and
    # also reads one uniform buffer. The binding must sit at set 0, binding 0,
    # offset 0 with the given stride (16 for one vec4), and its descriptor-set
    # dword must be a fourth user-data dword distinct from the table, base
    # vertex and LDS layout RADV's NGG ABI already takes.
    declared = vertex.get("descriptor_bindings") or []
    if len(declared) != 1:
        fail(f"vertex stage declares {len(declared)} descriptor bindings, expected 1")
    binding = declared[0]
    if (binding.get("set"), binding.get("binding"), binding.get("offset"),
            binding.get("stride")) != (0, 0, 0, stride):
        fail(f"unexpected vertex binding {binding!r}")
    lds = vertex.get("ngg_lds_layout")
    if (not isinstance(lds, dict) or not isinstance(lds.get("value"), int)
            or not 0 <= lds["value"] <= 0xFFFF):
        fail(f"vertex stage has no usable NGG LDS layout: {lds!r}")
    descriptor = dword(vertex, "descriptor_set0_user_data_dword", "vertex")
    table = dword(vertex, "vertex_buffer_table_user_data_dword", "vertex")
    base = dword(vertex, "base_vertex_user_data_dword", "vertex")
    lds_dword = dword({"user_sgpr_count": vertex["user_sgpr_count"],
                       "ngg_lds_layout.user_data_dword": lds.get("user_data_dword")},
                      "ngg_lds_layout.user_data_dword", "vertex")
    if len({descriptor, table, base, lds_dword}) != 4:
        fail(f"vertex user-data dwords overlap: descriptor set {descriptor}, table {table}, "
             f"base vertex {base}, LDS layout {lds_dword}")
    notes.append(attributes)
    notes.append(f"vertex binding metadata: {json.dumps(binding, sort_keys=True)}")
    return [
        ("vertex_user_sgpr_count", vertex["user_sgpr_count"]),
        ("vertex_buffer_table_dword", table),
        ("vertex_base_vertex_dword", base),
        ("vertex_ngg_lds_dword", lds_dword),
        ("vertex_ngg_lds_layout", lds["value"]),
        ("vertex_descriptor_set0_dword", descriptor),
        ("vertex_set0_binding0_offset", binding["offset"]),
        ("vertex_set0_binding0_stride", binding["stride"]),
    ]

if set_name == "m3-uniform":
    if vertex.get("descriptor_bindings"):
        fail("vertex stage unexpectedly declares descriptor bindings")
    bindings = [("address32_hi", expected_hi), *pixel_descriptor(16)]
elif set_name == "v0-robust":
    if vertex.get("descriptor_bindings"):
        fail("vertex stage unexpectedly declares descriptor bindings")
    bindings = [("address32_hi", expected_hi), *pixel_descriptor(32)]
elif set_name == "m3-vertex":
    if pixel.get("descriptor_bindings"):
        fail("pixel stage unexpectedly declares descriptor bindings")
    bindings = [("address32_hi", expected_hi),
                *vertex_input("vertex attributes: location 0 r32g32_float offset 0, "
                              "location 1 r32g32b32a32_float offset 8, stride 24, binding 0")]
elif set_name in ("m4-depth", "m4-blend"):
    if pixel.get("descriptor_bindings"):
        fail("pixel stage unexpectedly declares descriptor bindings")
    bindings = [("address32_hi", expected_hi),
                *vertex_input("vertex attributes: location 0 r32g32b32_float offset 0, "
                              "location 1 r32g32b32a32_float offset 12, stride 28, binding 0")]
elif set_name == "m3-texture":
    bindings = [("address32_hi", expected_hi),
                *vertex_input("vertex attributes: location 0 r32g32_float offset 0, "
                              "location 1 r32g32_float offset 8, stride 16, binding 0"),
                *pixel_descriptor(48)]
elif set_name == "c2-instance":
    if pixel.get("descriptor_bindings"):
        fail("pixel stage unexpectedly declares descriptor bindings")
    bindings = [("address32_hi", expected_hi),
                *vertex_input("vertex attributes: location 0 r32g32_float offset 0, "
                              "location 1 r32g32b32a32_float offset 8, stride 24, binding 0")]
elif set_name == "c7-diag":
    if pixel.get("descriptor_bindings"):
        fail("pixel stage unexpectedly declares descriptor bindings")
    bindings = [("address32_hi", expected_hi),
                *vertex_input("vertex attributes: location 0 r32g32_float offset 0, "
                              "location 1 r32g32_float offset 8, stride 16, binding 0")]
elif set_name in ("c7-mip", "c7-mip-linear"):
    bindings = [("address32_hi", expected_hi),
                *vertex_input("vertex attributes: location 0 r32g32_float offset 0, "
                              "location 1 r32g32_float offset 8, stride 16, binding 0"),
                *pixel_descriptor(48)]
elif set_name == "b8-corner":
    # The second pipeline of the C1 clear probe, which writes this stage's
    # vertex user data before its draw (src/diagnostics.cpp, run_clear_frames).
    if pixel.get("descriptor_bindings") or vertex.get("descriptor_bindings"):
        fail("the corner shaders unexpectedly declare descriptor bindings")
    bindings = [("address32_hi", expected_hi), *vertex_ngg()]
elif set_name == "c1-clear":
    bindings = [("address32_hi", expected_hi),
                *vertex_input("vertex attributes: location 0 r32g32b32a32_uint offset 0, "
                              "stride 16, binding 0"),
                *pixel_descriptor(16)]
elif set_name == "c3-quad":
    # The first probe set whose vertex stage reads a vertex buffer and a
    # descriptor: the stage transforms the buffer's position with the uniform
    # buffer, and the pixel stage is a constant colour with no inputs.
    if pixel.get("descriptor_bindings"):
        fail("pixel stage unexpectedly declares descriptor bindings")
    bindings = [("address32_hi", expected_hi),
                *vertex_input_descriptor("vertex attributes: location 0 r32g32_float offset 0, "
                                         "stride 8, binding 0", 16)]

packages = {"vertex": output / "vertex.bin", "pixel": output / "pixel.bin"}
(output / "checksums.txt").write_bytes("".join(
    f"{label} {fnv1a64(path.read_bytes())}\n" for label, path in packages.items()).encode("ascii"))
receipts = [*packages.values(), output / "checksums.txt", output / "compile.txt",
            output / "vertex.spv", output / "pixel.spv"]
if bindings:
    (output / "bindings.txt").write_bytes(
        "".join(f"{key} {value}\n" for key, value in bindings).encode("ascii"))
    receipts.append(output / "bindings.txt")
(output / "SHA256SUMS").write_bytes("".join(
    f"{sha256(path)}  {path.name}\n" for path in receipts).encode("ascii"))

try:
    version = subprocess.run([str(glslang), "--version"], capture_output=True, text=True,
                             check=False).stdout.splitlines()[0].strip()
except (OSError, IndexError):
    version = "unknown"
inputs = [vertex_source, pixel_source, work / "vertex.spv", work / "pixel.spv",
          work / "vertex.ngg.bin", work / "vertex.hw.json",
          work / "pixel.raw.bin", work / "pixel.hw.json"]
lines = [
    f"PS5 Vulkan {set_name} shader packages, built by tools/build-probe-shaders.sh {set_name}.",
    "pipeline: GLSL -> SPIR-V (glslang, vulkan1.0) -> opengnm-psbc NIR/ACO -> "
    "ps5-opengl agc_shader_package_writer.py",
    f"glslang: {version}",
    f"opengnm-psbc sha256: {sha256(compiler)}",
    *([f"pixel compiler (tools/build-psbc-cli.sh) sha256: {sha256(pixel_compiler)}"]
      if pixel_compiler != compiler else []),
    f"vertex: -g -s vertex --ngg {vertex_flags}".rstrip()
    + "; writer --esgs-ring-itemsize 1 --allow-unresolved",
    f"pixel: -g -s fragment --raw {pixel_flags}".rstrip() + "; writer --allow-unresolved",
    *notes,
    "program-checksum registers are unresolved, as in ps5-opengl's runtime packages",
]
lines += [f"{sha256(path)}  {path.relative_to(root)}" for path in inputs]
(output / "PROVENANCE.txt").write_bytes(("\n".join(lines) + "\n").encode("ascii"))
PY

(cd "$root/$output" && sha256sum --check --quiet SHA256SUMS)
echo "$set_name shader packages: $root/$output"
