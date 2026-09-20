# Milestone 5, Phase B: first end-to-end Vulkan program

Append-only run log: add new entries at the end; do not rewrite an existing
section. Current state and next actions live in
[VULKAN_PROBE_ACTIVE.md](VULKAN_PROBE_ACTIVE.md).

Phase B builds the first Vulkan program through the driver: a headless
triangle. The roadmap is in [VULKAN_PROBE_PLAN.md](VULKAN_PROBE_PLAN.md). Its
riskiest step, B4, was probed first.

| Step | What | Status |
|---|---|---|
| B1 | Generate the Vulkan dispatch and entry points from `vk.xml`; build the common Vulkan runtime for the PS5 and the PC | done: Mesa 26.2.0's util library and lite runtime build for both with 0 warnings; the PC smoke test passes and the PS5 link is accepted by the title converter |
| B2 | Driver skeleton: instance, physical device, device, queue | done: `driver/` on Mesa's runtime; the same test passes through the Khronos loader (28 checks) and directly (30), and its PS5 link is accepted by the title converter |
| B3 | Memory, buffers and images on direct memory | done on the PC: device memory mapped inside the high-word-2 address window (B3a), 256-byte-aligned buffers (B3b), and images sized by ps5-opengl's layouts, the 4K colour and depth targets exactly the runner's 32 MiB (B3c) |
| B4 | Probe: a submission that completes without a flip | done: a completion marker is written after the stream's last packet; passed with VideoOut open and fully headless. Run pid 108 (B5c): the marker arrives after the last draw has rendered |
| B5 | Command buffers and submission; fences and semaphores on the B4 signal | done: command buffers, submission, fences and semaphores on the PC (B5b); on the console, streams without draws complete (B5a, pid 107) and the marker follows the last draw's rendering (B5c, pid 108), so marker-based fences are safe for readback |
| B6 | Pipelines from SPIR-V, one pixel-shader variant per export format | pending |
| B7 | Render targets; draw and read back the M2 triangle through the Vulkan API | pending |
| B8 | Probe: several command buffers in one submission | pending |

This log records progress and findings in order. Hardware findings also go
into VULKAN_PROBE_PLAN.md.

## 2026-09-14: B4, completion without a flip

Every M1-M4 frame ended in a flip, and the runner confirmed completion
through VideoOut's flip status. Headless Vulkan has no flip: fences, queue
waits and offscreen rendering need another signal that the GPU finished a
submission.

### Where the signal came from

ps5-opengl's native title runtime (`src/platform/ps5_agc_native_runtime.c`)
already renders without flipping. For frames it does not present:
- The stream has no wait-until-safe packet and no flip.
- After the draws it adds the colour-buffer barrier
  (`sceAgcCbReleaseMem(45, 12, 1, 0, NULL, 0, 0, 0, 1, 0, 0)`), then
  `sceAgcCbReleaseMem(40, 0x30c, 0, 0, marker, 1, value, 0, 0, 0, 0)`.
- After `sceAgcDriverSubmitDcb` and `sceAgcSuspendPoint` it polls the marker
  word, flushing its cache line, every 1 ms for up to 2 s, until it holds
  `value`.

### What was built

- `src/diagnostics.cpp`:
  - `FrameResources` gains `completion_marker` and `completion_value`. With
    them, `build_own_shader_frame` ends the frame with the barrier and the
    completion marker instead of the flip. Without VideoOut
    (`memory.video < 0`) the frame has no wait packet.
  - `submit_and_confirm` is the shared submission core. `submit_frame`
    confirms through the flip marker as before; `submit_until_marker` polls
    the completion marker (`wait_for_completion_marker`, logged as
    `agc_completion_marker` with the observed value, sleeps and
    microseconds).
  - The marker is the last word of the stage workspace (`+0xfffc`), cleared
    and flushed before building the frame.
- Two runner tests, both drawing the M2 triangle:
  - `b4-marker`: exactly `m2-solid` with its flip replaced by barrier +
    marker. VideoOut is open, and the wait packet names framebuffer 0, which
    is never shown. It passes when the marker arrives and framebuffer 0 reads
    back exactly.
  - `b4-headless`: no VideoOut at all. A direct-memory target the size of the
    framebuffer (SWAP_STD, so R,G,B,A bytes), no wait packet, no flip. It
    passes when the marker arrives and the target reads back exactly
    (`0xffffa020` in every pixel).
- `jobs/b4/queue.txt`: `capture`, `hold 60`, `b4-marker`, `b4-headless`, then
  `m2-solid` to show the flip path still works afterwards.

### Result on the console

Runner pid 297, `Klog_Logs/klog-20260914-233744.log`. All three tests passed.

| Test | Stream | Marker | Readback |
|---|---|---|---|
| `b4-marker` | 66 words: wait, CX, UC, SH, draw, barrier, marker | `0x42340001` written | 8,294,400 of 8,294,400 framebuffer pixels `0xff20a0ff`, padding zero |
| `b4-headless` | 34 words: CX, UC, SH, draw, barrier, marker | `0x42340002` written | 8,294,400 of 8,294,400 target pixels `0xffffa020`, padding zero |
| `m2-solid` | 114 words, flip | flip marker reached | exact, as before |

- In both B4 tests the marker already held its value at the first poll, 20
  µs after `sceAgcSuspendPoint` returned, so the poll never slept.
- The two B4 tests show nothing on screen, by design: neither flips, and
  `b4-headless` never opens VideoOut. The only frame on the TV during the run
  is `m2-solid`'s.
- Probe statuses: 146 PASS, 25 INFO, 12 NOT_REQUIRED, 3 ARMED, 1 WARN (the
  known benign VideoOut busy result on unregister).

The recorded completion marker packet (`b4-headless`, marker at
`0x20002bffc`, value `0x42340002`):

```text
c0064900 0030c528 20000000 0002bffc 00000002 42340002 00000000 00000000
```

- Header: `RELEASE_MEM` (`0x49`) with 7 payload words, the same length as the
  barrier.
- Payload word 0 is `control << 12 | 0x500 | event`: `0x0030c528` for event 40
  and control `0x30c`, and the barrier's `0x0000c52d` for event 45 and control
  12.
- The address follows as `0x20000000` and its low word. Only address high word
  2 was recorded, so the high word's encoding is not generalised.
- Then `2` for the one value, and the value itself.

### PC tooling taught from the recording

- Golden files: `golden/b4` holds the three frames of pid 297.
  `golden/runner` (pid 260) is unchanged.
- `tools/golden.py extract` records `flips_before`, the flips the process
  made before each frame. Only streams that call `sceAgcDcbSetFlip` advance
  it: the `m2-solid` stream of pid 297 is frame 3, but the process's first
  flip (flip counter word `0x08000101`). Files without the field default to
  frame − 1, which holds for pid 260, where every stream flipped.
- Replays carry `flips`, and name the VideoOut handle only when a wait or flip
  does. The host runner (`host/runner/runner_host_main.cpp`) resets the flip
  model from `flips`. `check-helpers` uses the same count.
- `host/agc/agc_host.cpp`: `sceAgcCbReleaseMem` encodes the recorded
  completion marker. It refuses any other form, including other address high
  words.
- `host/ps5/ps5_host.cpp`: `sceAgcDriverSubmitDcb` completes every recorded
  completion marker in the submitted stream, writing its value to its
  address in a mapped replay region. That is what the console GPU had done by
  the time the suspend point returned. `sceKernelUsleep` is a PC no-op.
- `tools/agc_rules.py`: completion markers are recognised. `wait-buffer`
  applies to frames that flip, and a frame that neither flips nor ends in a
  completion marker still breaks it. `tools/test_agc_rules.py` checks both
  golden sets and proves the new branch on `b4-headless` with its marker's
  event cleared.

Results:
- `check-helpers`: 129 of 129 calls in `golden/runner`, and 19 of 19 in
  `golden/b4` (4 `RELEASE_MEM`: 2 barriers, 2 completion markers).
- `rebuild`: 10 of 10 frames in `golden/runner` and 3 of 3 in `golden/b4`
  identical. On the PC both B4 tests now log `agc_completion_marker` `PASS`
  with the recorded values. Their `runner_test` stays `FAIL`, because the PC
  draws no pixels.
- Rules: all 13 frames pass all 8 rules; the 10 rule proof tests pass.
- Before the recording, the PC runner stopped both tests cleanly: the model
  refused the unrecorded packet, and `b4-headless`'s target allocation was not
  in the M2 replay.

### Findings

- A submission can be confirmed without VideoOut: the GPU writes a chosen
  32-bit value into a GPU-visible word after the stream's last packet. A
  headless Vulkan device needs neither VideoOut, nor a wait packet, nor a
  flip.
- Rendering into a plain direct-memory target and reading it back works
  without VideoOut, with the same tiled RGBA8 layout as the framebuffer.
- A flip-free submission does not disturb the flip path. `m2-solid`
  afterwards flipped normally as the process's first flip.

### Open questions for B5

- The marker was already written when `sceAgcSuspendPoint` returned, for a
  one-draw frame. Either the suspend point waits for the GPU, or the frame
  simply finished first. Fences and queue waits need to know which: a probe
  with a stream long enough to outlast the call, logging the marker before
  and after the suspend point, separates them.
- The barrier before the marker follows ps5-opengl. Whether completion needs
  it was not tested on its own.

## 2026-09-14: B4 follow-up, completion timing (result: console run pid 107, below)

Question: does `sceAgcSuspendPoint` wait for the GPU? B5's fences depend on
the answer. If it waits, a fence is signalled when the suspend point returns;
if not, a fence must poll or wait on the completion marker.

What was built:
- `FrameResources::auto_draws` repeats `DRAW_INDEX_AUTO`, at most 256 times.
- Every submission logs `agc_linked_gpu_completion` `suspend_microseconds`,
  the duration of `sceAgcSuspendPoint`.
- `submit_until_marker` logs the marker straight after submission, before the
  suspend point (`agc_completion_marker` `after_submit`).
- Runner test `b4-timing`: three headless frames with 1, 16 and 128 full-target
  triangles, the target cleared before each. Each is confirmed by its marker
  (`0x42350001`-`0x42350003`) and read back exactly.
- `jobs/b4-timing/queue.txt`: `hold 60`, `b4-timing`, `m2-solid`. There is no
  capture: 128 draws exceed the capture's helper-call record, which then
  marks the capture incomplete rather than overflowing.

How to read the result, per frame:
- If `after_submit` is 0, the suspend point is longer for more draws, and every
  marker reads as written on the first poll (`sleeps` 0), then the suspend
  point waits for the GPU.
- If the suspend point stays short while `sleeps` grows with the draw count,
  it does not wait, and completion must be polled.

The PC runner cannot answer this: it completes markers at submission.
Rebuilds and rules are unchanged: 10 of 10 and 3 of 3 frames identical, and 0
violations.

## 2026-09-15: B1, Mesa's Vulkan runtime for the PS5 and the PC

The driver is built on Mesa's common Vulkan runtime (`src/vulkan/runtime` and
`src/vulkan/util`, MIT), as decided for Milestone 5. B1 generates its
dispatch and entry-point sources from `vk.xml` and builds it for both
targets.

### Where the sources come from

- The opengnm-psbc tree the shader compiler is built from carries Mesa's
  Vulkan runtime, but is not usable for it. Its git history is a single
  commit (`a92a1228`), and in it core runtime headers are placeholders:

| Header | Tree | Mesa 26.2.0 |
|---|---|---|
| `vk_device.h` | 3 lines | 501 lines |
| `vk_queue.h` | 3 | 316 |
| `vk_instance.h` | 10 | 277 |
| `vk_physical_device.h` | 3 | 263 |
| `vk_command_buffer.h` | 3 | 260 |
| `vk_pipeline.h` | 32 | 243 |
| `vk_log.h` | 17 | 118 |
| `vk_command_pool.h` | 3 | 104 |

