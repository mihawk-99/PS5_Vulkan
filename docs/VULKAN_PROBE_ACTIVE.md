# Active work

Volatile by design. Rewrite this file in place; keep it under about 120 lines.
Specifications belong in `docs/VULKAN_PROBE_PLAN.md`, and run results belong in
the phase logs (`docs/M5_PHASE_A.md`, `docs/M5_PHASE_B.md`,
`docs/M5_PHASE_C.md`).

_Updated: 2026-09-21_

## Now

**The second request batch (`PS5_VULKAN_REQUESTSv2.md`, R7-R9) is worked**, one commit
per item, and its evidence was weaker on purpose -- each item was a prediction read
from vkQuake's source, so each was re-checked at HEAD before anything was written:

- **R8, the clamp decision, R9 and R4's residual are closed**: dynamic depth bias from
  the command buffer's state, a by-name refusal of a non-zero `depthBiasClamp`, push
  constants declared through the pointer form and written by the draw, and a colour clear
  read back with nothing over it. Details and run digests: `docs/M5_PHASE_C.md`.
- **R7 is confirmed, the route is chosen, and Round 1 of four is done**: the two-set
  refusal happens at the *draw*, and route (b) -- multi-set within the advertised four --
  is chosen. Round 1 removed the compiler wrapper's single-set assumption (one layout per
  set, per-set tables sized per set, a pointer per set in the metadata, a total slot
  budget), with `probes/v0-multiset` (two sets that differ in kind) and the host test
  `psbc_multiset` proving it: set 0's pointer at user-data dword 2, set 1's at 3, and both
  new bounds refusing. **Round 2** is the driver's per-set tables, per-set
  `vkCmdBindDescriptorSets` and its "more than the four advertised" refusal; **Round 3**
  the console case that shows a value arriving from set 1 and a command buffer that
  submits; **Round 4** the fallout. The round also corrected a stale artifact: the shipped
  `probes/v0-push` package had been written by a probe CLI built *before* the R9 compiler
  fix (R9's own conclusion is unaffected).
- **R4's coverage note is answered by correcting our own claim**: the runner *does*
  install a `VK_EXT_debug_utils` messenger and forwards every refusal to the log; the
  gap was that a frame expecting a refusal passed no report. Fixed, and one sweep now
  carries all three refusal sentences.

**The first batch (R1-R6) is answered too**, one commit each: R6, R2, R1, R3, R5 and
R4, with `docs/REQUESTS_RESPONSE.md` as the hand-off.

Eight console-proved cases carry them -- `v0-two-passes`, `v0-sampler-address`,
`v0-cull`, `v0-stencil-clear`, `v0-depth-bias`, `v0-dynamic-depth-bias`, `v0-two-sets`
and `v0-resolve-usage` -- each with a
queue under `jobs/`, a golden under `golden/` and, for the depth bias, a focused host
gate (`driver/tests/vk_c5_depth_bias_test.c`).

**The linked runtime was four days older than the migration.** The stale
`libvk_runtime.ps5.a` was rebuilt, the title digest moved with it (`c3a99b98…` ->
`b33b813c…`), and the nine-case battery was re-run against the rebuilt artifact. The one
recorded stream that moved is `golden/v0-stencil`'s first submission (`docs/M5_PHASE_C.md`).

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

Command audit 137 required (90 driver, 0 refused, 47 runtime, 0 gap); limits audit 106
members, 97 compared, 0 missing; format audit 179 required, 58 reported, 55 conditional
and no `must:` clause unmet; the split 0 / 0 / 0 / 0 / 0; no blocker left from the
enum, depth/stencil, sRGB or console-fault families.

## Last verified

| Check | Result |
| --- | --- |
| R7 Round 1, the compiler (host, then the console) | One layout per set: `psbc_multiset` 9 of 9 checks direct ("set 0 at user-data dword 2, set 1 at 3, 4 user SGPRs"), loader and PS5 link PASS; every probe package byte-identical to its pre-patch build except `probes/v0-push`, which the rebuild *corrected* (its package predated the R9 fix); `tools/check-driver.sh` 288 comparisons identical, none `DIFFERENT`, 47 tests PASS; `build/gates.sh` 11 of 11 -- the new `check-probe-packages.sh` rebuilds all 41 buildable probe sets byte-identically and asserts its own coverage; the console regression 9 of 9 PASS (pid 114, digest `9adf1241…`, `Klog_Logs/r7-round1.log`), only the named clamp refusal in the FAIL records |
| the second batch, one sweep (pid 380) | Ten of ten tests PASS on title digest `c9c57b40…`: R8's `v0-dynamic-depth-bias`, R7's `v0-two-sets`, R5's `v0-resolve-usage`, R1's `v0-depth-bias`, R6's `v0-two-passes`, R2's `v0-sampler-address`, R3's `v0-stencil-clear`, `v0-cull`, `c8-resolve` and `m2-solid`, with all three refusal sentences in the klog (`Klog_Logs/r-coverage.log`). `v0-push-constant` (R9) is red on purpose and is not in that sweep |
| R7's two-set layout (pid 378) | The pipeline was created and its draw refused: "descriptor set 1 is beyond the 1 this driver binds; sets past 0 are D1" (`Klog_Logs/r7-two-sets.log`, digest `084a7c84…`) |
| the R round on the rebuilt runtime (pid 331, digest `b33b813c…`) | Nine of nine tests PASS, 1509 PASS records, no `signal:` |
| the six requests' probes | R6's guard, R1's four cull frames (132/65/67/0 drawn pixels), R2's four-group sampling, R3's 65536-byte stencil plane |
| the linked runtime | rebuilt: the Sep 16 archive predated the fork (stamp tree `a92a1228…`, script `cf4765ca…`); the rebuild changes the stubs and moves the title digest. All 60 objects differ, three of them by source |
| the goldens and the replay model | `check-driver.sh` PASS: 288 comparisons identical, no `DIFFERENT`, no fault; `check-runner-cases.sh` PASS |
| the host gates | `make lint` (199 files), `make test` (30 tests), the three audits (`--check`, counts unchanged), `check-sdk-fork-migration.sh`, `check-mip-layout.sh`, `check-psbc-link.sh`, `check-probe-packages.sh` (43 committed sets, 41 rebuilt byte-identically, 2 named as not rebuildable), `check-vulkan-runtime.sh`, `check-runner-cases.sh`, `check-driver.sh` |

**Phase E1 is open: the capability set is an artefact** (2026-09-21). The runner's
`device-report` case walks the device's own reporting and
`tools/collect-device-report.py` collects it into
`conformance_inventory/device_report.json`, completeness-checked against the Vulkan
headers and diffed by `tools/check-runner-cases.sh` so it cannot drift. It carries 97
limits, 55 features with **one** true (`robustBufferAccess`), one queue family, one
host-coherent memory type, `display`/`surface`/`swapchain` among the extensions, and the
format matrix: **184 of 184 core formats probed, 58 with any feature** (the format
audit's own count), 307 image-format combinations accepted, none linear and none 3D. Two
self-inconsistencies it exposed are the campaign's first work items:
`maxImageDimension3D` is 256 while no 3D combination exists, and four formats claim
cube-compatibility while two answer a cube query (`docs/M5_PHASE_C.md`). The pinned CTS
is a fetch now -- `tools/fetch-vk-gl-cts.sh`, 2.0 GB under `.deps` including its 988 MB
external tree, its revision verified and its `conformance_inventory/cts_pin.json`
separating the revision, the externals the CTS only *declares*, and the ones actually
checked out (all seven equal the declared pins) -- with `make lint` holding the record to
the pin in `docs/CTS.md`.

**The CTS runs against this driver** (2026-09-21). `tools/run-cts-host.sh` builds the
pinned `deqp-vk` (vulkan_headless, 1,841,090 cases) and runs a group against the host ICD
through the loader: `dEQP-VK.info.*` is 16 pass / **0 fail**, `dEQP-VK.api.info.*` is 2539
pass / **5 fail** / 1342 not supported, and all five failures are reporting -- two missing
`STORAGE_TEXEL_BUFFER_ATOMIC_BIT` claims, a missing `COLOR_ATTACHMENT_BIT` for R32_SFLOAT,
an illegal compressed-format feature combination, and `VK_KHR_surface`'s version word
(`conformance_inventory/cts_host_baseline.json`). Two are taken. The instance was claiming
Vulkan 1.3 while the driver implements 1.0: a regression test in the runner's
`device-report` case now fails if the two disagree, and the claim is gone (its CTS case
still fails; the driver's own answer is 1.0 and the loader's version is ruled out, so the
hunt for CTS's `getUsedApiVersion()` continues). And `R32_SFLOAT` gained the
colour-attachment bit the specification requires, with a new probe set and runner case
proved on the console (`v0-targets-float` PASS, digest `1ac8b831…`), then its two
texel-buffer bits with both cases' rows proved the same way (digest `c1f75ff6…`, 17 of 17
uniform and 8 of 8 storage). Its `VERTEX_BUFFER` bit followed in round 9, claimed
only once the driver's vertex-format table and the v0-vertex-formats case could fetch one
(digest `7e8ac5ea…`) -- which **closed that CTS case**: `dEQP-VK.api.info.*` is now 2540
passed / **4 failed**. The rest:
`STORAGE_TEXEL_BUFFER_ATOMIC_BIT` for R32_UINT/SINT, a compressed-format set (every BC,
ETC2 and ASTC format reports `0x0` today), CTS's `getUsedApiVersion()` source, and the
console payload (`docs/M5_PHASE_C.md`, rounds 6 to 8).

**An upstream AGC source was checked against this driver** (2026-09-21). The
static-recompilation project's published tile-equation table agrees, texel for texel, with
every tiled map this driver measured (`tools/check-tile-equations.py`, run by the
mip-layout gate), and its AGC shader-handle resource-slot table is the largest untaken
item -- it would turn R9's silent-zero class into a named refusal. Ranked list, with what
was corroborated and what does not transfer: `docs/AGC_UPSTREAM_NOTES.md`.

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
