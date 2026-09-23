# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-22 (late evening)_

## Now

**R13: upload metadata is now one record per region.** R12 let vkQuake present
its first human-visible menu/console frame, then PID 197 ran out of host memory
recording map-staging image copies. The old row path appended one 272-byte
record per row. It now reuses the existing region-copy executor; two linear
sides copy whole rows. Tiled maps and command order remain unchanged.

**Host witness:** a 36-row upload fell from 36 records to one. Sixty-four repeated
regions use 64 records and 17,408 bytes of capacity, below 32 KiB. Reproduce:
`jobs/r13-upload/README.md`. This bounds upload metadata; it does not establish
that every contributor to the port's heap pressure is gone.

**Gates:** explicit driver build PASS, zero warnings; targeted check-driver
c4_texture/c7_mip_upload/c7_copy/c7_blit_formats/v0_formats PASS (15 arms).
All eleven driver gates PASS. Template relink PASS. The compile/pipeline path
was unchanged; the full 167-arm suite passed in R12. Archive 14,383,804 bytes,
SHA-256 `b3bb7ac9…`.

**Console PID 198, PPSA99988:** m2-solid, c4-padded, c7-mip-upload, c7-copy PASS;
486 PASS records, zero FAIL; known benign unregister-busy warning then closure.
Twelve submissions replay exactly with same-run defaults. Evidence and exact
commands: `golden/r13-upload/README.md`. Port PID 199 passed the prior OOM and presented, then reached new tiled-chain
blit and multiple-dynamic-offset refusals and an indirect-stride assertion.
Port evidence/m2-r13-map-recording records the two matching reads and kernel abort.
User priority is now persistent shader caching, with measured cold/warm startup.

**Port M2 is met:** R12 c8658bf, PID 197, identity a779b2bd…, QueuePresent success
and human-visible Quake menu/console; port evidence/m2-first-frame. M3–M6 remain
open. That boot's staging path ignored EndCommandBuffer=-1 and submitted the
invalid recording, causing a runtime assertion; that error handling is a
separate port issue. R11 secondary replay/diagnostics are proven in PID 194.

**Other open rendering findings:** R10 input-attachment fetch matched only the
first quarter-width; `v0-lines` still fails. R13 does not alter either path,
gamma, contrast, OIT or resolution.

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
- The pinned CTS and seven externals are fetched. `dEQP-VK.info.*` is 16 pass /
  0 fail; the latest `dEQP-VK.api.info.*` is **2542 pass / 2 fail**. The open
  driver item is compressed-format reporting; `extension_core_versions` is
  labelled HARNESS/PORT after four candidate inputs were eliminated. The
  console CTS payload remains outstanding. Details: `docs/CTS.md` and the
  phase log's CTS rounds 6-12.
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
