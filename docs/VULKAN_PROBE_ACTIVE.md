# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-23_

## Now
**Persistent SPIR-V shader cache accepted on PS5.** User requested much faster
vkQuake startup. The shared graphics/compute compiler boundary now saves each
successful output atomically in /app0/ps5vk-shader-cache and reuses it across
launches, including after an application crash. The key covers shader bytes,
entry point, every compile option, specialization maps/data, compiler archive,
relevant driver sources/headers and build flags. Corrupt, stale or unavailable
entries fall back to compilation. No GPU pointers or live resources are saved.

**Same-binary vkQuake measurement:** identity 78bd43a2…; PID 202 cold first
present 30.410 s, 99 SPIR-V compiles + 433 hits, 99 files stored. PID 203 warm
first present 13.018 s, zero SPIR-V compiles + 532 hits, zero stores. Eight
internal NIR stages still compile in each. One-second trace polling; these are
presentation-return timings, not optical measurements. Both traces were read
twice and PID-correlated; console idle. jobs/shader-cache/README.md and its
cold/warm-startup.txt contain exact evidence and reproduction.

**R14–R19 accepted; game runs sustained demos.** R14 2925ff6 (single-draw stride), R15
b838832 (independent dynamic UBO offsets), R16 c7f6f95 (mip-tail XOR/blits)
passed PS5 probes. Port PID 208 presented past those three old failures, then
named a three-element descriptor array and padded pitch 256. Its evidence is
../PS5_vkQuake/evidence/m2-r16-map-recording.

R17 descriptor arrays and R18 padded mip chains pass on console. Their complete
readbacks/replays and failed attempts are retained in jobs/r17-descriptor-array
and jobs/r18-padded-mips. R18 PID 214: 531 PASS, 19 mip frames, 21 exact replays.
R19 now passes PID 216: 257 PASS, zero FAIL; three UINT32 full-frame pixel
checks and UINT16 before/after regression. Five streams replay exactly.
Each draw writes its index size; bounds and firstIndex use the element width.
Host 170 arms, eleven gates, port five gates/scan and template relink PASS.
Archive 14,434,594 bytes, SHA-256 f2666ab8…; deployed ELF segments match,
title closed, count=0. jobs/r19-index32 and golden/r19-index32 hold evidence.
Port PID 217 then ran 180 seconds without refusal/Quake error, progressing
from Necropolis to The Door To Chthon; harness closed it, count=0 verified.
R20 d8080dc retires R10's quarter-width diagnosis: the old probe misread tiled
bytes and never mapped its writer. PID 219 matches every writer/reader pixel
in two frames; no production driver change. jobs/r20-subpass retains evidence.

**Port:** M2 human-confirmed earlier; input adapter opens DualSense. Audio
now feeds native 48 kHz stereo PCM, PID 222 alive 300 seconds, 6,335 presents,
zero refusal/Quake/audio errors. Port d537cbc; physical/audible checks pending.
**R21 profiler verified:** default-off queue timing leaves packets unchanged.
PID 222 averages 509.54 MiB target flushes/frame, 7.87 ms flush, 15.20 ms
queue, 2.85 ms native-submit/marker and 7.79 ms flip waiting (overlapping
metrics). Archive dae28c8a…; 170 arms/cache checks, eleven gates, port and
template relinks pass. Two R20 streams replay exactly with profiling enabled.
See jobs/r21-profile. R22 now skips identical target address/size entries within
one flush, preserving each step and every wait/barrier/copy boundary. PID 224:
1,043 PASS, zero FAIL, 14 exact replays; incomplete timeout PID 223 retained.
Host 170/cache, eleven gates, port/template PASS. Archive b95beefd….
Game PID 225 ran 300 seconds without error: 6,557 presents. First 27 intervals
reduce flush 7.872 -> 5.788 ms and 509.54 -> 373.73 MiB/frame; flip wait grows,
FPS remains about 20–30. No FPS improvement claimed. jobs/r22-target-flush.
R23 now proves swapchain TRANSFER_SRC: PID 229, 238 PASS / zero FAIL; four
copies each match all 8,294,400 pixels, 16 strict replays. Original presentation
regression passes. Archive e662f699…; eleven gates, port/template pass. Host
replay corrections are explicit in jobs/r23-display-readback/replay-notes.txt.
R25 fixes depth/stencil state leaking into colour-only passes. PID 243 reproduces
518,400 wrong overlay pixels; PID 244 has 439 PASS/zero FAIL for D32/D16 detach,
original depth and swapchain readback. PID 245 stencil/bias regressions pass
(including the intentional nonzero-clamp refusal). 24 new streams replay exactly.
170 host/cache, eleven gates, port/template pass. Archive e172ce0f…. Evidence:
jobs/r25-depth-detach. Port PID 246 now shows all tested HUD styles correctly.
R26 fixes sampler LOD bias: game scaling requested bias 1 and previously exited.
PID 250: 383 PASS/zero FAIL; eight complete frames match every pixel for signed
and fractional bias, plus original mip tests. Twenty streams replay exactly.
Full host/cache, eleven gates, port/template pass. Archive b83fc4ac….
Evidence: jobs/r26-lod-bias. Game scaling retest and black menu background remain.
## Standing work
- Graphics R7 rounds 1-4, R8 dynamic depth bias, the first batch's R9 (push
  pointers), and the R4 clear/refusal coverage are in `docs/M5_PHASE_C.md`; the
  first R1-R6 batch is in `docs/REQUESTS_RESPONSE.md`. **The port's letters
  restart per batch** -- this round's R9 is specialization constants, last
  round's was push constants -- so cite the commit, not the letter. Follow-ups
  from graphics R7: the advertised-set-limit probe and vulkan-runtime header
  dependencies.
