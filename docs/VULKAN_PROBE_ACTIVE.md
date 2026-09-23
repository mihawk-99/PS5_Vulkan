# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-23 (R47)_

## Now
**vkQuake performance is the priority; no CTS work.** Rounds R33-R40 this
session (jobs/r33-begin-split, jobs/r37-mapped-flush; port evidence m6-r33-*
through m6-r40-*):

- The profile is nearly free (44337ef): TSC timestamps (a system call costs
  ~20 us on this console, a TSC read 12 ns) and one write(2) on fileno(stderr).
  Earlier profiled window means are inflated by a 1.6-5.8 s summary write.
- R37 (6f347ba): colour targets are flushed only in memory the application has
  mapped; blits/resolves invalidate their source first, the runner's
  image-storage pointer invalidates on handover. Flush 3.9 -> 0.002 ms at E1M1.
  Runner battery: every test except b8-indirect, per-test statuses identical to
  the pre-change driver (138 tests, 130 PASS each). v0-mrt stops the runner on
  both drivers -- a pre-existing open item.
- R38 (d915015): the completion marker is spun on for up to 1.5 ms before the
  first 1 ms sleep; every step is found in the spin, poll 1.12 -> 0.15 ms.
- R40 (b4c0b2e): vkCmdExecuteCommands 0.14 ms a frame; begin 0.33 ms, draw
  encoding 0.08 ms. The driver is ~0.9 ms of an E1M1 frame; the application's
  ~17-19 ms is the remaining cost.
- The owner enabled VRR for unsupported games; with it the present does not
  wait and FPS follows work. E1M1 55.76 FPS, start map 33.08 (was 29.58/19.72
  at R29). The console's runner title currently holds the R37 baseline build.

R41-R47 (jobs/r42-parallel-blit, r43-hitch-recorder, r46-nir-cache,
r47-shipped-cache): blits resampled on five threads (walking the start map
52-55 FPS, runner blit/mip/resolve tests identical); a per-frame hitch report;
the internal NIR cache landed (warm launches compile nothing); one cache
directory per build, 0777 so the port can harvest and ship it. The owner's New
Game stutter was the port's unbuffered console stream, now fixed there.

Next: the application's ~17 ms; then the 120 Hz mode at swapchain creation
(enumerate, restore, fall back, report truthfully).

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
- Entry-point accounting is complete (90 driver, 0 refused, 0 gap), but that
  does not prove every parameter combination. R16–R29 now cover the game's
  tiled mip/readback and filtered-copy paths. Other shapes/formats/aspects still
  need focused proofs; do not treat historical refusal lists as current coverage.
- The AGC compiler has no bounds-checking option and refuses uniform blocks
  larger than 16 bytes, which is why robustness is the driver's index-count clamp
  rather than a shader-side check.
