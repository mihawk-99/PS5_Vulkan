# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-22 (late evening)_

## Now

**R10 (the port's subpass input, its refusal ask, and a capability audit): two of three landed and
measured, one defect named.** Three commits: `f9131e5` (a shader the compiler cannot lower is
refused, not fatal -- pre-check by addressing model and capability, plus a `SIGABRT`/`SIGTRAP` guard
around the compile), `3ee2f96` (the subpass read through the input attachment's descriptor: one
field in the fork's RADV call site, plus the driver binding it from the subpass), `5f7e910` (the
console case and the harness's two-subpass mode, with the frame's own measurements).

- **The refusal is done and console-seen.** `v0_capability` 14/14 loader, direct and PS5 link; the
  guard's sentence appears in the round's own console log where the port's run had a dead title.
- **The subpass read works and is not a permanent limit.** The port's `postprocess_frag` compiles
  here (324 bytes, metadata set 0 binding 0 type 4 stride 32); on the console subpass 0's attachment
  is right in its own memory (16 of 16) and subpass 1's fetch returns the writer's texels for the
  first quarter-width (4 of 16). **Open**: past x = 960 the read returns band 0, i.e. the fetch
  behaves as if the row-stored attachment were 960 texels wide -- `SQ_RSRC_IMG_WORD2`'s
  `(extent.width - 1) >> 2`. A descriptor question for a row-stored image, measurable on the host
  before another console cycle (docs/M5_PHASE_C.md, the R10 subpass entry).
- **The audit** (a host sweep: each deployed shader's SPIR-V extracted from its C array, its own
  descriptors declared from its decorations, then the patched probe CLI -- re-run after the patch):
  of the port's deployed shaders
  with capabilities beyond `Shader`, 15 compile and 6 do not -- the three 5347 ones (addressing
  model) and the three 4472 ones (bindless store) -- both classes refused by name. The port's own
  scanner mislabels 35, 46, 49 and 61; the reply carries the correction.
- **Port handoff.** The archive is rebuilt at `5f7e910`: `build/driver/ps5/libps5vk.ps5.a`,
  14 415 958 bytes, sha256 `6e12550b…`. `docs/REQUESTS_RESPONSE.md` has the R10 answer.

**Open items from the rounds before this one.** **`v0-lines` FAILs on the console** (R8's case, its
first run, `Klog_Logs/r9-spec.log`): read its own log lines before touching the driver. R9
(`fef4387`) is console-proven (`v0-r9`, 13 of 14 that run) and its archive digest was
`f3d749d6…`; R6's strip correction, the fragment-less case and R4's swapchain refusals are in
`docs/M5_PHASE_C.md`. The port's `debug_lines` / `md5_debug` edits stay until `v0-lines` passes.

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
