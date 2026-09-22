# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-22_

## Now

**R7 compute startup blocker: driver acceptance passed.** The promoted vkQuake
request took priority over CTS and the MRT dump. Compute creation no longer
requires exactly one storage buffer. Draw and dispatch share their descriptor
builder: every declared binding, one table per set, the same 16 user-data dword
budget, and the existing descriptor-type writers. Details and boundaries:
`docs/M5_PHASE_C.md`, entry "R7 promoted to startup".

Console pid **162**, digest `1ccd486d…`, passed **6/6** cases: the new
`d2-compute-images` (one dispatch, texture in set 0, storage image in set 1,
**0 mismatches over 256 texels**), the direct/indirect `d2-compute` buffer probe
(both `0xa5a5a5a5`), `v0-multiset-quake`, `v0-two-sets`, `v0-formats`, and `m2-solid`.
Evidence: `evidence/r7-compute/capture.json`, `golden/d2-compute-images/`,
`Klog_Logs/r7-compute-regression.log`. The host image stream is identical to the
console's 14 packets; the old D2 goldens remain unchanged.
Full driver gate: 152 build/run checks and 292 identical golden comparisons.
Lint, unit tests, runner cases and all three audits pass.

**Port handoff.** Relink `build/driver/ps5/libps5vk.ps5.a` and check
`cs_tex_warp` creation. The actual vkQuake startup run, its six-binding kernels,
and the lightmap pass at M6 are not claimed by this probe.

**Working tree.** Pre-existing R6 triangle-strip edits remain separate. Their
unfinished harness had removed the vertex-less early return, causing unrelated
cases to fail before creating a pipeline. That early return is restored locally;
the rest of the strip harness and its acceptance remain pending.

## Standing work

- Graphics R7 rounds 1-4, R8 dynamic depth bias, R9 push pointers, and the R4
  clear/refusal coverage are recorded in `docs/M5_PHASE_C.md`; the first R1-R6
  request batch is in `docs/REQUESTS_RESPONSE.md`. The advertised-set-limit probe
  and vulkan-runtime header dependencies remain follow-ups from graphics R7.
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
