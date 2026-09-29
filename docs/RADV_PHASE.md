# RADV on the console: run log

Append-only, like the M5 phase logs: dated entries, never rewritten. The plan
is [VULKAN_1_4_PLAN.md](VULKAN_1_4_PLAN.md) (route B); the CTS set-up is in
[CTS.md](CTS.md). The driver is my Mesa fork `PS5_Mesa` and the CTS my fork `PS5_VK-GL-CTS`,
each on its branch `main` (called `ps5-port` in the entries up to
2026-09-28); the platform pieces are in the payload SDK fork's `platform/`.

## 2026-09-26 — the CTS runs on the console

RADV passes the smoke title (PPSA99014, 9 of 9: device creation, fill, copy,
compute and a triangle read back exactly). The CTS runs as PPSA99015
(`tools/build-cts-title.sh`, `tools/run-cts.py`), first against 1.4.5.3 and
then against 1.4.6.2. What the first runs found, in order:

1. **An 11.5 s case was 17 ms of work.** The image format query cases each
   took exactly 11.51 s, which read as a hang. Each logs 322 messages, and
   `--deqp-log-flush=enable` flushes the log after every XML element; a
   `write()` to the console's storage costs about 3.3 ms whatever its size
   (the SDK platform's PROBE.md). Without the flush the same cases take 17 and
   6.5 ms, and `dEQP-VK.api.info.*` (8,175 cases) runs in 56 s.
2. **libc's heap ran out in the first shader build.** `api.smoke.create_shader`
   was a ResourceError: glslang's 8 KiB pool page (`operator new[]`) was
   refused. The platform layer now gives a title a heap in direct memory
   (`ps5platform/heap.h`: dlmalloc over one reserved 16 GiB range, reached
   through `--wrap` of the malloc family); `api.smoke.*` passes 6 of 6.
3. **What RADV reported that the console does not have.** With no window
   system, `VK_EXT_headless_surface` came without `VK_KHR_surface`; the fd,
   sync-fd and dma-buf external handles and host-pointer import were exposed
   with nothing behind them; memory reports carried object id 0. RADV now
   takes these from the winsys (`has_external_fd`, `has_userptr`, a unique
   `obj_id`). `dEQP-VK.info.*` and `dEQP-VK.memory.*` then had one failure:
   `VK_KHR_device_address_commands` is unknown to CTS 1.4.5.3, which is why
   the pin moved to 1.4.6.2, where it passes.
4. **Event status read a stale 1.** amdgpu zeroes GTT buffers and RADV relies
   on it; the console's direct memory arrives holding what it last held. PS5
   GTT buffers are now zeroed (`pool_reset_reuse`, `submit_count_*` pass).
5. **E5B9G9R9 does not render** (HARDWARE_FINDINGS.md, this date): 500
   blits read back wrong; the format is no longer a colour target there.
6. **A submission's size and starting state** (HARDWARE_FINDINGS.md, this
   date). 131,000 draws in one command buffer never completed; split into two
   AGC submissions they drew only the first part. RADV now splits a long
   stream before a draw or dispatch when the winsys asks and re-emits its
   state after the split; the winsys splits only there or where a stream
   starts and repeats the preamble. `record_many_draws_*_2` pass.
7. **The tooling had to see failures.** A title may not `dup2` (EPERM), so the
   platform's `ps5_klog_capture_stderr` moves the `stderr` stream to a pipe
   that a thread writes to klog: RADV's messages and assertion failures now
   arrive. The CTS's crash handler hung the title (it writes its backtrace
   through a file in the working directory), so the PS5 platform reports a
   crash itself: the case is logged as Crash, the fault and a stack scan go
   to klog, and the title ends. A hang report gives the main thread's place
   after 45 s without output. `run-cts.py` runs lists in batches, resumes
   after a crash or hang, and records every result.

The E5B9G9R9 finding was on 1.4.5.3 with the fixes of item 3; the others
were rechecked on 1.4.6.2 (`recheck-5`: every case of the list passes or is
not supported). The full `api` group is the next run.

## 2026-09-26 — tessellation and geometry

The first full pass over the mustpass list (`main-1`) stopped at
tessellation and geometry, which faulted or drew nothing. The RADV smoke
title (PPSA99014) got checks for both (a quad patch from coordinates, from
control points and with varyings, levels 2 to 9; geometry strips of 16 to 128
vertices from 1, 2 and 4 points, counts from gl_PrimitiveIDIn, a colour
varying, recorded primitive IDs) and a probe loop around them: 50 of 50 pass.

1. **AGC owns the tessellation rings' registers** (HARDWARE_FINDINGS.md).
   RADV's winsys now hands AGC the factor ring (`sceAgcDriverSetTFRing`,
   through a new `ctx_set_tess_factor_ring` hook) and RADV's off-chip
   parameter (`sceAgcDriverSetHsOffchipParam`).
2. **No integer dot products** (HARDWARE_FINDINGS.md). An earlier fix that
   turned NGG culling off was the wrong cause and is gone: culling is on and
   the dot-product instructions are off.
3. **No legacy geometry shaders.** A legacy GS hangs and AGC exports no way
   to set its rings, so a GS compiled with the stage before it stays NGG
   (`radeon_info.has_legacy_gs`). Open: a tessellated GS amplifying past 256
   vertices (dEQP-VK.tessellation.geometry_interaction.limits.*, which NGG
   multi-cycling cannot serve with tessellation), streamout from a GS, and
   separately compiled GS shader objects (dEQP-VK.shader_object.link.*
   with an unlinked GS). Each still loses the device.
