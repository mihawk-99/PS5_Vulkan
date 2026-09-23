# PS5Vulkan

**A Vulkan 1.0 driver and a hardware compatibility probe for the PlayStation 5.**

[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Tooling](https://github.com/mihawk-99/PS5_Vulkan/actions/workflows/tooling.yml/badge.svg)](https://github.com/mihawk-99/PS5_Vulkan/actions/workflows/tooling.yml)

This repository builds a real Vulkan driver for the PS5's GPU — a Mesa-derived
Vulkan 1.0 implementation that programs the console's own AGC command backend
and VideoOut display — together with the probe harness that proves, one
capability at a time on real hardware, what that GPU actually does. It is a
companion to the [PS5 OpenGL SDK](https://github.com/blackbearreloaded/ps5-opengl)
(release 0.3.0, adapted into `.deps/native/opengl-sdk` by
[`tools/adapt-opengl-sdk.sh`](tools/adapt-opengl-sdk.sh)) and is developed with
the public [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)
toolchain.

> The physical device reports Vulkan **1.0**. Command, limit and format audits
> and targeted console probes document its coverage; they do not establish full
> Vulkan conformance. Known semantic limitations remain. Current work prioritizes
> stable, playable and faster vkQuake; console CTS is outside that work.

**Contents:**
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
  input location defect found through it is fixed and owner-confirmed, with a
  30-second watch recording zero refusals
  ([evidence](evidence/fragment-inputs/),
  [findings](docs/HARDWARE_FINDINGS.md)).
- ✅ **vkQuake runs on the console.** The owner confirmed the deployed R29 game
  works on September 23, 2026. Console readbacks already verify textured worlds,
  menu alpha and HUD rendering. The owner estimates 15–30 FPS; systematic
  gameplay acceptance and performance work continue. See
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

- ❌ **No console CTS acceptance or conformance certification.** Host results
  and the console payload's outstanding work are recorded in
  [docs/CTS.md](docs/CTS.md). Audits do not substitute for semantic conformance;
  CTS is deferred while game stability and performance are the priority.
- ❌ **Rungs 1.1 → 1.4.** The goal is a Vulkan 1.4 device; each rung is its own
  commit, gated by a CTS subset for that version.
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
- ❌ **A title cannot load a graphics module at run time.** Every `dlopen` and
  `sceKernelLoadStartModule` of a repository-built `.so` is refused by the
  console, so the driver is delivered *linked* into the title; the untried route
  is publishing application exports from the module writer.
- ❌ **Occlusion queries are coarse.** One `ZPASS_DONE` count is 16 samples, so
  `occlusionQueryPrecise` is reported false.
- ❌ **Broad application acceptance is incomplete.** RetroArch and vkQuake run;
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
| Rung 1.1–1.4 | One commit a rung, each gated by a CTS subset | ❌ |
| Phase E1 | CTS-style semantic validation against the advertised set | ❌ recipe written |
| Real applications | RetroArch ✅ · vkQuake runs, optimization/acceptance 🔄 · other frontends ❌ | 🔄 in progress |

## vkQuake and performance

The native PS5 vkQuake port in the sibling `../PS5_vkQuake` checkout links this
repository's static driver archives. Its README covers installation and controls. Its owner confirmed the deployed R29 game
works; the current 15–30 FPS figure is a manual estimate. Stable, faster gameplay
is the immediate goal. See [active state](docs/VULKAN_PROBE_ACTIVE.md) for the
exact tested driver and pending work.

The compatibility rounds now cover vertex stride, dynamic UBO offsets,
descriptor arrays, padded pitches/mip tails and 32-bit indices. Later rounds
fixed depth state leaking into colour-only UI, sampler LOD bias and menu blend
control. Swapchain readback provides complete screenshots as evidence, rather
than treating a successful present call as proof of a correct image.

| Evidence | What it establishes |
| --- | --- |
| [Persistent shader cache](jobs/shader-cache/) | Same-binary cold/warm first present 30.410/13.018 s, 99/0 SPIR-V compilations and 433/532 hits |
| [R25 depth detach](jobs/r25-depth-detach/) | Correct depth transitions and colour-only UI |
| [R26 LOD bias](jobs/r26-lod-bias/) | Signed/fractional sampler bias and mip sampling |
| [R27 menu alpha](jobs/r27-menu-alpha/) | Complete 4K alpha frames and constant blending |
| [R28 CPU-copy profile](jobs/r28-copy-profile/) | Separates CPU copies, cache flushes, submission and present costs |
| [R29 tile addressing](jobs/r29-tile-address/) | Direct evaluation of the existing common tile equation; 3,501 console mip/upload/copy/format checks pass |

Cache entries live in the application's `/app0/ps5vk-shader-cache` directory,
survive title restarts/crashes and are preserved by normal game deployments.
Changed shader/compiler inputs invalidate their keys. Eight internal NIR stages
still compile per launch. A [parked NIR-cache candidate](parked/nir-shader-cache/)
has host cold/warm equality and replay checks, but is **not enabled in the
production R29 driver**; console startup and gameplay validation remain pending.
Startup improvements must not be presented as steady-frame-rate improvements.

### Measured bottlenecks and the 4K120 target

Before the R29 address optimization, the [R28 baseline](jobs/r28-copy-profile/metrics.txt)
measured these per-frame means:

| Cost | Start map | E1M1 spawn |
| --- | ---: | ---: |
| CPU image copies | 24.095 ms | 0.041 ms |
| Cache flush | 5.954 ms | 3.974 ms |
| Queue total | 33.222 ms | 6.905 ms |
| Flip path | 14.415 ms | 7.431 ms |
| Flushed target bytes | 384.09 MiB | 256.06 MiB |

Queue time includes other categories, so the rows cannot all be summed. The
`gpu_ms` field includes native submission/completion waiting; it is not isolated
GPU execution time. The start-map copy cost points toward water mip generation,
which needs finer attribution before selecting an implementation. R29's console
pixel proofs establish correctness; its controlled game benchmark is pending.
The previous integer-filter experiment reduced copy cost without improving FPS
and was rolled back. Engine `r_scale 2` and `tasks 0` also failed to improve FPS.

120 FPS allows **8.33 ms for an entire frame**. Work should start with measured
CPU image/mip paths, possible GPU blits, excessive cache work and opportunities
for correct CPU/GPU overlap. Engine-side water passes, uploads and scheduling
also deserve profiling. Neither missing synchronization nor smaller render
resolution is an established fix. Each optimization needs image comparisons,
repeatable game timing and stability checks.

There is a separate output limit: [the current WSI](driver/ps5vk_wsi.c) exposes
**one 3840×2160 mode at 60 Hz, FIFO only**. 120 distinct displayed frames per
second require implementing and validating a real 120 Hz VideoOut mode and frame
pacing, plus a suitable display/HDMI setup. PS5 supports 4K120 output in general
([Sony's guide](https://www.playstation.com/en-ca/support/hardware/ps5-4k-resolution-guide/));
that does not establish support in this driver. Stable 4K60 is the next practical
performance milestone; 4K120 remains a target requiring both driver and
application measurement, not a promised result.

## What this is — and what it is not

**It is** a probe that answers "what does this console's GPU actually do" with
console runs instead of assumptions, and a Vulkan 1.0 driver built on those
answers: a Mesa-derived frontend, the NIR/ACO shader compiler, and a
PS5-specific AGC and VideoOut backend.

**It is not** a Sony SDK, a retail-package builder, an exploit, or a conformance
submission. It ships no Sony file, no key and no game content. It needs a
homebrew-enabled console that you own, and it never configures that console for
you. The device reports 1.0, with coverage recorded command by command, limit
by limit and format by format. The audits, targeted pixel proofs and working
applications are evidence of progress, not a claim of certified conformance.

## How it works

### The stack

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

Nothing Linux-specific crosses over: no `amdgpu`, no DRM, no ioctls, no RADV
queue submission. The driver programs AGC directly, and the PS5-specific parts —
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

### What the device reports (rung 1.0)

| Property | Value |
| --- | --- |
| Instance API version | 1.3 (Mesa's instance level; promises nothing about the device) |
| Device API version | 1.0 |
| Extensions | `VK_KHR_surface`, `VK_KHR_display`, `VK_KHR_swapchain`, `VK_KHR_get_physical_device_properties2`, `VK_EXT_debug_report`, `VK_EXT_debug_utils` |
| Queue families | 1 |
| Features | `robustBufferAccess` |
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

Five titles build from this tree. Four are canaries or diagnostics; one is the
runner that produces the evidence:

| Title | What it is |
| --- | --- |
| `PPSA99988` | **The probe runner**: the Vulkan driver, the test runner and every probe package |
| `PPSA99999` | The default diagnostics app (the graphical Hello World) |
| `PPSA99997` | The AGC driver canary |
| `PPSA99998` | The linked canary with the complete-state canary |
| `PPSA99996` | The live-submission canary |

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
| [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) | ps5-payload-dev | Optional prebuilt PS5 ports (SDL2, OpenSSL, …) for applications | Optional, pinned to v0.40.2 |
| [opengnm-psbc](https://github.com/PS4-OpenGNM/opengnm-psbc) | PS4-OpenGNM | The Mesa-derived shader compiler (NIR + ACO) the driver links for SPIR-V, and the tree the 0.3.0 SDK patches | Fetched by the SDK's patch over its pinned revision; this project's compiler patches are re-applied on top (`tooling/psbc/`) |
| [ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) | mpereiraesaa | A second native PS5 Vulkan implementation: its per-format console evidence and its reporting inventory are cross-checks for this project's audit | Reference; read, not fetched or linked |
| [Mesa 3D](https://gitlab.freedesktop.org/mesa/mesa) | Mesa contributors (freedesktop.org), with AMD's AddrLib inside | The Vulkan common runtime and utilities the driver builds on, and AddrLib, which the tiling oracle compiles | Fetched, pinned to 26.2.0, checksum-verified |
| [LLVM / Clang](https://github.com/llvm/llvm-project) | The LLVM project | The host compiler, and the target compiler the payload SDK packages | Host packages plus the SDK's toolchain |
| PS5 system modules (AGC, VideoOut) | Sony Interactive Entertainment | The console's real GPU command, register and display interfaces | Used on the console through the SDK's published import stubs; never redistributed |
| [AMD GPU documentation](https://llvm.org/docs/AMDGPUUsage.html) · [BC-250 documentation](https://elektricm.github.io/amd-bc250-docs/hardware/specifications/) | AMD; elektricm | Instruction definitions, register fields, and an external reference for the PS5-derived BC-250 board | Reference |
| [Vulkan-Docs](https://github.com/KhronosGroup/Vulkan-Docs) · [Vulkan-CTS](https://github.com/KhronosGroup/Vulkan-CTS) | Khronos Group | The version requirement tables the ladder is built from (pinned v1.4.354) and the conformance subsets Phase E1 will run | Pinned docs; the CTS is the next gate |
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