- The tree also commits 3-line placeholders for nine generated headers
  (enough for the compiler's two NIR passes) and edits other runtime files
  (`vk_image.c`, `vk_pipeline.c`, `vk_video.c`, `vk_meta_blit_resolve.c` and
  more). Its `src/vulkan/util` and `vk.xml` are identical to Mesa's; its
  `src/util` (17 files), NIR (45) and SPIR-V (8) are modified. No full copy
  existed anywhere on the machine.
- With the user's permission, `tools/fetch-mesa.sh` downloads the Mesa release
  the ps5-opengl SDK pins in `dependencies.json`:
  `https://archive.mesa3d.org/mesa-26.2.0.tar.xz`, 68,461,648 bytes, SHA-256
  `efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef`,
  verified before use, kept in ignored `.deps/native/mesa` and extracted
  into the Mesa source cache.
- The runtime's sources therefore come from the release, while compilation
  uses the compiler tree's include paths and flags. That keeps util and
  compiler structures identical to `libpsbc.ps5.a`, which the driver links
  beside it.

### What was built

- `tools/build-vulkan-runtime.sh` copies `src/vulkan/{runtime,util}` from
  the release and runs Mesa's generators with the arguments of its
  `meson.build` files. They produce the dispatch table, enum strings,
  structure-type casts, extensions, common and command-enqueue entry points,
  command queue, dispatch trampolines, feature, property and SPIR-V
  capability tables, synchronization helpers and format information. Mesa's
  generators need mako and markupsafe; the host provides them as the Arch
  packages `python-mako` and `python-markupsafe`, and `PS5VK_MAKO_PATH` can
  point at another location.
- `tooling/vulkan-runtime/Makefile` compiles meson's `libvulkan_util` and
  `libvulkan_lite_runtime` sources, without the Android and DRM sync-object
  files, plus `vk_instance.c` with `VK_LITE_RUNTIME_INSTANCE=1`. That is 57
  sources per target, into `libvk_runtime.ps5.a` and `libvk_runtime.a`,
  installed with their headers in `.deps/native/vulkan-runtime`.
- `tooling/vulkan-runtime/wsi/`: the release's window system is not used; the
  PS5 has none, and presentation will be this driver's own VideoOut path.
  `wsi_common.h` includes `vk_internal_exts.h`, which already defines the WSI
  image structure the runtime reads. `wsi_common_get_time_domain`, reachable
  only through `VK_EXT_present_timing` with a Mesa swapchain, is a stub.
- A source stamp (Mesa archive, stubs, Makefile, tree) discards old objects
  when inputs change, because rsync keeps the release's file times.

Build problems found and fixed:
- `max_align_t` undeclared on the PS5: FreeBSD's `sys/cdefs.h` caps ISO C
  visibility at C99 whenever `_XOPEN_SOURCE` is set. The runtime compiles
  without the tree's `-D_XOPEN_SOURCE=700`; the default environment exposes
  POSIX 2008, XSI, BSD and C11.
- `PACKAGE_VERSION` undeclared: the one global meson define these sources use
  (`vk_get_driver_version` parses it). It comes from the pinned version.
- `drm-uapi/drm_fourcc.h` not found: `vk_image.c` includes it on Linux and BSD,
  and the PS5 counts as BSD. Only Mesa's `include/drm-uapi` is added; its whole
  `include` directory would bring Vulkan headers over the Vulkan-Headers copy
  the compiler archive is built with.

Result: 57 of 57 sources per target, 0 compiler warnings, 27 s.
`libvk_runtime.ps5.a` is 3,313,290 bytes (`9a9a47f1`), `libvk_runtime.a`
2,722,018 bytes (`0dad6ccb`), debug information stripped. (B2 builds the PC
archive with `-fPIC`: 2,706,506 bytes, `4e8a1fc9`.)

### Verification (`tools/check-vulkan-runtime.sh`)

`tooling/vulkan-runtime/smoke/vk_runtime_smoke.c` creates and destroys a
`vk_instance` through the runtime:
1. With only the common entry points the runtime is a Vulkan 1.0
   implementation, and `vk_instance_init` must reject apiVersion 1.3 with
   `VK_ERROR_INCOMPATIBLE_DRIVER`, as the spec requires. The first version of
   the test missed this and got exactly that result.
2. With a driver `EnumerateInstanceVersion` reporting 1.3, the instance is
   created, and the application name, API version 1.3 and driver version
   26.2.0 must match.

On the PC, linked with the SDK's host `libpsbc.a` for Mesa util: PASS.

On the PS5 the program is linked the way a console title links, with the
runtime archive whole (as meson's `link_whole`) and the compiler recipe of
`tools/psbc-link.sh` for Mesa util, then passed through the title converter.
It took three rounds:
1. Five undefined symbols, from util sources the PS5 compiler archive leaves
   out:

| Missing | Used by | Now supplied by |
|---|---|---|
| `os_time_get_nano`, `os_time_get_absolute_timeout` | `vk_sync.c`, `vk_fence.c` waits; RMV | the tree's `util/os_time.c` in `libpsbc_support.ps5.a` |
| `util_get_process_name` | RMV capture dump | the tree's `util/u_process.c`, FreeBSD path through `getprogname` |
| `mesa_log` | `vk_log.c` | `psbc_ps5_shims.c`: `mesa_log`/`mesa_log_v` write log.c's file-logger format to stderr |
| `localtime_r` | RMV capture dump | `psbc_ps5_shims.c`: the console exports `localtime` but not `localtime_r`; serialise `localtime` under Mesa's statically initialised `simple_mtx` (the compiler's `glsl_types.c` already uses one on the console) and copy the result. `localtime_s` is exported too, but no SDK header declares it, so its argument order is unknown |

   The two util objects compile without `-D_XOPEN_SOURCE=700`
   (`tooling/psbc/support.mk`): `getprogname` is declared only when
   `__BSD_VISIBLE`, and `usleep` only when `__XSI_VISIBLE` is at most 600 or
   `__BSD_VISIBLE`.
2. lld resolved everything, but the converter rejected
   `vk_cmd_enqueue_CreateInstance`. Mesa's entry-point tables name every Vulkan
   function through weak references meant to read as NULL. The converter
   imports every dynamic symbol, and the ELF held 1,555 undefined weak ones
   (`vk_common_*`, `vk_cmd_enqueue_*`, `vk_cmd_enqueue_unless_primary_*`)
   beside 122 real imports. `-z nodynamic-undefined-weak` alone changed
   nothing.
3. With `--no-dynamic-linker`: PASS. The ELF has 5,164,144 bytes, 0 undefined
   weak dynamic symbols, and 0 relocations naming them. Its relocations are
   122 `GLOB_DAT`, 12 `JUMP_SLOT` and 6,312 `RELATIVE`, with no interpreter
   segment. The converter accepted all 122 imports.

   Correction, made during B3b: the passing link also carried
   `-z nodynamic-undefined-weak`, and this log first credited both options.
   - LLD 18.1.3 does not know that option. It prints
     `warning: unknown -z value` and ignores it.
   - Linking the B2 driver test with and without it gives byte-identical ELFs
     (`5604d32b…`).
   - `--no-dynamic-linker` alone does the work: 2,302 undefined weak dynamic
     symbols, 2,429 imports and 8,773 dynamic relocations without it; 0, 127
     and 6,471 with it.
   - The option has been removed from the check scripts.

The runtime adds eight imports to those the compiler already uses:
`clock_gettime`, `getpid`, `getprogname`, `strncpy`, `strstr`, `strtok`,
`time` and `usleep`. All are named by SDK stubs; a console launch confirms
they are exported.

Regressions:
- `tools/check-psbc-link.sh`: PASS, now 126 imports; the shims object also
  imports `localtime`.
- All five titles build without warnings. The runner's digest changed
  (`6f45225486c5eb46`) because the support archive's shims member did; the
  deployed runner is still the `b4-timing` build.

### Consequences

- B2 onwards: the driver links `libvk_runtime.ps5.a` whole, with
  `--no-dynamic-linker`.
- Mesa's common runtime already provides the Vulkan 1.0 physical-device
  queries (wrapping their `2` variants), `GetPhysicalDeviceFeatures2`,
  `GetPhysicalDeviceProperties2`, `EnumeratePhysicalDevices`,
  `EnumerateDeviceExtensionProperties` and `GetDeviceQueue`/`GetDeviceQueue2`.
  B2 writes the instance entry points, queue family, memory, format and image
  format properties, and device creation.
- B6, when a title links the compiler and the full runtime together:
  - `psbc_stubs.c` in `libpsbc.ps5.a` defines `vk_debug_report`,
    `vk_sampler_state_init`, `vk_format_to_pipe_format` and
    `vk_format_get_ycbcr_info`, which the runtime defines too.
  - The stub `vk_format_to_pipe_format` returns `PIPE_FORMAT_NONE`, and A3's
    byte identity was measured with that stub on both sides.
  - The compiler's two NIR passes were compiled against the tree's
    placeholder runtime headers.

  All three need resolving there before the compiler and driver share a
  title.

## 2026-09-15: B2, driver skeleton

The driver (`driver/`, GPL-3.0-or-later like the rest of this repository)
starts on Mesa's runtime from B1: instance, physical device, logical device
and queue. Nothing reaches the GPU yet.

### What was built

| File | Role |
|---|---|
| `driver/ps5vk_private.h` | `ps5vk_instance`, `ps5vk_physical_device`, `ps5vk_device`, `ps5vk_queue`, each embedding its runtime base object first, with Mesa's handle casts |
| `driver/ps5vk_instance.c` | instance entry points, physical-device enumeration callback, `vk_icdGetInstanceProcAddr` |
| `driver/ps5vk_physical_device.c` | properties and limits, queue family, memory properties |
| `driver/ps5vk_device.c` | `CreateDevice`/`DestroyDevice`, the queue, the submission stub |
| `driver/ps5vk_icd.map` | version script: the PC library exports only the loader interface |
| `driver/Makefile` | driver sources (held to `-Werror`) plus generated entry points, per target |
| `driver/tests/vk_b2_device_test.c` | the B2 test, built for the loader and for direct use |
| `tools/build-driver.sh` | entry points (`vk_entrypoints_gen.py --prefix ps5vk`), both archives, PC loader library and manifest |
| `tools/check-driver.sh` | the three runs below |
| `tools/mesa-python.sh` | mako detection, now shared with `tools/build-vulkan-runtime.sh` |
| `tooling/psbc/Makefile.opengnm-psbc-host-pic` | position-independent PC compiler archive (below) |

What the driver reports, grounded in the Vulkan 1.4.354 specification sources
(downloaded to ignored `.deps/native/vulkan-docs`):
- Instance: version 1.3, because `vk_instance_init` treats a driver without
  `EnumerateInstanceVersion` as 1.0 and rejects newer applications (B1). One
  extension, `VK_KHR_get_physical_device_properties2`; no layers. Mesa's
  runtime enumerates physical devices lazily through the driver's callback.
- Physical device: API 1.0, driver version 0.2.0, vendor `0x1002`, integrated
  GPU, name `PS5 AGC GPU (ps5vk)`. Every feature is off: Vulkan 1.0 requires
  none, and each will be turned on with the step that makes it work.
- Limits follow the Required Limits table (`limits.adoc`): the core minimum
  where the capability exists, and the "unsupported" column where it depends
  on a feature that is off (tessellation, geometry, dual-source blending,
  anisotropy, multiple viewports, wide points and lines, clip and cull
  distances: 0 or 1). `maxPerStageResources` is 44 (footnote 2's formula);
  per-set descriptor limits are the per-stage minimums times the three stages
  this driver will have, vertex, pixel and compute (footnote 8). Sample counts
  1 and 4, map alignment 4096, offset alignments 256.
- One queue family, graphics, compute and transfer, with one queue and no
  timestamps: a graphics family must also offer compute (`devsandqueues.adoc`).
- Memory: one type, `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`, satisfying
  both required types at once (`memory.adoc`), in one device-local heap sized
  by `sceKernelGetDirectMemorySize()`. On the PC the host layer reports 4 GiB.
- Device: `vk_device_init` rejects features and extensions the physical device
  lacks. The queue's submission returns `VK_ERROR_FEATURE_NOT_PRESENT` until
  B5; the runtime then marks the queue lost.
- Loader interface: Mesa's runtime supplies
  `vk_icdNegotiateLoaderICDInterfaceVersion` and
  `vk_icdGetPhysicalDeviceProcAddr`; the driver adds
  `vk_icdGetInstanceProcAddr`, which is also where a console title, with no
  loader, enters. `tools/build-driver.sh` fails if the PC library exports
  anything but those three. The manifest comes from Mesa's `vk_icd_gen.py`
  (`api_version` 1.0.354).

### Problem found: the SDK's PC compiler archive cannot go into a shared library

The loader loads the driver as a shared library. Linking it failed on
`R_X86_64_TPOFF32` in `libpsbc.a(u_call_once.o)`: the SDK's host archive is
built as position-dependent code (the compiler's default PIE model uses
local-exec TLS). The runtime needs about 130 of its members, NIR, SPIR-V to
NIR, util and formats among them, so selective rebuilding would be fragile.

`tooling/psbc/Makefile.opengnm-psbc-host-pic` rebuilds the whole archive in
the compiler build cache with the SDK's own host configuration plus `-fPIC`, as
`libpsbc.pic.a` with `.pic.o` objects beside the PS5 build's `.ps5.o`. Like
the SDK's PS5 makefile it takes the object list from the tree's Makefile. The
first build compiled 480 sources (212 MB with debug information, not
installed); its 260 warnings are in the compiler tree's sources, which build
with the SDK's flags unchanged. The driver's PC outputs link this archive.

The runtime's PC archive is now `-fPIC` too. The runtime build's source stamp
includes the build script's hash, so a flag change rebuilds, and
`tools/build-driver.sh` records each target's flags and discards objects built
with other ones. The PS5 archives are unchanged.

### Verification (`tools/check-driver.sh`)

| Run | How | Result |
|---|---|---|
| PC, loader | the Khronos loader (`libvulkan.so.1`), `VK_DRIVER_FILES` naming only this driver, layers disabled | 28 of 28 checks |
| PC, direct | the test linked with the driver and runtime archives, entering through `vk_icdGetInstanceProcAddr` | 30 of 30 |
| PS5 | the direct build linked as a console title links (`--no-dynamic-linker`), then the title converter | 6,057,512-byte ELF, 123 imports accepted |

The test checks the instance version and extensions, rejection of an unknown
instance extension, one physical device and its properties and limits,
`GetPhysicalDeviceProperties2KHR` through the extension, every feature off, the
queue family, memory types and heap, device creation with one queue and
`GetDeviceQueue`, and rejection of `geometryShader` (`FEATURE_NOT_PRESENT`) and
`VK_KHR_swapchain` (`EXTENSION_NOT_PRESENT`). The direct run adds two checks the
loader would mask: exactly version 1.3 and exactly one instance extension.

Regression: `tools/check-vulkan-runtime.sh` PASS. The PS5 test is not run on
the console yet; a console title first gets the driver when something reaches
the GPU (B5).

### Consequences

- B3: the heap is the whole direct memory. Allocations must keep shader-visible
  addresses at high word `0x2` (32-bit shader pointers), which may shrink what
  the heap can honestly report.
- B5: replaces the submission stub, with fences on the B4 completion marker.
- The pipeline cache UUID is a constant until pipelines exist (B6).

## 2026-09-15: B3a, device memory

B3 is split into three steps, each with its own tests and commit:
- B3a: device memory.
- B3b: buffers.
- B3c: images and formats.

B3c's storage layouts will come from ps5-opengl's resource code
(`ps5_resource_layout` in `src/gallium/ps5/ps5_screen.c`, GPL-3.0-or-later).
Sampled images use linear rows padded to 256 bytes. Colour and depth targets
use 128x128 tiles of 64 KiB, with allocations rounded to 2 MiB. The runner
already matched both layouts on the hardware: the M3 texture was untiled RGBA8
with 256-byte rows, and the M2-M4 targets and depth were tiled.

### What was built

- `driver/ps5vk_memory.c`: each `VkDeviceMemory` is one direct-memory
  allocation.
  - It uses type 12 and protection `0x33` (CPU and GPU read and write), as the
    runner allocates.
  - Its size is rounded up to the 16 KiB direct-memory page.
  - Allocations of 2 MiB or more are 2 MiB-aligned, the alignment ps5-opengl
    gives render allocations, so later resources that need it can be bound at
    aligned offsets.
  - It stays mapped for its whole life. The GPU uses the same addresses and
    the memory is host-coherent, so `MapMemory2` returns a view of the
    mapping, and unmap, flush and invalidate do nothing.
- The address window: the shader compiler combines 32-bit pointers with high
  word 2 (`--address32-hi 2`), and both console mappings recorded so far lie
  there (`0x20001c000`, `0x200200000`).
  - An allocation the kernel maps anywhere else is released again and fails
    with `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
  - The heap is the direct memory size, capped at the window's 4 GiB.
- Errors:
  - Kernel failures return `VK_ERROR_OUT_OF_DEVICE_MEMORY`, and the kernel's
    code appears in the error message.
  - More than `maxMemoryAllocationCount` (4096) live allocations on one device
    return `VK_ERROR_TOO_MANY_OBJECTS`.
- `host/ps5/ps5_host.cpp`: direct memory without a replay, for the driver's
  PC tests.
  - Allocations are first-fit, page-aligned ranges of the 4 GiB reported size.
  - A range maps at `0x200000000` plus its start, the console's high word.
  - `PS5_HOST_DIRECT_MAPPING_BASE` moves the mappings, for negative tests.
  - Replays keep the region model: `golden/runner` still rebuilds 10 of 10
    frames identically, and `golden/b4` 3 of 3.
- Tests:
  - `driver/tests/ps5vk_test.h` holds the scaffolding every test shares; the
    B2 test now uses it.
  - `tools/check-driver.sh` runs every test in every mode and prints a summary
    table.

### Verification (`tools/check-driver.sh`)

| Test | Loader | Direct | PS5 link + converter |
|---|---|---|---|
| `vk_b2_device_test.c` | 28 of 28 | 30 of 30 | 127 imports accepted |
| `vk_b3_memory_test.c` | 13 of 13 | 13 of 13 | 127 imports accepted |
| `vk_b3_window_test.c` (negative) | - | 4 of 4 | - |

`vk_b3_memory_test.c` allocates 1 byte, 3 MiB and 16 x 64 KiB, and checks:
- every mapping lies at high word 2, page-aligned, or 2 MiB-aligned for the
  3 MiB allocation;
- the first and last pages read back, and the allocations are pairwise
  disjoint;
- unmapping and mapping again returns the same, unchanged memory, and a mapping
  at an offset starts there;
- flush and invalidate succeed;
- freed memory can be allocated again;
- an allocation larger than the heap fails with `VK_ERROR_OUT_OF_DEVICE_MEMORY`.

`vk_b3_window_test.c` runs with the host mapping direct memory at high word 3:
- Allocations of one page and 3 MiB are refused.
- So are 64 of 256 MiB, four times the heap together.
- The variable is then cleared, and 15 x 256 MiB (3.75 GiB) must allocate,
  which only fits if every refused allocation released its direct memory.

The negative test was itself proven. A copy of the driver with the release
removed passes the three refusals and fails the fourth check (3 of 4, exit 1).

The PS5 test programs now import four more kernel functions than in B2 (127,
up from 123): the direct-memory allocate, map, unmap and release calls.

### Not yet known from the console

- Whether `sceKernelMapDirectMemory` keeps placing mappings at high word 2 for
  many and large allocations; two mappings were recorded. If it does not, the
  driver fails loudly instead of handing shaders unreachable memory. The first
  console title with the driver (B5) will show.
- The console does not clear direct memory between allocations (runner
  finding). Vulkan does not require new memory to be zero, so nothing clears
  it.

## 2026-09-15: B3b, buffers

### What was built

`driver/ps5vk_buffer.c`:
- `CreateBuffer` and `DestroyBuffer`, on Mesa's `vk_buffer`. A buffer that
  cannot fit the 4 GiB address window after rounding is refused with
  `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- `GetDeviceBufferMemoryRequirements`:
  - The size is rounded up to 256 bytes, the alignment is 256, and the only
    memory type is 0.
  - Mesa's `vkGetBufferMemoryRequirements(2)` call it with a create info
    rebuilt from the buffer, so the requirements depend on the create info
    alone.
- `BindBufferMemory2`: the buffer's GPU address (`vk_buffer::device_address`)
  is the allocation's mapping plus the bind offset, asserted inside the
  window.

Why 256 bytes:
- The minimum uniform, storage and texel buffer offset alignments are 256.
- So every address a descriptor can name keeps a zero low byte. That is the
  runner's rule for descriptor addresses: the compiled high word and a zero
  low byte.

Not handled, because a valid application cannot pass them:
- `VkMemoryDedicatedRequirements` needs Vulkan 1.1 or
  `VK_KHR_dedicated_allocation`.
- `VkBindMemoryStatus` needs Vulkan 1.4 or `VK_KHR_maintenance6`.

The device reports Vulkan 1.0 and exposes neither extension.

### Verification (`tools/check-driver.sh`)

| Test | Loader | Direct | PS5 link + converter |
|---|---|---|---|
| `vk_b2_device_test.c` | 28 of 28 | 30 of 30 | 127 imports accepted |
| `vk_b3_memory_test.c` | 13 of 13 | 13 of 13 | 127 imports accepted |
| `vk_b3_buffer_test.c` | 8 of 8 | 8 of 8 | 127 imports accepted |
| `vk_b3_window_test.c` (negative) | - | 4 of 4 | - |

`vk_b3_buffer_test.c` checks:
- Buffers of 1, 255, 256, 4097 bytes and 3 MiB, with every usage, need memory
  type 0, alignment 256, and their size rounded up to 256.
- A buffer with a subset of the usage needs no more.
- A 4 GiB buffer can be created; one byte more is refused.
- A vertex buffer and a uniform buffer bind next to each other in one
  allocation, and their contents stay apart through the mapping.
- A third buffer can alias the uniform range.
- A 3 MiB buffer binds at a 2 KiB offset of a 4 MiB allocation.
- The memory can be freed before the buffer is destroyed.

The buffers' GPU addresses cannot be read through the API without the
`bufferDeviceAddress` feature, which is off. B5 and B7 use them on the
console.

### Found while testing

- The PS5 compile (clang) caught an uninitialised memory handle in the new
  test, on a path taken only when buffer creation fails; the PC compile (gcc)
  did not warn. The test is fixed.
- `tools/check-driver.sh` still linked a test whose PS5 compile had failed,
  because `set -e` does not apply inside the `if` that records results. The
  PS5 step now stops at a failed compile.
- The link option `-z nodynamic-undefined-weak` does nothing with LLD 18. The
  B1 section above has the correction, and the check scripts no longer pass
  it. `tools/check-vulkan-runtime.sh` still passes, with the same
  5,164,144-byte ELF and 122 imports, and every driver test ELF is unchanged.

## 2026-09-15: B3c, formats and images

### Formats

`driver/ps5vk_image.c` reports only formats whose features the runner has
shown on the hardware:

| Format | Optimal-tiling features | Evidence |
|---|---|---|
| `R8G8B8A8_UNORM` | sampled, linear filter, transfer src/dst, colour attachment | colour target (M2); sampled nearest exactly and bilinear within one level (M3 step 3); render to texture (M4) |
| `D32_SFLOAT` | depth/stencil attachment | tiled depth, stored depth equal to clip z (M4 step 1) |

Notes on the table:
- Transfer is listed because the specification requires it on every sampled
  format (maintenance1).
- Blending is not listed, because blending with only `CB_BLEND0_CONTROL`
  programmed was incomplete (M4 step 2).
- No format has linear-tiling or buffer features yet.
- The mandatory format table of `formats.adoc` is much larger. Formats join it
  as probes prove them, the same rule the features follow.

Image format properties support 2D images with optimal tiling, no create
flags, and usages the format's features allow. What they report follows
`capabilities.adoc`:

| Property | Value | Why |
|---|---|---|
| `maxExtent` | 4096x4096 | the image limits |
| `maxMipLevels` | 13 | the complete chain, as required for optimal tiling |
| `maxArrayLayers` | 256 | `maxImageArrayLayers` |
| `sampleCounts` | 1 and 4 | required to cover the framebuffer and sampled-image limits for 2D optimal images |
| `maxResourceSize` | 4 GiB | the address window |

Everything else returns `VK_ERROR_FORMAT_NOT_SUPPORTED` with zeroed
properties: linear tiling, other image types, storage, create flags such as
`MUTABLE_FORMAT`, sampled depth and other formats.

The sparse queries report nothing. Before B3c, Mesa's
`vkGetPhysicalDeviceSparseImageFormatProperties` and
`vkGetImageSparseMemoryRequirements` forwarded to driver entry points that did
not exist.

### Image storage

Sizes follow ps5-opengl's resource layouts (`ps5_resource_layout` and
`ps5_resource_create_unlocked` in `src/gallium/ps5/ps5_screen.c`,
GPL-3.0-or-later), which the runner matched on the hardware:

| Storage | Used for | Size | Alignment |
|---|---|---|---|
| Rows | images that are not attachments | each level row-major, rows padded to 256 bytes, level extents rounded up; times layers and samples | 256, the zero low byte texture descriptors need |
| Tiles | colour and depth attachments | each level in 64 KiB tiles (128x128 texels for 4-byte texels; ps5-opengl's tile sizes for other texel sizes and for 4x), times layers, rounded up to 2 MiB | 2 MiB |

Images larger than the 4 GiB window fail with
`VK_ERROR_OUT_OF_DEVICE_MEMORY`, as `capabilities.adoc` prescribes for images
beyond `maxResourceSize`. `BindImageMemory2` records the memory and the GPU
address: the mapping plus the offset, asserted inside the window.

The hardware has read back only single-level, single-layer, single-sample 2D
storage: the M3 texture and the M2-M4 targets. Mip chains, array layers and
4x storage use the same rules, but where their texels sit is proven only by
the probes that first render into or sample them (Phase C).

### Verification (`tools/check-driver.sh`)

| Test | Loader | Direct | PS5 link + converter |
|---|---|---|---|
| `vk_b2_device_test.c` | 28 of 28 | 30 of 30 | 127 imports accepted |
| `vk_b3_memory_test.c` | 13 of 13 | 13 of 13 | 127 imports accepted |
| `vk_b3_buffer_test.c` | 8 of 8 | 8 of 8 | 127 imports accepted |
| `vk_b3_image_test.c` | 18 of 18 | 18 of 18 | 127 imports accepted |
| `vk_b3_window_test.c` (negative) | - | 4 of 4 | - |

`vk_b3_image_test.c` checks the two reported formats exactly, an unproven
format with no features, the image format properties of both formats, six
refused combinations with zeroed properties, and the empty sparse queries. It
also checks these storage sizes:

| Image | Expected | Source of the expectation |
|---|---|---|
| 64x36 sampled RGBA8 | 9,216 bytes, 256-byte alignment | the runner's M3 texture |
| the same with its 7-level chain | 256 x (36+18+9+5+3+2+1) bytes | rows of 256 bytes per level |
| 100x10 sampled RGBA8, 6 layers | 512 x 10 x 6 bytes | 400-byte rows padded to 512 |
| 3840x2160 RGBA8 colour attachment | 0x2000000 bytes, 2 MiB alignment | 30x17 tiles in the runner's 32 MiB framebuffer allocation |
| 3840x2160 D32 depth attachment | 0x2000000 bytes, 2 MiB alignment | the runner's 32 MiB depth allocation |
| 16x16 colour attachment | 2 MiB | one tile rounded up |
| 512x512 4x colour attachment | 4 MiB | 64 tiles of 64x64 texels |
| 4096x4096 sampled, 256 layers | refused, `VK_ERROR_OUT_OF_DEVICE_MEMORY` | 16 GiB, beyond the window |

It also binds two sampled images next to each other in one allocation, and a
colour attachment in its own 2 MiB allocation.

### Which format conversion the PS5 build uses

Image sizes depend on `vk_format_to_pipe_format`, and the compiler archive's
`psbc_stubs.ps5.o` defines a stub of it that returns `PIPE_FORMAT_NONE` (a B1
consequence for B6). The PC tests only prove the PC link, so the PS5 link of
`vk_b3_image_test.c` was traced with `--trace-symbol`:
- `vk_format_to_pipe_format` (0x19b bytes, with `vk_format_map`) and
  `vk_format_get_ycbcr_info` are defined by Mesa's runtime `vk_format.o`.
- The stub member is not extracted at all. The runtime links whole before the
  compiler archive, and an extracted stub would have been a duplicate-symbol
  error.
- Links are reproducible: two plain links are byte-identical to the
  check-driver ELF (`dba8b8d9…`). Only a link with `--trace-symbol` differs,
  because the option itself changes the output.

The stubs still matter for B6: once a title calls the compiler, members that
define other symbols can come in with it.

## 2026-09-15: B5a, a submission without draws (result: console run pid 107, below)

### Why

B5 submits Vulkan command buffers and confirms them with the B4 completion
marker. A Vulkan submission can hold no drawing at all: empty command buffers,
or only a fence. Its stream is then nothing but what confirms it. Every stream
the console has completed so far drew first: the recorded `b4-headless`
stream carries register tables and a draw before the barrier and the marker.

Two console questions therefore decide B5's submission:
1. Does a stream without draws complete, and does it need the barrier? This
   also answers B4's open question about the barrier.
2. Does `sceAgcSuspendPoint` wait for the GPU? This is the B4 timing probe,
   still unlaunched.

Neither can be answered on the PC.

### What was built

- Runner test `b5-empty` (`run_empty_stream_frames` in `src/diagnostics.cpp`):
  - Two headless submissions from the stage workspace, with no register
    tables and no draws.
  - The first holds ps5-opengl's end of a frame it does not present: the
    colour-buffer barrier, then the completion marker (16 words).
  - The second holds the marker alone (8 words).
  - Each passes when its marker (`0x42360001`, `0x42360002`) arrives within
    2 s. If the first fails, the test stops with the stage kept mapped, the
    runner's rule while a submission may still be in flight.
  - Each frame logs its words, GPU pointers, indirect-table validation and, in
    capture mode, its helper calls and workspace image, so the recording can
    become golden files.
- `jobs/b5/queue.txt`: `capture`, `hold 60`, `b5-empty`, `b4-headless`,
  `b4-timing`, `m2-solid`. One launch answers both questions; `b4-headless`
  afterwards shows that the GPU still draws.
- `tools/golden.py`:
  - A frame's register defaults came from its own first context table, which
    a stream without register tables does not have.
  - `register_defaults` now reads them where a frame shows them, and `rebuild`
    replays a table-less frame with the defaults of another frame of the same
    console run (klog and pid).
  - It reports a clear failure when no frame of the run has any, instead of
    stopping on an exception.
- The runner build: all five titles build with 0 warnings; runner
  `PPSA99988` digest `b436a1cb2285bfde`.

### Verification on the PC

The PC cannot show what the GPU does; the host layer completes markers at
submission. What it can check is the test's own code. A dry run replayed the
recorded `b4-headless` inputs with the queue `capture`, `b5-empty`,
`b4-headless`:

| Check | Result |
|---|---|
| `b5-empty` | PASS, both frames |
| Barrier + marker stream | `c0064900 0000c52d 00010000 0 0 0 0 0` then `c0064900 0030c528 20000000 0002bffc 00000002 42360001 0 0` |
| Marker-only stream | the second packet alone, value `42360002` |
| `golden.py extract` | 3 golden files (`b5-empty` 16 and 8 words, `b4-headless` 34 words) |
| `check-helpers` | 9 of 9 helper calls reproduced by the PC models |
| `rebuild` | 3 of 3 frames identical, the `b5-empty` frames with the defaults of `b4-headless` |
| `agc_rules.py` | 8 of 8 rules pass on all 3 frames |

The first version of the test did not log its GPU pointers. Extraction then
failed with "no workspace image", because golden files need the workspace
bounds and the stream address. The test now logs them, as
`build_own_shader_frame` does.

Regressions:
- `golden/runner` 10 of 10 and `golden/b4` 3 of 3 frames still rebuild
  identically.
- `check-helpers` reproduces 129 of 129 and 19 of 19 helper calls.
- `tools/test_agc_rules.py` passes 10 of 10.

### Deployment

The console did not answer FTP during this work, so nothing is deployed. The
console still runs the `b4-timing` runner. The `b5-empty` build contains
`b4-timing` too. Deploy it and install the queue:

```text
PS5_HOST=<console-lan-ip> PARAM_PATH=sce_sys/param-runner.json \
APP_DEFINITIONS="AGC_LINKED_CANARY=1 AGC_TEST_RUNNER=1 AGC_OUTPUT_4K=1 AGC_LIVE_SUBMIT_ARMED=1 AGC_SHADER_COMPILER=1" \
make deploy
python3 tools/ps5_console.py push-jobs --replace PPSA99988 jobs/b5/queue.txt
```

### B5b design, pending the answers

The driver's queue will use Mesa's immediate submit mode, with timeline mode
none.
- `driver_submit` waits on its input syncs, and copies the command buffers'
  streams into a GPU-visible submission buffer.
- It appends the barrier and a completion marker, submits with
  `sceAgcDriverSubmitDcb`, polls the marker as the runner does, then signals
  its output syncs.
- Fences and semaphores use a CPU sync type (mutex and condition variable,
  after lavapipe's) claiming binary, GPU wait, CPU wait, reset and signal.
  Polling is correct whether or not the suspend point waits; the timing answer
  only decides how fast.
- If `b5-empty` shows that a marker-only stream does not complete, empty
  submissions need a harmless packet before the marker, and the recording
  shows which.

## 2026-09-15: B5b, command buffers, submission, fences and semaphores (PC)

### What was built

- `driver/ps5vk_cmd_buffer.c`: command pools, allocation, reset and freeing
  are Mesa's common implementation. A command buffer adds the PM4 words
  recorded into it, and `BeginCommandBuffer`/`EndCommandBuffer` follow Mesa's
  state machine. No command records words yet; drawing arrives with B6 and B7.
- `driver/ps5vk_sync.c`: a CPU-signalled binary sync object for fences and
  semaphores, after lavapipe's `lvp_pipe_sync` (Mesa, MIT): a flag under a
  mutex and condition variable.
  - It claims binary operation, GPU wait, GPU multi-wait, CPU wait, reset and
    signal. GPU wait is honest because submission waits for its input syncs
    before anything reaches the GPU.
  - It is the physical device's only sync type. Mesa therefore selects
    timeline mode none and immediate submission.
- `driver/ps5vk_queue.c`: each queue maps a 2 MiB GPU-visible submission
  buffer when the device is created; a buffer outside the address window
  refuses the device. `driver_submit`:
  1. waits for the submission's input syncs;
  2. copies the command buffers' words into the buffer;
  3. appends the colour-buffer barrier and a completion marker aimed at the
     buffer's last word, with a non-zero value unique to the submission;
  4. calls `sceAgcDriverSubmitDcb` and `sceAgcSuspendPoint`, then polls the
     marker as the runner does (flush the cache line, compare, sleep 1 ms, up
     to 2 s);
  5. signals the output syncs.

  A submission without command buffers has nothing for the GPU and only
  signals. A failure from step 3 on (the helpers, the submit, the suspend
  point, or a marker that never arrives) loses the queue and the device.
- `driver/ps5vk_device.c`: `sceAgcInit(8)`, the version the runner passes,
  once per process.
- `host/ps5/ps5_host.cpp`:
  - Markers now also complete inside the no-replay direct-memory mappings.
  - `PS5_HOST_DROP_COMPLETION_MARKERS` leaves every marker unwritten, for
    the device-lost negative test.
  - Replays are unchanged: `golden/runner` 10 of 10 and `golden/b4` 3 of 3
    frames still rebuild identically.
- Builds:
  - The driver's PC library and direct tests link `host/agc/agc_host.cpp`,
    the PC model of `sceAgcCbReleaseMem`.
  - The PS5 test links build the linked-canary AGC link-stub libraries, as
    `tools/build.sh` does for the runner, and pass them to the linker and the
    title converter.

### Verification (`tools/check-driver.sh`)

| Test | Loader | Direct | PS5 link + converter |
|---|---|---|---|
| `vk_b2_device_test.c` | 28 of 28 | 30 of 30 | 132 imports accepted |
| `vk_b3_memory_test.c` | 13 of 13 | 13 of 13 | 132 imports accepted |
| `vk_b3_buffer_test.c` | 8 of 8 | 8 of 8 | 132 imports accepted |
| `vk_b3_image_test.c` | 18 of 18 | 18 of 18 | 132 imports accepted |
| `vk_b5_submit_test.c` | 10 of 10 | 10 of 10 | 132 imports accepted |
| `vk_b3_window_test.c` (negative) | - | 5 of 5 | - |
| `vk_b5_lost_test.c` (negative, markers dropped) | - | 4 of 4 | - |

`vk_b5_submit_test.c` checks:
- a command pool, two command buffers, two fences and a semaphore;
- recording empty command buffers;
- a new fence is unsignalled, and one created signalled is signalled;
- a submitted command buffer signals its fence once its marker arrives;
- a reset fence is unsignalled, and waits on it time out;
- a submission without command buffers signals its fence;
- a submission waits on the semaphore another submission signalled;
- 100 submissions of two command buffers each complete in turn;
- a reset command buffer records and submits again;
- `QueueWaitIdle` and `DeviceWaitIdle`.

On the PC the host layer writes a marker only when it finds a well-formed
marker packet aimed at mapped direct memory in the submitted stream, so every
completed submission also proves that packet.

`vk_b5_lost_test.c` runs with markers dropped:
- the submission loses the device;
- its fence reports the lost device;
- the lost queue refuses further submissions.

`vk_b3_window_test.c` now also refuses a device whose queue buffer maps at
high word 3. Its final reuse check then covers the refused queue buffer too.

### Found while testing

- The PS5 links first failed on four undefined AGC symbols. AGC imports come
  from the project's link stubs (`vendor/ps5/sdk/stubs`), not from the payload
  SDK's stub libraries.
- Mesa's immediate submission does not mark the queue lost when
  `driver_submit` fails. The first run of `vk_b5_lost_test.c` passed the lost
  submission but failed the fence and resubmission checks (2 of 4). The queue
  now calls `vk_queue_set_lost` itself.
- `vk_b3_window_test.c` first failed at `vkCreateDevice`. Once the queue maps
  its buffer at device creation, a high-word-3 mapping refuses the device,
  which is correct. The test now sets the variable itself around the calls it
  checks, and checks that refusal as well.
- The PS5 test programs import 132 functions, 5 more than in B3: the four AGC
  calls and `sceKernelUsleep`. The pthread functions behind the sync objects
  were already imported through the runtime and the compiler's C11 threads.

### What only the console can show

The PC completes markers at submission, so these tests prove the driver's
protocol, not the GPU. The console still has to show:
- whether a stream of only the barrier and marker completes. That is exactly
  what an empty command buffer submits (`b5-empty` in `jobs/b5`);
- how long the marker takes to arrive (`b4-timing`);
- `sceAgcInit(8)` in a title other than the runner. The driver has not run on
  the console yet; the first title with it comes once drawing exists (B7).

## 2026-09-15: Console run pid 107, the B5a probe and the B4 timing probe

The runner build `b436a1cb2285bfde` was deployed with `jobs/b5`. The klog is
`Klog_Logs/klog-20260915-084812.log`; it stays local, because `*.log` is
ignored.

All four tests pass: `b5-empty`, `b4-headless`, `b4-timing` and `m2-solid`.
The run logged one FAIL and one WARN, both expected:
- FAIL: `b4-timing`'s 128 draws overflowed the capture's helper-call record.
- WARN: VideoOut reported busy at shutdown, a known benign status.

### Per frame

| Frame | Stream words | Marker right after submission | `sceAgcSuspendPoint` | Marker found |
|---|---|---|---|---|
| `b5-empty`, barrier + marker | 16 | not yet written | 127 µs | first poll, 20 µs |
| `b5-empty`, marker alone | 8 | already written | 131 µs | first poll, 20 µs |
| `b4-headless`, 1 draw | 34 | already written | 123 µs | first poll, 20 µs |
| `b4-timing`, 1 draw | 34 | not yet written | 127 µs | first poll, 20 µs |
| `b4-timing`, 16 draws | 79 | already written | 126 µs | first poll, 20 µs |
| `b4-timing`, 128 draws | 415 | not yet written | 128 µs | first poll, 20 µs |

Every drawn frame also read back exactly.

### Findings

1. Streams without draws complete. The barrier followed by the marker
   completed, and so did the marker alone, so completion does not need the
   barrier. This answers B4's open question. The stream B5b submits for
   empty command buffers works on the hardware.
2. `sceAgcSuspendPoint` takes 123–131 µs, whatever the stream holds: from a
   lone marker to 128 full-target draws. Its duration does not grow with the
   GPU work.
3. The completion marker does not follow rendering:
   - Every marker was found on the first poll, 20 µs after the suspend point.
   - For three frames (the marker alone, `b4-headless` and 16 draws) the
     marker was already written when `sceAgcDriverSubmitDcb` returned, before
     the suspend point.
   - 128 full-target 3840x2160 triangles are about 1.06 billion pixel
     writes, some 4 GB written to the target. Memory bandwidth alone rules out
     finishing them in the roughly 150 µs before that frame's marker was seen,
     unless the GPU discards the repeated draws.
   - So the marker is most likely written while the stream is taken in,
     rather than when the frame is finished. B4's reading that "the GPU writes
     the marker once the work is done" is not established.
   - The readbacks all passed, but they started after the marker and took
     longer than the frames could need, so they cannot show when rendering
     finished.
4. The consequence for B5b: a fence is signalled when its marker arrives,
   which may be before the submission's rendering is finished. Nothing reads
   GPU output yet, so nothing is wrong today, but B7's readback would be.
   Before B7, a probe must separate the marker from rendering. It should
   sample a target pixel straight after submission and again the moment the
   marker is seen, in a frame whose last draw is the only one to change that
   pixel.

   Correction from run pid 108: findings 3 and 4 were wrong. The B5c probe
   (next section) sampled exactly that pixel. With 256 heavy draws, the pixel
   held the heavy colour when submission returned and the last draw's colour
   when the marker arrived. The marker follows rendering; the
   memory-bandwidth estimate above does not hold for this GPU and workload.
   Findings 1 and 2 stand.

### Golden files

`golden/b5` holds the recorded frames of `b5-empty` (2), `b4-headless` and
`m2-solid`. `b4-timing` is left out: its third frame's capture is incomplete
by design, and extraction writes nothing if any frame is incomplete.
`tools/golden.py extract` gained `--test` for this; frames of the other tests
still count towards frame numbers and flips.

| Check | Result |
|---|---|
| `check-helpers golden/b5` | 15 of 15 helper calls reproduced by the PC models |
| `rebuild golden/b5` | 4 of 4 frames identical, the `b5-empty` frames with the register defaults of their run |
| `agc_rules.py golden/b5` | 8 of 8 rules pass on all 4 frames |
| `test_agc_rules.py` | 10 of 10 |
| Regression | `golden/runner` 10 of 10 and `golden/b4` 3 of 3 still identical |

## 2026-09-15: B5c, the completion marker follows rendering (console run pid 108)

### Why

Run pid 107 suggested the marker arrives before rendering is finished, which
would make B5b's fences unsafe for readback. B5c tests that directly.

### What was built

- Runner test `b5-order` (`run_render_order_frames`, the `m3-vertex`
  shaders): two headless frames into a 4K direct-memory target cleared to 0.
  - Heavy full-target triangles in colour A (`0x40,0x80,0xc0`) are drawn as
    one indexed draw: 16 of them in the first frame, 256 in the second.
  - One last full-target triangle follows in colour B (`0xf0,0x30,0x60`),
    then the barrier and the completion marker.
  - Only the last draw puts B into any pixel.
- The centre pixel is sampled straight after submission and again the
  instant the marker is seen. It is then polled until it holds B, and the
  whole target must read back B.
- The sampling uses `submit_and_confirm`'s two hooks; no shared submission
  code changed.
- `jobs/b5c/queue.txt`: `capture`, `hold 60`, `b5-order`, `b4-headless`,
  `m2-solid`.
- Runner `PPSA99988` digest `58c852140b7080bc`; commit `d86980c`.
- A PC dry run checked the stream (61 words, 13 helper calls), validation,
  the marker and the sampling. Its poll timed out at 0, as it must, because
  the PC draws nothing.

### Result (klog `Klog_Logs/klog-20260915-090031.log`, not committed)

All three tests pass. The only non-pass is the known VideoOut-busy warning.

| Frame | Right after submission: marker | Right after submission: pixel | `sceAgcSuspendPoint` | At the marker: pixel |
|---|---|---|---|---|
| 16 heavy draws + last draw | already written | B | 126 µs | B |
| 256 heavy draws + last draw | not yet written | A, the heavy colour | 129 µs | B |

- In the 256-draw frame, submission returned while the heavy draws were still
  rendering. By the time the marker was seen, the last draw's colour had
  arrived.
- Both frames read back B in every pixel.
- The logged "microseconds after marker" (about 4.5 ms) is not rendering
  time. The pixel already held B at the first sample; the interval covers the
  log writes between the marker check and the poll.

### Conclusions

- The completion marker is written after the stream's last draw has
  rendered, at least at the sampled pixel. Run pid 107's reading (findings 3
  and 4, corrected there) was wrong.
- A fence signalled at the marker (B5b) is therefore consistent with
  rendering being complete. Readback after `vkWaitForFences` is safe on
  this evidence.
- The suspend point still takes ~125 µs regardless of the work, and the
  marker was always found on its first poll. For these workloads the GPU
  finished within that window; heavier workloads may need the poll's later
  iterations, which the driver already performs.

### Golden files

`golden/b5c` holds `b5-order` (2), `b4-headless` and `m2-solid` from pid
108.

| Check | Result |
|---|---|
| `check-helpers golden/b5c` | 38 of 38 helper calls reproduced by the PC models |
| `agc_rules.py golden/b5c` | 8 of 8 rules pass on all 4 frames |
| `rebuild golden/b5c --test b4-headless`, `--test m2-solid` | identical |

The `b5-order` frames are not rebuilt on the PC. Their test polls for
rendered pixels, which the PC does not produce, so it stops after the first
frame.

## 2026-09-15: B6, graphics pipelines from SPIR-V (PC)

### What was built

- `ps5vk_shader_module.c`: a shader module keeps a copy of its SPIR-V words.
  Mesa's `vk_shader_module.c` is not used, because its BLAKE3 hashing serves
  pipeline caches the driver does not have.
- `ps5vk_descriptor_set_layout.c`: each binding gets a place in the set's
  GPU-visible descriptor table.
  - Bindings take consecutive entries in binding order.
  - Entry sizes are those the M3 probes compiled and ran: 16 bytes for a
    uniform buffer, 48 for a combined image sampler.
  - Other types get stride 0, which pipelines refuse.
  - Pipeline layouts, and destruction of both kinds of layout, are Mesa's
    common code.
- `ps5vk_pipeline.c` (`vkCreateGraphicsPipelines`, `vkDestroyPipeline`)
  compiles each stage with `psbc_compile_shader` and packages it with
  `ps5_agc_package_build` (ESGS item size 1), as the console compiled the
  probes in Phase A3. The options come from the create info:

  | Option | Source |
  |---|---|
  | target, optimise | PS5, on |
  | NGG | vertex stage |
  | `address32_hi` | 2 for both stages (the B3 address window) |
  | vertex attributes | vertex input state: R32G32, R32G32B32 and R32G32B32A32 SFLOAT, stride from the binding, alignment 4 |
  | descriptor bindings | set 0 bindings whose stage flags include the stage, at their table offset and stride |
  | `spi_shader_col_format` | per colour attachment, FP16_ABGR (4) if it blends and 32_ABGR (9) if not; 0 when nothing blends |

  - Compilations are serialized by one mutex. Each device holds a
    `psbc_init` reference.
  - On Linux, `PS5VK_PIPELINE_DUMP=<prefix>` writes each new pipeline's
    packages to `<prefix>-vertex.bin` and `<prefix>-pixel.bin`.
  - Refused with `VK_ERROR_UNKNOWN` and a logged reason:
    - topologies other than triangle lists, and primitive restart;
    - multisampling;
    - per-instance vertex input;
    - descriptor sets other than 0, and types without a proven table entry;
    - specialization constants;
    - colour formats other than RGBA8;
    - pipelines without both a vertex and a fragment stage.
  - The pipeline is not yet created in AGC or linked. That needs the
    console and comes with drawing in B7.
- The three vertex formats report `VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT`, which
  valid usage requires of attribute formats.

### Problem found: the compiler archive stubs five runtime functions

Linking `psbc_compile_shader` beside Mesa's runtime fails on both targets.
The archive's `psbc_stubs` member defines five functions the runtime also
defines:
- `vk_debug_report`
- `vk_format_get_ycbcr_info`
- `vk_format_to_pipe_format`
- `vk_sampler_state_init`
- `vk_spec_info_to_nir_spirv`

Dropping the stubs is not an option either. The compiler's SPIR-V debug
callback passes a NULL report to `vk_debug_report`, and Mesa's version
dereferences it.

`tools/build-driver.sh` therefore derives a copy of each compiler archive:
- the copies are `build/driver/ps5/libpsbc_driver.ps5.a` (with
  `prospero-objcopy`) and `build/driver/host/libpsbc_driver.pic.a` (with GNU
  `objcopy`);
- `--redefine-sym` renames the five to `psbc_*`, in the stubs and in the
  compiler's references alike;
- the build fails if an original name is still defined or a renamed one is
  missing.

The compiler keeps exactly the behaviour Phase A3 measured, and the runtime
keeps its own functions. No compiler source is patched or copied.

The PC library also links `ps5_agc_package.c`. The PS5 links take it from
`libpsbc_support.ps5.a`.

### Problem found: libpsbc crashes on SPIR-V without an entry point

The first test run segfaulted on a header-only module. Calling
`psbc_compile_shader` directly reproduces the crash for both stages with the
original PC archive too, so it is the compiler's own, not the renaming's. Its
only validation is the magic number (`libpsbc/psbc_compile.c:1767`).

The driver now walks the module before compiling. The module must be a
well-formed instruction stream, every instruction's word count inside the
module, declaring an `OpEntryPoint` with the stage's execution model and
`pName`. Anything else is refused with `VK_ERROR_UNKNOWN`. Such modules break
Vulkan valid usage, but this is a cheap check that turns a crash into an
error.

### Verification (`tools/check-driver.sh`, test `b6_pipeline`)

The PC test builds, through the Vulkan API, the pipeline each probe set's
`compile.txt` describes: vertex input, set-0 bindings and blending, with a
render pass holding one RGBA8 attachment. It compares the dumped packages with
the set's `vertex.bin` and `pixel.bin`, the packages the hardware ran.

| Check | Loader | Direct |
|---|---|---|
| The three vertex formats report vertex-buffer features | PASS | PASS |
| `m2`, `m3`, `m3-vertex`, `m3-texture`, `m4-depth`, `m4-blend`: both packages byte-identical | 6 of 6 | 6 of 6 |
| `m4-depth`'s SPIR-V with blending equals `m4-blend`'s pixel package, and differs from `m4-depth`'s | PASS | PASS |
| Point-list pipeline refused | PASS | PASS |
| Entry point name missing from the SPIR-V refused | PASS | PASS |
| Header-only SPIR-V refused, no pipeline returned | PASS | PASS |

- **Totals:** the B6 test passes 12 of 12 checks in both PC builds. B2, B3
  and B5 and the two negative tests are unchanged and pass.
- **PS5:** every test links with the driver's compiler copy, and the title
  converter accepts 144 imports. That is 12 more than B5's 132, from the
  compiler and its C++ runtime.
- **The `m4-depth` / `m4-blend` pair** shows the M4 blend rule working from
  the API: the same SPIR-V gives the `--color-format 0x99999994` package when
  the attachment blends.
- **`m2`** was compiled with no `--address32-hi`, but the driver's
  `address32_hi 2` gives the same bytes.

### Consequences

- The driver produces, from Vulkan create infos, the exact packages every
  M2 to M4 hardware run used. B7 can create them in AGC, link them and draw
  the M2 triangle.
- Compilation runs inside `vkCreateGraphicsPipelines`, serialized. Pipeline
  caches and background compilation can come later without changing packages.

## 2026-09-15: B7a, drawing through the Vulkan API (PC)

### What was built

**Drawing (`driver/ps5vk_draw.c`).** A draw records the frame of the runner's
`b4-headless` test, which the console rendered exactly in runs pid 107 and
108:
1. One indirect context register table:
   - the colour target's 16 CB_COLOR0 registers, from AGC's defaults adjusted
     as `append_target_registers` adjusts them (COMP_SWAP_STD);
   - 15 viewport, guard-band, generic-scissor and `CB_TARGET_MASK` registers;
   - the 34 linked context records;
   - both shaders' context registers.
2. The linked uniform table.
3. One SH table with both shaders' SH registers.
4. `DRAW_INDEX_AUTO` with the vertex count.

Submission then appends the barrier and the completion marker, as since B5.
Register names were checked against Mesa's `gfx103.json`; offset `0x31e` is
`CB_COLOR0_DCC_CONTROL`, and `0x08e` is `CB_TARGET_MASK`, not a clip enable.

**Viewport orientation.** The driver programs Vulkan's viewport transform as
RADV does, with y scale +height/2. ProsperoLight's registers, which the runner
uses, have -height/2. The M3 vertex probe shows why this matters: with -h/2,
clip y = -0.5 landed on the lower half, OpenGL's orientation. The full-target
M2 triangle covers every pixel either way, so the new console test `b7-corner`
(below) reads the orientation back.

**Render passes and image views.** Render passes and framebuffers are Mesa's
common implementation over `vkCmdBeginRendering`. Image views are Mesa's
common objects. `vkCmdPipelineBarrier2` records nothing, because layouts carry
no state and every submission already ends with the colour barrier.

**AGC shader objects (`driver/ps5vk_pipeline.c`).**
- They are created the first time a command buffer draws with a pipeline, in
  a 64 KiB stage workspace laid out as `link_shader_packages` lays it out:
  - both headers and code on 4 KiB boundaries;
  - `sceAgcCreateShader` for each stage;
  - `sceAgcLinkShaders` for triangle lists, primitive type 4;
  - context records at +0x5000 and uniforms at +0x6000.
- Packages pass the runner's `validate_shader_package` checks first.
- Pipelines that never draw use no GPU memory.
- Creation still succeeds for state that cannot be drawn yet, so B6's package
  checks keep working. Draws with such pipelines are refused (next item).

**Refusals.** Recording fails with `VK_ERROR_UNKNOWN` at `vkEndCommandBuffer`,
with the reason logged, for:
- any rendering other than one colour attachment that is a single-level
  3840x2160 R8G8B8A8_UNORM image, over its whole area;
- attachments that are cleared or resolved;
- depth and stencil attachments;
- instancing, or a first vertex or instance other than 0;
- viewports and scissors other than the whole target;
- vertex input, descriptor sets, dynamic state, culling, polygon modes other
  than fill, depth bias or clamp, depth and stencil tests, blending, logic
  operations and colour write masks other than RGBA.

**Direct memory (`driver/ps5vk_direct_memory.c`).** One helper now allocates,
maps and window-checks every GPU-visible allocation: device memory, the
queue's submission buffer, stage workspaces, and command buffers' register
tables (256 KiB chunks kept across resets).

**Shared memory and the CPU cache.** CPU and GPU share memory, and the
application keeps colour targets mapped. So the queue evicts the CPU cache
lines of every colour target a submission renders to twice, as the runner's
`flush_gpu_data` does:
- before submission, so no CPU write reaches memory after the GPU's;
- after the marker arrives, so the CPU reads what the GPU wrote.

**Driver messages.** The instance now exposes `VK_EXT_debug_report` and
`VK_EXT_debug_utils`, so applications, including the console runner, receive
the driver's reasons.

### Found while testing

- **Mesa's render passes set a rendering flag.** They always pass
  `VK_RENDERING_LOCAL_READ_CONCURRENT_ACCESS_CONTROL_BIT_KHR` to
  `vkCmdBeginRendering`. The first version refused any flags, so every render
  pass failed. The bit only describes concurrent input-attachment access and
  is now ignored; each rendering refusal names what it saw.
- **Mesa logs driver errors as warnings.** `vk_errorf` messages arrive at
  warning severity (`__vk_errorv`). The test program counted only errors, so
  the refusal was silent until it counted warnings too. A link-time wrap of
  `__vk_errorf` located it.
- **GCC 13 false positive.** `-Werror=stringop-overflow` flagged a `memcpy`
  into a struct pointer field; the pointer is now read through a helper.

### PC verification

**Replay and comparison.**
- **Replays for driver tests.** `PS5_HOST_REPLAY` gives the driver's PC tests
  a replay without the runner:
  - Allocations stay first-fit, except that each replayed region goes to the
    first allocation of its size at its captured address.
  - Shader creation, linking and register defaults replay from it.
  - The runner's strict replay path is unchanged: the golden rebuilds below
    still pass.
- **Submission dumps.** `PS5_HOST_SUBMISSION_DUMP` records each submission's
  words with every register table it points at.
- **New golden.py commands.**
  - `replay` writes a golden file's replay.
  - `compare-submission` compares a dump with a golden frame packet by packet
    and table by table. Table addresses and the marker's address and value may
    differ; a documented record difference must actually appear.

**The B7 test** (`driver/tests/vk_b7_draw_test.c`) runs the shared program
`driver/tests/ps5vk_triangle.c`. That is an ordinary Vulkan 1.0 program:
instance, device, 3840x2160 image on host-visible memory, view, render pass,
framebuffer, pipeline from `probes/m2`, one command buffer with one draw, and
a fence. It draws first normally, then with a clearing render pass.

| Check | Loader | Direct |
|---|---|---|
| The M2 triangle records, submits and signals its fence | PASS | PASS |
| The frame is readable through the mapped 32 MiB image memory | PASS | PASS |
| A clearing render pass fails recording with `VK_ERROR_UNKNOWN`, with the driver's reason | PASS | PASS |
| The submission against golden `b5/b4-headless-1.json` | identical: 6 packets, 3 register tables | identical |

The single difference is the intended one: register `0x111`
(PA_CL_VPORT_YSCALE) is `0x44870000` (+1080.0) where the console frame has
`0xc4870000` (-1080.0).

**The rest of `tools/check-driver.sh`: driver check PASS.**
- B2 30 of 30, now expecting three instance extensions.
- B3 and B5 unchanged, and both negative tests pass.
- B6 13 of 13, including the new `b7-corner` set, whose packages are
  byte-identical to `tools/build-probe-shaders.sh b7-corner`.
- Every PS5 link converts with 151 imports. The 7 new ones are
  `sceAgcGetRegisterDefaults`, `sceAgcCreateShader`, `sceAgcLinkShaders`, the
  three register-table loads and `sceAgcDcbDrawIndexAuto`.

**Golden regressions with the changed host layer:** `golden/runner` 10 of 10,
`golden/b4` 3 of 3, `golden/b5` 4 of 4, `golden/b5c` (`b4-headless`,
`m2-solid`) 2 of 2, and `check-helpers golden/b5c` 38 of 38.

### What only the console can show (B7b)

- **A second `sceAgcInit(8)`.** The runner initializes AGC at start and the
  driver's `vkCreateDevice` calls it again. If that fails, `b7-triangle` logs
  the result.
- **Rendering and readback.** Whether the Vulkan-recorded frame renders
  exactly into the application's image, including the cache evictions.
- **Orientation and culling defaults.** Vulkan's orientation (`b7-corner`),
  and whether the unprogrammed culling defaults draw the flipped winding.

## 2026-09-15: B7b, the Vulkan driver in the test runner

### What was built

- **Build mode.** `tools/build.sh` has a mode for the definition
  `AGC_VULKAN_DRIVER=1`. It links the runner the way `tools/check-driver.sh`
  links driver tests:
  - the driver and Mesa's runtime whole, with `--no-dynamic-linker`;
  - the driver's renamed compiler copy in place of `libpsbc.ps5.a`;
  - `driver/tests/ps5vk_triangle.c`, with the Vulkan headers.
  
  The runner (`tools/build-all-titles.sh`, PPSA99988) is built with it. The
  mirror build runs `tools/build-driver.sh` first, because the mirror keeps
  its own `build/`.
- **Runner tests** in `src/diagnostics.cpp`, both calling
  `ps5vk_triangle_draw` through `vk_icdGetInstanceProcAddr` and logging every
  step as `b7_<step>`, including the driver's messages:
  - `b7-triangle` (set `m2`): exact readback with `check_solid_frame` against
    `0xffffa020`, as `b4-headless` checks.
  - `b7-corner` (set `b7-corner`): `check_corner_frame` requires every pixel
    centre more than two rows above the top-right to bottom-left diagonal to
    be drawn, every pixel below it to stay zero, and no other colour or
    padding. It also counts how many pixels agree with OpenGL's orientation.
  
  A submission that does not complete keeps its objects allocated and stops
  the queue, as the runner's own tests do.
- **Job queue** `jobs/b7/queue.txt`: `hold 60`, then `b4-headless`,
  `b7-triangle`, `b7-corner`, `m2-solid`.

### Build and deployment

- All five titles build with 0 warnings and aligned segments. Runner
  PPSA99988 has digest `26f66612e52ed065`.
- Deployed to `/data/homebrew/PPSA99988` (79 files), with `jobs/b7` uploaded.
- The host runner is built without the definition, so the golden rebuilds are
  unaffected.

## 2026-09-15: Console run pid 121, the first frames through the Vulkan driver

Queue `jobs/b7`, klog `Klog_Logs/klog-20260915-140828.log` (not committed).
All four tests pass. The only non-pass record is the known, benign
VideoOut-busy warning of `m2-solid`; 183 records passed.

| Test | Result |
|---|---|
| `b4-headless` | PASS: 8,294,400 of 8,294,400 pixels `0xffffa020` |
| `b7-triangle` | PASS: every Vulkan step, then 8,294,400 of 8,294,400 pixels `0xffffa020`, no padding written |
| `b7-corner` | PASS: 4,139,524 of 4,139,524 top-left pixels drawn, 0 of 4,139,524 bottom-right pixels drawn, no other colour or padding |
| `m2-solid` | PASS: the GPU is still healthy after both Vulkan devices |

### Findings

1. **The M2 triangle renders exactly through the Vulkan API.** An ordinary
   Vulkan 1.0 program did it all: instance, device, image on its own memory,
   render pass, framebuffer, pipeline compiled on the console from SPIR-V,
   command buffer and fence. The frame read back through the image's mapped
   memory equals the runner's own `b4-headless` frame, pixel for pixel. The
   target was the Vulkan allocation at `0x200400000` (32 MiB, 2 MiB aligned).
2. **The viewport orientation is Vulkan's.** With y scale +height/2, clip
   y = -1 lands on the first row: the corner triangle filled the top-left
   half. It agrees with OpenGL's orientation on exactly half the pixels, as
   Vulkan's orientation predicts; OpenGL's would have agreed on all of them.
   The M3 probe's lower-half result with ProsperoLight's -height/2 is thereby
   confirmed as OpenGL's convention.
3. **AGC's culling defaults do not cull.** The flipped y scale reverses every
   triangle's winding relative to the runner's frames, and both triangles
   still drew. Culling remains unprogrammed until pipelines need it.
4. **AGC tolerates a second `sceAgcInit(8)`.** The runner initialises AGC at
   start, and each `vkCreateDevice` calls it again (once per test here). No
   driver message was logged and both devices were created.
5. **Two devices in one process.** They were created and destroyed in turn,
   with their own queues, memory and stage workspaces, and the runner's own
   frame still rendered afterwards.
6. **Target cache eviction.** The queue's eviction of target cache lines
   before submission and after the marker produced exact readbacks.

### Consequences

- B7's pass condition holds: the M2 triangle, exact, through the Vulkan API.
- The draw path is ready to grow into the Phase C tutorial steps. Each refused
  feature (clears, vertex buffers, descriptors, depth, blending, other target
  sizes) joins as its probe proves the registers.

## 2026-09-15: B8, command streams in several buffers

### Why

A Vulkan application can split its work across draws, command buffers,
`VkSubmitInfo`s and `vkQueueSubmit` calls. There are two ways to hand that to
AGC:
- **Copy (since B5).** The driver copies every command buffer of a submission
  into one stream. The console has run such streams only from the runner,
  never several command buffers recorded by the driver.
- **PM4 INDIRECT_BUFFER (`0x3F`).** The submitted stream points the GPU at
  each command buffer where it lies, as RADV does. That would avoid the
  copies, and `vkCmdExecuteCommands` could call secondary command buffers. No
  AGC helper writes the packet and no reference title uses it, so only the
  console can say whether AGC runs it.

### What was built

**B8a, runner test `b8-indirect` (set `m2`, no driver).** It draws the
`b4-headless` frame three ways, each read back exactly:
1. The submitted stream calls one buffer holding the state tables and the
   draw, then ends with the barrier and the completion marker.
2. It calls a buffer holding the state tables, then one holding the draw.
3. It only chains to a buffer holding the whole frame, including the marker,
   as RADV chains its growing streams.

The packet encoding follows Mesa (`src/amd/registers/pkt3.json` `IB_CONTROL`,
`ac_emit_cp_indirect_buffer`): header `0xc0023f00`, address low, address high,
then the size in words with bit 23 VALID and, for a chain, bit 20 CHAIN.
- **Placement.** The buffers lie at stage + `0xd000` (`0x200029000` in the
  replay layout), inside the GPU-visible workspace. Every register table they
  load is validated like the submitted stream's.
- **Order.** A variant whose frame the GPU does not complete ends the test.
  The chain, the variant furthest from what AGC has run, comes last.
- **PC model.** The host layer now follows `INDIRECT_BUFFER` calls and chains
  into mapped memory when it completes markers (`complete_markers`, after
  Mesa's `ac_parse_ib.c`). That is a model for dry runs, not evidence.

**B8b, several draws through the driver.**
- **Shared program.** `driver/tests/ps5vk_triangle.c` now creates its
  objects once: up to two pipelines, a first render pass and a loading render
  pass, and two command buffers. It draws a frame in a chosen grouping:
  - one draw (B7);
  - two draws in one command buffer;
  - one command buffer per draw, both in one `VkSubmitInfo`;
  - two `VkSubmitInfo`s in one `vkQueueSubmit`;
  - two `vkQueueSubmit` calls.
- **Runner test `b8-groups`.** The first draw fills the target with the M2
  colour; the second draws the b7 corner triangle in a second colour
  (`probes/b8-corner`, `shaders/b8/second.frag`, `0xff6030f0`).
  `check_split_frame` requires the top-left half in the second colour, the
  bottom-right half in the first, no other colour and no padding, in every
  grouping.
- **PC test `vk_b8_groups_test.c`.** It draws the M2 pipeline twice in each
  grouping, since the replay's one stage serves one pipeline.
  `tools/check-driver.sh` compares every recorded submission with
  `b4-headless`, its draw section repeated (`golden.py compare-submission
  --draws`).

### Found while testing

**Mesa merges submit infos.** Two `VkSubmitInfo`s of one `vkQueueSubmit`
reach the driver as one submission of two draws. In immediate mode, Mesa's
queue merges consecutive submits that have no signal between them
(`vk_queue_submits_merge`). The PC test first expected six submissions and
recorded five: 2, 2, 2, 1, 1 draws.

### PC verification

- **`b8-indirect` dry run.** The PC runner replayed `b4-headless` and ran all
  three variants: the streams and buffers encode as intended, every table
  validates, every submission starts, and the host finds each marker through
  the buffers. The readbacks fail only because the PC renders nothing, as in
  the B5c dry run.
- **`tools/check-driver.sh`.** B7 4 of 4 and B8 6 of 6, through the loader
  and directly. Every recorded submission is identical to `b4-headless` with
  its draw repeated, apart from the intended register `0x111`:
  - B7: one submission of 6 packets and 3 register tables;
  - B8: three submissions of two draws (10 packets and 6 register tables
    each), then two of one draw.
  
  The B2 to B6 tests are unchanged and pass.
- **Golden rebuilds** with the host layer that follows indirect buffers:
  runner 10/10, b4 3/3, b5 4/4, b5c 2/2.
- **Titles.** All five build with 0 warnings; runner digest
  `370a2e55639c938d`.

## 2026-09-15: Console run pid 122, several buffers

Queue `jobs/b8`, klog `Klog_Logs/klog-20260915-150247.log` (not committed).
Runner PPSA99988 digest `370a2e55639c938d`.

| Test | Result |
|---|---|
| `b4-headless` | PASS |
| `b7-triangle` | PASS, with the reworked program |
| `b8-groups` | PASS: all four groupings exact (details below) |
| `b8-indirect` | FAIL in variant 1 (details below) |
| `m2-solid` | not run: the runner stopped because a submission might still be in flight |

**`b8-groups`**, in every grouping (one command buffer, two command buffers,
two `VkSubmitInfo`s, two `vkQueueSubmit` calls):
- 4,139,524 of 4,139,524 top-left pixels hold the second colour;
- 4,139,524 of 4,139,524 bottom-right pixels hold the M2 colour;
- no other colour, and exactly 8,294,400 non-zero words, so no padding was
  written.

**`b8-indirect`, variant 1.** AGC accepted the 20-word stream (submission
result 0) and the suspend point returned after 27 µs, but the completion
marker was still 0 after 2,000 polls (2.2 s). At submission the kernel
reported:

```
# GPU Protection fault. hub:0 vmid: system process0 pid:0x0 client:CPG(6) access:Read permission:0x3
# reason: Unmapped page access, Protection fault addr(VA): 0x0000000200029000
# Raw fault info:0x00000c30 Timestamp:845029025999 REMAP err:0
```

A GPU status dump followed: CP, CPF, CPG and GUI_ACTIVE busy, every other
block idle, IB list rptr `0x26` of `0x28` used. In the log, the shell missed
its heartbeat for about 2 s and 127 frames were dropped; then the VR thread
resumed and `gfx_app_apu_reset_status` was 0. The runner kept every buffer of
the submission, stopped its queue as designed and logged the end of its run.
On screen, the console froze, and the runner crashed about 30 s later: the
log understates the effect.

### Findings

1. **Several draws, command buffers and submissions render exactly through
   the driver.** Copying every command buffer of a submission into one stream
   keeps draw order across command buffers, `VkSubmitInfo`s and
   `vkQueueSubmit` calls, including the merged submission of two submit
   infos. A render pass that loads the attachment keeps the previous
   submission's pixels.
2. **Titles cannot call command buffers in their own memory.** The fault
   address is exactly the indirect buffer, `0x200029000`, in the same address
   window from which the GPU reads the stream's register tables and into which
   it renders. The command processor's fetch of it fails as an unmapped page
   and is attributed to the system context (vmid system, pid 0), not to the
   title. So AGC runs a submitted stream where a title's GPU addresses are
   valid as data but not as command buffers. Variants 2 and 3 fetch the same
   memory and were not run.
3. **The fault freezes the console and kills the title.** The kernel log
   reports a stall of about 2 s without a reset, but the console froze and the
   runner crashed about 30 s later. The console itself recovered: FTP and klog
   answered afterwards.

### Consequences

- The driver keeps copying command buffers into its submission buffer
  (`driver/ps5vk_queue.c` is unchanged). Secondary command buffers
  (`vkCmdExecuteCommands`) will be copied inline the same way.
- `b8-indirect` stays in the runner as the recorded probe but must not be
  queued again: it freezes the console and crashes the title. The runner
  leaves it out of its default and `all` queues, so only a queue that names
  it runs it.
- Phase B is complete: an ordinary Vulkan program draws exactly through the
  driver, with fences and several command buffers and submissions. Phase C
  starts from here.

## 2026-09-18: the reported limits, audited against their source

Offline round (the console has not answered since the 2026-09-17 runs). B2's
other half is the limits table: `driver/ps5vk_physical_device.c` reports every
`VkPhysicalDeviceLimits` member at the value the Vulkan specification's Required
Limits table asks for, taking the "Unsupported Limit" column for the features
the device does not support, and until now nothing but review held the
transcription to its source -- the same review that read a mip level's base off
the wrong address (`docs/HARDWARE_FINDINGS.md`).

`tools/limits_audit.py` reads `limits-v1.4.354.adoc`'s Required Limits table
(the vendored copy `tools/setup-native-dependencies.sh` fetches), reads the
driver's initializer, and reports every member that does not meet the table.
Two footnotes are what make the comparison correct rather than merely strict:

- **Footnote 2**: `maxPerStageResources` must be at least the *smallest* of the
  sum of the per-stage descriptor limits and `maxColorAttachments`, or 128. The
  driver reports 44 (12 + 4 + 16 + 4 + 4 + 4), which a plain "at least 128"
  comparison would call a failure and the footnote makes correct.
- **Footnote 8**: the `maxDescriptorSet*` minimum is *n* times the
  corresponding per-stage minimum, where *n* is the number of shader stages the
  device supports -- three here, so 48 samplers rather than the table's 96,
  which assumes all six.

The audit also has to read the table the way adoc writes it: footnote markers on
names and values, `+`-continued cells, `2^27^` powers, masks written
`(ename:VK_SAMPLE_COUNT_1_BIT \| ename:VK_SAMPLE_COUNT_4_BIT)`, and rows the
spec repeats in its extension sections (the core row is the one a 1.0 device
answers to).

**The result: 106 required members, 95 compared, 0 that miss the table.** The
eleven it does not compare are the ones no number can answer: `pointSizeRange`,
`lineWidthRange` and their granularities are formulas (`64.0 - ULP`), the
interpolation-offset rows are formulas, and `standardSampleLocations`,
`timestampComputeAndGraphics`, `strictLines`, `timestampPeriod` and the two
recommendation alignments require nothing of a 1.0 device at all. Its `--check`
mode is the gate, and `tests/test_tools.py` runs it in `make test-integration`
(it skips when the specification is not fetched).

Nothing else changed: no driver source, no stream. The parked C8 resolve patch
is rebased in the same round after the component mapping's field collided with
it (commit 96db75a), and that collision is now a test of its own.


## 2026-09-18: the commands with no path refuse instead of crashing

A Vulkan 1.0 application that calls a core command this driver has no path for
asked for its entry point and got what the runtime's shared dispatch table had:
for most commands a trampoline or an implementation of its own, and for some
nothing at all -- a NULL the loader turns into an abort. Which commands those
are took **two measurements**, and the first one was wrong in a way worth
recording.

**The wrong reading.** A probe called each candidate on a command buffer and
watched the signal (one fork per call), and eight commands died with SIGSEGV:
the `vkCmdBeginRenderPass` family, `vkCmdDispatch` and its indirect form,
`vkCmdFillBuffer`, `vkCmdUpdateBuffer` and `vkCmdCopyImageToBuffer`. Refusing
the three render pass commands turned the tree red: `c8_msaa`, `c7_blit_formats`,
`v0_query_full`, `v0_robust`, `v0_formats` and `d1_dynamic_ubo` all failed with
`vkCmdBeginRenderPass has no path in this driver` in their logs. The reason is
that Mesa's meta operations -- the colour clears the driver asks `vk_meta` for --
**run through the render pass API**, which the runtime implements over the
driver's dynamic rendering. The probe had called those commands with a NULL
render pass and a NULL framebuffer: the crash was its own invalid usage, not a
gap. A command's entry point existing says nothing about whether the call behind
it is implemented.

**The reading that holds.** Every candidate was called again with the handles it
needs -- a buffer and an image created and bound to memory -- and only the
genuine gaps still crashed. `driver/ps5vk_refusals.c` is those, six entry
points, each refusing by name with the phase that would implement it:

| Command | Before | After |
| --- | --- | --- |
| `vkCmdFillBuffer`, `vkCmdUpdateBuffer`, `vkCmdCopyImageToBuffer` | SIGSEGV with valid handles | command buffer in error, message names the transfer path |
| `vkCreateComputePipelines` | NULL from the ICD | `VK_ERROR_UNKNOWN`, message names D2 |
| `vkCmdClearColorImage`, `vkCmdClearDepthStencilImage` | NULL from the ICD | command buffer in error, message names the attachment clears |

The render pass family, the two dispatch commands and everything else the
runtime answers are left alone: they are paths the driver reaches through
`vk_common_*`, and refusing them was the round's mistake. Nothing in this file
renders, copies or dispatches, so no stream changed either way.

**Verified on this host.** `driver/tests/vk_b2_commands_test.c` calls all six and
asserts the refusal: 7 of 7 checks in the loader and the direct arms, and the PS5
link accepts the test. The valid-handle probe reports "returned" for the three
transfers where it reported CRASH before. `tools/build-driver.sh` builds 19 host
and 19 PS5 sources with no warning. The full `tools/check-driver.sh` (every test,
three arms, both negatives), `make lint`, `make ci`, `make test`,
`make inspect`, `tools/check-psbc-link.sh`, `tools/check-vulkan-runtime.sh` and
`tools/build-all-titles.sh` are recorded with the round's commit. The console is
off the network for the eighteenth round running, so nothing was deployed.

## 2026-09-18: a second sweep, and the commands it found

The round before this one added entry points for six commands whose calls landed
on empty dispatch slots, and the method it settled on -- call the command, in a
fork, with the handles a valid call needs -- is what this round ran over the rest
of Vulkan 1.0. The sweep builds an instance, a device, a queue, a command pool, a
command buffer, a buffer and an image (each bound to host-visible memory), an
image view, a sampler, a render pass and framebuffer, a fence, an occlusion and a
timestamp query pool, and a semaphore, then calls every core command whose
arguments those objects can make valid, one fork per call, and reports the
signal. Every crash is called a second time with the handles the command's valid
usage needs, because a crash from an empty handle says nothing (that was the
previous round's mistake, and this sweep reproduced it four times:
`vkGetImageSubresourceLayout` needs a linear image, `vkCmdSetEvent` a valid
event, `vkCmdResolveImage` a four-sample source and a different destination, and
`vkCmdWriteTimestamp` a timestamp pool).

Seven more commands have no path, and each refuses by name now
(`driver/ps5vk_refusals.c`):

| Command | Before | After |
| --- | --- | --- |
| `vkCreateEvent` | no entry point: a call crashes | `VK_ERROR_UNKNOWN`, message names the unprobed `vkCmdSetEvent` family |
| `vkCreatePipelineCache` | no entry point | `VK_ERROR_UNKNOWN`, message names B6's compiler-built pipelines |
| `vkCreateBufferView` | no entry point | `VK_ERROR_UNKNOWN`, message names D2's texel buffers |
| `vkCmdExecuteCommands` | SIGSEGV with a valid secondary buffer | command buffer in error, message names B8's faulting INDIRECT_BUFFER |
| `vkCmdWriteTimestamp` | SIGSEGV with a timestamp pool | command buffer in error, message names V0-query's occlusion counters |
| `vkCmdCopyQueryPoolResults` | SIGSEGV with a timestamp pool, a bound buffer and a reset-and-write before it | command buffer in error, message names `vkGetQueryPoolResults` |

That is five recording commands and two creations; the matching destructors
(`vkDestroyEvent`, `vkDestroyPipelineCache`, `vkDestroyBufferView`) are no-ops,
because their objects can never exist. The event commands
(`vkCmdSetEvent`, `vkCmdResetEvent`, `vkCmdWaitEvents`) needed no entry point of
their own: they are unreachable once the event they would name cannot be created.

One more was missing for the same reason and is **implemented** rather than
refused: `vkGetDeviceMemoryCommitment`. Its entry point was absent too, but the
answer is the driver's own state -- `ps5vk_AllocateMemory` maps the whole
allocation into the address window at once and the device reports no lazily
allocated memory -- so the commitment is the allocation's size
(`driver/ps5vk_memory.c`), which the test checks with a 4096-byte allocation.

What the sweep did **not** settle, and why: `vkGetImageSubresourceLayout` cannot
be called validly at all, because it requires a linear-tiled image and the driver
reports no linear tiling features; `vkCmdResolveImage` crashes without the parked
C8 patch and is that patch's to implement; `vkCmdDispatch`,
`vkCmdDispatchIndirect`, `vkCmdDrawIndirect` and `vkCmdDrawIndexedIndirect` need
a compute or graphics pipeline this sweep does not build, so their slots are
still untested -- the indirect draws are B8's faulting route and the dispatch pair
is D2's.

**Verified on this host.** `driver/tests/vk_b2_commands_test.c` now calls all
thirteen commands and the commitment query: 14 of 14 checks in the loader and the
direct arms, and the PS5 link accepts the test. The sweep probe reports
"returned" for each of the seven where it reported a crash or a NULL entry point.
`tools/build-driver.sh` builds 19 host and 19 PS5 sources with no warning. The
full `tools/check-driver.sh` (every test, three arms, both negatives),
`make lint`, `make ci`, `make test`, `make inspect`,
`tools/check-psbc-link.sh`, `tools/check-vulkan-runtime.sh` and
`tools/build-all-titles.sh` are recorded with the round's commit. The console is
off the network for the eighteenth round running, so nothing was deployed.

## 2026-09-18: the indirect draws, and what "present" means for an entry point

The sweep left four commands untested because they need a pipeline: the two
indirect draws (B8's faulting route) and the two dispatch commands (Phase D2).
This round built the pipeline for the indirect draws and found that the answer
did not need one.

**The chain, read out of the runtime.** `vkCmdDrawIndirect` is not the driver's:
Mesa's shared table answers it with `vk_common_CmdDrawIndirect`
(`vk_command_buffer.c`), which forwards straight to
`cmd_buffer->base.device->dispatch_table.CmdDrawIndirect2KHR` -- the *driver's*
slot. Querying the driver's table for those names, with a device in hand, gives:

| Name an application calls | What it resolves to | The slot behind it |
| --- | --- | --- |
| `vkCmdDrawIndirect` | the runtime's trampoline | `vkCmdDrawIndirect2KHR` **NULL** |
| `vkCmdDrawIndexedIndirect` | the runtime's trampoline | `vkCmdDrawIndexedIndirect2KHR` **NULL** |

So a call with valid usage -- a graphics pipeline bound, a valid indirect buffer,
inside a render pass -- jumps through NULL and crashes. That is the same
mechanism as the thirteen commands the two earlier sweeps found, and it is what
`driver/ps5vk_refusals.c` refuses now:

| Command | Before | After |
| --- | --- | --- |
| `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect` | NULL jump with a valid pipeline bound | command buffer in error, message names B8's faulting INDIRECT_BUFFER |

**How this round nearly got it wrong, again.** The probe that was meant to
observe the crash built a graphics pipeline and a 3840x2160 target and recorded
through the 1.0 render pass API -- and every recording, *including a control with
no draw at all*, ended with `vkEndCommandBuffer` returning `VK_ERROR_UNKNOWN`.
Reading that as "the indirect draws crash" would have been the third wrong
conclusion of this series. The control is what stopped it; the cause of the
refusal was not pinned down (every object the probe creates reports success, the
image and render pass match what `driver/tests/ps5vk_triangle.c` creates, and the
driver's own message did not reach the probe's debug messenger), so the evidence
recorded here is the entry-point chain rather than a call. Two lessons for the
next probe: a *control recording* is what makes a crash mean anything, and a
command whose name resolves is not a command that has an implementation -- the
runtime answers a name with a trampoline and the trampoline's target is the thing
to query.

**Verified on this host.** `driver/tests/vk_b2_commands_test.c` calls all fifteen
refused commands: 16 of 16 checks in the loader and the direct arms, and the PS5
link accepts the test. `tools/build-driver.sh` builds 19 host and 19 PS5 sources
with no warning. The full `tools/check-driver.sh` (every test, three arms, both
negatives), `make lint`, `make ci`, `make test`, `make inspect`,
`tools/check-psbc-link.sh`, `tools/check-vulkan-runtime.sh` and
`tools/build-all-titles.sh` are recorded with the round's commit. The console is
off the network for the eighteenth round running, so nothing was deployed.

## 2026-09-18: events and pipeline caches, and the wait that had to keep time

Two more families of the 1.0 API are implemented, both of them objects and
commands the driver answers from its own state rather than work the GPU runs.
`driver/ps5vk_refusals.c` is down to **six** refusals: `vkCreateComputePipelines`,
`vkCmdClearDepthStencilImage`, `vkCreateBufferView`, `vkCmdExecuteCommands`,
`vkCmdWriteTimestamp` and `vkCmdCopyQueryPoolResults`.

**Events (B5) are the binary sync flag with Vulkan's rules.**
`driver/ps5vk_sync.c` creates an event as the flag a fence already is -- a
`bool` under a mutex -- and `vkSetEvent`/`vkResetEvent`/`vkGetEventStatus` read
and write it from the host. The three recording commands are the mechanism the
CPU-side transfers use: `vkCmdSetEvent`, `vkCmdResetEvent` and `vkCmdWaitEvents`
each record a **split with an action on it** -- the record `ps5vk_cmd_buffer_split`
already makes for a render-to-texture sample, with nothing to copy -- and the
queue runs the action where that split falls in the words. That is what makes
Vulkan's wording true here: the step before the split has been submitted and its
completion marker waited for before the flag is touched, so a set means
"everything recorded before it has completed", and a wait holds the words after
it until the flag is set. The memory barriers `vkCmdWaitEvents` carries are the
split itself, which flushes the same targets and caches a C4 split does.

**The core spelling is the entry point, and that is a measurement.** The
indirect-draw round found that `vkCmdDrawIndirect` reached the runtime's
trampoline and jumped through a NULL `vkCmdDrawIndirect2KHR`; the answer here is
narrower than "the 2-spelling is always the one to define". Mesa's
`vk_device_dispatch_table_from_entrypoints` adds common entry points "without
overwriting driver-provided ones" (`vk_device.c`): the driver's table is built
from **weak** symbols, so a name the driver defines wins, and the common
implementation only fills a slot the driver left NULL. Events are the case that
shows it: `ps5vk_CmdSetEvent`, `ps5vk_CmdResetEvent` and `ps5vk_CmdWaitEvents`
(the 1.0 spellings) are what an application's calls reach, no `...Event2`
spelling is needed, and the debug trace of the first test run shows the queue's
action records appearing for exactly those three commands. The draw round's
`2KHR` conclusion was about a name the driver had *not* defined.

**A wait that used a timed condition variable did not wait.**
`driver/tests/vk_b5_events_test.c` measures it: the last case sets the event from
another thread 150 ms into a submission that waits for it and requires the
submission to have taken at least half that. The first version of the wait used
`cnd_timedwait` in the completion-marker poll's shape -- a millisecond at a time
for two seconds -- and the test caught two thousand polls completing in **67
microseconds**: the runtime's c11 wrapper for a *timed* wait returns without
waiting on this build, so the device was lost before the host's set arrived. The
second version used the `sceKernelUsleep` the marker poll uses, and the PC runner
deliberately does not implement it ("The PC runner does not wait: nothing on the
PC completes asynchronously", `host/ps5/ps5_host.cpp`), which is the same
failure. The committed wait sleeps with **`os_time_sleep`** -- `clock_nanosleep`
on Linux, `usleep` on the console's POSIX -- so it keeps time on both, and the
timing check is what says so. This is the first driver wait that has to keep time
on the PC, because a host thread is the thing that sets an event there. An event
nothing ever sets is a lost device with its own message after the same two
seconds, not a hang.

**Pipeline caches (B6) are the empty cache an implementation may have.**
`driver/ps5vk_pipeline_cache.c` creates a cache object, reports **zero bytes**
from `vkGetPipelineCacheData` (both the size query and the data query, leaving
the application's buffer untouched), accepts `vkMergePipelineCaches` as a no-op
and frees the object; `vkCreateGraphicsPipelines` still ignores the handle it is
given, which is the same statement. The specification allows this because the
data in a cache is an optimization and the implementation decides what a cache
holds, and the device's `pipelineCacheUUID` is the one constant identifier for
the one format this driver has. No probe was needed and no console run is owed:
nothing in the four entry points reaches the GPU or the command stream -- unlike
the events, whose recorded commands still want a console run. That run needs a
runner case that records the three commands and reads the flag back first: the
driver tests are built for the console but only *linked* there
(`tools/check-driver.sh`'s third arm), so `vk_b5_events_test` cannot be a queue
line.

**Verified on this host.** `driver/tests/vk_b5_events_test.c` passes 11 of 11
checks in the loader and the direct arms (an unsignaled new event, host set and
reset, a set and a reset recorded in a submission, two actions in one command
buffer in record order, a wait passed by a set earlier in the same command buffer
and by one in an earlier command buffer of the same submission, and the wait
blocked and released by another thread's `vkSetEvent`).
`driver/tests/vk_b6_pipeline_cache_test.c` passes 6 of 6 in both arms
(the UUID, the object, the zero-byte data both ways round with the buffer
untouched, the merge and the merged cache still empty).
`driver/tests/vk_b2_commands_test.c` is 8 of 8 in both arms now: the two object
creations it used to assert refusals for are objects, and the six remaining
refusals are still refused by name. Both new tests are in `tools/check-driver.sh`'s
list and their PS5 links are accepted (170 imports each, so `pthread_create`,
`clock_gettime` and the driver's own sleep all link for the console).
`tools/build-driver.sh` builds 20 host and 20 PS5 sources with no warning. The
full `tools/check-driver.sh` (every test, three arms, both negatives),
`make lint`, `make ci`, `make test`, `make inspect`,
`tools/check-psbc-link.sh`, `tools/check-vulkan-runtime.sh`,
`tools/check-mip-layout.sh`, `tools/limits_audit.py --check` and
`tools/build-all-titles.sh` are recorded with the round's commit. The console is
off the network for the twentieth round running, so nothing was deployed.

## 2026-09-18: the driver runner on the PC, and the case the console will run

The events' recorded commands still owe a console run, and a console run needs a
runner case -- but the runner's *driver* cases were the one part of the tree that
could not be exercised on the PC: `tools/build-host-runner.sh` compiled the
runner without `AGC_VULKAN_DRIVER=1`, so every `run_vulkan_*` case was outside
its table and a new one could not be verified before it was committed. This round
made them runnable and added the events case.

**The driver runner.** `bash tools/build-host-runner.sh --driver` builds
`build/host/runner_host_driver`: the same runner with `AGC_VULKAN_DRIVER=1`, the
B7 program `driver/tests/ps5vk_triangle.c` (compiled as C, the way
tools/check-driver.sh compiles it) and the driver's host archives
(`build/driver/host/libps5vk.a` and Mesa's runtime whole, with
`libpsbc_driver.pic.a` in place of the SDK's `libpsbc.a`) -- the link
tools/build.sh makes for the console title. The AGC-level runner is unchanged: it
still builds `build/host/runner_host`, and `tools/golden.py rebuild golden/c0`
reports 2 of 2 frames identical with it.

**A replay is not one thing.** The first attempt at a driver case failed in
`vkCreateDevice`: "the queue's submission buffer could not be mapped in the
address window: 0x8002000c". The runner's replay hands *every* allocation a
captured region (`g_strict_replay`, host/ps5/ps5_host.cpp), while the driver maps
its own memory exactly as it does on the console, and the capture had no 2 MiB
region to give the queue's buffer. `ps5_host_load_replay` takes a `regions` flag
now, which `runner_host_main` exposes as `--memory free|replay`: `free` keeps
direct memory free and takes the register defaults and the pipeline stage images
of the replay alone -- what a driver case needs -- and `replay` is the AGC-level
behaviour. With `--memory free` and a replay taken from a *driver* golden,
`v0-formats` (a driver case that draws nothing) runs on the PC and reports 30 of
30 formats as audited, which is what says the new binary really drives the driver
and not a stub.

**The events case (`b5-events`).** `run_vulkan_events` (src/diagnostics.cpp,
inside the `AGC_VULKAN_DRIVER` region) creates an event and records eight cases:
a new event is unsignaled; the host's set and reset move the flag; a recorded set
and a recorded reset; the two of them in one command buffer, where the last one
recorded holds; a wait the same command buffer passes; a wait an *earlier*
command buffer of the same submission passes; and a wait the host's set before
the submission passes. Each is submitted with a fence, the flag is read back, and
each is logged as its own `b5_events_case` record with `cases` and
`cases_passed` and one PASS or FAIL. The case count is compared with the number
of cases the function records, so a case that stops running fails the test
instead of shrinking it (the first version hard-coded seven of eight and failed
the run while all eight passed). On the PC the case reports **8 of 8 passed** and
the runner's summary "1 of 1 queued tests passed". Its console run is its own
proof and its golden will be the report, not a frame; the PC counterpart stays
`driver/tests/vk_b5_events_test.c`, which also covers the one thing a console
case here cannot -- a wait released by another thread's `vkSetEvent`.

**Verified on this host.** `tools/build-host-runner.sh --driver` builds with no
warning and `run_vulkan_events` reports 8 of 8; the AGC-level runner still
rebuilds `golden/c0` 2 of 2 identically; `make lint` (161 files), `make test`
(15 tests), `make ci`, `make inspect`, `tools/build-all-titles.sh` (5 titles, 0
warnings) and the full `tools/check-driver.sh` are recorded with the round's
commit. The console is off the network for the twenty-first round running, so
nothing was deployed; `b5-events` is queued in `jobs/c8-resolve/queue.txt` for the
first battery, which is where the events' console proof will come from.

## 2026-09-18: the C2 transfers case, and what a driver case can do on the PC

The six CPU-side commands still owed a console run, so the second driver runner
case is `c2-transfers`: `run_vulkan_transfers` (src/diagnostics.cpp) creates a
256-byte transfer buffer in mapped memory, records a fill over its first 64
bytes, a fill followed by an update of the bytes that fill wrote (overwriting the
update's source between the call and the submission, which is what proves the
snapshot), and a `VK_WHOLE_SIZE` fill from an offset, submitting each with a fence
and reading every word back out of the mapped buffer. Six checks, counted, with
`checks` and `checks_passed` and one PASS or FAIL -- **6 of 6 passed** on the PC
through `build/host/runner_host_driver --memory free`, and the case is queued in
`jobs/c8-resolve/queue.txt` beside `b5-events` for the first console battery.

**A driver case that draws cannot be run on the PC through the runner yet.**
The round first added `c2-indirect` -- the same triangle through
`vkCmdDrawIndirect`, a two-line wrapper around the frame case -- and its run
failed at the *recording*:

    b7_vk_message FAIL  a created shader's register tables are out of bounds (VK_ERROR_UNKNOWN)
    b7_record_command_buffer FAIL  one draw

The check is `ps5vk_pipeline.c`'s: `tables->cx` and `tables->sh` have to lie
inside the stage mapping the AGC model built. An **existing** driver draw case
fails identically in the same environment (`c2-staging`, same message, same
point), so the cause is the runner's, not the new case's: the runner's
linked-canary prelude runs before the queued case and leaves the AGC model
without the pipeline capture a draw's register tables come from, which the driver
tests get because they load the replay themselves (`PS5_HOST_REPLAY`). The
wrapper was therefore reverted rather than committed: the indirect draws' PC
proof stays `driver/tests/vk_c2_indirect_test.c` (5 of 5, its submission identical
to the console's b4-headless frame), and a runner case for them has to wait for
either the model-state fix or its own console replay. Both facts are written into
`docs/TESTING.md`, where the runner recipe lives, so the next case does not
rediscover them.

**Verified on this host.** `build/host/runner_host_driver --memory free` reports
`v0-formats` 30 of 30 formats as audited, `c2-transfers` 6 of 6 and `b5-events`
8 of 8 in one queue, with the runner summary "3 of 3 queued tests passed";
`tools/build-host-runner.sh --driver` builds with no warning; `make lint`
(161 files), `make test` (15 tests), `make inspect`, `tools/build-all-titles.sh`
(5 titles, 0 warnings) and `make ci` are recorded with the commit. The console is
off the network for the twenty-first round running, so nothing was deployed.

## 2026-09-18: what a driver draw case can and cannot do on the PC

The previous entry left one thing open: a driver runner case that *draws* fails
on the PC with "a created shader's register tables are out of bounds", and an
existing case (`c2-staging`) fails the same way. This round measured why, so the
next person does not have to.

**The numbers.** `ps5vk_pipeline.c`'s check reads the two register-table pointers
out of the linked shader and requires them inside the stage mapping. A temporary
print of the four values, in one round, gives:

| Where | Stage mapping | `tables->cx` | `tables->sh` | Result |
| --- | --- | --- | --- | --- |
| `tools/check-driver.sh c2_staging` | 0x200038000 | 0x200038090 | 0x200038060 | PASS |
| the same case through `runner_host_driver` | 0x20000c000 | 0x78 | 0x40 | FAIL |

Absolute addresses inside the stage on one side, bare offsets on the other: in
the runner the driver's linked shader output was never written at all.

**Why.** A replay carries the pipelines *one* console run built, and
`golden/b5/b4-headless-1.json` -- the replay a driver draw case would use -- has
no `stage N` region at all, only the runner's own 64 KiB `stage` workspace, so
nothing replays the link either. And the runner cannot simply run the case first:
`run_linked_agc_canary` *is* the runner's entry point (the canary, then the
queue), so a build that skips it runs nothing -- exit 0, no records at all, which
is what the experiment returned. The host AGC model serves the runner's own
AGC-level canary and a driver's AGC calls in one process, and the second of the
two does not get valid link output; the real AGC on the console has no such
problem, which is exactly why a drawing driver case is proven there and not here.

Both facts are in `docs/TESTING.md` beside the runner recipe. The driver draws'
PC proof stays `tools/check-driver.sh`, which loads the replay itself
(`PS5_HOST_REPLAY`) and works.

## 2026-09-18: the C7 clear and readback case, and three of the case's own bugs

The third driver runner case is `c7-clear`: `run_vulkan_image_clear` clears a
linear `R8G8B8A8_UNORM` level whole and reads it back tight, clears a tiled colour
attachment through its tile map, clears level 1 of a two-level image, reads a
region back into a buffer it filled with a sentinel first, and reads the whole
image back with `bufferRowLength` rows -- comparing bytes, with `checks` and
`checks_passed` and one PASS or FAIL. **7 of 7 passed** on the PC through
`build/host/runner_host_driver --memory free`, in a queue with the other three
(`v0-formats`, `c2-transfers`, `b5-events`: "4 of 4 queued tests passed"), and it
is queued in `jobs/c8-resolve/queue.txt` for the first console battery.

**The first version's failures were the case's, not the driver's**, and all three
are worth recording because the next case will meet the same shapes:

- It "cleared a region" and called the result wrong. `vkCmdClearColorImage` has
  no region: its `VkImageSubresourceRange` names a level and its layers and the
  clear covers that whole level -- offset and extent live in
  `VkBufferImageCopy`, which is the readback's. The image really was the clear
  colour everywhere, so the *whole-image* readback saw the colour at row 0 and
  the check blamed the driver. The case now checks the readback's own region
  handling instead: a region readback has to hold that region's texels and write
  nothing past the first byte after them.
- It expected a gap between the rows of a *tight* readback. There is none: with
  `bufferRowLength` 0 the pitch is the region's width, so the byte to check is the
  first one past the last texel of the last row, not the one past a row's width.
- It sized the readback buffer for tight rows (9,216 bytes) while the sentinel
  fill used the padded pitch (11,520), which wrote 2,304 bytes past the mapping.
  A runner case has to size its buffers for its widest case; the failure it
  produced looked like a driver bug in the *padded* readback (the padded case's
  data check failed while the gap check passed, which reads as "the readback did
  nothing").

The driver itself needed no change: the C7 clear and readback are the CPU work at
a submission split point they were, and `driver/tests/vk_c7_clear_image_test.c`
(25 checks) and `driver/tests/vk_c7_readback_test.c` (11) still pass unchanged.

**The recipe is a gate now.** `tools/check-runner-cases.sh` does what the
hand-run recipe did: it builds the driver and the driver-enabled runner when they
are missing, writes the queue and a replay taken from a driver golden, runs the
four cases and requires the runner's own summary to pass -- `runner cases: PASS
(191 PASS records)`. It takes case names as arguments (an unknown one is refused
with exit 2), and a new `tests/test_tools.py` case checks that every name in its
list is one `src/diagnostics.cpp`'s runner table holds, so a typo fails on the PC
instead of in the console's queue parser.

**Verified on this host.** One queue of the four driver cases reports "4 of 4
queued tests passed" (`v0-formats` 30 of 30 formats, `c2-transfers` 6 of 6,
`c7-clear` 7 of 7, `b5-events` 8 of 8), and `tools/check-runner-cases.sh` reports
the same four as PASS; `make lint` (162 files), `make test` (16 tests), `make ci`,
`make inspect` and `tools/build-all-titles.sh` are recorded with the commit. The
console is off the network for the twenty-second round running, so nothing was
deployed: `b5-events`, `c2-transfers` and `c7-clear` are all waiting in
`jobs/c8-resolve/queue.txt`.

## 2026-09-18: why a drawing driver case cannot run in the host runner, measured

The previous entry said a driver runner case that draws fails on the PC in
`ps5vk_CreateGraphicsPipelines` and that an existing case (`c2-staging`) fails
identically. This round found the mechanism, which is an allocation order in the
host model and not a driver bug.

A console replay of a driver frame carries the pipelines the run built --
`stage 0 0x200038000 0x10000` plus its `stageimage` chunks -- and
`host/ps5/ps5_host.cpp` hands a captured region to the **first allocation of its
size**, at its captured address, which is what makes the captured absolute table
pointers valid. A temporary trace of that handoff, with `c2-staging`'s own replay
in the driver-enabled runner, prints:

    DBG alloc request 0x10000
    DBG region pipeline0 at 0x200038000 size 0x10000 -> handed out
    DBG region tables-0x20002c000 at 0x20002c000 size 0x4000 -> handed out
    ...
    DBG alloc request 0x10000            <- the driver's own stage: nothing left

The first 0x10000 request is the *runner's* AGC-level stage workspace (one 64 KiB
half per linked package set, `src/diagnostics.cpp`), allocated by the queue's
`link_shader_packages` before any case runs; it takes the driver's captured
pipeline stage, so the driver's own stage allocation goes to free memory
(0x20000c000) and the capture's stage image is never used. The linked tables then
hold the compiler's unrelocated offsets (0x78 and 0x40) instead of the captured
addresses (0x200038090 and 0x200038060), and the driver's bounds check refuses
the pipeline. The driver test passes because it loads the replay itself with
`PS5_HOST_REPLAY` and has no runner workspace at all.

Fixing it is a host-model change -- the runner's workspace and a driver's stage
have to coexist in one replay, e.g. by giving the workspace its own
(non-`stage`-indexed) region or by letting a driver case skip the AGC-level
package staging it does not use -- and it is written down rather than worked
around, because the drawing cases' PC proof already comes from
`tools/check-driver.sh` and their console proof is the console's own run.

**A queue name is guarded now.** A console queue is rejected whole when one line
names a case the runner does not have, which is why the `c8-resolve` battery
needs its parked patch deployed first. `tests/test_tools.py` walks every
`jobs/*/queue.txt` and requires each name to be a case `src/diagnostics.cpp`'s
table holds or one a parked `*-wip.patch` adds (verified by appending a bogus name
to the battery's queue, which fails the test, and removing it, which passes).

## 2026-09-18: the driver-case path in the host runner, and the indirect draws

The previous entry measured why a drawing driver case refused its pipeline in the
host runner: the runner's own AGC-level stage workspace (64 KiB, allocated before
any case runs) was handed the captured pipeline stage, so the driver's stage went
to free memory and its linked tables kept the compiler's unrelocated offsets.
This round fixed it rather than working around it.

**`--cases driver`.** A case that goes through the Vulkan driver brings its own
device and its own shaders (`ps5vk_triangle_create`, `vk_icdGetInstanceProcAddr`),
so it needs none of the AGC-level package staging `run_test_queue` does for every
case. The host runner now says so with `--cases driver`, and `run_test_queue`
skips `link_shader_packages` for the whole queue (the guard is
`PS5VK_HOST_BUILD`, so a console run never takes the path and its klog keeps the
staging records a golden's replay is extracted from). `tools/check-runner-cases.sh`
passes the flag, and its four non-drawing cases still pass (139 PASS records
against 191 before, the difference being exactly the AGC-level staging records).
The gate covers the drawing case too, in the one way the PC can: `c2-indirect` is
run on its own with the replay its golden came from (the register defaults a
replay carries are what the SH records come from, so another run's replay differs
there -- 0x08b = 0x00100006 against 0x00100004, which the first version of the
gate showed) and its dumped submission is compared with
`golden/b5/b4-headless-1.json`: "c2-indirect PASS (submission identical)". Its own
record still reports the frame as failed, which is what the console run is for.

**A drawing driver case runs on the PC now.** `c2-staging` with its own replay
gets past `vkCreateGraphicsPipelines`, records its command buffer and submits --
the pipeline-creation refusal is gone. Its *frame* checks still fail, and that is
correct: nothing renders on the PC, so the frame is what the console run proves.
The `--cases driver` path is what the frame's replay needs for its pipeline stage,
which is why the earlier note that a drawing case "cannot run in the runner" was
half right: it could not *create its pipeline*, and now it can.

**The indirect draws (C2) are a runner case.** `c2-indirect` is
`run_vulkan_indirect_frames`, a wrapper around the frame case with the harness's
`indirect` flag set, so the case draws the same M2 triangle through
`vkCmdDrawIndirect` -- the parameters read from the bound buffer when the draw is
recorded (driver/ps5vk_draw.c). On the PC its submission was dumped and compared
with the console's own b4-headless frame:

    ind.dump submission 1 (1 draw): identical to b4-headless-1.json: 6 packets,
    3 register tables; expected differences: register 0x111 = 0x44870000
    (console 0xc4870000); user data at SH 0x0c, written by the driver; user data
    at SH 0x8c, written by the driver

which is the same comparison `driver/tests/vk_c2_indirect_test.c` makes (5 of 5).
The case is queued in `jobs/c8-resolve/queue.txt`, where its console run proves the
frame itself.

**Verified on this host.** `tools/check-runner-cases.sh` PASS (4 cases, 139 PASS
records) with `--cases driver`; the indirect submission identical to
`golden/b5/b4-headless-1.json`; `make lint`, `make test`, `make ci`, `make inspect`
and `tools/build-all-titles.sh` are recorded with the commit. The console is off
the network for the twenty-third round running, so nothing was deployed.

## 2026-09-18: the format audit's gaps are a check now, not a reading

The V0-formats step's acceptance is "every required format must either carry the
correct feature bits or be recorded as a deliberate gap with its tile-layout
reason", and `tools/format_audit.py` has been the tool for the first half since it
was written: it reads `formats-v1.4.354.adoc`'s Mandatory Format Support tables,
reads the driver's table in `driver/ps5vk_image.c`, and prints every required
feature no entry carries. What was missing is the check on the second half -- the
record -- so nothing noticed when the two drifted.

`docs/V0_FORMATS_AUDIT.md` now carries the audit's list as well as its families:
the four counts it has always had, and the 55 rows of formats and the features no
entry carries. Two `tests/test_tools.py` cases (`FormatAuditTests`) run the audit,
parse its summary and its list, and require the document to agree, so a driver
change that closes a gap, opens one, or changes which features a format is missing
fails until the document moves with it. Both were verified by breaking the
document (55 to 54 in the count, and one gap row deleted) and restoring it.

The counts it records today are the same they were: 179 formats required, 20
reported, 55 missing a required feature, 55 with conditional requirements only.
The gaps themselves are console work -- each closing path is named in the table
above them -- and `tools/format_audit.py --check` stays red until they are closed,
which is what makes the record worth checking rather than the tool.

**Verified on this host.** `python3 -m unittest tests.test_tools.FormatAuditTests`
(2 tests) PASS, and the same two cases fail on a document that disagrees with the
audit. `make lint` and `make test` run them with the rest of the suite. The
console is off the network for the twenty-fourth round running, so nothing was
deployed.

## 2026-09-18: vkCmdExecuteCommands, the last refusal that could be closed offline

The driver's refusals are down to **five** (compute, the depth clear, buffer views,
timestamps and the query copy). `vkCmdExecuteCommands` is implemented, and with it
the 1.0 API's secondary command buffers -- not through the PM4 `INDIRECT_BUFFER`
packet B8 measured as faulting the GPU, which is what the refusal used to say the
command would need, but by copying the secondary's recorded words into the primary
at the call, which is what the queue submits anyway.

**Two halves.** A secondary recorded with
`VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT` has no rendering of its own: it
runs inside the render pass and framebuffer its inheritance info names. So
`vkBeginCommandBuffer` now takes the same colour and depth registers
`vkCmdBeginRendering` takes, from the framebuffer's attachments
(`ps5vk_cmd_buffer_inherit`, driver/ps5vk_draw.c), and
`ps5vk_CmdExecuteCommands` (driver/ps5vk_cmd_buffer.c) appends the secondary's
words, moves its CPU work with them -- each record's `after_words` shifted by where
its words landed, with the update snapshots left to the secondary to free -- and
adds the targets it rendered into to the submission's, so the queue evicts their
cache lines and waits for the swapchain buffers among them. A secondary that
executes another is refused by name, as is a command buffer of the wrong level: the
driver would have to copy a nested execution twice.

**Proven packet-identical to a direct frame.** `driver/tests/vk_b8_secondary_test.c`
draws the M2 triangle through a secondary the primary executes, and
`tools/check-driver.sh b8_secondary` compares its submission with the console's own
b4-headless frame:

    b8_secondary_direct.dump submission 1 (1 draw): identical to b4-headless-1.json:
    6 packets, 3 register tables; expected differences: register 0x111 = 0x44870000
    (console 0xc4870000); user data at SH 0x0c, written by the driver; user data at
    SH 0x8c, written by the driver

which is the same comparison the direct (`b7_draw`) and indirect (`c2_indirect`)
frames get. The harness grew the mode the case needs -- a real secondary command
buffer allocated at the secondary level, `input.secondary` as its last field so
every earlier caller's positional initializer still names what it did, and the
frame's pass-interior recording split into `record_body` so the secondary records
the same body the primary wraps in its pass.

**The runner case and the gate.** `b8-secondary` (`run_vulkan_secondary_frames`) is
the same frame through the console runner. On the PC its submission is identical to
the same golden, and `tools/check-runner-cases.sh` covers it in its drawing list:

    v0-formats PASS, c2-transfers PASS, c7-clear PASS, b5-events PASS,
    c2-indirect PASS (submission identical), b8-secondary PASS (submission identical)

It is queued in `jobs/c8-resolve/queue.txt`, where the console run proves the
pixels.

The test also guards the two refusals the implementation keeps -- a primary named
as the buffer to execute, and a secondary that executes another -- by recording
each and requiring `vkEndCommandBuffer` to report the driver's message, so it
passes 6 of 6 checks in both PC arms.

**Verified on this host.** `tools/check-driver.sh b8_secondary`: loader PASS 6 of 6,
direct PASS 6 of 6, PS5 link 170 imports; the full `tools/check-driver.sh` (every
test, three arms, both negatives) PASSes, which also says the harness change moved
no other frame; `tools/check-runner-cases.sh` PASS (6 cases); `tools/build-driver.sh`
builds 20 host and 20 PS5 sources with no warning. The console is off the network
for the twenty-fifth round running, so nothing was deployed.

## 2026-09-18: the C8 patch rebased, and what the PC can say about its crash

The B8 work (previous entry) split the harness's frame recording into
`record_body` and put its own field at the end of `ps5vk_triangle_input`, which is
exactly where the parked C8 patch had put `resolve_output` and its hunk of the
recorder. `tests/test_tools.py`'s parked-patch guard was green through both of
those rounds only because it skips while the tree is dirty; the moment the tree was
clean it reported the conflict. The patch no longer applied to HEAD.

**Rebased.** Applied in three-way mode with both fields kept (`resolve_output`,
then `secondary`), rebuilt (20 host and 20 PS5 sources, no warning), and then
verified against the console's own goldens *with the patch applied*:

| Test | What it covers | Result |
| --- | --- | --- |
| `c8_msaa` | the four-sample frame and its storage | loader, direct and PS5 link PASS |
| `c7_tiled_mip` | the tiled mip chain's addresses | PASS |
| `c7_copy` | the tiled destination a copy wrote | PASS |
| `c4_rtt` | a render target sampled by a later draw | PASS |
| `c5_depth` | the depth attachment's registers | PASS |

so the patch changes no stream any console run has proved, which is the same
result `c2_instancing` and `c7_copy` gave before, now with the four-sample and
tiled-map frames added. The regenerated patch is 645 lines over seven files, applies
to the commit that carries it, and the guard passes on a clean tree.

**The crash is not host-visible.** With the patch applied, the driver-enabled host
runner ran the patch's own two cases:

    {"probe":"agc_resolve_step","field":"frame","value":0}
    {"probe":"agc_resolve_step","status":"INFO","detail":"creating"}
    {"probe":"agc_resolve_step","status":"INFO","detail":"created"}
    {"probe":"agc_resolve_step","status":"INFO","detail":"drawn"}
    {"probe":"agc_capture_submission","status":"INFO","detail":"one sample, no resolve"}
    {"probe":"runner_test","status":"PASS","detail":"c8-resolve"}

`c8-resolve` **passes** on the PC -- every step the patch's INFO records name, up to
the submission capture -- while `c8-msaa`'s storage check fails there for the
expected reason (nothing renders on the PC, and the four-sample storage a replay
carries is another run's). So the console crash that pids 170-176 show right after
the first frame's shader link is not a host-visible memory error in the patch's own
code: it lies in what only the console runs, the real AGC link and the GPU. The
patch's readiness is where it was, and now with the PC evidence that its
bookkeeping is sound; the bisect still needs the console.

**Verified on this host.** With the patch applied: the five tests above, three arms
each; `tools/build-driver.sh` and `tools/build-host-runner.sh --driver` build with
no warning. With the tree restored: `tests/test_tools.ParkedWorkTests` PASS (the
patch applies to HEAD), the parked patch is committed on its own, and the driver,
the AGC-level runner and the driver-enabled runner are rebuilt from the committed
sources. The console is off the network for the twenty-sixth round running, so
nothing was deployed.

## 2026-09-18: the 1.0 command surface is an audit now, and it is complete

Phase B2's promise is that the device's Vulkan 1.0 claim is true, and the part of
it that can be checked without a console is whether *every* 1.0 command is
answered at all: implemented by the driver, supplied by Mesa's runtime
(`vk_common_*`), or refused by name with the phase that would implement it. The
three sweeps that found the commands in none of those classes were manual -- one
call per fork, then every crash re-checked with the handles a valid call needs --
and nothing kept the answer complete afterwards.

`tools/command_audit.py` is that audit. It reads the Mesa registry's
`VK_VERSION_1_0` feature and follows its `depends` chain (the registry splits the
core feature: the commands are in `VK_GRAPHICS_VERSION_1_0` and its own
dependencies), then classifies each of the 138 commands it names:

| Class | How it is decided | Count |
| --- | --- | --- |
| driver | `driver/*.c` defines `ps5vk_<Name>` (or the promoted spelling the runtime forwards to: `...KHR`, `...2`, `...2KHR`) | 82 |
| refused | the definition is in `driver/ps5vk_refusals.c` and refuses, i.e. names the phase (`vkDestroyBufferView` is a no-op destructor there, not a refusal, so it counts as an implementation) | 5 |
| runtime | Mesa's runtime archive defines `vk_common_<Name>`, so the shared dispatch table answers the call | 51 |
| **gap** | none of the three: the call reaches an empty slot | **0** |

`--check` exits 1 while any command is a gap, and `tests/test_tools.py` runs it
with the counts as a gate, so a command that loses its implementation, its refusal
or its runtime entry point fails on the PC rather than in a console session. The
audit needs the fetched Mesa registry and the built runtime archive and skips
without them, like the format audit skips without the vendored specification.

The five refusals it lists are the five `driver/ps5vk_refusals.c` holds:
`vkCreateComputePipelines` and `vkCreateBufferView` (D2's compute probe and texel
buffers), `vkCmdClearDepthStencilImage` (the D32 transfer claim),
`vkCmdWriteTimestamp` and `vkCmdCopyQueryPoolResults` (`V0-query`'s clock). Every
one of them needs the console; the audit is what says there is nothing else.

**Verified on this host.** `python3 tools/command_audit.py --check` PASS (138
commands: 82 driver, 5 refused, 51 runtime, 0 gap); `python3 -m unittest
tests.test_tools.CommandAuditTests` PASS; `make lint` (164 files). The console is
off the network for the twenty-sixth round running, so nothing was deployed.

## 2026-09-18: the surface audit covers the exposed extensions too

The command audit above answered a question about the core API. The device makes a
second promise in the same breath: it exposes six extensions, and an extension
whose commands reach an empty slot is a promise it cannot keep. The audit covers
them now, and with the require blocks read properly:

    6 extensions are exposed:
      VK_EXT_debug_report                             3 commands: 0 driver, 0 refused, 3 runtime, 0 gap
      VK_EXT_debug_utils                             11 commands: 0 driver, 0 refused, 11 runtime, 0 gap
      VK_KHR_display                                 12 commands: 12 driver, 0 refused, 0 runtime, 0 gap
      VK_KHR_get_physical_device_properties2          7 commands: 5 driver, 0 refused, 2 runtime, 0 gap
      VK_KHR_surface                                  5 commands: 5 driver, 0 refused, 0 runtime, 0 gap
      VK_KHR_swapchain                               10 commands: 10 driver, 0 refused, 0 runtime, 0 gap, 4 conditional

Two things had to be right for that to mean anything, and the first version of the
extension audit was wrong in both:

- The registry declares a few extensions **without a body**, so pairing an opening
  `<extension>` with the next `</extension>` loses the extension that follows the
  bare one. The parse is by tag boundaries now (a bare tag is its own block), which
  is why `VK_KHR_surface` is in the table at all.
- A `<require>` may carry a `depends`, and those commands are required only of a
  device that meets it. `VK_KHR_swapchain`'s three presentation commands are under
  `<require depends="VK_VERSION_1_1">`: device groups, which this 1.0 device does
  not report. Counting them as gaps would have been a false finding; the audit
  reports them as conditional and leaves them out, and it counts the distinct
  commands of each surface (the core feature lists 137, not the 138 entries its
  dependency chain spells out with one name twice).

An extension command that is the KHR spelling of a core one counts as implemented
when the driver has the core spelling (`vkGetPhysicalDeviceFeatures2KHR` against
`vkGetPhysicalDeviceFeatures2`), which is how the properties2 extension's seven
commands come out as five driver and two runtime with no gap.

**Verified on this host.** `python3 tools/command_audit.py --check` PASS (137 core
commands and six exposed extensions, every surface with 0 gap);
`python3 -m unittest tests.test_tools.CommandAuditTests` PASS; `make lint` and
`make test`. The console is off the network for the twenty-seventh round running,
so nothing was deployed.

## 2026-09-18: the whole tree re-verified, and the C8 session's first three steps

Rounds 45 to 47 changed the driver (secondary command buffers), the harness
(`record_body`, a secondary command buffer), the host layer (`--cases driver`,
`--memory free`) and the tools (the command audit), so this round re-ran every
gate rather than trusting the per-change runs:

| Gate | Result |
| --- | --- |
| `tools/check-driver.sh` (full) | PASS: every test, three arms, both negatives |
| `tools/check-runner-cases.sh` | PASS: 4 non-drawing cases, `c2-indirect` and `b8-secondary` by submission (139 PASS records) |
| `make ci` | every reproduced step passed |
| `make inspect` | StaticErrors 0 |
| `tools/build-all-titles.sh` | 5 titles, 0 warnings |
| `make lint` / `make test` | 164 files / 20 tests, OK |
| `tools/command_audit.py --check` | 137 core commands + six exposed extensions, 0 gap |
| `tools/limits_audit.py --check` | 106 required members, 95 compared, 0 missing |
| `tools/check-psbc-link.sh`, `tools/check-vulkan-runtime.sh`, `tools/check-mip-layout.sh` | PASS |
| `tools/golden.py rebuild golden/{c0,c1,c1-clear,b4}` | 2 of 2, 9 of 9, 5 of 5 and 3 of 3 frames identical on the PC |

The last row is the one the host-layer change needed: the PC model still
reproduces those console captures exactly, which is what the driver tests'
replay comparisons rest on.

**The C8 session's first three steps**, from what the PC has already said. The
patch's crash happens right after the first frame's shader link, including on a
one-sample frame with no resolve and no clear, so the resolve executor is not the
suspect:

1. Apply the patch and run the battery's *existing* cases first -- `c2-instancing`,
   `c3-quad`, `c8-msaa`, `c7-copy` -- which the PC has under the patch and which
   still match their goldens. If one of those crashes on the console, the cause is
   in the patch's shared changes (`driver/ps5vk_image.c`'s map parameterisation or
   `driver/ps5vk_queue.c`), not in the new frames.
2. If they pass and the new frames crash, the patch's own per-step INFO records
   name the step: the PC run reaches `creating`, `created`, `drawn` and the
   submission capture, so the last record before the crash is the step to look at
   (`agc_resolve_step`, `agc_capture_submission`).
3. If the map parameterisation is implicated, reverting just that hunk is safe:
   it is the patch's only change to code a console run has already proved, and
   `git checkout -- driver/ps5vk_image.c` after applying the patch removes it
   while leaving the case, the executor and the harness in place.

**Verified on this host.** The table above is this round's run: `tools/check-driver.sh`
(full), `tools/check-runner-cases.sh`, `make ci`, `make inspect`,
`tools/build-all-titles.sh`, `make lint` (164 files), `make test` (20 tests), both
audits, the two link checks, the mip-layout oracle and four golden rebuilds. The
console is off the network for the twenty-eighth round running, so nothing was
deployed.

## 2026-09-18: the console is not hiding, and three more limits are compared

**A fresh sweep.** Twenty-eight rounds of an unreachable console is long enough to
check the obvious: the host is on WiFi only (`wlan0`, <host-lan-ip>/24, both
Ethernet ports down), a `/24` sweep of <lan>/24 for port 2121 found nothing,
and a ping sweep found three live addresses -- the gateway, this host, and
<neighbour-lan-ip>. That last one answers pings (a matching MAC) but has none of
the console's ports open (21, 22, 80, 1337, 2121, 3232, 9020, 9021, 9026-9029,
9111, 50000) and does not answer an SSDP M-SEARCH, so it is not the console. The
console is off the network, not moved.

**The limits audit compares 97 of 106 members now, was 95.** Two rows the audit
skipped as "the supported column is a formula or a boolean" are answerable after
all: `lineWidthRange` and `pointSizeRange` must be exactly `(1.0,1.0)` while
`wideLines` and `largePoints` stay off, which is the *unsupported* column of those
rows, and the audit compares against it when the supported column is a formula. The
comparison learned equality as well as min/max, so a Boolean row whose core column
carries a value -- `standardSampleLocations` looked like one, but the specification
gives it `-` at core level, so it stays out -- is compared exactly rather than
skipped. The nine that stay uncompared are now all rows no number can answer: a
recommendation, an implementation-dependent value, a duration, a row with no core
requirement, and the two conditional interpolation offsets.

`tests/test_tools.py` asserts the coverage now, not just the misses: `--check` only
fails on a limit that misses, so a parser change that quietly stopped comparing
rows would have gone unnoticed. The assertion was verified in both directions (97
passes, a bound of 98 fails) -- and the first attempt at that proof lied, because a
same-size edit to the test file reused a stale `tests/__pycache__` entry; the
second cleared the cache and ran with `python3 -B`. The practical rule is in
docs/TESTING.md.

**Verified on this host.** `tools/limits_audit.py --check` PASS (106 required
members, 97 compared, 0 missing, 9 not compared), including a sensitivity check
that broke `pointSizeRange[1]` and got "the driver reports 2.0, the table requires
exactly 1.0"; `python3 -B -m unittest tests.test_tools` PASS; `make lint`. The
console is off the network for the twenty-ninth round running.

## 2026-09-18: the console batteries are two, so one crash cannot cost both

The queued battery had grown to thirteen cases in one file, and one of them --
`c8-resolve` -- is a case only the parked patch provides. That has a consequence
worth fixing before the session rather than during it: the runner rejects a queue
whole when a line names a case it does not have ("unknown test name"), so an
unpatched deploy cannot run *any* of the thirteen, and a patched deploy whose
crash is the very thing being bisected risks the twelve proven cases with it.

Two queues now, each with its deploy commands in its header:

- `jobs/c8-controls/queue.txt` runs on the committed tree: the two C7 mip probes
  whose goldens are in the tree, `c7-mip-pages-six`, the `c2-instancing` and
  `c3-quad` controls, `c8-msaa`, then the cases the last rounds added --
  `b5-events`, `c2-transfers`, `c7-clear`, `c2-indirect`, `b8-secondary` -- and
  `m2-solid` last.
- `jobs/c8-resolve/queue.txt` is the patched battery: `c8-msaa` as its control and
  then `c8-resolve`, with the patch, the rebuild and the deploy written out above
  the queue.

`tests/test_tools.py`'s queue guard walks every `jobs/*/queue.txt`, so both are
checked on the PC: each name is a case the runner's table holds or one a parked
patch adds. The first battery's cases stay in the same order they had, so a session
that ran the old queue gets the same evidence from the new one, plus the twelve
proven cases even if the resolve patch crashes.

**Verified on this host.** `python3 -B -m unittest tests.test_tools.QueueCaseTests`
PASS with both queues; `make lint`. The console is off the network for the
twenty-ninth round running.

The queue guards grew with the split: `tests/test_tools.py` now checks the limits
the runner's parser enforces besides the names -- more than 32 tests, a `hold`
outside 1..3600, and a queue that names no test and no `all`. Both new checks were
proven by breaking a real queue (40 extra lines; `hold 9000`) and restoring it, and
the `all` case had to be allowed for: `jobs/compile` and `jobs/regression` queue
every default test with that keyword rather than listing them. docs/DEPLOYMENT.md
says what is checked, so a queue that would be rejected is a `make test` failure
rather than a wasted console session.

## 2026-09-18: the C8 crash is a 33 MiB heap allocation, found in the logs

Seven console runs (pids 170-176) died right after the first frame's shader link,
and the patch's own step records never appeared. This round read the klogs again
and found what they actually say.

**The last records are the runner's, not the case's.** `Klog_Logs/c8-resolve-run{5,6,7}.log`
end with the *staging* of the test's package set -- `agc_shader_package_vertex/pixel`,
`agc_shader_checksum_*`, `agc_shader_package_validation`, `agc_shader_stage_copy`,
`agc_shader_create_vertex/pixel` and `agc_shader_link PASS` -- and those records come
from `link_shader_packages` in `src/diagnostics.cpp`, not from the driver. After
them: nothing. The case's first record (`agc_resolve_step frame 0 creating`) is
never written, so the title died between the staging and the case's first line.

**What is between them.** `run_test_queue` logs `runner_test_start`, stages the
set, builds a `TestContext` and calls the case. The case's first statements were:

    const VkVertexInputAttributeDescription attributes[2] = {...};
    std::vector<std::uint32_t> resolved(kFramebufferBytes / 4u, 0u);   // 33 MiB
    unsigned framed = 0;

A 33 MiB `std::vector` on the title's heap, and the runner is built with
`-fno-exceptions` (tools/build.sh, tools/build-host-runner.sh): libstdc++'s
allocation failure calls `std::terminate`, which aborts the title *without writing
another record*. That is the crash: deterministic (the same line every run),
console-only (the PC's heap has the 33 MiB to spare, which is why the same case
passes there), and at exactly the point the logs stop. It also explains why the
one-sample control frame in run 7's log died the same way: the allocation happens
before any frame, whatever the flags.

**The fix.** The comparison no longer copies a frame. Each frame's words are folded
into an FNV-1a checksum as they are read, and the first 4,096 of them are copied
into a 16 KiB prefix: the reference frame's checksum equal to the resolved frame's
is what says the whole 33 MiB frame matches -- stronger than the old copy, which
only ever compared the two buffers -- and the prefix names a differing word when it
does not. Both frames are read where the harness mapped them, nothing is allocated,
and neither frame has to outlive its own iteration. The case's records now log both
checksums, the word count, the prefix words compared, the differing count, the first
differing word and the worst channel error.

The fix was verified on the PC before the patch was regenerated: the case runs its
four frames and passes ("1 of 1 queued tests passed"), and the patch applies to the
committed tree (the packed-patch guard passes). A first attempt at the fix kept the
resolving frame's harness alive instead -- which does remove the allocation, but on
the PC it stops the *next* frame's pipeline from replaying (its stage allocation no
longer lands where the capture's stage image is), so the last frame was refused with
"a created shader's register tables are out of bounds". The checksum version leaves
the frames exactly as they were and has no such cost.

**Verified on this host.** With the patch applied: the fixed `c8-resolve` case runs
all four frames and passes through the driver-enabled host runner; the regenerated
patch is 689 lines over seven files and applies to HEAD; with the tree restored,
`tests/test_tools.ParkedWorkTests` PASS and the driver and both host runners rebuild
from the committed sources. The console is off the network for the thirtieth round
running, so the fix's own proof is the next session's run -- which now cannot die at
that allocation.

## 2026-09-18: the console came back, and the first battery passed

The console answered again at <console-lan-ip> (FTP 2121, klog 3232, etaHEN
1337/9028). The control payload was loaded and answered `ok ps5vkctl 1 pid=108`,
the runner title deployed (150 files), and `jobs/c8-controls/queue.txt` ran
complete as **pid 109**:

| Case | Result | What it proves on the console |
| --- | --- | --- |
| `c7-mip-pages`, `c7-mip-tiled` | PASS | the deployed build is the proven one |
| `c7-mip-pages-six` | PASS | the six-level chain, whose bases the oracle predicted |
| `c2-instancing`, `c3-quad` | PASS | the controls for the C8 frames' geometry |
| `c8-msaa` | PASS | the four-sample frame again (pid 169 before) |
| `b5-events` | PASS (8 of 8 cases) | an event is the flag a fence is, and the three recording commands follow the command order |
| `c2-transfers` | PASS (6 of 6 checks) | `vkCmdFillBuffer` and `vkCmdUpdateBuffer` write the mapped buffer, in order, with the update snapshotted |
| `c7-clear` | PASS (7 of 7 checks) | the colour clear and the readback: a linear level, a tiled attachment, level 1 of two, a region and padded rows, byte for byte |
| `c2-indirect` | PASS | the M2 triangle through `vkCmdDrawIndirect` |
| `b8-secondary` | PASS | the same triangle through a secondary the primary executes |
| `m2-solid` | PASS | the plain frame, last as always |

The run's summary: 748 PASS records, 485 INFO, 48 NOT_REQUIRED, 1 ARMED and one
benign `agc_live_video_unregister` WARN (VideoOut busy, known). Nothing failed.
This is the console proof the last rounds owed for the six CPU-side commands,
events, the clear and readback, the indirect draws and secondary command buffers;
each case's capture is under `golden/<test>/` (`c2-indirect`,
`b8-secondary`, `c7-mip-pages`, `c7-mip-pages-six`, `m2-solid` are new
directories).

**One trap, recorded.** Extracting *all* the run's goldens into the existing
per-test directories replaced `c2-instancing`, `c3-quad`, `c7-mip-tiled` and
`c8-msaa` with captures that carry no context register table, and
`tools/check-driver.sh` stops at the first replay it cannot build ("no context
register table to take register defaults from"). Those four keep their earlier
captures -- a run's own capture is not automatically a better golden -- and the
check passes with the new directories beside them, which the extractor writes with
`golden/<test>` as its destination and `--test <name>`.

**Verified on this host.** `tools/check-driver.sh c2_instancing c8_msaa` PASS on
the tree with the new goldens; the full check follows the patched battery. The
console is up, so the next step is `jobs/c8-resolve/queue.txt` with the parked
patch applied.

## 2026-09-18: the C8 case runs on the console now, and names where it differs

Two console runs with the parked patch applied, after the 33 MiB heap fix:

- **pid 110**: `c8-msaa` PASS, `c8-resolve` FAIL -- the case runs all four frames
  to completion for the first time (the crash is gone, which confirms the
  allocation was the cause), and the comparison fails: resolved checksum
  0x1d575582d7eb883 against the reference's 0x4d935eccc4aea783, with the first
  4096 words equal and no channel error in them.
- **pid 111**: the same, with the case folding each frame into row and column
  hashes as well, which says *where*: `first_differing_row` 142, `differing_rows`
  752 of 2160, `first_differing_column` 0, `differing_columns` 3840 of 3840. The
  752 rows are the m3-vertex square's own rows, so the difference is inside the
  drawn square and nowhere else: the resolve's arithmetic runs, and the samples it
  averages are not the texels' own.

That points at the C8 step's remaining unknown, the four sample words' places
inside a tile: the patch reads them as four consecutive words of the texel
(`samples[sample * 4 + byte]`, the sample-major layout its comment names), and the
localisation says that reading is wrong -- if it were only a shifted texel, the
square's interior would still match its own colour and only its edges would differ,
and the count would be a thin frame rather than 752 rows. The next console step is
the layout probe the plan already names ("the four sample words' places inside a
tile measured by *coverage*"), and the case is the place for it: frame 1 already
draws the square into a four-sample image whose storage the harness maps, and a
one-sample reference frame says which texels the square covers, so candidate
layouts can be scored against the coverage in one run.

**Verified on this host.** With the patch applied, the case's four frames run and
its comparison is the folded one; the regenerated patch is 742 lines over seven
files and applies to HEAD; with the tree restored, the parked-patch guard passes
and the driver and both host runners rebuild from the committed sources. The
console answered for this whole session (FTP 2121, klog 3232, etaHEN 1337/9028),
so the next run can be made from here.

**A note on the addresses above.** The network addresses this log recorded -- the
console's, the build host's and a neighbouring device's with its MAC -- are
replaced by placeholders (`<console-lan-ip>`, `<host-lan-ip>`, `<lan>`,
`<neighbour-lan-ip>`) when the repository was published under its new home. The
runs themselves are unchanged; only those values are.
