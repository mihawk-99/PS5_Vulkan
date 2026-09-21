# Active work

Volatile by design. Rewrite this file in place; keep it under about 120 lines.
Specifications belong in `docs/VULKAN_PROBE_PLAN.md`, and run results belong in
the phase logs (`docs/M5_PHASE_A.md`, `docs/M5_PHASE_B.md`,
`docs/M5_PHASE_C.md`).

_Updated: 2026-09-20_

## Now

**The SDK fork migration is landed and the console fault is closed.** Two
workstreams finished in this session and are one commit:

- **The compiler migration.** The driver links ps5-opengl 0.3.0's own
  `opengnm-psbc` tree (metadata 14, this repository's patches re-applied), and
  the work copy is the fork's own tree, which
  `tools/check-sdk-fork-migration.sh` assembles and verifies against the
  release's `patched_tree` (`a27cbecc`).
- **The SIGFPE was the runner's zero divisor, not ACO.** `jobs/aco-min` dies at
  the sampled-format loop's `div` because `sampled_unsigned_formats()` declared
  ten rows and initialized seven: the eighth row's texel size is 0 and the loop
  divides the packed buffer's length by it. The "ACO frames" every earlier round
  read were console addresses symbolized without the eboot's `0x400000` load
  base. Both sampled tables now declare their rows and carry a
  `static_assert` that rejects a declared-but-unfilled one.

**What is left in the objective.** Nothing from the mission: the migration is
committed, the fault is fixed and proved on the console, and every gate is
green. Rung 1.0's requirements were already met; the next gate is the CTS
subset (`docs/CTS.md`).

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
| the console fault, on the committed build (`jobs/aco-min`, pid 299) | Seven of seven tests PASS, each `v0-formats-sampled-uint` "7 of 7 sampled formats fetched the colour their texel holds", 1581 PASS records, **no `signal:` record** (`Klog_Logs/aco-min-final.log`). The sampler's own case and `m2-solid`/`v0-formats` regress |
| the compile-stage fix, on the console (pid 294) | `c7-clear`, `v0-formats-sampled-uint`, `c5-depth`, `m2-solid` PASS, 528 PASS records, no signal (`Klog_Logs/verify-compile-stage.log`): the meta-clear path whose host tests faulted |
| the goldens and the replay model | `tools/check-driver.sh` PASS: the whole loader/direct/PS5-link table, no `DIFFERENT` comparison and no fault, from 148 failing comparisons and 8 host-test segfaults. `tools/check-runner-cases.sh` PASS (7 cases, 204 PASS records) |
| the host gates | `make lint` (194 files), `make test` (30 tests), the three audits (`--check`, counts unchanged), `check-sdk-fork-migration.sh`, `check-mip-layout.sh`, `check-psbc-link.sh`, `check-vulkan-runtime.sh`, `check-runner-cases.sh`, `check-driver.sh` |

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