4. **Tooling.** Driver messages reach klog (the platform's stderr capture now
   moves the stream, since `dup2` is refused); the CTS title reports crashes
   itself (the CTS's handler hung) and names the case; a hang report gives
   the main thread's place; `run-cts.py --env` and the smoke title's
   `/app0/radv-smoke-env.txt` set the driver's environment.

Also this round: host image copy is off on this GPU as upstream keeps it off
for GFX10's swizzles (`radeon_info.gfx10_1_swizzles`); default thread stacks
come from direct memory (256 concurrent threads had exhausted flexible
memory); a fill-buffer case asked for a compute-only queue without checking
for one (backported upstream's check to the CTS fork); and the driver
rejects layers and reports only the priorities it serves.

## 2026-09-27 — shader objects, host image copy, depth clears

The second pass over main-1's failures (190 cases) left two groups, and a
rerun of the shader-object geometry cases (181) a third. All three are fixed
in the Mesa fork, and every one of those cases now passes or is not
supported.

1. **Geometry shader objects stay NGG.** A GS compiled alone read its NGG
   mode from a stage that was never initialised, and a VS or TES compiled
   for a GS disagreed with it (an assertion on user SGPRs, or a lost
   device). Where no legacy GS can run, every GS and every stage before one
   is NGG, and merged shaders compiled separately have no NGG culling. The
   14 cases left need mesh shaders, which this GPU does not have.
2. **Host image copy.** Vulkan 1.4 asks for hostImageCopy or a second queue
   that transfers (dEQP-VK.info.device_mandatory_features). Upstream keeps
   host image copy off on GFX10 for addrlib regressions; they are two bugs
   in addrlib's microblock copies with GFX10's (non-RB+) swizzles, which
   this GPU uses: an 8bpp 64KB_R_X microblock is not a rectangle (addrlib
   trapped), and x3 of a 16bpp 64KB_R_X or 64KB_Z_X microblock also flips
   address bit 8 (half of each microblock landed in another). addrlib now
   uses the microblock copies only where a microblock is 256 contiguous
   bytes of exactly its rectangle. A host check against addrlib's own
   per-texel address (every mode, 8 to 128 bpp, full mip chains, both
   directions, memcpy round trips) found both and passes; on the console,
   dEQP-VK.image.host_image_copy (73291 cases) passes or is not supported,
   and RADV turns host image copy on for GFX10.
3. **The depth clear bug** (HARDWARE_FINDINGS.md): the GPU description turns
   on Mesa's workaround for GFX1013's TC-compatible HTILE clear bug.

Next: the rest of main-1 on this build, with the shader-object group back
in; transform feedback stays out until a GS can stream out.

## 2026-09-27 — graphicsfuzz, device-generated commands, scratch

main-1's next groups turned up three problems, and the runner a fourth.

1. **Floats parsed as integers.** Graphicsfuzz shaders whose colour is decided
   by constants took their else branch: the CTS assembles SPIR-V text with
   SPIRV-Tools, whose `istream >> float` read 0.100000001 as 100000001. The
   console's `localeconv()` reports an empty decimal point while its `strtod`
   reads '.', and the platform's C-locale `strtof_l` spliced the empty point
   in place of '.'. Fixed in the shared platform layer (PS5_PayloadSDK
   9cf8084, its PROBE.md records the measurement). Found by comparing
   `NIR_DEBUG=print_fs` on the host and on the console pass by pass: the
   first difference was the SPIR-V constants.
2. **No device-generated commands.** The console cannot run a command buffer
   the GPU wrote (INDIRECT_BUFFER, B8), so VK_EXT_device_generated_commands
   is no longer reported (`radeon_info.has_gpu_written_ibs`).
3. **Buffer scratch** (HARDWARE_FINDINGS.md): ACO addresses scratch through
   buffer instructions on this GPU; the smoke title gained scratch checks
   (58 of 58 pass) and graphicsfuzz passes whole (757 cases).
4. **Tooling.** An asynchronous GPU fault kills the CTS title after the draw
   that caused it, and the unflushed QPA log with it: the runner blamed one
   innocent case per launch and lost the rest. It now takes the results klog
   printed for what the log lost, and notes when a fault was asynchronous.
   `run-cts.py --stderr-file` sends the driver's stderr and stdout to a file
   on the console and fetches it (klog drops lines of a large dump).

Checked and ruled out on the way: the console's libm results, its half-float
conversions (F16C and software) and its MXCSR (flush-to-zero and
denormals-are-zero are on, but clearing them changed nothing).

Open: the tessellated GS with more than 256 output vertices per input
primitive (dEQP-VK.tessellation.geometry_interaction.limits.*): NGG needs
per-instance multi-cycling there, which does not work with tessellation on
GFX10-class hardware, and no legacy GS can run.

Later the same day, three capabilities the console cannot serve stopped
being reported (each group now reads not supported):

- **Performance queries** (`radeon_info.has_perf_counters`): their
  profiling lock needs a stable power state, which no exported function
  sets.
- **Ray tracing.** Ray tracing pipelines call their shaders with a stack in
  scratch, addressed with flat scratch on GFX9+ (`radv_rt_pipelines_enabled`).
  An ACO port of those calls to buffer scratch compiles pipelines on the host
  (the Mesa fork's local branch `ps5-rt-buffer-scratch`, unverified), but
  every acceleration structure build faulted the GPU first, a write far past
  every buffer, even for an empty top level, so the GPU description turns
  ray tracing off until the build runs.
- A tessellation distribution mode does not rescue the GS amplification
  limit: the limits cases lost the device in all four modes.

Then three more from main-1's glsl and barycentric groups, each measured on
the console first (HARDWARE_FINDINGS.md has the measurements):

- **Fragment shader barycentrics** are no longer reported: this GPU's
  parameter cache is GFX10.1's, and per-vertex inputs of a second triangle
  arrived rotated whatever ROTATE_PC_PTR said (`radeon_info.
  has_ps_strict_vertex_order`; upstream draws the same line at GFX10.3). The
  smoke title's probes reproduce it: one triangle of either winding reads in
  order, the CTS's pair of triangles does not.
- **Depth mip layout.** addrlib now gets GFX1013's revision: with Navi10's it
  laid out 8 and 16 bpp depth mips without the mipmap fix the console's depth
  block follows, and shadow textures were written past their end.
- **The IEEE floating-point state.** The console starts a title with
  flush-to-zero and denormals-are-zero; the CTS's double-precision reference
  intervals flushed denormal quotients and rejected correct results. The
  platform's `ps5_fp_ieee()` now runs first in every PS5_Vulkan title, and
  threads inherit their creator's MXCSR. RetroArch gets the same call when it
  moves to the platform helpers.

**Subgroup IDs and ray tracing.** A compute wave's TG_SIZE carries no GFX10.3
wave ID here (HARDWARE_FINDINGS.md); with the ordered wave ID the subgroup
cases pass, and GPU acceleration structure builds no longer write past their
buffers. Ray tracing is still off: with it on (and the buffer-scratch calls
from `ps5-rt-buffer-scratch`), empty acceleration structures pass, but every
ray query case with primitives faults in the test's own shader, a global
load from an address the traversal read out of the acceleration structure:
the build writes wrong contents once there are primitives
(`RADV_EXPERIMENTAL=emulate_rt` faults the same way, so the intersection
instruction is not the cause). Next there: dump a small build's nodes and
compare them with what RADV's encoder should have written.

## 2026-09-27 — no variable-rate shading

main-1's fragment_shading_rate group failed 1803 of the 5653 cases it ran:
every case whose combined rate is 1x1 passed, every coarser one failed,
including a bare pipeline rate with no attachment and no per-primitive rate.
The smoke title gained a shading-rate check (a triangle over the target at a
2x2 and a 1x1 pipeline rate, counting fragment invocations and the rates they
read), run while the driver still reported the extension: at 2x2 the fragment
shader ran once a texel and read 1x1 (HARDWARE_FINDINGS.md).
`radeon_info.has_vrs` now gates VK_KHR_fragment_shading_rate, its features,
the mesh shader's primitive rate and RADV_FORCE_VRS; the PS5 GPU description
clears it (Mesa fork 39e0a54). The smoke title skips the check when the
extension is not reported (59 of 59 pass).

Tooling: `tools/build-radv.sh` moves to a new pinned revision by content, so
only what the revision touched is rebuilt (62 s, not a full Mesa build), and
the title scripts label their build with the fork's revision marked -dirty
when its tree has changes the revision does not hold. The titles link the
fork's own build-ps5 tree; build-radv.sh builds the pinned dependency record.
`tools/run-cts.py` now uploads the CTS title as last built before it runs
(a first verification run had used the title already on the console, built
before the change) and notes the build, from the title's cts/build.txt, in the
run's batches.log. On the gated driver the fragment_shading_rate cases (every
50th of the group) report the extension unsupported, and dEQP-VK.info plus
api.info, api.device_init and api.feature_info pass or are not supported
(run vrs-2, 10640 cases).

## 2026-09-27 — capture and replay, shader objects, transform feedback

- **Capture and replay** is no longer reported. The winsys cannot place a
  buffer at a requested address and refused every replay address, yet
  bufferDeviceAddressCaptureReplay was reported: all 723 replay cases of
  binding_model.buffer_device_address failed. `radeon_info.has_replayable_va`
  now gates every capture and replay feature (Mesa fork 2f26f3a); the replay
  cases report it unsupported and the info checks pass (run replay-1).
- **Separately compiled geometry shaders work.** Rerun on the current driver,
  every shader_object case main-1 had recorded as failing (the link cases
  with an unlinked GS, the rest device-generated commands) passes or is not
  supported (run so-1), so that open item is closed.
- **Transform feedback**, a sample of every 20th case (run xfb-1, 6685
  cases): streamout from a vertex or tessellation evaluation shader passes
  (legacy hardware VS streamout runs here), and every case that captures from
  a geometry shader fails (4460 primitives-generated-query cases, the fuzz
  geometry cases, multiple streams). A GS here is always NGG, and NGG
  streamout before GFX11 orders its writes with GDS, which a title cannot
  reach.
- **The tessellated GS limits** (tessellation.geometry_interaction.limits,
  2 cases) need 4 invocations of 256 vertices and 32 invocations: more than
  one NGG subgroup can hold, and the per-instance mode that splits them hangs
  with tessellation on GFX10-class hardware.

Both of the last two need a geometry shader the hardware cannot run, so the
plan is to run those geometry shaders as compute: Mesa's `src/poly` (Asahi's
and KosmicKrisp's geometry and tessellation lowering, with ordered prefix
sums for transform feedback) for exactly the pipelines NGG cannot serve.

## 2026-09-27 — pipeline executable properties, memory streams

main-1's pipeline group crashed the CTS title in every
executable_properties.*internal_representations case (NULL in strlen): RADV
counted an Assembly representation it had not got (ACO disassembles through
LLVM, which this build does not have) and passed the missing ACO IR text to
strlen. The IR was missing because the platform's open_memstream was an ENOSYS
stub. Three fixes, each general:

- RADV counts only the representations it has and never reads a missing one
  (Mesa fork 0210fab).
- The shared platform layer has a real open_memstream: libc's FILE on a pipe,
  drained by a reader thread and published through fflush and fclose wraps
  (PS5_PayloadSDK 2facde3; its PROBE.md has the measurement). The RADV link
  recipe wraps both.
- The SDK install moves a new revision into place by content (8f5341f), so a
  one-file platform change no longer rebuilds the whole CTS.

All 42 dEQP-VK.pipeline.*.executable_properties cases pass (run execprops-3).

## 2026-09-27 — link bindings, the implicit primitive ID

- **Unbound platform functions.** libc++'s random_device called arc4random,
  which no system module exports, through NULL
  (pipeline.*.creation_cache_control crashed). The RADV link recipe now binds
  every function of the platform's libc to its ps5_ version, local to the
  title (a generated version script: the title converter refuses exports).
  All 20 creation_cache_control cases pass.
- **The implicit primitive ID** read 0 behind an NGG vertex shader
  (HARDWARE_FINDINGS.md): it goes per vertex now where NGG has no
  per-primitive parameters (Mesa fork 3057cb5). The smoke title checks it
  (62 of 62 pass) and all 14 misc.implicit_primitive_id cases pass.
- Open, from main-1's pipeline group: VK_EXT_sample_locations, whose
  verify_location cases fail for custom and standard locations alike in every
  construction type.

Correction to the entry above: VK_EXT_sample_locations was not open. Its
verify_location cases tell each sample's triangle apart by gl_PrimitiveID
with no stage writing it, so they failed with the implicit primitive ID; on
the fixed driver all 80 (custom and standard locations, every construction
type) pass and 20 are not supported (run sampleloc-2).

## 2026-09-27 — geometry shaders as compute: transform feedback works

The geometry shaders NGG cannot run here now run as compute with Mesa's poly
lowering (RADV_GS_COMPUTE.md; Mesa fork branch ps5-gs-compute, 88e5322, not
yet merged into ps5-port). Every draw command handles them, including
indirect, indirect count, byte count and primitive restart, and a geometry
shader with transform feedback outputs always takes this path. Transform
feedback from a geometry shader, which failed in every case (run xfb-1),
now works, with its queries: the 6685-case transform_feedback sample goes
from 1291 passing and 4529 failing to 5818 passing and 2 failing (run
xfb-gsc-3), and the 2 left use graphics pipeline libraries, which are not
routed yet. The smoke title passes 73 of 73 with every geometry shader
forced through compute.

## 2026-09-27 — ray queries work; the chip description audited against GFX1013

**Ray tracing was a driver bug.** RADV's traversal rebuilt node addresses
assuming Linux's top-half address layout; the acceleration structure builds
were right all along (HARDWARE_FINDINGS.md). Fixed on the Mesa fork's branch
ps5-rt (d55c9c0), which also adds the probe-only switch RADV_PS5_RAY_TRACING=1.
With it, every ray-related mustpass case (42,745) passes or is not supported:
9,348 pass, none fail (run rq-full-1). The smoke title checks that GPU-built
bottom levels of one and two triangles hold their vertices.

**Two limitations on record had wrong causes** (legacy GS rings, ray
tracing); HARDWARE_FINDINGS.md has both and the rule that follows.

**The chip description.** The winsys describes the GPU as Navi21 (GFX10.3)
and sets deviations by hand as they are found. Mesa's CHIP_GFX1013 (the
BC-250's chip, GC 10.1.3 upstream) was derived on the host from the same
winsys inputs and compared field by field (every scalar of radeon_info and
ac_compiler_info):

| Subsystem | Set by hand in the winsys | GFX1013 gives | Verdict | Evidence |
| --- | --- | --- | --- | --- |
| Shader ALU | `has_accelerated_dot_product` false | false | redundant | NGG repack drew nothing (2026-09-26) |
| Compute dispatch | `has_cs_wave_id` false | false | redundant | subgroup ID read 0 (2026-09-27) |
| Parameter cache | `has_ngg_per_prim_params` false | false | redundant | implicit primitive ID read 0 (2026-09-27) |
| Parameter cache | `has_ps_strict_vertex_order` false | false | redundant | second triangle's inputs rotated (2026-09-27) |
| Rasterizer | `has_vrs` false | false | redundant | shading-rate check (2026-09-27) |
| Colour block | `has_rgb9e5_color_target` false | false | redundant | 500 blit cases (2026-09-26) |
| Colour block | `rbplus_allowed` false | false | redundant | set with the description, no run of its own |
| Depth block | both HTILE TC Z clear bugs true | true | redundant | discard.depth (2026-09-27) |
| Depth block | addrlib revision 0x82 | 0x82 | redundant | D16 mip layout (2026-09-27) |
| Shader memory | `has_flat_scratch` false | true | platform, stays | s_setreg of FLAT_SCRATCH faults (2026-09-27) |
| Geometry | `has_legacy_gs` false | true | stays, cause open | legacy GS hangs; the recorded reason was wrong |
| Command processor | `has_gpu_written_ibs` false | true | platform, stays | B8: command fetch in the system context |
| Profiling | `has_perf_counters` false | true | platform, stays | no exported stable power state |
| Ray tracing | BVH instruction off | on | driver bug, gate lifts | rq-full-1 |
| Memory and sync | replay VA, sparse, userptr, VRAM, 32-bit window, timelines | kernel inputs | platform | the winsys |

Nine of the fourteen chip overrides are exactly Mesa's GFX1013 model. The
other five are platform restrictions, a hang whose cause is open and the ray
tracing bug; none is a chip trait the model gets wrong. GFX1013 also differs in fields the port has not set, each a
prediction with no console evidence yet: `has_htile_stencil_mipmap_bug`,
`has_zero_index_buffer_bug`, `has_two_planes_iterate256_bug`,
`has_ngg_fully_culled_bug`, `has_mad32`, the shader core's limits (20 waves a
SIMD, 2,160 SGPRs, a VGPR granule of 4 in wave64), `l3_cache_size_mb` 0 (Navi21's
128 MB Infinity Cache, which the PS5 lacks; on GFX10.3 it is only printed and
put in RGP captures), and amdgpu leaving GFX1013's compute rings off as
broken, which bears on the open compute queue probe. main-1's failures so far
match none of them.

