# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-23_

## Now
**vkQuake stability and performance are the priority; no CTS work.**
The user confirms the deployed R29 game works, estimates 15–30 FPS and requests
README updates plus performance advice. Detailed acceptance remains pending.
Heartbeat PAUSED; do not launch automated tests or close their game. Services
returned; R29 game e3525e30… is deployed with two matching ELF reads/all five
segments. The user has now launched it; port evidence/manual-r29-deployment proves deployment.

Persistent SPIR-V outputs survive launches and crashes. Same-binary cold/warm
PIDs 202/203: first present 30.410/13.018 s, 99/0 compiles, 433/532 hits.
Eight internal NIR stages still compile per launch. Exact evidence, cache rules
and reproduction: jobs/shader-cache. M2 was human-confirmed earlier.

R17-R29 are closed and each keeps its own proof in jobs/r*-*: descriptor arrays
and padded mips (R17/R18 3be25f1/aafd697), UINT32 indices (R19 8d11392), the
retired quarter-width diagnosis (R20 d8080dc), default-off timing (R21 5925f03),
deduplicated target flushes (R22 a85010a, PID 224, 1,043 PASS, 14 replays; game
flush 7.872 -> 5.788 ms with no useful FPS gain), swapchain TRANSFER_SRC (R23
4f8037f, PID 229, four entire-frame copies), depth state leaking into colour-only
passes (R25, PID 244), sampler LOD bias (R26, PID 250), the blend-control
assignment (R27, PID 258), copy/wait/signal timing (R28, PID 261), and the common
tile-address evaluation (R29 c3e51f6, PID 267/268, 3,501 regressions). Every
landed round has an explicit build, the full host/cache suite, eleven gates, port
gates/scan and template relinks. Port native input opens DualSense and audio feeds
48 kHz stereo with no reported errors over repeated 300-second runs; physical and
audible acceptance is still pending. Port PNG allocation and native shell exit are
fixed, so screenshots give real console readback. R29's rolled-back integer filter
(PID 265, copy 22.657 vs 24.095 ms, no FPS gain) stays in jobs/r29-tile-address.

The earlier 09:24 UTC network outage ended before the verified manual deployment.
Automated R29 benchmarking and movement/fire/save/load/all-map acceptance remain
paused during manual testing. Port build/r29b-* and build/r30-* hold the prepared
artifacts; preserve existing saves/configs when automated work resumes.

R31 adds default-off instrumentation for the four things the R28 means cannot
separate: the application's own CPU time before and after a submission, the
submission call itself apart from the polling that observes it, the software
polling (unsuccessful marker checks per step, and whether the first check
already saw the marker), and the presentation wait (flip-status calls, vblank
waits with their durations, present-to-present period histogram, present marker
slots). It also measures the display's real cadence with a run of bare
sceVideoOutWaitVblank calls when /app0/ps5vk-vblank-probe.txt or
PS5VK_VBLANK_PROBE opts in, and prints residual_ms, the sum of the four parts
against the measured present period, so an incomplete attribution names itself
instead of being averaged over. No packet, wait, flip or copy changes; with
profiling off the golden replays are unchanged. Host build is clean, eleven
gates and check-driver/shader-cache pass. Evidence: jobs/r31-frame-profile.

The console is unreachable again (2026-09-23 12:36 UTC, no route to host on
2121/3232/9111), so the R31 run and the R29 game baseline behind it are both
pending. The 15-30 FPS estimate is still not an instrumented R29 benchmark, and
R29's address change is still unmeasured against R28's game numbers.

Watch this while reading R28/R29 as a work budget rather than an FPS: at E1M1
the four R28 categories leave about 19.5 ms a frame the driver never sees, and
the start map about 19.9 ms, almost identical across two very different scenes.
If the period is pinned to a whole number of 60 Hz vblanks - 33.8 ms is two and
67.5 ms is four - then no partial saving can raise FPS at all until the whole
frame fits in one 16.667 ms interval, which is exactly what R22's 2 ms flush
saving and R29's 1.4 ms copy saving both observed. R31's period histogram is
what decides between that and a genuinely work-bound frame.

During the earlier outage, a NIR-cache candidate reuses Mesa serialization and
the existing output cache. Host fresh-process cold/warm/disabled outputs match;
warm compilation count zero. Eight captured mip submissions replay exactly.
Full host/cache checks and eleven gates pass. Production source is restored;
parked/nir-shader-cache holds the patch, checks and remaining console acceptance.
Keep it separate from the saved R29 game binary.
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
- CTS remains outside this user-requested game work; prior status is in docs/CTS.md.
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
- Entry-point accounting is complete (90 driver, 0 refused, 0 gap), but that
  does not prove every parameter combination. R16–R29 now cover the game's
  tiled mip/readback and filtered-copy paths. Other shapes/formats/aspects still
  need focused proofs; do not treat historical refusal lists as current coverage.
- The AGC compiler has no bounds-checking option and refuses uniform blocks
  larger than 16 bytes, which is why robustness is the driver's index-count clamp
  rather than a shader-side check.
