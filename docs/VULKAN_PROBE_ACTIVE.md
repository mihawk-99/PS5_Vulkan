# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-22 (late evening)_

## Now

**The round's console run happened: 13 of 14, one open item.** `jobs/r8-lines/queue.txt`,
`Klog_Logs/r9-spec.log`, pid 178: `v0-r9`, `v0-strip` and `v0-fragmentless` **PASS**, the
standing cases PASS, **`v0-lines` FAIL** -- R8's own case on its first console run, and the
round's one open item. R9 is committed; the archive is rebuilt and checked by content.

- **R9, specialization constants** (`fef4387`, console-proven): the fork's Mesa copy had a stub
  for `vk_spec_info_to_nir_spirv` and no way to pass entries, so `pSpecializationInfo` was
  refused outright -- what stopped vkQuake's world pipelines at `-13`.
  `tooling/psbc/patch-specialization.py` (4 edits, metadata stays 14) and
  `ps5vk_specialization_options()` close it; malformed entries are refused by name. Case
  `v0-r9`: 3 of 3 constant sets drew the colour they select (`0xffff0000` / `0xffff4000` /
  `0xffff80ff`) and 2 of 2 invalid sets were refused. Compiler half: `vk_v0_spec_test.c` 4/4,
  40 / 48 / 48 bytes against a hand-written literal twin.
- **R8, `VK_PRIMITIVE_TOPOLOGY_LINE_LIST`** (`6d000b7`): DI_PT_LINELIST 2 to the link, both
  stages compiled with `primitive_type = 2` (the NGG vertex stage exports two vertices a
  primitive), a line's draw records VGT_GS_OUT_PRIM_TYPE LINESTRIP (0x29b = 1), PA_SU_LINE_CNTL
  8 and PA_SC_LINE_CNTL 0, cull bits cleared. Wide and multisampled lines, points, line strips,
  fans, adjacency, patches and restart stay refused. Case `v0-lines` -- **FAIL, open**; keep the
  port's `debug_lines` / `md5_debug` edits until it passes.
- **R6, the strip** (`15878c3`): the "GPU wedge" was the harness aborting on a zero-size index
  buffer, and `a6f43d7` linked the strip as DI_PT **5, the fan**; the strip is 6, so the port's
  `warp` pipelines were created against the fan value. **Fragment-less** (`bfbfc32`) is RADV's
  GFX10 no-export form, case `v0-fragmentless`; **R4** (`1414853`) refuses swapchain and
  plane-surface creates by field, loader-gated, no frame. R7 (`358bde4`) stays console-proven:
  pid 162, 6/6, `evidence/r7-compute/capture.json`. R2, R3 and R5 are closed.

**Host evidence, this round's run on this host.** check-driver PASS: 1620 checks, **284**
identical golden comparisons (the previous session's 292 was a clean clone, which runs two extra
PS5-link cases; `v0_topology` 35/35 and `v0_fragmentless` 9/9 direct, B6 24/24, C1 16/16),
`v0_spec` 4/4 direct. lint 212 files, `make test` 31 OK, the three audits, migration (now
covering the specialization patch), runner cases 206 records, psbc link, mip layout and the
Vulkan runtime gate PASS. **`probe-packages` failed on the first full pass and passes now**: the
two new sets were one `v0-spec|v0-spec-hardcoded)` case alternative and that check reads a
builder off the build script's own case labels, so it saw two committed sets nothing could
rebuild; one label each, 51 committed sets with 49 rebuilt and compared byte for byte. One
failure: `c1_present` in loader mode only, 15 of 16 (*"a plane surface whose imageExtent the plane
does not have is refused ... naming imageExtent"*), direct 16 of 16 with the same sentence in both
logs -- **loader-environmental and pre-existing** (this round touches no plane-surface or
swapchain code), recorded rather than fixed. A clean clone still differs only in
`probes/v0-push/bindings.txt` (`pixel_user_sgpr_count` 4 against the committed 7, packages
byte-identical); re-check that on the reference host.

**Port handoff -- done on this host.** `build/driver/ps5/libps5vk.ps5.a` is rebuilt
(14 374 298 bytes, sha256 `f3d749d6…`) and checked by content: `strings` finds `only triangle
lists, triangle strips and line lists`, the two new specialization sentences are there, and
`specialization constants are not supported` is **gone**. Relink the port against it. Then the
port's `debug_lines` and `md5_debug` edits can retire, **pending `v0-lines` passing**; the world
pipelines should create where `-13` used to be.

**Next.** `v0-lines`: read the case's own lines in `Klog_Logs/r9-spec.log` first (`runner_test`
result -1, detail `v0-lines`, line 2214). The line registers and the topology are already
measured there, so the next step is the case's own comparison, not another console sweep; claim
R8 by re-running `jobs/r8-lines/queue.txt` once it is green.

**Parked.** MRT's four-attachment frame (counts 1 and 2 console-proven; the advertised maximum
dies in Mesa's `vk_object_base_assert_valid` before the registers): the next action is a host
dump of four rows, both mask words `0x08e`/`0x08f` and `CB_COLOR_CONTROL`, no console cycle.
The CTS subset was not re-run this round (its build tree was not available); reporting did not
change, which the inventory diff in the runner-cases gate confirms.

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