What it implies: the GPU is a hybrid, and neither identity describes it.
Register programming and the shader core behave as GFX10.3 (RADV's GFX10.3
programming passes the CTS; the VGPR granule is inferred), while the
fixed-function blocks are GC 10.1.3's. The deviations were being found one
symptom at a time where Mesa already models them. Once main-1 and the merges
below are done, the description should state that: keep GFX10.3 as the
identity and take the fixed-function traits from Mesa's GFX1013 model in one
place instead of nine lines, and test each open prediction before adopting
it, the stencil mipmap and zero index buffer bugs first (both have a RADV
workaround ready). Until then the description is unchanged.

ACO targeting gfx1013 is a separate decision. The compiler's gfx_level
decides the instruction set (GFX10.3 drops v_mad_f32 and v_mac_f32, which
`has_mad32` names, and adds the dot products that do not compute here), the
VGPR granule and wave limits, and GFX10.1's hazard workarounds. Measured:
GFX10.3 code runs the CTS, and the dot products do not compute. Not measured:
whether v_mad_f32 exists, and whether GFX10.1's hazards (the ones ACO works
around for gfx1010-gfx1013) exist on this shader core. The second matters most,
because a missing workaround corrupts rarely and depending on data. ACO stays
on gfx1030 without dot products until two shader probes answer those.

