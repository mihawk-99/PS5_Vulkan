# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-23_

## Now
**vkQuake performance and stability are the priority; no CTS work.** vkQuake
runs at up to 120 FPS at 4K: walking the start map, 119.88 FPS with every frame
8.29-8.40 ms, 4.0-4.4 ms of work (port evidence m6-r49-kstuff-paused). That needs
two console settings: etaHEN's "pause kstuff on game launch" (with kstuff active
a system call costs ~20 us instead of 0.73 us, and the game manages ~52 FPS) and
VRR for unsupported games (frames present as soon as ready, 48-120 Hz).

Driver rounds this session, each with its console proof in jobs/:
R33-R36 (jobs/r33-begin-split) a nearly free profile with TSC timestamps and one
write(2), and the finding that its old fputs was the multi-second "stall";
R37 (jobs/r37-mapped-flush) flush colour targets only in mapped memory, runner
battery identical to the pre-change driver (138 tests, 130 PASS each); R38 a
bounded marker spin; R40 vkCmdExecuteCommands timed (0.14 ms, ruled out);
R42 (jobs/r42-parallel-blit) blits on five threads; R43
(jobs/r43-hitch-recorder) a per-frame hitch report; R46 (jobs/r46-nir-cache)
the internal NIR cache; R47 (jobs/r47-shipped-cache) one cache directory per
build, 0777, which the port harvests and ships; R51 (jobs/r51-output-mode)
119.88 Hz selected where the title declares it, 59.94 Hz otherwise; R53
(jobs/r53-output-retention) one VideoOut per process, configured when the modes
are listed (a refused 119.88 Hz is never offered) and, retained by the
application (`ps5vk_display_retain`), kept with its image across swapchains so
RetroArch's context rebuilds no longer blank the panel.

Next, in order:
1. Colour targets other than 3840x2160 (vkQuake's raster warp path renders
   512x512 and is refused), then multiple colour attachments (v0-mrt, which
   also stops the runner).
2. The port's gameplay/stability acceptance.

The test runner on the console (PPSA99988) holds the R46 build. An intermittent
texture glitch seen in play is set aside until it can be captured.

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
