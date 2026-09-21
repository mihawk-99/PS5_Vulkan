# Active work

Volatile by design. Rewrite this file in place; keep it under about 120 lines.
Specifications belong in `docs/VULKAN_PROBE_PLAN.md`, and run results belong in
the phase logs (`docs/M5_PHASE_A.md`, `docs/M5_PHASE_B.md`,
`docs/M5_PHASE_C.md`).

_Updated: 2026-09-20_

## Now

**The second request batch (`PS5_VULKAN_REQUESTSv2.md`, R7-R9) is worked**, one commit
per item, and its evidence was weaker on purpose -- each item was a prediction read
from vkQuake's source, so each was re-checked at HEAD before anything was written:

- **R8 is closed**: `VK_DYNAMIC_STATE_DEPTH_BIAS` is in the whitelist and the bias is
  the command buffer's state, with the *enable* still the pipeline's (Vulkan 1.0's
  dynamic depth bias covers the three factors only). `v0-dynamic-depth-bias` proves it
  with one pipeline and two draws whose bias changes between them.
- **The clamp decision is taken and implemented**: a non-zero `depthBiasClamp` is
  refused by name in both the static and the dynamic form, because the hardware's
  register measured inert and a silent cap would report a wrong depth as a success.
  The driver no longer caps anything.
- **R9 is confirmed open, with the mechanism measured**: push constants reach the
  driver's own block (the debug API reads the second draw's bytes back) and never reach
  the shader, because the standalone compiler lowers an application's
  `layout(push_constant)` to a user-data location its metadata does not report. The
  port must keep W4; two fix routes are named in `docs/M5_PHASE_C.md`.
- **R7 is confirmed and the route is chosen**: the two-set refusal happens at the
  *draw*, not at creation (the prediction's one wrong detail), and the route favoured
  is (b) -- multi-set within the advertised four, vkQuake merging five layouts into
  four -- because Vulkan requires at least four sets, which makes today's one-set
  behaviour a conformance gap.
- **R4's coverage note is answered by correcting our own claim**: the runner *does*
  install a `VK_EXT_debug_utils` messenger and forwards every refusal to the log; the
  gap was that a frame expecting a refusal passed no report. Fixed, and one sweep now
  carries all three refusal sentences.

**The first batch (R1-R6) is answered too**, one commit each: R6 (a split
submission's step capture), R2 (the sampler's address modes), R1 (cull, discard and
depth bias), R3 (the stencil clear's own value), R5 (the resolve refusal's usage bit)
and R4 (the audits' blind spots), with `docs/REQUESTS_RESPONSE.md` as the hand-off.

Eight console-proved cases carry them -- `v0-two-passes`, `v0-sampler-address`,
`v0-cull`, `v0-stencil-clear`, `v0-depth-bias`, `v0-dynamic-depth-bias`, `v0-two-sets`
and `v0-resolve-usage` -- each with a
queue under `jobs/`, a golden under `golden/` and, for the depth bias, a focused host
gate (`driver/tests/vk_c5_depth_bias_test.c`).

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
| the second batch, one sweep (pid 380) | Ten of ten tests PASS on title digest `c9c57b40…`: R8's `v0-dynamic-depth-bias`, R7's `v0-two-sets`, R5's `v0-resolve-usage`, R1's `v0-depth-bias`, R6's `v0-two-passes`, R2's `v0-sampler-address`, R3's `v0-stencil-clear`, `v0-cull`, `c8-resolve` and `m2-solid`, with all three refusal sentences in the klog (`Klog_Logs/r-coverage.log`). `v0-push-constant` (R9) is red on purpose and is not in that sweep |
| R8's dynamic depth bias (pid 371) | Biasing the second draw kept 0 of 3 left samples and 3 of 3 right; biasing the first kept 3 and 0; biasing both left two different depths in one image (0x3efff000 and 0x3effe000). The clamp frame was refused by name (`Klog_Logs/r8-dynamic-bias2.log`, digest `4aec17c7…`) |
| R9's push constants (pid 377) | The driver's block held the second draw's bytes (0,0,1,1) with the descriptor and user data naming it, and both halves read back 0x00000000: the upload arrives, the stage's read does not (`Klog_Logs/r9-push-constant-final2.log`, digest `6306b4aa…`) |
| R7's two-set layout (pid 378) | The pipeline was created and its draw refused: "descriptor set 1 is beyond the 1 this driver binds; sets past 0 are D1" (`Klog_Logs/r7-two-sets.log`, digest `084a7c84…`) |
| R1's depth bias and R5's refusal, on the committed build (one sweep, pid 369) | Five of five tests PASS, no FAIL, no `signal:`: the depth-bias frames kept 2880 / 0 / 2880 / 2880 / 2880 / 0 of 2880 samples and 3 / 0 on the two ramps, with the biased depth in the plane (0x3effe000 and 0x3f001000) and the capped factor 0xc327c5ac in the recorded table, and the probe the ask described (`v0-resolve-usage`) was refused without the colour-attachment bit and submitted with it (`Klog_Logs/r-verify3.log`, `golden/v0-depth-bias`). Title digest `998037c4…`. The focused host gate is `c5_depth_bias`: 12 of 12 checks direct, 4 of 4 loader, PS5 link PASS |
| the R round on the rebuilt runtime (`jobs/verify`, pid 331) | Nine of nine tests PASS, 1509 PASS records, **no `signal:`**: the four new cases, `v0-stencil`, `c8-resolve`, `m3-texture` and `m2-solid` twice (`Klog_Logs/r-verify-runtime.log`). Title digest `b33b813c…` |
| the six requests' probes | R6's guard proved the overflow on the console before the fix and is silent after; R1's four frames read back 132 / 65 / 67 / 0 drawn pixels (no cull, back, front, discard); R2's probe samples a four-group texture across u 0 -> 4 and reads each group back; R3's stencil plane reads 65536 zero bytes at both clear depths (`Klog_Logs/r2-final.log`, `r1-cull-run4.log`, `r3-stencil-clear-{before,after}.log`) |
| the linked runtime | rebuilt: the Sep 16 archive predated the fork (stamp tree `a92a1228…`, script `cf4765ca…`); the rebuild changes the stubs and moves the title digest. All 60 objects differ, three of them by source |
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