**Merge order.** main-1 measures the pinned revision (3057cb5), which both
branches are compared against, so neither merges before it ends. The branches
touch no common file.

1. ps5-rt first. Its gate is done: rq-full-1 and the smoke title's
   acceleration structure check. Turning the BVH instruction on by default
   (reporting acceleration structures and ray queries) is its own commit, with
   the same gate rerun on the merged pin. Ray tracing pipelines stay off until
   the buffer-scratch branch passes dEQP-VK.ray_tracing_pipeline.
2. ps5-gs-compute second. Measured so far: the transform_feedback sample
   (xfb-gsc-3), the pipeline library sample (gsc-regress-1) and the smoke
   title (73 of 73, both GS paths). Still to run before it merges: the whole
   transform_feedback, geometry, pipeline, query_pool and conditional_rendering
   groups against main-1, where conditional_rendering.transform_feedback's 9
   known failures (four-stream capture under conditional rendering) are still
   open.
3. Then every main-1 case that did not pass, rerun on the merged pin.

**Upstream.** The traversal fix is upstream-correct: upstream Mesa's two
conversions disagree the same way. Freedreno's `bvh/copy.comp` does not share
the pattern (it stores and restores full 64-bit addresses and packs no node
IDs), and no other Mesa driver has it. A patch with an upstream message is
prepared for review before it is submitted.

## 2026-09-27 — main-1 ends; conditional capture from a geometry shader

main-1, every mustpass group but transform feedback, ended with 2,786,062
cases: 1,098,388 pass, 43 quality warnings, 1,675,626 not supported, 11,909
failures, 66 crashes, 26 lost devices and 4 resource errors. Of the 12,005
that did not pass, 1,435 pass in later runs and 10,560 belong to features
switched off while it ran; what is left, and every "not supported" the port
caused, is in [CTS_GAPS.md](CTS_GAPS.md).

1. **Seven failures had already been fixed.** The six
   descriptor_indexing.*_minNonUniform cases and
   rasterization.culling.primitive_id failed at 01:11 and 01:27, on builds
   from before the fixes made during main-1, and pass on 3057cb5
   (triage-untriaged-1). The minNonUniform shaders compile to the same
   waterfall loop as upstream's for Navi21 (host builds of both, compared).
2. **Conditional capture from a geometry shader.** The nine
   conditional_rendering.transform_feedback cases on ps5-gs-compute captured
   nothing on any stream. Forcing the compute passes to run unpredicated
   (condxfb-nopred-2) captured nothing either, so conditional rendering was
   not the cause; a one-shot capture of the draw parameters showed the passes
   right. The capture happens in the rasterization copy of the geometry
   shader, which is the vertex stage's shader, and the test's shader picks
   its stream from a push constant pushed for the geometry stage alone:
   RADV emitted it to no shader. Geometry-stage constants now also go to the
   vertex stage while such a pipeline is bound (b26797e); the 9 pass and the
   8,885 cases of gsc-regress-1 are unchanged (gsc-pc-gate-1).
3. **Inherited conditional rendering is not the port's gap.** Its 482 cases
   are not supported, and upstream RADV reports the feature false as well.
4. **memfd_create.** The 8 placed-mapping cases that need two views of one
   memory object were not supported because the console's libc has no
   memfd_create. The platform layer now builds it as FreeBSD 13 does, on
   libkernel's anonymous shared memory objects (SDK fork 71f2494, host tests
   185 of 185); the console proof comes with ps5-gaps' placed mappings.
5. **Asynchronous compute: what the public sources say.** Mesa keeps
   GFX1013's compute queue off as broken. A public BC-250 project traces that
   to asynchronous dispatches with partial threadgroups mis-executing (the fix
   is RADV's has_async_compute_threadgroup_bug workaround, which Iceland and
   Tonga already take) and to amdgpu's queue teardown, and reports the
   synchronization2 group passing on the compute queue with both fixed. Its
   mesh shader patch does not transfer: this GPU has no per-primitive
   parameters, which RADV's mesh path needs. The AnyPS5 emulator, which boots
   a PS5 game, models sceAgcDriverSubmitAcb as a queue number (0x20 to 0x57)
   and the description sceAgcDriverSubmitDcb takes. None of this is console
   evidence; a probe of that submission is designed and not run yet.

## 2026-09-27 — the GFX1013 traits gate; four branches in ps5-port

