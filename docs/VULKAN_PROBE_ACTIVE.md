# Active work

Volatile by design. Rewrite this file in place; keep it under about 120 lines.
Specifications belong in `docs/VULKAN_PROBE_PLAN.md`, and run results belong in
the phase logs (`docs/M5_PHASE_A.md`, `docs/M5_PHASE_B.md`,
`docs/M5_PHASE_C.md`).

_Updated: 2026-09-20_

## Now

**The objective's four items are answered and the follow-up work is the SDK fork
migration.** Rounds 11 to 17 closed the format audit and explained the ACO
fault:

- **The audit has no gaps (round 17).** 179 formats are required, **58 reported,
  0 missing a required feature**, 55 conditional, 0 `must:` clauses unmet, split
  **0 / 0 / 0 / 0 / 0**, so `python3 tools/format_audit.py --check` exits 0. The
  descriptor types (rounds 4-9) and vertex formats (rounds 1-3) are closed, the
  depth/stencil clause R1's report disclosed is satisfied by the stencil path
  (rounds 11-12), and `B8G8R8A8_SRGB` is proved through fetch, blits, transfers
  and its attachment pair (rounds 13-14). The packed
  `A8B8G8R8_SRGB_PACK32`, round 16's measured hardware limit, closed the way its
  twin's mechanism allows (round 17, evidence below).
- **The ACO fault is a genuine compiler bug with a named fix**: one case alone
  reproduces it (SIGFPE with a zero divisor, `jobs/aco-min`, pid 211), the pinned
  compiler's `update_vgpr_sgpr_demand` can leave `program->num_waves = 0` and
  `get_addr_regs_from_waves` divides by it, and ps5-opengl 0.3.0's patch guards
  exactly that point (round 15).
- **The migration's work list is measured** (round 16):
  `tools/check-sdk-fork-migration.sh` assembles the fork's compiler from the
  SDK's pins, verifies it is the SDK's own tree (`a27cbecc`), and finds one moved
  anchor (`patch-fragment-inputs.py`), one dropped patch
  (`patch-compute-metadata.py`, obsolete at metadata v14), two carrying over
  unchanged and the version 8 to 14 bump that rebuilds every package and re-runs
  every battery. That migration is the follow-up; nothing in the objective waits
  on it.

**Round 17's evidence.** All six of the packed sRGB row's features are proved
(pid 235, `Klog_Logs/v0-srgb-packed-run1.log`: seven of seven cases, 0 FAIL). The
mechanism is `ps5vk_format.storage_reversed`: the console's curve reaches the
first three *fetched* components and the format's Vulkan layout puts red last, so
the driver stores texels in the R, G, B, A order its `R8G8B8A8_SRGB` twin has and
swaps the bytes at every application boundary -- uploads, readbacks, both blit
directions. docs/HARDWARE_FINDINGS.md records the fetch order that forces it.

**What is left in the objective.** Nothing: every clause has implementation, a
host gate, a console probe, an audit move and committed evidence. The follow-up
is the SDK fork migration, on its own terms and with its own re-proof cost.

**Rules this workstream keeps.** One mechanism a round; the encoding comes from a
public codebase or the register database before anything is written; the compiler
changes as a patch script (never an SDK edit); a claim lands only with a console
run, a focused host gate and the audit move in the same commit; a mechanism that
fails its battery reverts its claim and is quoted against what failed; every
battery regresses `v0-formats` (the audit mirror) and `m2-solid`.

## Rung 1.0's remainder, after the audit's correction

Every feature a per-row `{sym1}` cell requires is proved by a console run and
reported by the driver; the rows left are the hardware's and one footnote clause's:

| | |
| --- | --- |
| command audit | 137 required: 90 driver, 0 refused, 47 runtime, 0 gap |
| limits audit | 106 members, 97 compared, 0 missing |
| format audit | 179 required, 58 reported, 0 missing a required feature, 55 conditional, 0 `must:` clauses unmet |
| the split | 0 / 0 / 0 / 0 / 0 |
| the blockers left | closed: the descriptor and vertex enums, the depth/stencil clause and the sRGB rows. The ACO fault is fixed in the 0.3.0 fork; the migration is the follow-up |

## Last verified

| Check | Result |
| --- | --- |
| blocker round 16: the SDK fork migration's work list (`build/sdk-fork`) | The SDK's compiler patch applies to its pinned base (`a92a1228`) and the assembled tree's hash is the SDK's own `patched_tree` (`a27cbecc`); `patch-vertex-formats.py` and `patch-descriptor-types.py` hold against it, `patch-fragment-inputs.py` needs its anchor moved and `patch-compute-metadata.py` is obsolete at `PSBC_SHADER_METADATA_VERSION 14u`. No repository input moved |
| blocker round 15: the ACO fault reduced and explained (pid 211; `jobs/aco-min`) | `v0-formats-sampled-uint` alone reproduces it: every row passes, the device idles, then SIGFPE, "integer divide fault", `rax = rcx = rdx = 0`. The pinned compiler's `update_vgpr_sgpr_demand` can leave `program->num_waves = 0` and `get_addr_regs_from_waves` divides by it; the fork's patch guards that point and the pinned tree has no such guard |
| blocker round 14: `B8G8R8A8_SRGB`'s attachment pair (pid 195; `jobs/v0-srgb-target`) | `v0-targets` holds the row's solid frame `0xff89bce1` and its additively blended frame `0xffbcbce1`, `v0-formats` 57 of 57, `m2-solid`; 3 of 3 tests, no FAIL. The audit's missing list falls to one row |

## Open findings

- **An unsupported image used to assert** (2026-09-20): `vkCreateImage` aborted
  the title on a format/usage combination the format's entry does not claim, and
  the abort's stale backtrace was read as an ACO fault in rounds 4, 13 and 16.
  It refuses by name now, so the next such case shows up in the log.
- **One compiler fault stands, and it is the console build's** (2026-09-20):
  the compile-order fault reproduces -- a SIGFPE after the unsigned texture
  case's last row with another case queued (pid 171), a SIGSEGV in the same
  case's setup alone (pid 181) -- while the host compiles the same 105 shaders in
  one process cleanly (`tools/check-aco-state.sh`), so it is the console
  compiler build's rather than driver state. It blocks no row: the case's frames
  all complete before it, which is why the case keeps the last place in its
  battery. The depth-descriptor shape is measured clean (round 21) and so is the
  signed shader's second compile (round 18). Next: the 0.3.0 fork's compiler.
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