- Anisotropy at the reported maximum is accepted as a no-op, proved by
  `v0-sampler-anisotropy` with zero differences. `samplerAnisotropy` stays FALSE.
- Rung 1.0 audit counts remain: commands 137 (90 driver, 47 runtime, 0 gap);
  limits 106 required, 97 compared, 0 missing; formats 179 required, 58 reported,
  55 conditional, no unmet mandatory clause.
- E1's device inventory is `conformance_inventory/device_report.json`, checked
  by `tools/check-runner-cases.sh`: 97 limits, 55 features, 184 core formats,
  307 image-format combinations. The 3D dimension claim versus no 3D images and
  cube-query inconsistencies remain inventory findings.
- CTS remains outside this user-requested game work; prior status is in docs/CTS.md.
- Upstream AGC tile equations agree with the measured maps; the resource-slot
  table remains an untaken diagnostic opportunity (`docs/AGC_UPSTREAM_NOTES.md`).
- Imported driver/host/vendor/tooling trees retain their own style with
  `DisableFormat: true`; the format gate enforces those markers.
## Open findings

- **A title cannot load a graphics library at run time** (2026-09-18): every `.so`
  this project builds is refused with ENOEXEC, so the delivery is the linked
  archive `build/driver/ps5/libps5vk.ps5.a`.
- Colour targets are **tiled** (a 128x128-word 0x10000-byte block with an XOR
  pixel map; a four-sample target's tiles are 64x64 texels of four 0x4000-byte
  sample planes); **depth targets differ**, and **no two-byte format can be an
  attachment at all**, so every two-byte image is the row layout.
- Blending is programmed for the formats whose export Mesa picks as FP16_ABGR and
  refused by name for a blend factor or equation the driver cannot write; the
  integer targets need UINT16_ABGR/SINT16_ABGR exports, and a blending case's
  pixel stage has to export what its target takes (`probes/m4-blend`).
- The occlusion counter is coarse: one `ZPASS_DONE` count is 16 samples, so
  `occlusionQueryPrecise` is false.
- `kFaultingTests` keeps `b8-indirect` out of the default and `all` queues: a PM4
  `INDIRECT_BUFFER` into title memory faults the GPU.
- No command refuses by name any more (90 driver, 0 refused, 0 gap). The paths
  that still refuse are the ones whose placement or decode no probe has recorded:
  a tiled copy or readback of a chain, a depth image read back to a buffer and an
  upload from one, a multi-sample copy/blit/resolve, a subset of array layers, a
  scaled blit whose format has no recorded decode, a filtered blit, and an image
  clear whose format or aspect is not the recorded one.
- The AGC compiler has no bounds-checking option and refuses uniform blocks
  larger than 16 bytes, which is why robustness is the driver's index-count clamp
  rather than a shader-side check.