The large traits gate (traits-large-1, 253,264 cases: every group the nine
consolidated traits and the HTILE and revision fields bear on) ran the
consolidation of ps5-gfx1013-traits against main-1: none worse, 9,551 from
a failure to a pass or not supported, 230,153 the same. The other 13,560
changes are all fragment barycentrics and variable-rate shading, which main-1
still reported for part of its run and which were switched off for measured
reasons before this gate.

ps5-port now carries, in this order: the ray traversal fix, geometry shaders
as compute (with b26797e, the push constants of the rasterization copy),
placed maps with the honest queue and conformance reports, acceleration
structures and ray queries reported by default (their gate was rq-full-1),
and the traits consolidation (c40a45e), whose one conflict, the ray tracing
line it replaced, resolves to reporting the BVH instruction. The host-model
profile of that tip differs from the one without the traits only in the
build-derived UUIDs. The CTS title links the main checkout's build, so the
next CTS title is this tip.

tools/build-radv.sh builds mesa_clc and vtn_bindgen2 from the pinned
revision, since poly's kernels need them.

## 2026-09-27 — ray tracing pipelines, capture and replay, sparse, calibrated timestamps

Four gaps in [CTS_GAPS.md](CTS_GAPS.md), each built on its own branch and
measured by a targeted run, then brought into ps5-port (7848a74):

1. **Ray tracing pipelines.** ACO's calls through the scratch buffer (as on
   GFX6-8) where a shader cannot set FLAT_SCRATCH, on top of the traversal
   fix. A sample of every 20th case of dEQP-VK.ray_tracing_pipeline
   (rtp-sample-1, 935): 302 pass, the rest not supported for features
   upstream RADV does not offer on GFX10.3 or the port did not yet (sparse,
   capture and replay).
2. **Capture and replay addresses.** A captured buffer outside the shaders'
   window takes the top of the device-memory region and a replay takes its
   exact address or fails. replay-2 (1,787 cases, every capture and replay
   case main-1 ran): 1,287 pass, 500 not supported, none fail. Shader group
   handle replay, which needs whole shader arenas replayed into the window,
   is still not reported. (replay-1 resumed an earlier run of that name, so
   358 of its results were from a morning build; it is not evidence.)
3. **Sparse resources.** The winsys maps a buffer's direct memory into a
   reserved range to bind and a shared zero block to unbind; a submission
   with binds waits for its waits on the CPU first. No exported function sets
   PRT bits, so shaderResourceResidency and residencyNonResidentStrict are
   not reported. The first sample (sparse-sample-1) lost the device on every
   image whose surface needs less than 16 KiB: RADV aligned sparse images to
   amdgpu's 4 KiB page, and the console maps 16 KiB at a time. Aligned to the
   GPU page (gart_page_size), the sample passes or is not supported
   throughout (sparse-sample-2: 176 pass), and with 3D residency on, a 3D and
   cube sample passes 298 of 394, the rest needing a device group
   (sparse-3d-1).
4. **Calibrated timestamps.** The winsys reads the GPU clock with a
   RELEASE_MEM of its timestamp, submitted alone. dev_domain_test and
   calibration_test passed at once; host_domain_test failed because Mesa's
   runtime reads FreeBSD's CLOCK_MONOTONIC_FAST for the raw monotonic
   domain, which lags a precise reading. Without CLOCK_MONOTONIC_RAW the
   domain is no longer offered, and all three pass (ts-2).

The placed-mapping cases that need two views of one memory object pass with
the platform's memfd_create (ts-mp-1: 5 pass, the 8 others quality warnings
only because /proc/self/maps does not exist to cross-check).

The CTS's own PS5 platform reported no window system of any kind, so the
about 4,200 headless WSI cases were not supported before the driver was
asked. It now offers headless displays (CTS fork 971a27e).

merged-1 reruns 311,927 cases on the merged driver: every case main-1 did
not pass, transform feedback, ray queries and pipelines, everything main-1
skipped for acceleration structures, ray tracing or sparse, capture and
replay, the headless WSI cases and device info.

## 2026-09-27 — merged-1 triage: sparse descriptor buffers, the sparse queue, a CTS test bug

merged-1 stopped at 144,298 cases with 58 crashes, all
dEQP-VK.binding_model.descriptor_buffer.sparse_*.*acceleration_structure*:
cases main-1 never ran, since neither sparse nor acceleration structures
were reported then. The GPU faulted reading at low addresses (0xc0012000 and
the like). Shaders reach a descriptor buffer through a 32-bit pointer with
the window's high word, so RADV creates one with RADEON_FLAG_32BIT, and the
winsys ignored the flag for a sparse range, which sat in the device-memory
region: the shaders read their descriptors from the window at the same low
bits, and the acceleration structure pointers read there faulted. A sparse
range with that flag is now a reservation in the window, where the kernel
places one given no address (PS5_Mesa 400560e).

The 22 compute cases left crashed in Mesa's runtime: the dedicated sparse
queue family enables a submit thread, which the runtime has only with native
timelines, and the winsys builds timeline semaphores over its binary sync
type. RADV now offers that family only when the winsys's own sync type is the
timeline; sparse binding stays on the one graphics family (a6bdf1a).
sparse-db-1 and sparse-db-2 (the 427 cases of the sparse descriptor buffer
groups sampled): all pass or are not supported.

dEQP-VK.binding_model.descriptor_heap.basic.fragment.input_attachment failed
in main-1 and merged-1 alike, whatever the driver's compression. The test's
subpass dependency made nothing written in the first subpass visible to the
second's input attachment reads (from TOP_OF_PIPE, with MEMORY_WRITE), and
upstream fixed the test after 1.4.6.2 (VK-GL-CTS c8ff9475c5, issue 6446; not
on the 1.4.6 release branch). The CTS fork carries the fix (4615d988e2) and
the case passes (sparse-db-1).

merged-1 resumed on a6bdf1a and CTS 4615d988e2 from case 144,298.

Past batch 11, merged-1 crashed on every
dEQP-VK.subgroups.ballot_broadcast.ray_tracing case with the widest types
(uvec4, dvec3, 64-bit vectors, bvec2 to bvec4): a GPU write at 0x3f...,
just below a scratch buffer at 0x40.... Those shaders spill, and with
buffer scratch (ray tracing pipelines here, GFX6-8 upstream) a callee whose
spills overflow the offset range moves the stack pointer, the scratch
descriptor's base, by the spill area and back. ACO restored it by adding the
negative offset with s_addc_u32, whose carry means nothing was borrowed,
and then subtracted that carry from the high word as a borrow: every
restore moved the base down 4 GiB. It now adds -1 plus the carry (PS5_Mesa
8b2a6d9). rt-spill-1, all 1,431 subgroups ray tracing cases: 1,018 pass,
413 not supported, none fail. merged-1 resumed on 8b2a6d9.

The host CTS (PS5_VK-GL-CTS/build-host, vulkan_headless) now runs against
the host model, which executes nothing but compiles every pipeline and
records every command: crashes and asserts in those paths show there before
a console run. The Vulkan loader unloads and reloads the driver between the
CTS's instances, which left the model's address window reserved by the
first load; the window now goes with the library (5e2849a).

