# PS5Vulkan

**Vulkan for the PlayStation 5: Mesa's RADV on a PS5 winsys, on its way to a
conformant Vulkan 1.4 device; ps5vk, the first driver; and the hardware probe
behind both.**

[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Tooling](https://github.com/mihawk-99/PS5_Vulkan/actions/workflows/tooling.yml/badge.svg)](https://github.com/mihawk-99/PS5_Vulkan/actions/workflows/tooling.yml)

This repository builds Vulkan drivers for the PS5's GPU, together with the probe
harness that proves, one capability at a time on real hardware, what that GPU
actually does. The driver going forward is **RADV**, Mesa's Vulkan driver for AMD
GPUs, built from my Mesa fork with a PS5 winsys that allocates, submits and
presents through the console's exported AGC, kernel and VideoOut functions
([The RADV port](#the-radv-port)). **ps5vk**, the first driver, is a Mesa-derived
Vulkan 1.1 implementation that programs AGC and VideoOut itself. vkQuake and
PS5 RetroArch have moved to RADV; ps5vk stays their `PS5_VULKAN_DRIVER=ps5vk`
build option. It is a
companion to the [PS5 OpenGL SDK](https://github.com/blackbearreloaded/ps5-opengl)
(release 0.3.0, adapted into `.deps/native/opengl-sdk` by
[`tools/adapt-opengl-sdk.sh`](tools/adapt-opengl-sdk.sh)) and is developed with
the public [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)
toolchain.

> RADV reports Vulkan **1.4** and runs the Khronos CTS on the console; its
> `conformanceVersion` stays 0.0.0.0 until the pinned 1.4 CTS passes in full
> there, and every case it does not pass yet, and every "not supported" the port
> itself causes, is listed with its owner in [docs/CTS_GAPS.md](docs/CTS_GAPS.md).
> ps5vk reports Vulkan 1.1, documented by command, limit and format audits and
> targeted console probes rather than by the CTS.

**Contents:**
[The RADV port](#the-radv-port) ·
[Progress and roadmap](#progress-and-roadmap) ·
[vkQuake and performance](#vkquake-and-performance) ·
[What this is](#what-this-is--and-what-it-is-not) ·
[How it works](#how-it-works) ·
[Findings](#what-has-been-established) ·
[Getting started](#getting-started) ·
[Gates](#5-the-gates) ·
[Credits](#built-with-and-on) ·
[License](#license-credits-and-trademarks)

---

## The RADV port

Route B of [docs/VULKAN_1_4_PLAN.md](docs/VULKAN_1_4_PLAN.md): RADV stays whole,
and only what the console does differently from Linux is new. The changes live in
my Mesa fork ([PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), checked out
as `../PS5_Mesa`, branch `main`: Mesa 26.2.0 with `-Dradv-winsys=ps5`), pinned
here by revision in
[`tools/build-radv.sh`](tools/build-radv.sh); gaps in the console's platform
(kernel declarations, libc, direct memory) go into the payload SDK fork's shared
platform layer, not into the Mesa fork. [docs/RADV_PHASE.md](docs/RADV_PHASE.md)
is the round-by-round record.

```text
  application (vkQuake, RetroArch, the CTS, the smoke test)
        |
  RADV + ACO + NIR ............ Mesa 26.2.0, unchanged but where the console
        |                       differs (a GC 10.1.3 fixed-function block
        |                       beside GFX10.3 shaders, below)
  PS5 winsys .................. memory from direct memory, command streams
        |                       copied into the ring as sceAgcDriverSubmitDcb
        |                       submissions, each ending with a write of its
        |                       sequence number, which the CPU waits for
  VK_KHR_display on VideoOut .. the swapchain: buffers registered with
        |                       VideoOut, flipped at vblank, 120 Hz where the
        |                       title declares it and the display takes it
  PS5 GPU
```

What stands on the console (2026-09-28):

- **The full CTS runs on the console.** The first full run of the pinned
  1.4 CTS (vulkan-cts-1.4.6.2, my fork `../PS5_VK-GL-CTS`, title PPSA99015)
  ended with 2,786,062 cases: 1,098,388 pass, 12,005 did not pass and 1,675,626
  were not supported. Since then the failures have been fixed or traced
  (10,560 of them belong to features switched off during that run: variable-rate
  shading and fragment barycentrics, which the hardware lacks, and capture and
  replay addresses, since implemented), and the port gained compute and transfer queue
  families (on the graphics ring), host-cached memory types, tessellation and
  geometry shaders run as compute where the fixed-function block cannot,
  ray queries and pipelines, sparse resources, calibrated timestamps and the
  VideoOut swapchain, each proved by a targeted CTS run of the groups behind it
  ([docs/CTS_GAPS.md](docs/CTS_GAPS.md)). With every item there closed, the
  second full run, full-1, started on 2026-09-28 on RADV `ecf916d`: 560,000
  of its 2,919,757 cases have run, and none has failed (228,553 pass, 8
  quality warnings, the rest not supported).
- **vkQuake ships on RADV.** Since 2026-09-28 the vkQuake title builds against
  the release archive by default and runs its demo loop at 119.88 fps, one
  vblank a frame, in the display's 120 Hz mode (PS5_vkQuake
  `evidence/radv-r2-main`). Since the same day RADV keeps its compiled
  pipelines in an on-disk cache in the title's folder, and since its
  exclusive mode (below) the first present comes 3.77 s after start from an
  empty cache and 2.44 s with it filled, of which the pipelines take 0.01 s.
  The rest, device and swapchain start-up against ps5vk's 0.55–0.77 s to a
  first frame, is still open.
- **Mesh and task shaders** are reported since 2026-09-28, though the GPU has
  neither per-primitive parameters nor the CP's task and mesh dispatch
  packets: mesh workgroups go out in parts that share one run's outputs, and
  task shaders run on the graphics ring in chunks (docs/RADV_PHASE.md).
- **RetroArch ships on RADV.** [PS5 RetroArch](https://github.com/mihawk-99/PS5_RetroArch)
  v0.5.0-alpha.5 (2026-09-28) is its first release on RADV: the menu, the
  software cores, PPSSPP (God of War: Ghost of Sparta), Dolphin (Wind Waker)
  and LRPS2 (GTA San Andreas) render, at full speed once a game has booted,
  and the PPSSPP and LRPS2 pictures match ps5vk's. PPSSPP needed a fix of its
  own: it used Vulkan 1.2 commands on a 1.1 instance. RetroArch's release
  battery passes on RADV as it did on ps5vk: every core with a game, closing
  and reloading content through the Quick Menu, threaded video, and a
  ten-minute PPSSPP soak.
- **The shader cache** is Mesa's cache database (`MESA_DISK_CACHE_DATABASE`)
  in the title's `radv-shader-cache/` folder, keyed by the pinned Mesa revision and open to
  the console's FTP service. On the console it runs in an exclusive mode,
  where each part stays open for the process instead of being reopened and
  locked on every read (docs/RADV_PHASE.md). Shader compiles on several
  threads no longer wait on one heap lock: the payload SDK fork's platform
  heap gives each thread an arena of its own.
- **Not there yet:** concurrency between queues (in the backlog), and
  Dolphin's first start of a game with an empty shader cache, which compiles
  its ubershaders and loses 17% and 8% of its first two 10 s windows on RADV
  (ps5vk: 14% and 1%).

Building it:

```bash
tools/setup-native-dependencies.sh   # the payload SDK fork at its pin
tools/build-radv.sh                  # .deps/native/radv: assertions on, for the smoke test and CTS
tools/build-radv.sh release          # .deps/native/radv-release: what titles ship
tools/build-radv-title.sh            # the smoke test title, PPSA99014
tools/build-cts-title.sh             # the CTS title, PPSA99015 (tools/run-cts.py runs it)
```

Titles link the archive with [`tools/radv-link.sh`](tools/radv-link.sh);
`RADV_ARCHIVE` names another one, such as the fork's own build while a change
to it is being worked on. Mesa's assertions and NIR validation make shader
compiles 5 to 6 times slower (docs/HARDWARE_FINDINGS.md), which is why titles
link the release build.

The rest of this README is mostly ps5vk's story: the probes, the findings both
drivers rely on, and the vkQuake and RetroArch work done on it.

## Progress and roadmap

### ✅ Proven on hardware

- ✅ **The GPU can be programmed at all.** The linked AGC canary loads, links
  shaders compiled from this repository's GLSL, and draws an exact 4K frame
  (M1–M2).
- ✅ **Resources reach the shader.** Uniform buffers, vertex and index buffers,
  sampled textures, arrays, cubemaps and texel uploads are all read back from a
  console frame (M3, D1).
- ✅ **Fixed-function state is understood.** Clears (colour and depth), the depth
  test and write, additive and constant-factor blending, render-to-texture, 4x
  MSAA and its resolve (M4, C8).
- ✅ **SPIR-V compiles on the console.** The AGC shader package built on the
  console is byte-identical to the package built on the PC (M5 A).
- ✅ **A Vulkan 1.0 driver runs the tutorial ladder** — command buffers, render
  passes, pipelines, descriptors, fences, semaphores, swapchain and display, one
  driver step per Vulkan Tutorial chapter (M5 B, M5 C).
- ✅ **A real application works.** RetroArch runs through the driver; a fragment
  input location defect found through it is fixed and confirmed on screen, with a
  30-second watch recording zero refusals
  ([evidence](evidence/fragment-inputs/),
  [findings](docs/HARDWARE_FINDINGS.md)).
- ✅ **vkQuake runs at up to 120 FPS at 4K** — on ps5vk, and since 2026-09-28
  on RADV, which it now ships with ([The RADV port](#the-radv-port)). On ps5vk,
  walking the start map, every frame
  takes 8.29–8.40 ms (119.88 FPS) on a 4K120 VRR display with kstuff paused;
  4K readbacks verify textured worlds, warps, particles, menu alpha and the HUD.
  Systematic gameplay acceptance continues. See
  [vkQuake and performance](#vkquake-and-performance).
- ✅ **Input attachments read the complete frame correctly.** R20 corrected the
  probe's tiled-memory reader; the earlier quarter-width diagnosis was a probe
  error, not a remaining descriptor-width defect ([R20 proof](jobs/r20-subpass/)).
- ✅ **A shader the compiler cannot lower is refused, not fatal.** The console's
  own failure was a title gone minutes into start-up: the fork's SPIR-V front end
  or its ACO printed one line and raised. The driver now reads a module's
  addressing model and declared capabilities before the compiler runs and refuses
  the ones it has no path for by name, and a `SIGABRT`/`SIGTRAP` guard behind
  that turns any other raise into a `VkResult` — proven by
  [`driver/tests/vk_v0_capability_test.c`](driver/tests/vk_v0_capability_test.c)
  on the host and by the round's console run, which shows the sentence where a
  title used to die (both recorded in
  [`docs/M5_PHASE_C.md`](docs/M5_PHASE_C.md)).
- ✅ **Push constants and specialization constants reach a shader.** Both are core
  Vulkan 1.0 and both arrived through the compiler fork rather than around it. A
  push-constant block is uploaded per draw, and the two halves of the target hold
  the two values (`v0-push-constant`); one module built with different
  specialization values draws the colour each set selects, read back word for
  word, with a hand-written literal twin as the compiler's own control (the
  `v0-r9` case).
- ✅ **The advertised set is audited, command by command.** All 137 required
  Vulkan 1.0 commands are accounted for: **90 in the driver, 47 in Mesa's
  runtime, 0 refused, 0 gap**, and six extensions are exposed, each after a
  console proof.
- ✅ **The format audit has no gaps.** All 179 formats in Vulkan 1.0's mandatory
  tables are answered: **58 reported, 0 missing a required feature**, 55 with
  conditional requirements only, and `python3 tools/format_audit.py --check`
  exits 0. Getting there meant proving every descriptor type the rows needed
  (uniform texel buffers, storage texel buffers, storage images and their
  atomics -- 74 features across 38 rows), every vertex layout they name (29 rows,
  8-bit and 16-bit components and the byte-reversed 8888 forms), the stencil path
  (`D24_UNORM_S8_UINT`, `D32_SFLOAT_S8_UINT`), and both byte-reversed sRGB
  formats ([`docs/V0_FORMATS_AUDIT.md`](docs/V0_FORMATS_AUDIT.md),
  [`docs/BLOCKERS.md`](docs/BLOCKERS.md)).
- ✅ **The console's SIGFPE was the runner's own divisor, and it is fixed.** The
  fault (`jobs/aco-min/queue.txt`) was a division by zero in the sampled-format
  loop: a table declared ten rows and initialized seven, so the eighth row's
  texel size was zero. The addresses that had been read as ACO frames were
  console `rip`s left unrebased by the eboot's `0x400000` load address, which
  the crash report's own `xotext:` line gives. The tables now declare their
  rows, `static_assert`s make a declared-but-unfilled row a build error, and
  the console run is `jobs/aco-min` pid 287: seven of seven tests, each uint case
  "7 of 7", no `signal:` record
  ([`docs/HARDWARE_FINDINGS.md`](docs/HARDWARE_FINDINGS.md)).
- ✅ **Offline oracles exist.** The console's tile maps are derived from AMD's
  own AddrLib in the console's exact configuration and checked against the
  console's measured rows (`measured match`), so a new map is computed rather
  than guessed ([`tools/mip-layout-oracle.cpp`](tools/mip-layout-oracle.cpp)).
- ✅ **The loop is reproducible.** A console run is captured from klog, reduced
  to golden command streams, and replayed on the PC against the same driver code
  ([`tools/golden.py`](tools/golden.py)).

### ❌ Not yet

- ❌ **No conformance yet.** The console CTS runs on RADV; its second full
  run (full-1) is in progress, and what the first left open is closed in
  [docs/CTS_GAPS.md](docs/CTS_GAPS.md). ps5vk's host results are in
  [docs/CTS.md](docs/CTS.md).
- ❌ **Rungs 1.1 → 1.4 on ps5vk.** Superseded: the Vulkan 1.4 device is RADV
  ([The RADV port](#the-radv-port)).
- ✅ **The SDK fork's compiler is migrated.** The driver links ps5-opengl
  0.3.0's own `opengnm-psbc` tree (metadata version 14, this repository's
  patches re-applied on top), assembled and verified against the release's own
  `patched_tree` by
  [`tools/check-sdk-fork-migration.sh`](tools/check-sdk-fork-migration.sh)
  ([`docs/M5_PHASE_C.md`](docs/M5_PHASE_C.md)).
- ❌ **Transfer coverage is not universal.** Padded pitches, tiled mip chains,
  filtered mip generation and the copy/readback cases needed by vkQuake now have
  console proofs. Unproven format, aspect and multisample combinations still
  require their own cases; supported paths are bounded by the measured layouts.
  See [R29 regression coverage](jobs/r29-tile-address/).
- ❌ **Line-list rendering does not draw right.** The topology is linked,
  compiled and programmed (DI_PT_LINELIST, the line's own registers), yet the
  console case's segments do not match the rectangles they must cover: a core
  1.0 topology is not yet correct on hardware, which is a defect in the drawing
  rather than in what the device reports (the `v0-lines` case).
- ❌ **Buffer device address is not advertised.** Shaders that need it —
  physical storage buffer addresses, `buffer_reference`, and the bindless image
  store that comes with them — are refused by name rather than compiled into a
  fault. The vkQuake port uses compatible paths instead of requiring these
  kernels to compile.
- ✅ **A hardware-rendered emulator works.** PPSSPP, as a RetroArch core, runs
  God of War: Ghost of Sparta and Yu-Gi-Oh! GX Tag Force at 10× internal
  resolution (4800×2720) with 16× anisotropy, at full speed on a 120 Hz display.
  What it needed is general driver work, not PPSSPP cases: every draw restates
  the context registers that persist between draws (target mask, blend, colour
  control, clip and rasteriser mode); a device-local memory type beside the
  host-visible one; a three-image FIFO swapchain whose present does not wait for
  the flip; GPU blits and copies through Mesa's `vk_meta` with push descriptors;
  component mapping composed onto the view's swizzle; and CPU image copies that
  evict and fence once per copy rather than per row. Loading a save state went
  from 21.5 s to 61 ms.
- ❌ **A title cannot load a graphics module at run time.** Every `dlopen` and
  `sceKernelLoadStartModule` of a repository-built `.so` is refused by the
  console, so the driver is delivered *linked* into the title; the untried route
  is publishing application exports from the module writer.
- ❌ **Occlusion queries are coarse.** One `ZPASS_DONE` count is 16 samples, so
  `occlusionQueryPrecise` is reported false.
- ❌ **Broad application acceptance is incomplete.** RetroArch, PPSSPP and vkQuake run;
  vkQuake still needs systematic movement/fire/save/load, all-map and long-soak
  acceptance. Other applications remain separate compatibility work.

### The ladder

| Stage | What it means | Status |
| --- | --- | --- |
| M1–M2 | AGC loads; shaders from this repo draw a 4K frame | ✅ |
| M3 | Uniform, vertex/index and sampled-texture resources | ✅ |
| M4 | Clear, depth, blend, render-to-texture | ✅ |
| M5 A | SPIR-V → AGC package, console and PC byte-identical | ✅ |
| M5 B–C | The Vulkan driver and the tutorial ladder | ✅ |
| M5 D | Breadth: dynamic state, compute, more formats | ✅ |
| **Rung 1.0 audits** | Entry-point, limit and format accounting; targeted console probes | ✅ audits: commands 90/47/0/0, limits 0 missing, formats 0 missing; semantic limitations remain |
| M5 E | The SDK fork's compiler (metadata 14) migrated and re-proven | ✅ migrated and re-proven |
| — | The console SIGFPE at `jobs/aco-min` | ✅ fixed: the runner's sampled-format table ran a row it never filled in |
| Rung 1.1–1.4 | ps5vk: one commit a rung, each gated by a CTS subset | superseded by RADV |
| RADV | Mesa's RADV on a PS5 winsys, Vulkan 1.4, the full CTS on the console | 🔄 second full CTS run in progress; vkQuake and RetroArch ship on it |
| Phase E1 | CTS-style semantic validation against the advertised set | ❌ recipe written |
| Real applications | RetroArch ✅ (ps5vk, and RADV since v0.5.0-alpha.5) · PPSSPP, Dolphin and LRPS2 (hardware-rendered) ✅ tested games · vkQuake at up to 120 FPS at 4K, on RADV since 2026-09-28, acceptance 🔄 · other frontends ❌ | 🔄 in progress |

## vkQuake and performance

The native PS5 vkQuake port in the sibling `../PS5_vkQuake` checkout links this
repository's RADV release archive since 2026-09-28 (ps5vk's archives with
`PS5_VULKAN_DRIVER=ps5vk`); its README covers installation, controls and the
console settings below. This section is the ps5vk work that got it to 120 FPS;
the exact tested ps5vk driver and what is pending are in the
[active state](docs/VULKAN_PROBE_ACTIVE.md).

The compatibility rounds cover vertex stride, dynamic UBO offsets, descriptor
arrays, padded pitches and mip tails, 32-bit indices, depth state leaking into
colour-only UI, sampler LOD bias and menu blend control. Swapchain readback
provides complete screenshots as evidence, rather than treating a successful
present call as proof of a correct image.

### Where the time went, and what changed

Measured on the console at 3840×2160 (port evidence, steady windows):

| Round | Change | Effect |
| --- | --- | --- |
| [R29](jobs/r29-tile-address/) | Common tile-address evaluation | Start map 14.8 → 19.7 FPS |
| [R33](jobs/r33-begin-split/)–R36 | A nearly free profile (TSC timestamps, one write) | Found that a system call costs ~20 µs with kstuff active, and that the profile's own write was the "unexplained" multi-second stall |
| [R37](jobs/r37-mapped-flush/) | Flush colour targets only in memory the application maps | 256–384 MiB and 3.9–5.8 ms of cache flush a frame gone; full runner battery identical to before |
| R38 | Bounded spin on the completion marker before sleeping | Poll 1.12 → 0.15 ms a step |
| [R42](jobs/r42-parallel-blit/) | Blits resampled on five threads | Water-warp mips 2.3–8.2 → 0.4–1.2 ms; walking the start map 34–47 → 52–55 FPS |
| [R43](jobs/r43-hitch-recorder/) | A per-frame hitch report | Located the port's New Game stutter (fixed in the port) |
| [R46](jobs/r46-nir-cache/), [R47](jobs/r47-shipped-cache/) | Internal NIR cache; one cache directory per build, shippable | A launch compiles nothing |

With the console's kstuff paused at game launch (an etaHEN setting), a system
call costs 0.73 µs instead of ~20 µs, and the same walk runs at the display's
120 Hz ceiling: **119.88 FPS, 4.0–4.4 ms of work a frame**, application 2.4 ms,
queue 2.1 ms. On a VRR display a frame presents as soon as it is ready, from 48
to 120 Hz; a frame over the 48 Hz window (20.8 ms) is held to ~29.2 ms.

### Shader cache

Entries live in `/app0/ps5vk-shader-cache/<first 16 hex digits of the driver
build>/`, one directory per build, since keys include the build. Directories are
0777 so the console's FTP service can read entries back and write shipped ones
in; `PS5VK_SHADER_CACHE_DIR`, or on the console `/app0/ps5vk-shader-cache-dir.txt`,
names another base for tests. Since R46 the driver's internal NIR stages are
cached too, and the port ships each build's compiled set, so a launch compiles
nothing ([R47](jobs/r47-shipped-cache/)).

### What is still open on ps5vk

RADV offers both of these; they are ps5vk's own gaps.

- **MSAA beyond 4×, with render pass 2.** PPSSPP's MSAA needs
  `VK_KHR_create_renderpass2` and depth/stencil resolve, and 8 samples; neither
  is offered yet.
- **Multiple colour attachments** are still refused (`v0-mrt`), and that case
  stops the test runner.

## What this is — and what it is not

**It is** a probe that answers "what does this console's GPU actually do" with
console runs instead of assumptions, and two drivers built on those answers:
RADV with a PS5 winsys, and ps5vk, a Mesa-derived frontend with the NIR/ACO
shader compiler and a PS5-specific AGC and VideoOut backend.

**It is not** a Sony SDK, a retail-package builder, an exploit, or a conformance
submission. It ships no Sony file, no key and no game content. It needs a
homebrew-enabled console that you own, and it never configures that console for
you. ps5vk reports 1.1 (R84; the 1.1 row of docs/M5_REFERENCE.md stays
open until the console CTS runs), with coverage recorded command by command,
limit by limit and format by format. The audits, targeted pixel proofs and working
applications are evidence of progress, not a claim of certified conformance.

## How it works

### The stack (ps5vk)

RADV's is under [The RADV port](#the-radv-port).

```text
  application (vkQuake, RetroArch, Vulkan Tutorial, probe runner)
        |
  Vulkan 1.0 frontend ......... Mesa's common runtime (vk_* entry points) +
        |                       this repository's driver (driver/ps5vk_*.c)
  shader translation .......... SPIR-V -> NIR -> ACO -> AGC shader package
        |                       (opengnm-psbc, built for the console)
  resource + command backend .. measured tile maps, register tables,
        |                       PM4 command streams, direct memory
  AGC and VideoOut ............ the console's own GPU command and display APIs
        |
  PS5 GPU
```

Nothing Linux-specific crosses over: no `amdgpu`, no DRM, no ioctls. ps5vk
programs AGC directly, and the PS5-specific parts —
tile maps, descriptor words, register values, synchronization — exist in this
repository because a console run measured them.

### Three ways to run the same code

| Mode | What it is for |
| --- | --- |
| **Console** (`PPSA99988`) | The proof. Every claim in this repository comes from a run here. |
| **PC replay** (`build/host/runner_host`) | Runs the runner's own code on the host against golden captures, so a regression shows up without a console. |
| **PC replay through the driver** (`build/host/runner_host_driver`) | The same runner with the Vulkan driver linked, for a case that has no console capture yet (`tools/build-host-runner.sh --driver`). |
| **Offline oracle** (AddrLib, Vulkan-Docs) | Computes expected tile maps and version requirements, so a probe is written against a specification instead of a guess. |

### The evidence loop

1. A queue of probe cases is uploaded and the runner title is launched
   (`tools/ps5_console.py battery`).
2. The run writes structured records to klog; the driver dumps its register
   tables, shader stages and command streams.
3. The capture becomes a golden file (`tools/golden.py extract`) and the PC
   replays it, comparing every word.
4. The result lands in a phase log and, when it changes a rule, in
   [`docs/HARDWARE_FINDINGS.md`](docs/HARDWARE_FINDINGS.md).

### What ps5vk reported at rung 1.0

| Property | Value |
| --- | --- |
| Instance API version | 1.3 (Mesa's instance level; promises nothing about the device) |
| Device API version | 1.0 |
| Extensions | `VK_KHR_surface`, `VK_KHR_display`, `VK_KHR_swapchain`, `VK_KHR_get_physical_device_properties2`, `VK_EXT_debug_report`, `VK_EXT_debug_utils` |
| Queue families | 1 |
| Features | `robustBufferAccess`, `samplerAnisotropy` (16×) |
| Limits | 97 of 106 required limits compared against the specification, 0 missing |
| Formats | 179 required: 58 reported, 0 missing a required feature, 55 conditional only |
| Commands | 137 required 1.0 commands: 90 driver, 47 runtime, 0 refused, 0 gap |

## What has been established

Probing paid for itself: this GPU does not behave the way a PC GPU does, and
each of these rules came from a console run that contradicted a reasonable
assumption. The evidence for every one is in
[`docs/HARDWARE_FINDINGS.md`](docs/HARDWARE_FINDINGS.md).

- Colour render targets are **tiled**, not row-major: 128×128 texels a 64 KiB
  tile with an XOR pixel map, so CPU access must convert the layout.
- A four-sample colour target is four 0x4000-byte sample planes a tile, while a
  four-sample **depth** target keeps all four samples in one 16-byte texel — a
  different map for the same idea.
- Element size changes the tiling: a two-byte element has its own 256×128-texel
  row, derived from AddrLib rather than scaled from the four-byte one. (A
  two-byte image can never be an attachment, so that row has no reachable path —
  recorded rather than pretended.)
- Blending needs more than `CB_BLEND0_CONTROL`: the pixel shader must export the
  format the pipeline's SPI state names, and a blending 8-bit target takes
  FP16_ABGR.
- Indirect register tables are dereferenced by the GPU and must live in
  GPU-mapped direct memory; a table on the stack faults.
- A PM4 `INDIRECT_BUFFER` into title memory faults the GPU, so the driver copies
  command buffers into one stream instead.
- A submission can complete without a flip, which is what makes marker-based
  fences safe for readback.
- The sRGB curve belongs to the **fetch**, not to a format's channels: the texture
  unit linearises the first three fetched components before any selector, so a
  byte-reversed sRGB texel is decoded correctly only if the image holds its
  channels in the order the curve reads. The driver chooses that order for
  `A8B8G8R8_SRGB_PACK32` and swaps the bytes where an application's bytes meet
  the image's.
- A combined depth/stencil attachment's stencil plane is its own **one-byte**
  surface, in the depth surface's 64 KiB `Z_X` swizzle at a 64 KiB-aligned offset
  beside it, and `DB_STENCIL_INFO` carries that swizzle with the tile-stencil
  bit — which is what makes a `D24_UNORM_S8_UINT` or `D32_SFLOAT_S8_UINT`
  attachment work.
- The shader compiler of the SDK fork is RADV's front end over a **different
  ACO**, and the difference is where a title dies: upstream's call site lowers
  `subpassLoad` to the tile coordinate intrinsic this ACO has no case for, so the
  *descriptor* form of the same pass is what compiles here, and an application's
  input attachment is bound from its subpass rather than from a descriptor write
  (which Vulkan forbids for that type).
- A stage's descriptor metadata is the caller's declaration **as the compiler
  echoes it back**, not what the shader reads: a layout binding the stage never
  fetches still arrives in its metadata, so the table the driver builds has to be
  narrowed to the bindings the module itself declares — otherwise the draw
  demands an application write for a descriptor no instruction uses.
- A raise inside the compiler is a dead title, because `vkCreate*Pipelines` has
  no result to return from inside it. Turning `SIGABRT`/`SIGTRAP` into a refusal on
  the thread that compiles is what lets a title survive a shader it cannot
  compile, and it is the backstop behind every named capability refusal.

## Repository layout

| Path | What lives there |
| --- | --- |
| [`driver/`](driver/) | The Vulkan driver: instance, device, images and tile maps, buffers, descriptors, pipelines, draws, queue and submission, WSI, debug capture |
| [`src/`](src/) | The probe application: the case table, the AGC canaries, and the diagnostics that produce the structured records |
| [`host/`](host/) | Host-side shims for the PC replay (the AGC host model and the runner host) |
| [`probes/`](probes/), [`shaders/`](shaders/) | GLSL sources and their compiled AGC shader packages, one set per probe canary |
| [`jobs/`](jobs/) | Job queues and results — one directory per probe battery |
| [`golden/`](golden/) | Golden command streams extracted from console runs |
| [`evidence/`](evidence/) | Captured evidence for app-level findings |
| [`docs/`](docs/) | The plan, the phase logs, the audit tables and the hardware findings |
| [`tools/`](tools/) | Build, check, oracle, decode and console tooling |
| [`tooling/`](tooling/) | The PS5 linker converter, FSELF writer and runtime-shim builder |
| [`payload/`](payload/) | `ps5vkctl`, the small console agent that launches and kills probe runs |
| [`runtime/`](runtime/) | The generated clean-room `libc.prx` used by directory titles |
| [`vendor/`](vendor/) | Link-time import stubs for the console's own libraries (AGC and the canaries' imports), so a title links without the Sony SDK |
| [`tests/`](tests/) | Host unit and integration tests, including the queue and audit guards |

Five titles build from this tree with `make` and `tools/build-all-titles.sh`.
Four are canaries or diagnostics; one is the runner that produces the
evidence. The RADV scripts build two more:

| Title | What it is |
| --- | --- |
| `PPSA99988` | **The probe runner**: the Vulkan driver, the test runner and every probe package |
| `PPSA99999` | The default diagnostics app (the graphical Hello World) |
| `PPSA99997` | The AGC driver canary |
| `PPSA99998` | The linked canary with the complete-state canary |
| `PPSA99996` | The live-submission canary |
| `PPSA99014` | RADV's smoke test (`tools/build-radv-title.sh`) |
| `PPSA99015` | The Khronos CTS on RADV (`tools/build-cts-title.sh`, run by `tools/run-cts.py`) |

## Getting started

### 1. Host

Arch/CachyOS is the supported host; `make doctor` is the authority. The short
list:

```bash
sudo pacman -S --needed base-devel clang llvm lld make python git curl wget \
  unzip tar pkgconf cmake glslang
python3 -m pip install --user mako markupsafe   # Mesa's Vulkan generators
make doctor
```

`glslang` compiles the probe shaders, `cmake` builds the pinned BC7 encoder for
presentation art, and `ccache` is picked up automatically when installed.

Two SDKs are involved, and neither is vendored into the tree:

```bash
# 1. The public payload SDK (headers, sysroot, prospero-clang18/lld) plus the
#    host test framework: make deps fetches the pinned releases into .deps/.
make deps

# 2. The PS5 OpenGL SDK, release 0.3.0: a release bundle or a checkout.
#    tools/adapt-opengl-sdk.sh reconstructs the layout the tools read under
#    .deps/native/opengl-sdk -- the shader-compiler fork the driver links, its
#    headers, the C package writer and the Mesa version pin -- and pins the
#    compiler half so a rebuild is reproducible. PS5_OPENGL_SDK overrides the
#    tree every script reads (tools/sdk-root.sh).
tools/adapt-opengl-sdk.sh ../ps5-opengl-sdk-0.3.0
```

The adapted tree is a *frozen* compiler: `tools/adapt-opengl-sdk.sh` copies the
Mesa-fork work copy `tools/build-psbc-ps5.sh` last built, so moving to the
0.3.0 fork's compiler is a deliberate migration with its own re-proof cost
(§ Not yet, `tools/check-sdk-fork-migration.sh`) rather than a side effect of
updating the SDK.

### 2. Console

You need a console you own with an already configured homebrew environment. The
environment this repository was validated against uses
[etaHEN](https://github.com/etaHEN/etaHEN) as the enabler,
[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) for directory
titles under `/data/homebrew`,
[ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) on port `2121` for
deployment, and
[klogsrv](https://github.com/ps5-payload-dev/klogsrv) on port `3232` for
capture. The repository does not configure any of it: follow those projects'
own instructions, keep the services on a trusted local network, and load this
repository's agent once per boot:

```bash
tools/build-ps5vkctl.sh                        # needs PS5_PAYLOAD_SDK or ~/ps5-payload-sdk
python3 tools/ps5_console.py deploy-payload    # /data/homebrew + /data/etaHEN/payloads
python3 tools/ps5_console.py payload           # -> ok ps5vkctl 1 pid=<n>
```

### 3. Build

RADV's build is under [The RADV port](#the-radv-port). ps5vk, the probe
shaders and the titles:

```bash
tools/fetch-mesa.sh            # pinned Mesa 26.2.0, checksum-verified, into .deps/
tools/build-psbc-ps5.sh        # the SPIR-V -> AGC shader compiler for the console
tools/build-vulkan-runtime.sh # Mesa runtime: host and PS5 archives
tools/build-driver.sh         # the driver: host and PS5 archives
tools/build-probe-shaders.sh   # GLSL -> SPIR-V -> AGC packages (needs glslang)
tools/build-all-titles.sh      # all five titles, warning-free, aligned segments
```

`tools/build-all-titles.sh` is also the strictest compiler gate here: it fails on
any warning or any misaligned `PT_LOAD` segment. ccache makes the Mesa-derived
builds cheap, and `PS5VK_DISABLE_CCACHE=1` opts out. Measured build times are in
[`docs/NATIVE_TOOLING.md`](docs/NATIVE_TOOLING.md).

### 4. Run a probe battery

```bash
# Build and deploy the runner title (these definitions are what make it the runner)
PS5_HOST=192.168.1.100 PARAM_PATH=sce_sys/param-runner.json \
APP_DEFINITIONS="AGC_LINKED_CANARY=1 AGC_TEST_RUNNER=1 AGC_OUTPUT_4K=1 AGC_LIVE_SUBMIT_ARMED=1 AGC_SHADER_COMPILER=1 AGC_VULKAN_DRIVER=1" \
make deploy

# Upload a queue, arm the capture, restart the title, and summarise the run
python3 tools/ps5_console.py battery PPSA99988 jobs/format-items/queue.txt \
    --output Klog_Logs/format-items.log
```

A queue is a small text file — `capture`, `hold <vblanks>`, the case names,
`m2-solid` as the canary, `exit` — and [`jobs/`](jobs/) holds examples, one per
battery, each with the run it belongs to in its comments. The
battery exits `0` when the run ended and passed, `3` when the run never ended,
and `2` when no run arrived. To watch a run you launch yourself, use
`python3 tools/ps5_console.py klog`; to decode what a run recorded, use
`python3 tools/pm4_decode.py stream <log>`, which turns every captured stream
into named AMD PM4 packets.

### 5. The gates

Everything below must be green before a change lands. This is the project's
definition of "verified":

```bash
tools/check-driver.sh          # driver: loader, host, PS5 link and negative arms
tools/check-runner-cases.sh    # the runner's cases on the PC, against golden streams
tools/check-mip-layout.sh      # tile maps against AddrLib, the oracle
tools/check-probe-packages.sh  # every committed probe package rebuilds to itself
tools/check-psbc-link.sh       # the shader-compiler link, the SDK tree it comes from
tools/check-vulkan-runtime.sh  # the runtime link
make test                      # host unit and integration tests, including the audits
make lint                      # format, static analysis, attribution, shell, assets
tools/build-all-titles.sh      # every title, no warning, aligned segments
python3 tools/command_audit.py --check   # 137 required commands, 0 gap
python3 tools/format_audit.py  --check   # 179 required formats, 0 missing (exit 0)
python3 tools/limits_audit.py  --check   # 106 required limits, 0 missing
```

Two reports are deliberately not gates, because their answer is "work remains":
`tools/check-sdk-fork-migration.sh` (the compiler migration's work list) and
`python3 tools/format_audit.py` without `--check` (the conditional rows).

## Application identity, packaging and releases

The application half of this repository is a small, self-contained native app
foundation, and the same commands apply to the probe titles:

- `make init TITLE_ID=PPSA12345 APP_NAME="My App"` coordinates identity in
  [`sce_sys/param.json`](sce_sys/param.json); the checked-in `PPSA99999` is a
  development identity, so keep it local.
- `make` builds `dist/<TITLE_ID>/`; `make ffpkg` and `make ffpfsc` add the
  optional UFS2 and compressed images; `make deploy PS5_HOST=...` updates a
  running console over FTP, uploading each file under a temporary name and
  publishing `eboot.bin` last.
- `make inspect INSPECT_FILE=dist/<TITLE_ID>/eboot.bin` statically validates the
  ELF or FSELF, and `make undeploy PS5_HOST=...` removes only this title's
  staged files.
- Releases are tagged with the exact `contentVersion` from `param.json`
  (`NN.NNN.NNN`), and `make ci` reproduces the GitHub Actions job on your host
  first.

Details: [configuration](docs/CONFIGURATION.md),
[deployment](docs/DEPLOYMENT.md), [output formats](docs/FFPKG.md),
[presentation assets](docs/PRESENTATION_ASSETS.md).

## Documentation map

| Document | Read it for |
| --- | --- |
| [`docs/VULKAN_PROBE_PLAN.md`](docs/VULKAN_PROBE_PLAN.md) | The top-level plan: gates, milestone map, invariants |
| [`docs/VULKAN_PROBE_ACTIVE.md`](docs/VULKAN_PROBE_ACTIVE.md) | Volatile state: current step, next actions, last verified runs |
| [`docs/PROBE_MILESTONES.md`](docs/PROBE_MILESTONES.md) | The M1–M4 canaries, the runner and the console tooling |
| [`docs/M5_REFERENCE.md`](docs/M5_REFERENCE.md) | The driver phases, the version ladder and the workflow |
| [`docs/HARDWARE_FINDINGS.md`](docs/HARDWARE_FINDINGS.md) | What console runs established, with build, run id and commit |
| [`docs/M5_PHASE_A.md`](docs/M5_PHASE_A.md) · [`_B`](docs/M5_PHASE_B.md) · [`_C`](docs/M5_PHASE_C.md) | Append-only run logs |
| [`docs/CTS.md`](docs/CTS.md) | The conformance-subset recipe that gates a version rise |
| [`docs/V0_FORMATS_AUDIT.md`](docs/V0_FORMATS_AUDIT.md) | Required format support, row by row -- and the record that none is missing |
| [`docs/BLOCKERS.md`](docs/BLOCKERS.md) | The mechanism log: each blocker, the round that closed it, and what it cost |
| [`docs/REQUESTS_RESPONSE.md`](docs/REQUESTS_RESPONSE.md) | The game port's capability requests (R1…R10) and this side's answers, measurement by measurement |
| [`docs/NATIVE_TOOLING.md`](docs/NATIVE_TOOLING.md) · [`docs/TESTING.md`](docs/TESTING.md) · [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md) | Build, test and console workflows |
| [`AGENTS.md`](AGENTS.md) | The repository's own working rules: read order, volatility contract, caching rules |

## Built with and on

This project stands on other people's work. Everything below is fetched, pinned
and checksum-verified, or used from a sibling checkout; nothing is vendored into
the tree.

| Project | Author / org | What it provides here | How it is used |
| --- | --- | --- | --- |
| [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) | BlackBearReloaded | The sibling PS5 OpenGL SDK: the pinned `opengnm-psbc` shader-compiler tree (Mesa NIR + ACO), the C shader-package writer, register knowledge, and the Mesa version pin | Release 0.3.0, adapted into `.deps/native/opengl-sdk` by `tools/adapt-opengl-sdk.sh` (`PS5_OPENGL_SDK` overrides) |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | BlackBearReloaded | This repository's foundation: the PS5 ELF converter, the FSELF writer, the clean-room `libc.prx`, and the identity and packaging tooling | The base of this repository (GPL-3.0-or-later) |
| [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) | ps5-payload-dev (John Törnblom) | Public PS5 headers and sysroot, the `prospero-clang18`/`lld` target toolchain, and libc++ headers | Fetched, pinned to v0.42 by SHA-256 |
| [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) | Mihawk-99, on ps5-payload-dev's SDK | My fork of the payload SDK with the shared platform layer (heap, direct and executable memory, libc gaps) RADV's winsys and the titles build on | Built by `tools/setup-native-dependencies.sh` at a pinned revision |
| [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) | Mihawk-99, on Mesa | My Mesa fork: RADV, ACO and NIR with the PS5 winsys and VideoOut WSI (swapchains up to the mode's size, scaled by VideoOut) | Exported at a pinned revision by `tools/build-radv.sh` from `../PS5_Mesa` |
| [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) | ps5-payload-dev | Optional prebuilt PS5 ports (SDL2, OpenSSL, …) for applications | Optional, pinned to v0.40.2 |
| [opengnm-psbc](https://github.com/PS4-OpenGNM/opengnm-psbc) | PS4-OpenGNM | The Mesa-derived shader compiler (NIR + ACO) the driver links for SPIR-V, and the tree the 0.3.0 SDK patches | Fetched by the SDK's patch over its pinned revision; this project's compiler patches are re-applied on top (`tooling/psbc/`) |
| [ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) | mpereiraesaa | A second native PS5 Vulkan implementation: its per-format console evidence and its reporting inventory are cross-checks for this project's audit | Reference; read, not fetched or linked |
| [Mesa 3D](https://gitlab.freedesktop.org/mesa/mesa) | Mesa contributors (freedesktop.org), with AMD's AddrLib inside | The Vulkan common runtime and utilities the driver builds on, and AddrLib, which the tiling oracle compiles | Fetched, pinned to 26.2.0, checksum-verified |
| [LLVM / Clang](https://github.com/llvm/llvm-project) | The LLVM project | The host compiler, and the target compiler the payload SDK packages | Host packages plus the SDK's toolchain |
| PS5 system modules (AGC, VideoOut) | Sony Interactive Entertainment | The console's real GPU command, register and display interfaces | Used on the console through the SDK's published import stubs; never redistributed |
| [AMD GPU documentation](https://llvm.org/docs/AMDGPUUsage.html) · [BC-250 documentation](https://elektricm.github.io/amd-bc250-docs/hardware/specifications/) | AMD; elektricm | Instruction definitions, register fields, and an external reference for the PS5-derived BC-250 board | Reference |
| [Vulkan-Docs](https://github.com/KhronosGroup/Vulkan-Docs) · [Vulkan-CTS](https://github.com/KhronosGroup/Vulkan-CTS) | Khronos Group | The version requirement tables the ladder is built from (pinned v1.4.354), and the conformance suite RADV is gated on | Pinned docs; the CTS is vulkan-cts-1.4.6.2 in my fork `../PS5_VK-GL-CTS`, with its PS5 platform, built by `tools/build-cts-title.sh` |
| [GoogleTest](https://github.com/google/googletest) | Google | The host-only unit-test framework | Fetched, pinned to 1.17.0; never linked into console output |
| [zlib](https://zlib.net/) | Jean-loup Gailly and Mark Adler | Compression for the host FSELF tool | Fetched, pinned to 1.3.2 |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) | Richard Geldreich | The BC7 encoder behind presentation-image conversion | Optional, pinned revision `b9438627` |
| [UFS2Tool](https://github.com/SvenGDK/UFS2Tool) | SvenGDK | Optional `.ffpkg` (UFS2 image) output | Optional, pinned commit, built with .NET |
| [MkPFS](https://github.com/PSBrew/MkPFS) | PSBrew | Optional compressed `.ffpfsc` output | Optional, pinned commit |
| [SharpProspero](https://github.com/SvenGDK/SharpProspero) | SvenGDK | A public PS5 format reference used during early research | Reference only; not fetched, linked or required |
| [etaHEN](https://github.com/etaHEN/etaHEN) | The etaHEN project | The homebrew enabler on the validation console | Console-side; the repository only talks to it |
| [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | drakmor | Mounts directory-style titles from `/data/homebrew` | Console-side |
| [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) · [klogsrv](https://github.com/ps5-payload-dev/klogsrv) | ps5-payload-dev (John Törnblom) | The FTP service `make deploy` uploads to, and the klog stream (port 3232) every run is captured from | Console-side |
| [RetroArch / libretro](https://github.com/libretro/RetroArch) | The libretro project | The real application this driver is exercised with, and the source of a real compiler defect it helped find | Consumer |
| [ccache](https://ccache.dev/) | The ccache project | Optional build acceleration for the Mesa-derived builds | Optional host tool |

Exact pins, digests and license notes are in [`NOTICE.md`](NOTICE.md).

## Contributing

Contributions are welcome. The most useful ones are probes that turn an unproven
claim into a console result, and fixes that make an advertised capability true.

Before opening a change:

1. Add the case or the fix with a focused regression — a behaviour change
   without a probe or a test is not reviewable here.
2. Run the gates in [§5](#5-the-gates) and paste the results.
3. For any platform-specific claim, state the firmware, the loader and the
   console run id you saw it on. "It works on my console" is not evidence
   without the log.
4. Keep every file's copyright and `GPL-3.0-or-later` SPDX header. The holder
   is the file's own -- `Mihawk-99` for this project's files,
   `BlackBearReloaded` for the ones inherited from the boilerplate, both for the
   ones derived from it; [`NOTICE.md`](NOTICE.md) records the split and
   `make lint` accepts either.
5. Never commit keys, Sony files, game data, console dumps or credentials.

The repository's own working rules — the read order, the volatility contract
that keeps the plan static and the active file volatile, and the
caching-friendly editing rules — are in [`AGENTS.md`](AGENTS.md). They are
written for AI agents working in this tree, and they are also a good
description of how the project keeps its documentation honest.

## License, credits and trademarks

Repository-authored code is Copyright (C) 2026 Mihawk-99 and
**GPL-3.0-or-later** ([LICENSE](LICENSE)); the files inherited from the
ps5-native-app-boilerplate are Copyright (C) 2026 BlackBearReloaded under the
same licence. Fetched dependencies keep their upstream licenses and are never
redistributed from this repository; the details are in
[NOTICE.md](NOTICE.md).

This project is independent and is not affiliated with or endorsed by Sony
Interactive Entertainment. "PlayStation" and "PS5" are trademarks of Sony
Interactive Entertainment.

Built on the work of the [ps5-payload-dev](https://github.com/ps5-payload-dev)
community, [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), and
the [Mesa](https://gitlab.freedesktop.org/mesa/mesa) and
[LLVM](https://github.com/llvm/llvm-project) projects. Thank you.
