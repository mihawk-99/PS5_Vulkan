# Active work

Volatile by design. Keep this file under about 120 lines. Specifications are in
`docs/VULKAN_PROBE_PLAN.md`; measurements are in `docs/M5_PHASE_C.md` and
`docs/HARDWARE_FINDINGS.md`.

_Updated: 2026-09-23_

## Now
**vkQuake stability and performance are the priority; no CTS work.**
The user has returned and requested the latest build for manual testing.
Heartbeat PAUSED; do not launch automated tests or close their game. Services
returned; R29 game e3525e30… is deployed with two matching ELF reads/all five
segments. Manual launch is left to the user; port evidence/manual-r29-deployment.

Persistent SPIR-V outputs survive launches and crashes. Same-binary cold/warm
PIDs 202/203: first present 30.410/13.018 s, 99/0 compiles, 433/532 hits.
Eight internal NIR stages still compile per launch. Exact evidence, cache rules
and reproduction: jobs/shader-cache. M2 was human-confirmed earlier.

R17/R18 descriptor arrays and padded mips pass (3be25f1/aafd697). R19 UINT32
indices pass (8d11392): PID 216, 257 checks, full pixels and five strict replays.
Game PID 217 then runs 180 seconds without refusal or Quake error. R20 d8080dc
retires the old quarter-width diagnosis: its probe misread tiled bytes; corrected
writer/reader match every pixel. Detailed evidence lives in each jobs/r*-*.

Port native input opens DualSense; audio feeds 48 kHz stereo with no reported
errors during repeated 300-second runs. Physical/audible acceptance is pending.
R21 5925f03 adds default-off timing. R22 a85010a deduplicates identical target
flushes: PID 224, 1,043 PASS, 14 exact replays. Game flush cost 7.872 -> 5.788 ms,
without useful FPS gain. R23 4f8037f proves swapchain TRANSFER_SRC: PID 229,
238 PASS, four entire-frame copies, 16 strict replays. Port PNG allocation and
native shell exit are fixed; screenshots now provide actual console readback.
All landed driver rounds have explicit builds, full host/cache and eleven gates,
plus port gates/scan and template relinks. Per-round jobs retain exact proofs.
R25 fixes depth/stencil state leaking into colour-only passes. PID 243 reproduces
518,400 wrong overlay pixels; PID 244 has 439 PASS/zero FAIL for D32/D16 detach,
original depth and swapchain readback. PID 245 stencil/bias regressions pass
(including the intentional nonzero-clamp refusal). 24 new streams replay exactly.
170 host/cache, eleven gates, port/template pass. Archive e172ce0f…. Evidence:
jobs/r25-depth-detach. Port PID 246 now shows all tested HUD styles correctly.
R26 fixes sampler LOD bias: game scaling requested bias 1 and previously exited.
PID 250: 383 PASS/zero FAIL; eight complete frames match every pixel for signed
and fractional bias, plus original mip tests. Twenty streams replay exactly.
Full host/cache, eleven gates, port/template pass. Archive b83fc4ac….
Evidence: jobs/r26-lod-bias. Port PID 251 scaling works; no FPS gain.
R27 restores the blend-control assignment removed by cfab0b9. PID 256 reproduces
full-frame alpha/constant-blend errors; PID 258 passes 285 checks, four exact
4K alpha frames and constant blend. Five strict replays; 170 host/cache, eleven
gates, port/template pass. Evidence: jobs/r27-menu-alpha. Port PID 259 verifies
correct menu/HUD blending in five PNGs and normal exit.
R28 splits CPU-copy/wait/signal timing. Game PID 261: start copy 24.095 ms,
E1M1 copy 0.041 ms; both sync operations 0.021 ms each. Full gates/relinks and
five profiler-enabled strict replays pass. jobs/r28-copy-profile records it.
R29 common tile-address evaluation passes: PID 267 has 178 PASS, four complete
4K mip frames and every generated mip texel correct; four strict replays.
PID 268 has 3,501 PASS across mip/upload/copy/format regressions. Host checks
compare 2,441,216 addresses against the old map and 144 random-colour blits.
Full driver/cache, eleven gates, port/template pass. jobs/r29-tile-address.
The earlier integer filter was rolled back: PID 265 copy 22.657 vs 24.095 ms,
no FPS gain. Its patch and partial/full probe runs remain in the R29 job.

**Console unavailable since about 09:24 UTC.** Deployment of game candidate
identity e3525e30… failed before FTP connected. FTP/control/klog return no route;
local route exists, neighbor failed, PS5 discovery returned no response. Last
probe PID 268 completed and closed normally. No new game fixture was uploaded.
Next: deploy/read back the already-built R29 game, stage its saved benchmark,
scan shaders, launch with listener; then movement/fire/save/load/all eight maps.
Port build/r29b-* and build/r30-* hold the prepared artifacts. Do not overwrite
existing saves/configs. The last game PID 265 exited normally; fixtures absent.

While hardware is offline, a NIR-cache candidate reuses Mesa serialization and
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
- No command refuses by name any more (90 driver, 0 refused, 0 gap). The paths
  that still refuse are the ones whose placement or decode no probe has recorded:
  a tiled copy or readback of a chain, a depth image read back to a buffer and an
  upload from one, a multi-sample copy/blit/resolve, a subset of array layers, a
  scaled blit whose format has no recorded decode, a filtered blit, and an image
  clear whose format or aspect is not the recorded one.
- The AGC compiler has no bounds-checking option and refuses uniform blocks
  larger than 16 bytes, which is why robustness is the driver's index-count clamp
  rather than a shader-side check.