The headless WSI cases then crashed: Mesa's headless swapchain gave any
driver but a software one DRM images, which exist only with libdrm, so
get_blit_type asserted (PS5_Mesa 5f016bf: CPU images and a blit to a host
buffer without libdrm), and RADV put that blit on a private SDMA queue on
every GFX9+ GPU, which asserted without SDMA (8dfaa00: the presenting queue
blits). headless-2, 85 of the headless cases: 3 pass, 82 not supported for
what Mesa's headless surface offers (present modes, scaling, transforms,
present timing), as upstream. merged-1 resumed on 8dfaa00.

merged-1 ended with 311,927 cases: 211,504 pass, 100,327 not supported, 11
quality warnings (placed maps' missing /proc/self/maps, two pipeline
library shader module identifiers and a pipeline binary, all as before),
and 85 that did not pass, each accounted for:

- 72 crashes fixed and proved since: sparse descriptor buffers (58,
  sparse-db-1 and -2), ray tracing spills (9, rt-spill-1), headless
  swapchains (5, headless-2);
- descriptor_heap.basic.fragment.input_attachment, the CTS bug (passes in
  sparse-db-1);
- the two shader object streams cases (ps5-gs-objects, gate queued) and the
  two tessellation limits cases (ps5-gs-tess, gate queued);
- 8 timeouts, every *.multiple.*buffers32_sets1 case of the sparse
  descriptor buffer groups: the title runs one CPU at 99% until the CTS's
  watchdog ends it, where the same cases with ordinary buffers pass. Open;
  run alone next (sparse-db-32-1) to tell a slow case from one slowed by
  what earlier cases left behind.

Against every earlier run, no case is worse but those the later runs above
fixed.

## 2026-09-27 — queues, memory types and slice 5 merged; mesh shaders; a VideoOut swapchain

Merged into ps5-port after their gates, each with no failure:

- ps5-queues (506ec2f): a compute and transfer family on the graphics ring.
  queues-gate-1, 25,318 cases: 20,390 pass.
- ps5-memtypes (20fe8ae): an integrated GPU's memory with host-cached types,
  and host pointer imports. memtypes-gate-1, 50,014 cases: 44,993 pass, 8
  quality warnings.
- ps5-gs-tess (ebaf6bc): slice 5 with two fixes to the rasterization copy's
  draw (RADV_GS_COMPUTE.md). s5-regress-2 (every geometry shader as compute)
  and s5-default-1, 13,455 cases each: 11,721 pass.

The 8 timeouts are explained: a development-build compile cost, not a sparse
defect (HARDWARE_FINDINGS.md). The release archive passes them
(sparse-db-32-rel-1).

Mesh shaders without task shaders (ps5-mesh, still behind RADV_PS5_MESH):
indirect draws go through draw records because the CP rejects
DISPATCH_MESH_INDIRECT_MULTI, and multiview's layer goes with the position.
mesh-exp-3 (the indirect and multiview cases, 295): 197 pass, no failure;
mesh-all-1 (every case without a task shader, 10,650): 733 pass, no failure,
the rest needing mesh shader queries or inherited conditional rendering, as
upstream. One gap keeps them off by default: in a workgroup exported in
parts, an atomic's result that decides outputs is undefined in the parts
after the first.

VK_KHR_display on VideoOut (ps5-wsi, with the platform layer's
ps5platform/videoout.h in PS5_PayloadSDK 2f27d3b): the smoke title's display
check passes (92 of 92 checks with it): 60 frames presented at the display's
pace and a replaced swapchain.

## 2026-09-27 — vkQuake runs on RADV

With the VideoOut swapchain merged (ps5-port 7e30f3e), vkQuake links RADV
(PS5_vkQuake's `radv` branch, PS5_VULKAN_DRIVER=radv, through
tools/radv-link.sh) and runs its demo loop on the console at 119.88 fps in
every steady window, one vblank a frame, taking the 120 Hz mode the title
declares; first present 3.3 s after start, the pipelines 0.69 s without a
shader cache (PS5_vkQuake evidence/radv-r1-demo). The installed title was
put back to the ps5vk build afterwards.

## 2026-09-28 — vkQuake ships on RADV

`tools/build-radv.sh` pins the fork's ps5-port at 7e30f3e (the VideoOut
swapchain merged) and gained a release variant: `tools/build-radv.sh release`
builds the same revision with Mesa's assertions off (debugoptimized,
b_ndebug=true) into .deps/native/radv-release, in 81 s from a fresh tree; the
default build keeps them for the smoke test and the CTS. PS5_vkQuake's `main`
now links that release archive by default (PS5_VULKAN_DRIVER=ps5vk still builds
ps5vk), and the installed title is that build: the demo loop at 119.88 fps, one
vblank a frame; first present 3.23 s after start, 2.07 s of it RADV's device
and the swapchain with the 120 Hz switch, 0.62 s the pipelines, which nothing
caches yet (PS5_vkQuake evidence/radv-r2-main). Start-up is open work.

Before that build, the installed vkQuake had stopped launching: the ps5vk
eboot.bin put back after the first RADV run was a copy read off the console
over FTP, which returns signed executables decrypted, and the loader refused
the plain ELF (sceSblAuthMgrAuthHeader error 46, launch error 0x80020008).
A title is restored by deploying a build, never by writing back what FTP read.

## 2026-09-28 — mesh workgroups run once; mesh shaders not reported yet

A mesh workgroup whose primitives go out in parts used to run once per part,
its memory side effects confined to the first, so an atomic's result that
decided an output was undefined in the later parts. Now the first part runs
the workgroup once and publishes what the others need in a slot of a 16 MiB
device ring: the output counts, the LDS outputs, and the outputs the scratch
ring would have held, which go straight to the slot. Later parts wait for the
slot, copy it back and export their share; the last one frees it. Workgroups
take slots in launch order, counted across the draw packet, and wait only for
earlier ones, which the hardware launched first; the packet's last user of a
slot zeroes it, and a VS partial flush separates packets (and multiview's
views). The draw records carry what the shader needs: the ring, the draw's
first workgroup (the records prepass now walks the draws in order, 32 at a
time with a subgroup scan) and the packet's workgroups.

