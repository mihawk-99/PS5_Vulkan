# Active work

Volatile by design. Rewrite this file in place; keep it under about 120 lines.
Specifications belong in `docs/VULKAN_PROBE_PLAN.md`, and run results belong in
the phase logs (`docs/M5_PHASE_A.md`, `docs/M5_PHASE_B.md`,
`docs/M5_PHASE_C.md`).

_Updated: 2026-09-20_

## Now

**The six requests in `PS5_VULKAN_REQUESTS.md` are answered, and the runtime the
earlier rounds measured against was stale.** One commit each, in the order they
were worked:

- **R6**: a split submission's step capture was sized for `split_count` steps
  where a recording can run `split_count + 1`, so the last step's words landed up
  to 64 bytes past the heap allocation. Sized for every step, and refused by name
  with the two numbers if it ever cannot fit.
- **R2**: the sampler's `addressModeU/V/W` are ps5-opengl's own encoding --
  REPEAT 0, MIRROR_REPEAT 1, CLAMP_TO_EDGE 2 -- so the default a zeroed
  `VkSamplerCreateInfo` holds is no longer refused; the two modes that are not
  core Vulkan 1.0 are refused by name.
- **R1**: `cullMode` and `rasterizerDiscardEnable` are programmed
  (PA_SU_SC_MODE_CNTL's CULL_FRONT/CULL_BACK/FACE, PA_CL_CLIP_CNTL's
  DX_RASTERIZATION_KILL); `depthBiasEnable` stays refused with its own named
  probe, and `polygonMode`/`depthClampEnable` stay refused by their feature bits.
- **R3**: `vk_meta` unwraps a stencil clear from the depth member, so the driver
  hands it a copy whose stencil attachment carries the stencil value
  (`WORKAROUND(R3)`, retirement in the comment).
- **R5**: the resolve refusal now names the usage bit that chose the storage, the
  rule and the workaround in one sentence.
- **R4**: each audit prints what it cannot see beside the case that covers it.

Four console-proved cases carry them -- `v0-two-passes`, `v0-sampler-address`,
`v0-cull`, `v0-stencil-clear` -- each with a queue under `jobs/` and a golden
under `golden/`.

**The linked runtime was four days older than the migration.** The stale
`libvk_runtime.ps5.a` (its stamp named the pre-fork tree and an older build
script) is rebuilt, the title digest moved with it (`c3a99b98…` ->
`b33b813c…`), and the nine-case battery was re-run against the rebuilt artifact.
The one recorded stream that moved is `golden/v0-stencil`'s first submission,
which the migration's extra user-data word explains and which
`tools/check-driver.sh` uses as a replay input rather than a comparison target
(`docs/M5_PHASE_C.md`, 2026-09-20).

**What is left in the objective.** Nothing from either mission: the next gate is
the CTS subset (`docs/CTS.md`).

**Rules this workstream keeps.** One mechanism a round; the encoding comes from a
public codebase or the register database before anything is written; the compiler
changes as a patch script (never an SDK edit); a claim lands only with a console
run, a focused host gate and the audit move in the same commit; a mechanism that
fails its battery reverts its claim and is quoted against what failed; every
battery regresses `v0-formats` (the audit mirror) and `m2-solid`.

## Rung 1.0's remainder

Every feature a per-row `{sym1}` cell requires is proved by a console run and
reported by the driver; the rows left are the hardware's and one footnote clause's:

| | |
| --- | --- |
| command audit | 137 required: 90 driver, 0 refused, 47 runtime, 0 gap |
| limits audit | 106 members, 97 compared, 0 missing |
| format audit | 179 required, 58 reported, 0 missing a required feature, 55 conditional, 0 `must:` clauses unmet |
| the split | 0 / 0 / 0 / 0 / 0 |
| the blockers left | none: the descriptor and vertex enums, the depth/stencil clause, the sRGB rows and the console fault are all closed |

## Last verified

| Check | Result |
| --- | --- |
| the R round on the rebuilt runtime (`jobs/verify`, pid 331) | Nine of nine tests PASS, 1509 PASS records, **no `signal:`**: the four new cases, `v0-stencil`, `c8-resolve`, `m3-texture` and `m2-solid` twice (`Klog_Logs/r-verify-runtime.log`). Title digest `b33b813c…` |
| the six requests' probes | R6's guard proved the overflow on the console before the fix and is silent after; R1's four frames read back 132 / 65 / 67 / 0 drawn pixels (no cull, back, front, discard); R2's probe samples a four-group texture across u 0 -> 4 and reads each group back; R3's stencil plane reads 65536 zero bytes at both clear depths (`Klog_Logs/r2-final.log`, `r1-cull-run4.log`, `r3-stencil-clear-{before,after}.log`) |
| the linked runtime | rebuilt: the Sep 16 archive predated the fork (stamp tree `a92a1228…`, script `cf4765ca…`); the rebuild changes the stubs and moves the title digest. All 60 objects differ, three of them by source |
| the console fault, on the committed build (`jobs/aco-min`, pid 299) | Seven of seven tests PASS, each `v0-formats-sampled-uint` "7 of 7 sampled formats fetched the colour their texel holds", 1581 PASS records, **no `signal:` record** (`Klog_Logs/aco-min-final.log`). The sampler's own case and `m2-solid`/`v0-formats` regress |
| the compile-stage fix, on the console (pid 294) | `c7-clear`, `v0-formats-sampled-uint`, `c5-depth`, `m2-solid` PASS, 528 PASS records, no signal (`Klog_Logs/verify-compile-stage.log`): the meta-clear path whose host tests faulted |
| the goldens and the replay model | `tools/check-driver.sh` PASS again on the rebuilt build: 288 run comparisons identical, no `DIFFERENT` record, no fault (`build/check-driver-final.log`); the first clean run closed 148 failing comparisons and 8 host-test segfaults. `tools/check-runner-cases.sh` PASS (7 cases, 204 PASS records) |
| the host gates | `make lint` (194 files), `make test` (30 tests), the three audits (`--check`, counts unchanged), `check-sdk-fork-migration.sh`, `check-mip-layout.sh`, `check-psbc-link.sh`, `check-vulkan-runtime.sh`, `check-runner-cases.sh`, `check-driver.sh` (`build/gates-runtime.log`) |

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
