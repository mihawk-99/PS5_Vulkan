# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-24_

## Now
**Dolphin (Wind Waker) through ../PS5_RetroArch is the priority.** Every
driver fault it shows is reduced to a runner probe, fixed as a general Vulkan
mechanism, proved on the console and replayed on the host. The rounds so far,
each with its job under jobs/ (docs/M5_PHASE_C.md, 2026-09-24): R57 border
colours, R58 primitive restart, R59 gl_FragCoord with inverted depth, R60
stages over 20 KiB, R61 one-layer array views (a gate), R62 uniform buffers as
byte ranges, R63 Dolphin's skinned record (a gate), R64 primitive restart as
command-buffer state behind SQ_NON_EVENT, and R65: every submission starts
with restart off, so a copy (which splits the submission) leaves it off. After
R65, Wind Waker from a save state on Outset Island draws without the stray
triangles and minimap spill it had.

Next, in order:
1. R62's uniform and push-constant descriptors use OOB_SELECT 2, which Mesa's
   register header names DISABLED (no bounds check), not RAW (3): a robustness
   fault to reduce and fix as R66.
2. The replays of jobs/r23-r29 differ from their goldens by the five
   per-draw context registers the per-draw-state round added (90 records
   against 85); re-capture them or restate the registers.
3. Wind Waker: speed at 1x, long play, then the torture profiles.

vkQuake at 120 FPS and PPSSPP (God of War, Yu-Gi-Oh!) stay the regression
titles. The test runner on the console (PPSA99988) holds the R65 build with
jobs/regression/queue.txt queued.

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
- CTS remains outside the game work I asked for; prior status is in docs/CTS.md.
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