The smoke title gained mesh tickets: 32 x 32 workgroups each take a ticket
from an atomic and paint their cell in its colour with 128 triangles, which go
out in two parts. On the console, one draw and a pair of indirect draws both
show every workgroup running once and every cell one ticket (1,024 workgroups
go round the ring's 256 slots four times). mesh-port-1 on the merge
(ps5-port b381f60): the 10,650 mesh cases without a task shader, 733 pass and
no failure, with mesh reported by default. The smoke title: 96 of 96.

One trap on the way: poly's library functions are serialized NIR made at
build time by the host's vtn_bindgen2, so a new NIR intrinsic needs host tools
from the same tree. A build that found older tools crashed deserializing them
(read_lookup_object, in geometry shaders run as compute for shader objects).
tools/build-radv.sh builds its host tools from the pinned revision, so its
archives are consistent; development build trees must be configured with tools
from their own tree.

Mesh shaders were reported by default for one merge (b381f60), and the
targeted rerun of every case main-1 and merged-1 did not pass
(final-targets-1) failed dEQP-VK.info.device_mandatory_features at once:
VK_EXT_mesh_shader requires taskShader as well as meshShader. So mesh shaders
went back behind RADV_PS5_MESH (ps5-port b0a175c) until task shaders run; the
publish ring and the draw records stay, as task shaders will need them.

## 2026-09-28 — task shaders on the graphics ring

VK_EXT_mesh_shader requires taskShader, and RADV runs task shaders on an
asynchronous compute queue with the CP's task and mesh dispatch packets: the
PS5 has neither in use (the compute queue is the backlog item; the CP already
refused DISPATCH_MESH_INDIRECT_MULTI). ps5-task runs them on the graphics
ring instead (radv_task_emulated). A task draw goes in chunks of as many task
workgroups as the task rings hold at the pipeline's payload size (16 KiB
payloads: 4,096; small ones up to 65,536): the task shader as a compute
dispatch fed the graphics state, its workgroups in one dimension from the
chunk's first, writing the task rings as ordinary buffers of the device's;
the draw records prepass from the draw ring's entries to the mesh
workgroups' records, whose index in the packet is their task workgroup's ring
entry; and the draw of those records. The next chunk waits for the mesh
shaders. An indirect draw's chunks are set up in memory by a compute pass;
all the chunks the task workgroup limit (2^22) allows are recorded, those
past a draw's workgroups empty.

One bug on the way: the chunk's records prepass cleared RADV's "compute
pipeline dirty" flag, so the next chunk's task dispatch, which writes the
compute registers itself, left the prepass pipeline looking bound, and the
second prepass ran the task shader's program on its own arguments (a GPU
fault reading a null 32-bit pointer). The task dispatch now marks the compute
state dirty.

The smoke title's task payloads: 8,192 task workgroups each fill a 16 KiB
payload for two mesh workgroups that paint their cells from its first and
last words, across two chunks, in one draw and in two indirect ones; every
cell names its task and mesh workgroup. The smoke title: 100 of 100.

The CTS on it: taskmesh-1 ran every main-1 case naming mesh or task shaders
(83,681) with the emulation on: 23,057 pass, 12 quality warnings and no
failure. Its four crashes were descriptor heap cases: the emulated dispatch
emitted only the heaps still marked dirty, which the draw's own flush had
cleared; it now emits every valid set and heap (69d7d30, task-heap-1: 4
pass). The same helper gave geometry shaders run as compute the dynamic
buffers they never had (a latent bug: with every geometry shader forced
through compute, the 960 dynamic buffer geometry cases crashed on an
assertion, gs-dyn-before-1, and now pass, gs-dyn-after-1). Task and mesh
shaders are reported since ps5-port 179f88d; the smoke title passes 100 of
100 with defaults.

## 2026-09-28 — ray tracing group handles captured and replayed

The last feature upstream reports on this GPU that the port did not, for a
reason of its own: rayTracingPipelineShaderGroupHandleCaptureReplay. The
group handles capture whole shader arenas, which must lie in the shaders'
32-bit window, where the kernel places mappings; a captured address could
be taken by the time it is replayed. The PS5 winsys now reserves the top
256 MiB of the window at initialisation and places the replayable window
buffers there itself, captures from the top down and replays at their
captured addresses (ps5-port ecf916d). rt-replay-1: the 165 cases of
merged-1 that asked for it pass; 17 need acceleration structure host
commands or mixed capture and replay, which upstream does not offer on this
GPU either. What upstream reports and the port does not is now all hardware
(variable-rate shading, barycentrics, the mixed-float dot product),
exported-function limits (device-generated commands, performance queries,
shader resource residency) and Linux (DRM, dma-buf, file descriptors,
display control).

## 2026-09-28 — RADV's shader cache on the console

RADV keeps its compiled pipelines on the console, as ps5vk keeps its
shaders: Mesa's disk cache in its database form, in
`/app0/radv-shader-cache` unless a title sets MESA_SHADER_CACHE_DIR or
MESA_DISK_CACHE_DATABASE itself (ps5-port 7261b06, 884f954). The archives
take zlib from Mesa's subproject whole, since a title links one archive, and
the platform layer gained getpwuid_r and posix_fallocate, which the cache
calls (PS5_PayloadSDK 2823efe). A build's cache entries are keyed by the
pinned revision (`-Dradv-build-id`), so a new pin never reads an old
build's binaries.

Everything the cache makes stays reachable by the console's FTP service:
folders 0777 and files 0666, whatever the umask, including parts an earlier
build made. The first working cache cost vkQuake 11 s of its launch from
an empty cache; profiling it on the console found two causes:

- The database opens its two files at every access, and the mode was set
  at every open. A change of mode is a metadata write of about 0.7 ms on
  the console (docs/HARDWARE_FINDINGS.md): 10.4 s of the 10.7 s spent
  locking. The mode now changes only when fstat shows it is wrong.
- A lookup that missed made every part (50 per cache, each a folder and two
  files, about 18 ms to make) and then opened all of them at each miss. A
  lookup now skips parts that are not there, and parts are made by writes.
  Mesa's 29 cache tests pass on the host with the change.

Profiling also showed that a title's sandbox refuses access() for every
path, existing or not (EPERM), while stat() answers. The cache now asks
stat() whether a part exists. The platform layer's ps5_access (PS5_PayloadSDK
489467e) answers from stat() and open(), and tools/radv-link.sh binds
access to it, for Mesa's own callers (the Vulkan trace trigger and file
notification). The smoke test checks it and the cache folder's mode on the
console.

vkQuake, release archive at 884f954: first present 3.83 s after start from an
empty cache (pipelines 0.96 s, against 0.62 s with no cache at all), 2.71 s
with it filled (pipelines 0.14 s), and the demo loop at 119.88 fps. The
installed title is that build. Shipping a harvested cache with a title, as
ps5vk does, is open, as is the device and swapchain start-up (about 2.5 s).

## 2026-09-28 — RetroArch runs on RADV

The first console runs of PS5_RetroArch's `radv` branch (release archive at
884f954, each launched from args.txt with a capture at a fixed frame): the
menu renders; PPSSPP's God of War: Ghost of Sparta, Dolphin's Wind Waker and
LRPS2's GTA San Andreas reach their title screens with every 10 s audio
window full after boot (LRPS2: two late windows at 99.8% and 98.9%, where
ps5vk's run of the same content had one at 99.4%). The PPSSPP and LRPS2
pictures match ps5vk's. PPSSPP first faulted on a null vkCreateRenderPass2:
it took the device's 1.4 as usable on RetroArch's 1.1 instance, and RADV
returns null for a core command above the instance's version, as it must;
the fix is PPSSPP's (PS5_RetroArch 15ea4b2), not the driver's. Open before
the cores leave ps5vk: pipeline compiles cost PPSSPP 0.7% of one window
with an empty shader cache, and RetroArch's release battery (every core,
menu actions, closing and reopening content, a PPSSPP soak) has not run on
RADV.

## 2026-09-28 — RetroArch's release battery passes on RADV

