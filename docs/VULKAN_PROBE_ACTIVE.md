# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-22 (late evening)_

## Now

**R11: framebuffer-free secondary recording is console-proven.** The port's
`vkBeginCommandBuffer` leaves the optional inheritance framebuffer unset. The
old driver refused that before any draw. Its reason was hidden behind Mesa's
optional debug logging. Secondary commands now use Mesa's owned command queue,
replayed into the primary's current subpass; no GPU INDIRECT_BUFFER is used.
Recording refusals write their command and sentence to stderr as well as the
existing Vulkan callback. A host negative test checks the no-messenger case.

**Console PID 194, PPSA99988:** `b8-secondary`, `c1-triangle`, `c4-rtt` all PASS;
235 PASS probe records, zero FAIL. The runner was closed. Evidence and exact
reproduction: `golden/r11-secondary/README.md`, `jobs/r11-secondary/queue.txt`.
All eleven captured submissions/flip streams reproduce exactly on the host,
using explicitly identified existing B4 register defaults (the new queue lacks
an AGC-level anchor). No existing golden changed.

**Gates:** `bash build/gates.sh` all eleven PASS; full `tools/check-driver.sh`
PASS (55 loader, 55 direct, 55 PS5 links, two negative arms). The template's
five gates pass and its final driver relink passes. Archive: 14,383,172 bytes,
SHA-256 `65550cae…`. The port's identity `6b437103…`, PID 195, compiled 540
shaders then reached a named `ps5vk_sampled_image` refusal: a 32-texel-wide
image has 256-byte padded rows. No frame presented; the harness ended with
count=0. R11 diagnostics work, but its positive presentation criterion is open.
Port evidence: `../PS5_vkQuake/evidence/m2-texture-row-pitch/`.

**R12 resumed and hardware-proven.** Mesa `align()` replaces the unavailable
macro; sampled single-level, single-layer 2D images encode padded row pitch in
word 4. Explicit driver build PASS; full 167-arm check PASS; all eleven gates
PASS. Template five-gate regression PASS. Archive: 14,385,036 bytes, SHA-256
`c37afdec…`. No new advertised format or mip/array layout claim.

**Console PID 196, PPSA99988:** m2-solid, c4-texture, c4-padded PASS. Both 64-wide
and 32-wide textures passed nearest and bilinear pixel readback: 224 PASS,
zero FAIL, one known benign VideoOut-busy close warning. Title closed. Four
streams replay identically with same-run defaults. `golden/r12-pitch/README.md`
has commands, deployment segment proof and readback data. The port relink/boot
is next; no M2–M6 acceptance is inferred from this driver probe.

**Still open from R10:** the subpass read is correct only through x=960 of 3840;
the row-stored input descriptor needs its own readback fix. `v0-lines` still
FAILs on hardware. R9 specialization constants remain console-proven. No
shader/compiler, descriptor, line, gamma, resolution or OIT change in R11.

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
