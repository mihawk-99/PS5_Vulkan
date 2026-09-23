# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-22 (late evening)_

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

**Verification:** explicit driver build, zero warnings; 167 check-driver arms
PASS plus key/invalidation/corruption/fresh-process package tests; eleven gates
PASS; port five gates PASS; template relink PASS. Archive 14,425,130 bytes,
SHA-256 e089e060… . PS5 probe PIDs 200 and 201 each returned 241 PASS/zero FAIL;
twelve submissions replay exactly, goldens in golden/shader-cache-cold and
-warm. Driver probe stdout is not a hit-count witness; vkQuake's trace is.

**M6 resumed by user; R14 stride fix accepted.** Single-draw count=1/stride=0
now records; indexed/non-indexed host checks PASS. PS5 PID 204: 121 PASS,
zero FAIL, indirect triangle pixels and exact submission replay PASS.
Six check-driver arms, eleven gates, port five gates and template relink PASS.
Evidence: jobs/r14-indirect-stride/README.md; golden/r14-indirect-stride.
No port retry yet; next are the named dynamic-offset and tiled-chain refusals.

**Port M2 met:** PID 197, QueuePresent success and human-confirmed Quake
menu/console; port evidence/m2-first-frame. M3–M6 remain open. Input/audio
engine adapters are stubs. R10's quarter-width input-attachment read and the
hardware line failure remain separate. No visual settings were changed.

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