The checks RetroArch's release passed on ps5vk, on RADV (PS5_RetroArch
13f1961). Every core with a game (FCEUmm, snes9x, mGBA, Genesis Plus GX,
FBNeo, PPSSPP, Dolphin, LRPS2), threaded video on for half of them: a pad
script opened and closed the menu, closed the content through the Quick
Menu and loaded it again, three driver initialisations a run. PPSSPP ran ten
minutes with 25 menu toggles. No crash, and the soak's windows were at 98-100%,
as on ps5vk. What stands between RetroArch and RADV is compile time: with an
empty shader cache PPSSPP and Dolphin lost 0.8% of one window each.

## 2026-09-28 — Compile time on the console: the heap's lock

RADV timed its own pipeline creation on the console (a scratch build): for
PPSSPP's God of War from an empty cache, 45 ms a pipeline, SPIR-V to NIR
15 ms, ACO 14 ms, the other NIR passes the rest; with the cache filled, every
pipeline a hit and all of the time disk reads (PPSSPP reads from many threads
at once). The same pipelines, captured on the console and compiled by the
PS5 host model, take 7.4 ms on one host thread; Dolphin's, compiled on few
threads, take as long on the console as on the host. On the host, one lock
over every allocation doubled PPSSPP's time a pipeline at eight threads
(19.6 ms median against 9.3), and both heaps the titles use were one locked
dlmalloc mspace.

PS5_PayloadSDK 714a6fc and 95c08f2 give the title heap up to eight arenas,
one for each allocating thread, a block going back to its own from any
thread, and offer it without its wraps; the RetroArch title's overflow heap
is that heap now (PS5_RetroArch 2d73b42). PPSSPP's compiles from an empty cache
on the console: 5,480 ms to 2,043 ms, median 37.4 ms to 17.4 ms, and its
windows after boot are full. vkQuake's start-up is unchanged (its pipelines
compile on fewer threads), the smoke test passes 102 of 102, and RetroArch's
battery and soak pass again. Dolphin's boot with an empty cache still loses
17% and 8% of its first two windows to its ubershaders.

## 2026-09-28 — The shader cache's reads: an exclusive mode

With the cache filled, PPSSPP's pipelines still took a median 11.3 ms each.
Timed on the console, a read of the cache's database spent 4.8 ms waiting
for the part's lock behind other threads' reads, 1.6 ms reopening and
relocking the files and 0.6 ms writing the entry's access time back, against
0.01 ms reading the entry. ps5-port cedb774 adds an exclusive mode to Mesa's
database, the default on the PS5, where a title's cache is its own: each part
is opened and locked once, accesses skip the reopening, relocking and
rereading, and access times are written in batches. A first version made
the streams unbuffered; the console's libc then read a byte a system call,
and pipelines took seconds, so the streams stay buffered and are reopened
after a compaction instead.

With the cache filled, on the console: PPSSPP's pipeline creation 1,419 ms to
29 ms, median 0.3 ms; vkQuake's pipelines at start 0.14 s to 0.007 s, first
present 2.44 s. Dolphin with its cache filled (87% and 99% for its first two
windows) now matches ps5vk's run from an empty cache (86% and 99%); what is
left is RADV's compile of its ubershaders from an empty cache (83% and 92%),
which on the host profile is Mesa's own NIR optimisation loop.

## 2026-09-28 — the second full CTS run; the fork on main

With every item in [CTS_GAPS.md](CTS_GAPS.md) closed by a targeted run, the
second full run of the pinned CTS, full-1, started at 02:31 on the debug
archive of ecf916d (every mustpass group main-1 ran, 2,919,757 cases in
batches of 20,000). At 560,000 cases: 228,553 pass, 331,439 not supported,
8 quality warnings, no failure, no crash and no lost device. It is a user
service on the host now (`systemd-run --user`), so the run outlives the
session that started it: restarts of the desktop app ended the earlier
orchestrators, and each time the run resumed from its results.

The Mesa fork's work is on its `main` branch: `ps5-port` and the feature
branches were merged into it, and `tools/build-radv.sh` exports the pinned
revision from there. vkQuake and PS5 RetroArch (v0.5.0-alpha.5) ship on the
release archive of cedb774.

## 2026-09-28 — full-1 interrupted once, by me

At 1,608,518 cases (758,443 pass, 850,066 not supported, 8 quality warnings)
I closed the CTS title in the middle of batch 53 to use the console for a
PS5 RetroArch fix: the batch's sparse descriptor-buffer cases ran at about
two a second, so it had some two hours left. The runner recorded the case it
was in,
`dEQP-VK.binding_model.descriptor_buffer.sparse_residency_buffer.push_template.graphics_tese_sets1_push_set0`,
as Crash ("the run exited (runner)"). That record is the interruption, not a
result: the case goes in the targeted rerun of every non-passing case. The
run resumes from the next case.

## 2026-09-29 — full-1 complete: no failure

The second full run of the pinned CTS on RADV ecf916d finished at 03:25 (every
mustpass group of main-1, 2,919,757 cases, 120 batches):

| Result | Cases |
| --- | --- |
| Pass | 1,569,390 |
| Not supported | 1,350,306 |
| Quality warning | 60 |
| Crash | 1 (my interruption, above) |
| Fail | 0 |

The quality warnings are four families:

| Group | Cases |
| --- | --- |
| `pipeline.pipeline_library.shader_module_identifier` | 33 |
| `pipeline.{monolithic,pipeline_library,fast_linked_library}.creation_feedback` | 18 |
| `memory.map_placed.{normal_,}unmap_reserve` | 8 |
| `pipeline.monolithic.pipeline_binary` | 1 |

Next, before any further full run: the targeted rerun of these 61 cases, and
the not-supported features, each either implemented for real or confirmed as
a genuine absence of the hardware or the platform.

## 2026-09-29 — swapchains smaller than the mode

Prospero Win (Wine WoW64 with DXVK 2.6.2) runs its games at a 1920x1080
desktop. The VideoOut swapchain only took 3840x2160, so Wine created the
host swapchain at 4K and the 1080p image showed in the top-left quarter.
The backend now registers a framebuffer set per swapchain size and reports
any size from 1x1 to the mode's, with `VK_PRESENT_SCALING_STRETCH`
(PS5_Mesa 5115a1d; HARDWARE_FINDINGS.md, same date). `tools/build-radv.sh`
pins 5d9f448 (that change and a README note).

Console, FW 12.02, through Prospero Win with the release archive linked as
a title `libvulkan.prx`:

| Check | Result |
| --- | --- |
| DXVK 2.6.2 pixel controls, D3D8/9/10/11, x64 and x86 | 8/8 pass (on cedb774; the WSI change does not touch them) |
| DXVK 2.6.2 real-draw controls, D3D8/9/10/11, x64 and x86 | 8/8 pass on cedb774; D3D11 x64 and D3D9 x86 again on the new code |
| 1920x1080 D3D11 and D3D9 four-quadrant scanout controls | pass, full screen on the TV and in Remote Play |
| 3840x2160 D3D11 control, and a 1080p to 4K resize in one swapchain | pass |
| x64 Vulkan probe, x86 placed-map probe | pass |

