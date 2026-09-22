# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-22 (evening)_

## Now

**Round of 2026-09-22 (port requests R8, R6, R4 and the fragment-less pipeline): implemented
and host-gated, nothing claimed.** No console run this round -- the session that did the work
could not reach the console -- so every capability below waits on one run of
`jobs/r8-lines/queue.txt`. Phase-log entries: `docs/M5_PHASE_C.md`, 2026-09-22.

- **R8, `VK_PRIMITIVE_TOPOLOGY_LINE_LIST`** (`6d000b7`): DI_PT_LINELIST 2 to the link, both
  stages compiled with `primitive_type = 2` (the NGG vertex stage exports two vertices a
  primitive), and a line's draw records VGT_GS_OUT_PRIM_TYPE LINESTRIP (0x29b = 1),
  PA_SU_LINE_CNTL 8 (width 1.0) and PA_SC_LINE_CNTL 0, all sourced from the register database,
  RADV and ps5-opengl. Cull bits cleared for lines. Wide and multisampled lines, points, line
  strips, fans, adjacency, patches and restart stay refused. Case `v0-lines`.
- **R6, the strip** (`15878c3`): the parked "GPU wedge" was the harness aborting on a
  zero-size index buffer before any frame recorded (`Klog_Logs/r6-strip.log`: "abort is called"
  after the vertex buffer mapped; reproduced on the host), and `a6f43d7` linked the strip as
  DI_PT **5, the fan**; the strip is 6. Both fixed, `v0-strip` rewritten and re-registered.
  The port's `warp` strip pipelines were created against the fan value.
- **Fragment-less pipelines** (`bfbfc32`): the draft read again (the empty shader is `s_endpgm`
  alone with colour and Z formats zero, RADV's GFX10 no-export form) and kept; case
  `v0-fragmentless`, host test `v0_fragmentless`, B6's vertex-only checks.
- **R4** (`1414853`): swapchain and plane-surface creates refuse by field with
  `VK_ERROR_UNKNOWN` instead of asserting; host-gated in C1 present, needs no console run.

**Before this round.** R7 (`358bde4`, compute shares the draw path's descriptor tables) is
console-proven: pid 162, 6/6, `evidence/r7-compute/capture.json`. R2, R3 and R5 are closed.

**Host evidence.** check-driver PASS with 292 identical golden comparisons (new tests
`v0_topology` 35/35 and `v0_fragmentless` 9/9 direct, B6 24/24, C1 16/16); lint, unit tests,
runner cases (inventory unchanged), the three audits, migration, mip layout, psbc link and the
Vulkan runtime gate PASS. Built in a clean clone with clang 18.1.3 (the host's archive is clang
22.1.8); `probe-packages` differs there only in `probes/v0-push/bindings.txt`
(`pixel_user_sgpr_count` 4 against the committed 7, packages byte-identical) at `a6f43d7` as
well, so it is that environment's and not this round's -- re-check on the reference host.

**Port handoff.** `build/driver/ps5/libps5vk.ps5.a` on the reference host predates these
commits: rebuild it (`tools/build-driver.sh`) before the port relinks, and check its content --
`strings` must find `only triangle lists, triangle strips and line lists`. Then the port's
`debug_lines` and `md5_debug` edits can retire, pending `v0-lines` passing on the console.

**Next.** One console run of `jobs/r8-lines/queue.txt`, capture with
`python3 tools/ps5_console.py klog`; record pids, goldens and the three verdicts, then claim.

**Parked.** MRT's four-attachment frame (counts 1 and 2 console-proven; the advertised maximum
dies in Mesa's `vk_object_base_assert_valid` before the registers): the next action is a host
dump of four rows, both mask words `0x08e`/`0x08f` and `CB_COLOR_CONTROL`, no console cycle.
The CTS subset was not re-run this round (its build tree was not available); reporting did not
change, which the inventory diff in the runner-cases gate confirms.

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
