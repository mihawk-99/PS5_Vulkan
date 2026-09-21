# Milestone 5, Phase C: the Vulkan Tutorial ladder

Append-only run log: add new entries at the end; do not rewrite an existing
section. Current state and next actions live in
[VULKAN_PROBE_ACTIVE.md](VULKAN_PROBE_ACTIVE.md).

Phase C takes the driver through the Vulkan Tutorial one chapter at a time.
The plan, workflow and acceptance criteria are in `docs/VULKAN_PROBE_PLAN.md`
(Phase C, adopted 2026-09-15).

| Step | What | Status |
|---|---|---|
| C1 | Swapchain and present over `VK_KHR_display`; colour clears | in progress |
| C2 | Vertex, staging and index buffers | pending |
| C3 | Descriptors, uniform buffers, push constants | pending |
| C4 | Textures and samplers | pending |
| C5 | Depth | pending |
| C6 | 32-bit index buffers | pending |
| C7 | Mipmaps and blits | pending |
| C8 | 4x multisampling | pending |
| C9 | vkcube | pending |

This log records progress and findings in order. Hardware findings also go
into `docs/VULKAN_PROBE_PLAN.md`.

## 2026-09-15: C1 design

### What exists

- **No window-system code to reuse.** The runtime build compiles Mesa's
  window-system code as a stub (`tooling/vulkan-runtime/wsi`). Mesa's own
  `VK_KHR_display` implementation (`src/vulkan/wsi/wsi_common_display.c`)
  drives Linux DRM, which the PS5 does not have. The driver therefore owns its
  surfaces, display and swapchain.
- **Display surfaces.** The loader interface (`vk_icd.h`, interface version
  7) defines `VkIcdSurfaceDisplay`: the display mode, plane index and stack
  index, transform, global alpha, alpha mode and image extent. Driver surfaces
  use this layout, so surfaces created by the loader and by the driver are
  interchangeable.
- **PC tools.** The host needs the Khronos loader plus Mesa's Vulkan drivers,
  including lavapipe (`lvp_icd.json`), for reference images. CachyOS packages
  them as `vulkan-icd-loader`, `vulkan-swrast`, and
  `vulkan-validation-layers`; install the validation layer when Phase C's PC
  tests need it.
- **Recorded presentation (M2, M3, B4).**
  - The runner opens VideoOut (`sceVideoOutOpen(0xff, 0, 0, NULL)`, flip rate
    0), maps two 3840x2160 framebuffers in one 64 MiB allocation, and
    registers them as SDR buffers (`sceVideoOutRegisterBuffers2`, pixel format
    `0x8000000000000000`).
  - A frame into buffer *n* starts with the wait packet naming buffer *n*
    (`sceAgcDriverWaitUntilSafeForRendering`) and ends with the flip of
    buffer *n* carrying a marker (`sceAgcDcbSetFlip`, mode 1).
  - The runner waits until `sceVideoOutGetFlipStatus` reports that marker.
    Naming the on-screen buffer in the wait packet deadlocks the stream (M3).
  - Framebuffers are B8G8R8A8 targets (`CB_COLOR0_INFO` SWAP_ALT).

### Decisions

- **Surface: `VK_KHR_display`** (with `VK_KHR_surface` and `VK_KHR_swapchain`).
  It is the standard extension for a screen without a windowing system, and
  `vkcube --display` uses it. VideoOut is the one display with one mode,
  3840x2160, and one plane.
- **Swapchain.**
  - Two driver-owned `B8G8R8A8_UNORM` images in VideoOut-registered direct
    memory; FIFO only.
  - A submission that renders into a swapchain image starts with that image's
    wait packet.
  - `vkQueuePresentKHR` submits the image's flip and waits for its marker in
    VideoOut's flip status.
- **Colour clears need no probe.** A clear can be a full-target triangle whose
  colour comes from a uniform buffer: M3's `m3-uniform` test rendered exactly
  that on the hardware. The driver will compile that shader pair itself and
  draw it for `VK_ATTACHMENT_LOAD_OP_CLEAR`, which keeps clears in command
  order on the GPU.
  
  *Superseded on the same day by the C1b design below.* A full-target triangle
  clears only what `vkCmdBeginRendering` clears; Vulkan 1.0 also requires
  `vkCmdClearAttachments` on arbitrary rectangles, so clears go through Mesa's
  `vk_meta`, which draws those rectangles and does need a probe.
- **What does need a probe: the flip in a submission of its own.** Vulkan
  splits a frame: `vkQueueSubmit` renders into the image and completes, then
  `vkQueuePresentKHR` flips it. Every recorded frame put the wait packet, the
  draw and the flip in one stream.

### Runner probe `c1-present` (set `m3`)

Four frames of the M3 uniform colour, alternating framebuffers 0 and 1 as
FIFO does. Colours: `ff4080`, `30d060`, `2060f0`, `e0c010`, each exact in
8 bits. Each frame is two submissions:
1. The wait packet naming the buffer, the state, the draw, the colour barrier
   and the completion marker, then an exact readback of the buffer before it is
   shown.
2. The flip alone (`build_flip_stream`), confirmed by VideoOut's flip status,
   then held on screen.

A frame whose draw or flip does not complete ends the test.

- **Shared setup.** `run_uniform_buffer_frames` and the probe now share
  `check_uniform_bindings`, `write_uniform_descriptor` and
  `write_uniform_colour`; the M3 frames are unchanged (golden rebuild below).
- **Job queue.** `jobs/c1/queue.txt` holds `capture`, `hold 60`, `m2-solid`
  and `c1-present`, so the probe's streams become golden files for the
  driver's C1 comparisons.
- **Risk.** A flip in a stream of its own is new to the console. If it does
  not complete, the runner keeps its buffers and stops. A fault could freeze
  the console, as B8's indirect buffer did, so the launch request names this
  risk.

### PC verification

- **Dry run.** The PC runner replayed `golden/runner/m3-uniform-1.json` and
  ran `c1-present`. All four frames:
  - encode the first submission as the wait packet, CX, UC and SH tables,
    pixel user data, the draw and the completion marker;
  - encode the second as the flip alone;
  - validate, submit, complete their markers, and reach their flip markers in
    the host's flip status.
  
  The readbacks fail only because the PC renders nothing.
- **Golden rebuilds** with the shared uniform-colour helpers: `golden/runner`
  10 of 10 (including both `m3-uniform` frames), `golden/b4` 3 of 3,
  `golden/b5` 4 of 4.

## 2026-09-15: Console run pid 134, the flip alone

Queue `jobs/c1`, klog `Klog_Logs/klog-20260915-153253.log` (not committed).
Runner PPSA99988 digest `debd5fdf6a1107d8`. Both tests pass; the only
non-pass records are the known, benign VideoOut-busy warnings. The user saw
light blue, then pink, green, blue and yellow.

| Test | Result |
|---|---|
| `m2-solid` | PASS |
| `c1-present` | PASS: 4 of 4 frames drawn exactly and flipped in a submission of their own |

Every `c1-present` frame, alternating buffers 0 and 1:
- **Draw submission** (71 words: wait, state, draw, barrier, marker): the
  completion marker was already written straight after submission and found
  on the first poll; the suspend point took 123-126 µs.
- **Readback before the flip:** 8,294,400 of 8,294,400 pixels equal the
  frame's colour (`0xffff4080`, `0xff30d060`, `0xff2060f0`, `0xffe0c010` in
  B,G,R,A bytes), with no padding written.
- **Flip submission** (64 words, the flip alone): VideoOut's flip status
  reached the frame's marker without waiting a vblank.

### Findings

1. **A frame can be drawn in one submission and flipped in a later one.** The
   wait packet belongs to the stream that renders into the buffer; the flip
   needs no wait of its own. This is the split `vkQueueSubmit` and
   `vkQueuePresentKHR` make, so the C1 swapchain design holds.
2. **The buffer can be read back before it is shown.** A presented image's
   contents are final when its draw submission completes, before the flip.

### Golden files and PC checks

`golden/c1` holds `m2-solid` and the eight `c1-present` streams (draw, flip,
four times) from pid 134.

| Check | Result |
|---|---|
| `golden.py check-helpers golden/c1` | 42 of 42 helper calls reproduced by the PC models |
| `golden.py rebuild golden/c1` | 9 of 9 frames rebuilt identically by the PC runner |
| `agc_rules.py` on `runner`, `b4`, `b5`, `b5c`, `c1` | 0 violations in 30 streams |
| `test_agc_rules.py` | 10 tests pass |

**Rule change.** The `wait-buffer` rule asked every flipping stream for a wait
packet, which flagged the four flip-only streams. It now asks only streams
that both draw and flip. A flip-only stream passes, and M2's frame with its
wait turned into a NOP still breaks the rule. `golden/c1` joins the sets
every rule must pass.

### Next

The driver side of C1: `VK_KHR_surface`, `VK_KHR_display` and
`VK_KHR_swapchain`. Swapchain images use the VideoOut buffers, a wait packet
starts each submission that renders into one of them, and
`vkQueuePresentKHR` submits the flip alone. PC checks compare the driver's
submissions with `golden/c1`.

## 2026-09-15: C1a, the driver's swapchain and presentation

### Driver

- **`driver/ps5vk_wsi.c` (new).**
  - `VK_KHR_display`: VideoOut is one display (`PS5 VideoOut`) with one mode,
    3840x2160 at 60 Hz, and one plane. `vkCreateDisplayModeKHR` is refused.
    Plane capabilities allow only the full screen, opaque.
  - `VK_KHR_surface`: display-plane surfaces in the loader's
    `VkIcdSurfaceDisplay` layout, supported by queue family 0. Capabilities:
    1 to 2 images, 3840x2160, identity transform, opaque, colour-attachment
    usage. Formats: `B8G8R8A8_UNORM` in `SRGB_NONLINEAR`. Present modes: FIFO.
  - `VK_KHR_swapchain`: the swapchain opens VideoOut as the runner does (flip
    rate 0, two 32 MiB tiled framebuffers in one cleared 64 MiB direct-memory
    allocation, registered as SDR buffers) and wraps them as two
    `B8G8R8A8_UNORM` images with the VideoOut handle and buffer index.
  - **Acquire** returns the buffer not on screen: 0 first, then alternating.
    It signals the given semaphore or fence at once. An application holds one
    image at a time, so `minImageCount` is 1; acquiring while holding one
    returns `VK_NOT_READY` or `VK_TIMEOUT`.
  - **Present** waits for and consumes its semaphores, then for each swapchain
    calls `ps5vk_queue_flip` with the next flip marker and records the buffer
    on screen.
  - **Replacement.** A swapchain created with `oldSwapchain` takes VideoOut
    over. The old one is retired, and presenting its images returns
    `VK_ERROR_OUT_OF_DATE_KHR` without a flip. A second swapchain without
    `oldSwapchain` gets `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`. Destroying the
    owner drains pending flips, unregisters (tolerating the benign busy
    result), closes VideoOut and releases the buffers.
- **`driver/ps5vk_queue.c`.**
  - A submission whose command buffers render into swapchain images starts
    with one wait packet per buffer (`sceAgcDriverWaitUntilSafeForRendering`).
  - `ps5vk_queue_flip` submits `sceAgcDcbSetFlip` (mode 1) in a stream of its
    own, passes the suspend point, and waits up to 200 vblanks for VideoOut's
    flip status to reach the marker; otherwise the device is lost. This is the
    split proven by run pid 134.
- **`driver/ps5vk_draw.c`.** `B8G8R8A8_UNORM` attachments are accepted and
  programmed with `CB_COLOR0_INFO` SWAP_ALT, the M2 framebuffer value
  (`0x8828` against R8G8B8A8's `0x8028`). Command buffers track each target's
  VideoOut buffer for the wait packets. `B8G8R8A8_UNORM` is a
  colour-attachment format (`ps5vk_image.c`) and a pipeline attachment format
  (`ps5vk_pipeline.c`).
- **Extensions.** The instance adds `VK_KHR_surface` and `VK_KHR_display`, and
  the device adds `VK_KHR_swapchain`. The B2 test now expects five instance
  extensions and accepts the swapchain extension.
- **`driver/ps5vk_debug.h` (new).** `ps5vk_debug_image_storage` gives the
  console runner a swapchain image's storage for readback. It is not a Vulkan
  API, and the PC loader driver does not export it.

### Tests and tools

- **`driver/tests/ps5vk_triangle.c`** draws either into its own image (B7, B8)
  or to the display (`PS5VK_TRIANGLE_OUTPUT_DISPLAY`), as the Vulkan Tutorial
  draws to a window:
  - display, plane and mode queries, a display-plane surface and a check that
    the surface is supported;
  - a FIFO swapchain of `minImageCount + 1` images, with views and
    framebuffers per image;
  - an image-available semaphore that the frame's first submission waits on,
    and a render-finished semaphore that `ps5vk_triangle_present` waits on;
  - `vkDeviceWaitIdle` before cleanup.
- **`driver/tests/vk_c1_present_test.c` (new).**
  - Four frames acquired, drawn and presented, alternating images 0 and 1;
    swapchain images are not mapped.
  - A second swapchain is refused.
  - An image acquired before replacement presents as out of date.
  - The replacement has two images.
- **`tools/golden.py compare-present` (new).** Each presented frame must be
  two submissions equal to the console's c1 streams:
  - **Frame submission:** the framebuffer's wait packet (`c1-present`),
    m2-solid's state and draw, then the barrier and completion marker
    (`c1-present`).
  - **Flip submission:** the flip alone, with the driver's marker and flip
    count *n* for frame *n*.
  
  Framebuffer 1's `CB_COLOR0_BASE` is expected half the framebuffer
  allocation after framebuffer 0's. `compare-submission` now shares the
  packet comparison (`compare_stream`), whose packets each carry their own
  golden document.
- **`tools/check-driver.sh`** runs `c1_present` through the loader, directly
  and as a PS5 link, against a replay of `golden/c1/m2-solid-1.json`, and
  compares with `compare-present`.
- **Runner test `c1-triangle`** (set `m2`, `AGC_VULKAN_DRIVER`): the program
  on the display for four frames. Each frame is read back exactly from its
  swapchain image through `ps5vk_debug.h` before the present, then held for
  the queue's hold time. Queue `jobs/c1-triangle` runs `b7-triangle`, then
  `c1-triangle`.

### PC results

| Check | Result |
|---|---|
| `build-driver.sh` | 15 driver sources per target, 0 warnings |
| `check-driver.sh` | PASS: 9 tests through the loader, directly and as PS5 links, and 2 negative tests |
| `c1_present`, loader and direct | 11 of 11 checks. Frames 1-4 and flips 1-4 identical to `golden/c1`. Expected differences only: `PA_CL_VPORT_YSCALE` `0x44870000`, and framebuffer 1's `CB_COLOR0_BASE` `0x02022000` (console buffer 0: `0x02002000`) |
| `b7_draw`, `b8_groups` | unchanged: identical to `b4-headless` |
| `build-all-titles.sh` | 5 titles, 0 warnings, segments aligned |
| `test_agc_rules.py` | 10 tests pass |

### Validation layer

- **Setup.** Install the Arch package `vulkan-validation-layers` and the
  loader (`vulkan-icd-loader`, with `vulkan-swrast` for lavapipe). The layer
  is used system-wide and nothing is unpacked inside the repository. The
  script runs the loader builds of `c1_present`, `b7_draw` and `b8_groups`
  with `VK_LAYER_KHRONOS_validation` and shader-validation caching off.
- **Shader caching.** A first run with caching on showed the SPIR-V error
  below only for the first program. `b7_draw` and `b8_groups` looked clean
  because the layer skipped SPIR-V it had already validated. Caching stays off
  in these runs.
- **Findings and fixes.** The runs reported these problems in the test
  program, none in the driver's own behaviour:
  1. **Acquire with a fence** (`VUID-vkAcquireNextImageKHR-fence-01287`,
     `UNASSIGNED-VkPresentInfoKHR-pImageIndices-MissingAcquireWait`,
     `VUID-vkDestroyFence-fence-01120`): the replacement check reused the
     frame fence without resetting it, then presented and destroyed it without
     waiting on it. It now resets the fence, acquires, and waits on the fence,
     which the driver has already signalled.
  2. **Acquire from a retired swapchain:** invalid usage
     (`VUID-vkAcquireNextImageKHR-swapchain-01285`). The check now presents an
     image acquired before the replacement, which is valid and returns
     `VK_ERROR_OUT_OF_DATE_KHR`.
  3. **Semaphore destroyed while in use:** the program released objects
     without `vkDeviceWaitIdle`. Fixed as the tutorial does.
  4. **SPIR-V version** (`VUID-VkShaderModuleCreateInfo-pCode-08737`, in all
     three programs): **open.** The probe SPIR-V is compiled with
     `--target-env vulkan1.2` (SPIR-V 1.5), while the driver's device reports
     Vulkan 1.0.
     
     These SPIR-V files produce the hardware-run packages, so they are not
     regenerated in C1a. Before C1 closes, the probe sets are compiled for
     Vulkan 1.0, with B6 proving their packages unchanged, or the device
     version rises with the features it needs.

## 2026-09-15: Console run pid 142, presenting through the driver

- **Run.** Queue `jobs/c1-triangle`, klog
  `Klog_Logs/klog-20260915-163203.log` (not committed). Runner PPSA99988 from
  commit `d5263b0`, build digest `0ccb31a6365f96fa`.
- **Records.** 165 PASS, 8 NOT_REQUIRED and 2 INFO records (the test-start
  markers), with no WARN, FAIL or SKIPPED record and no driver warning.

| Test | Result |
|---|---|
| `b7-triangle` | PASS: the draw path is unchanged; 8,294,400 of 8,294,400 pixels |
| `c1-triangle` | PASS: 4 of 4 frames drawn exactly through the swapchain and presented |

`c1-triangle` through the driver's `VK_KHR_display` and `VK_KHR_swapchain`:
- **Setup.** The display query returned `PS5 VideoOut`, 3840x2160 at 60000 mHz.
  The swapchain was created with FIFO and two images.
- **Frames.** Images were acquired in the order 0, 1, 0, 1, as designed: the
  image on screen is never acquired.
- **Storage.** Image 0 lies at `0x200400000` and image 1 at `0x202400000`, each
  32 MiB: the two framebuffers of one VideoOut allocation.
- **Readback.** Each frame read back 8,294,400 of 8,294,400 pixels in the M2
  colour from its swapchain image after `vkQueueSubmit`, before
  `vkQueuePresentKHR`.
- **Present.** Every present returned `VK_SUCCESS`: the flip submitted alone
  and VideoOut's flip status reached its marker. Each frame was held for 60
  refreshes.
- **Teardown.** The swapchain drained its flips, unregistered and closed
  VideoOut without a driver error.

### Findings

1. **The C1 design holds through the Vulkan API.** An ordinary Vulkan program
   uses the Tutorial's display surface, swapchain, acquire, submit and present
   flow. It renders into VideoOut's framebuffers and flips them on the
   console, and every submission equals the console's own c1 streams on the PC.
2. **Presentation needs no new hardware knowledge beyond pid 134.** The wait
   packet in the rendering submission, followed by the flip alone, is
   sufficient.

### Open for C1

- **Colour clears** (`VK_ATTACHMENT_LOAD_OP_CLEAR`), which the triangle's
  frames need outside the triangle, with the clear colour read back exactly on
  the console.
- **The SPIR-V version finding above**, for a validation-clean program.

When both are done, C1 closes with its `feat(vulkan): implement C1 -
swapchain and VideoOut presentation` commit.

## 2026-09-15: C1b design, colour clears on Mesa's vk_meta

`VK_ATTACHMENT_LOAD_OP_CLEAR` and `vkCmdClearAttachments` go through Mesa's
shared `vk_meta` clear path (`src/vulkan/runtime/vk_meta_clear.c`) rather than
a hand-built full-target uniform-colour pipeline. Vulkan 1.0 already requires
`vkCmdClearAttachments` on arbitrary rectangles, and 1.4 requires eight colour
attachments and 7680-pixel framebuffers, so the driver would end up writing
`vk_meta`'s shader and rectangle builder itself. `vk_meta` is also what C4's
`vkCmdBlitImage`, C5's depth clears and C8's resolves already plan to use.

### What vk_meta asks of the driver

`vk_meta_clear_attachments` binds a pipeline, pushes the clear colours as push
constants, and calls `meta->cmd_draw_rects`, whose default
(`vk_meta_draw_rects`) sets a dynamic viewport and scissor, fills a
command-buffer-owned vertex buffer with two triangles per rectangle, binds it
and draws `6 * rect_count` vertices. `vk_meta_create_graphics_pipeline` calls
the driver's own `vkCreateGraphicsPipelines` with the shaders as NIR
(`VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA`). So C1b needs,
in the driver: NIR pipeline stages, push constants, vertex buffers, dynamic
viewport and scissor, and a buffer the command buffer can allocate and map
while recording. That is C2's and C3's plumbing arriving one step early, which
is the price of not writing a second clear path later.

### Question 1: how push constants reach the shader

**Decision: the driver lowers push constants into a driver-owned uniform
buffer in descriptor set 0, and never lets the compiler place them in user
data.**

- `PsbcShaderMetadata` (`psbc_compile.h`) reports where the compiler put the
  vertex-buffer table, the base vertex, the start instance, the streamout and
  scratch tables, the NGG LDS layout and the set-0 descriptor table. It has no
  push-constant field. RADV, which libpsbc is built on, allocates
  `AC_UD_PUSH_CONSTANTS` as soon as a shader loads push constants
  (`radv_declare_shader_args`), but `fill_shader_metadata` does not export that
  location. A shader compiled that way would read an SGPR the driver never
  wrote: an unmapped pointer, which is how B8's indirect buffer faulted the
  GPU.
- The alternative reaches the shader through state that is recorded. M3's
  uniform colour proved the whole chain on the hardware: a set-0 descriptor
  table whose address sits in the pixel user data
  (`probes/m3/bindings.txt`: `pixel_user_sgpr_count 3`,
  `pixel_descriptor_set0_dword 2`), holding one 16-byte uniform-buffer
  descriptor at `set0 binding0 offset 0 stride 16`, and the buffer it names.
- So, before the NIR reaches libpsbc, the driver runs
  `nir_lower_explicit_io(nir_var_mem_push_const, nir_address_format_32bit_offset)`
  itself and rewrites each resulting `load_push_constant` into a `load_ubo`
  from a reserved binding of set 0, emitted as the Vulkan resource-index tuple
  (`nir_vulkan_resource_index` and `nir_load_vulkan_descriptor`) that
  `radv_nir_lower_descriptors` expects. libpsbc's `lower_gallium_ubo_index`
  only rewrites single-component UBO indices, so it leaves these alone, and the
  same binding numbering serves the SPIR-V and the NIR path.
- At the draw the driver writes the command buffer's push-constant bytes into a
  GPU-visible buffer, writes its descriptor into the pipeline's set-0 table in
  ps5-opengl's recorded stride form, and names the table in the stage's user
  data. `vkCmdPushConstants` then costs what a uniform buffer costs, which is
  what the hardware has run.

The cost is one uniform-buffer descriptor and one buffer per draw that uses
push constants, instead of a handful of SGPR writes. That is the price of
staying inside recorded behaviour; C3 can revisit it if libpsbc ever reports
the push-constant location.

### Question 2: the vk_meta_device_init flags

| Flag | Value | Why |
|---|---|---|
| `use_gs_for_layer` | false | The driver exposes no geometry stage, and `geometryShader` is off. The rectangle vertex shader writes `gl_Layer` itself. |
| `use_stencil_export` | false | There is no stencil attachment yet (C5), so the shader stencil-export path can never be right. |
| `use_rect_list_pipeline` | true | Despite the name this is what makes `vk_meta` supply the rectangle vertex shader, its vertex input and its dynamic viewport state (`create_rect_list_pipeline`); without it the driver would have to supply a vertex stage of its own. It needs no hardware rectangle primitive: `vk_meta_draw_rects` writes six vertices per rectangle, and the driver takes `VK_PRIMITIVE_TOPOLOGY_META_RECT_LIST_MESA` as a triangle list, as nvk does. |
| `use_layered_rendering` | false | `vkCmdBeginRendering` refuses `layerCount != 1`. |
| `max_bind_map_buffer_size_B` | three quarters of the command buffer's vertex chunk | `vk_meta_draw_rects` divides this by 72 bytes a rectangle, while `create_vertex_buffer` writes 96, so a batch can ask for four thirds of it. Three quarters of the real chunk keeps every batch inside one chunk. |
| `buffer_access.optimal_wg_size` | unset | Only the compute copy and fill paths read it, and the driver has no compute pipelines (D2). |

### Question 3: vkCmdPushConstants

Mesa's `vk_common_CmdPushConstants` only forwards to `CmdPushConstants2`, and
the common runtime stores nothing. **The driver implements
`vkCmdPushConstants2`** and gets `vkCmdPushConstants` from the runtime; the
bytes go into a `MESA_VK_MAX_PUSH_CONSTANT_SIZE`-byte array in the command
buffer and become a uniform buffer at the next draw, because the driver's
recording model builds every draw's tables at the draw.

Dynamic viewport and scissor need no driver entry point at all:
`vk_common_CmdSetViewport` and `vk_common_CmdSetScissor` write into
`vk_command_buffer::dynamic_graphics_state`, which `vk_command_buffer_begin`
already resets. The draw reads the viewport and scissor from there, and
`vkCmdBindPipeline` copies the pipeline's own state into it with
`vk_cmd_set_dynamic_graphics_state`. That also gives the meta save and restore
its snapshot, as `kk_meta_save` uses.

### Question 4: what the clear draw emits

Against today's draw (`ps5vk_draw.c`, 18 words: CX, UC and SH table loads and
`DRAW_INDEX_AUTO`), a `vk_meta` clear adds:

1. **A viewport and scissor from the dynamic state, not the pipeline.**
   `setup_viewport_scissor` rounds the rectangles up to a power of two, so a
   clear of the whole 3840x2160 target uses a 4096x4096 viewport and scissor
   and scales the rectangle corners by `2/4096`. Every corner of a
   whole-target clear still lands exactly on 0, 3840, 0 and 2160 in the
   viewport transform, but a viewport and scissor larger than the target is
   new.
2. **Vertex-buffer user data in the SH stream.** The vertex stage takes a
   `R32G32B32A32_UINT` attribute, so the draw writes the vertex-buffer table,
   the base vertex and the NGG LDS layout into the vertex user data
   (`sceAgcCbSetShRegisterRangeDirect`, offset `0x8c`), and the pixel stage's
   set-0 table into the pixel user data (offset `0x0c`). Today's draws write
   no user data at all.
3. **A second pipeline in one stream.** The clear draw and the application's
   draw have different linked shader state, so the stream carries two CX, UC
   and SH table loads. B8's two draws in one stream shared a pipeline, and
   M4's render to texture re-emitted only context registers and pixel user
   data between draws.
4. **A non-indexed draw from a vertex buffer.** `vk_meta` draws with
   `vkCmdDraw`, so `DRAW_INDEX_AUTO` reads the vertex buffer. Every recorded
   vertex-buffer draw (M3, M4) went through an index buffer.
5. **A vertex shader that writes `gl_Layer`.** `vk_meta_draw_rects_vs_nir`
   always writes it, which sets `PA_CL_VS_OUT_CNTL.USE_VTX_RENDER_TARGET_INDX`
   and adds a parameter export on a target that is not layered.

`firstVertex`, `firstInstance` and the instance count stay at 0, 0 and 1, so
the draw does not need what C2's probe will prove.

### The probe this needs

Points 1 to 5 are in no capture, so the PC checks cannot model them and C1b
starts on the console, with the workflow `docs/VULKAN_PROBE_PLAN.md` Phase C
step 4 sets out: a runner test in raw AGC, run with the `capture` keyword,
whose golden frames then let the driver be checked on the PC.

## 2026-09-15: Console run pid 117, the vk_meta clear

Queue `jobs/c1-clear`, klog `Klog_Logs/klog-20260915-193751.log` (not
committed). Runner PPSA99988 from commit `bf55bbb`, build digest
`0d4c389654d665e4`. 149 PASS records, 12 NOT_REQUIRED, 39 INFO and 5 ARMED;
the only non-pass records are the two known, benign VideoOut-busy warnings. No
GPU fault, no stall: the risk named before the launch did not materialise.

| Test | Result |
|---|---|
| `m2-solid` | PASS |
| `c1-clear` | PASS: 2 of 2 frames cleared and drawn exactly, on the GPU and on the CPU |

**Frame 0, the GPU clear** (98 words: wait, the clear pipeline's state, user
data and draw, the corner pipeline's state, user data and draw, barrier,
marker):

```
  0- 32  sceAgcDriverWaitUntilSafeForRendering  video, buffer 0
 32- 37  sceAgcDcbSetCxRegistersIndirect        stage+0x7000, 85 records
 37- 42  sceAgcDcbSetUcRegistersIndirect        stage+0x6000, 3
 42- 47  sceAgcDcbSetShRegistersIndirect        stage+0x6800, 10
 47- 52  sceAgcCbSetShRegisterRangeDirect       SH 0x8c, 3 dwords
 52- 57  sceAgcCbSetShRegisterRangeDirect       SH 0x0c, 3 dwords
 57- 60  sceAgcDcbDrawIndexAuto                 6 vertices
 60- 65  sceAgcDcbSetCxRegistersIndirect        stage+0x7400, 85 records
 65- 70  sceAgcDcbSetUcRegistersIndirect        stage+0x46000, 3
 70- 75  sceAgcDcbSetShRegistersIndirect        stage+0x6900, 10
 75- 79  sceAgcCbSetShRegisterRangeDirect       SH 0x8c, 2 dwords
 79- 82  sceAgcDcbDrawIndexAuto                 3 vertices
 82- 90  sceAgcCbReleaseMem                     colour barrier
 90- 98  sceAgcCbReleaseMem                     completion marker
```

Each context table is the 16 colour-target registers, that draw's 15 viewport,
guard-band, scissor and target-mask registers, the 34 linked context records
and both shaders' context registers.

**Frame 1, the CPU clear** (70 words): the framebuffer filled by the CPU
before the submission, then the corner triangle alone.

**Readback**, both frames: 8,294,400 of 8,294,400 words written, 0 pixels in
any other colour, 4,139,524 of 4,139,524 top-left pixels in the corner colour
and 4,139,524 of 4,139,524 bottom-right pixels in the clear colour. Nothing
outside the target was written.

### Findings

1. **A clear drawn as vk_meta draws it renders exactly.** A 4096x4096 viewport
   and scissor over a 3840x2160 target clip nothing and shift nothing: the
   rectangle's corners land on 0, 3840, 0 and 2160, and every pixel of the
   target holds the clear colour. The scale `2/4096` is a power of two, so
   each corner is exact in binary floating point.
2. **Two pipelines run in one stream.** The second pipeline's context, uniform
   and SH tables replace the first's, and both draws render where they should.
   The linked context of each pipeline lives in its own workspace half.
3. **A non-indexed draw reads a vertex buffer.** `DRAW_INDEX_AUTO` with six
   vertices fetched the rectangle from the vertex-buffer table, which every
   recorded vertex-buffer draw had reached through an index buffer.
4. **Vertex user data must be written per draw.** The frame writes SH `0x8c`
   before each draw: three dwords for the clear (vertex-buffer table, base
   vertex, NGG LDS layout) and two for the triangle (base vertex, NGG LDS
   layout). The register tables a shader object carries hold no user-data
   registers, so without the second write the triangle would have taken the
   clear's vertex-buffer table address as its base vertex. **The driver
   therefore writes each pipeline's vertex user data before every draw**, and
   its pixel user data whenever the pixel stage reads a descriptor set.
5. **The GPU clear is the right choice, and by a wide margin.** Clearing the
   3840x2160 framebuffer on the CPU took **17,530 µs**, more than a 60 Hz
   frame (16,667 µs), and in Vulkan it also forces the submission to be split
   at the clear. The same clear on the GPU added 41 words to the stream and
   nothing measurable to it: the frame with the clear passed its suspend point
   in 29 µs and the frame without it in 27 µs, and both completion markers had
   already been written when the first poll read them.

### Golden files and PC checks

`golden/c1-clear` holds `m2-solid` and the four `c1-clear` streams (the GPU
clear frame and its flip, the CPU clear frame and its flip) from pid 117.

| Check | Result |
|---|---|
| `golden.py check-helpers golden/c1-clear` | 30 of 30 helper calls reproduced by the PC models |
| `golden.py rebuild golden/c1-clear` | 5 of 5 frames rebuilt identically by the PC runner |
| `agc_rules.py golden/c1-clear` | 8 of 8 rules pass on all 5 streams, 0 violations |
| `test_agc_rules.py` | 10 tests pass; `golden/c1-clear` joins the sets every rule must pass |

No rule needed changing: a stream with two pipelines still waits for the
buffer it renders into, ends in a completion marker, and keeps every indirect
table inside the workspace.

## 2026-09-15: C1b driver work, in progress

The probe is done and its findings are recorded above. The driver side is
started but **not finished**. It builds and every check passes; what it does
not do yet is clear, so `vkCmdBeginRendering` still refuses `loadOp` CLEAR.
What follows is where it stands and what is left.

### Done

- `tooling/vulkan-runtime/Makefile` builds Mesa's `vk_meta.c`,
  `vk_meta_clear.c` and `vk_meta_draw_rects.c` into the runtime archive. Both
  targets compile them with 0 warnings.
- `driver/ps5vk_nir.c` (new) prepares a meta stage's NIR: it clones what
  vk_meta hands the driver, drops the always-zero `gl_Layer` store from the
  rectangle vertex shader, and lowers push constants with
  `nir_lower_explicit_io` and then a pass that rewrites each
  `load_push_constant` into a `load_ubo` from the reserved slot. libpsbc's
  `lower_gallium_ubo_index` turns that into set 0, binding
  `PSBC_GALLIUM_UBO_BINDING_BASE`, which is the binding
  `PS5VK_PUSH_CONSTANT_BINDING` names.
- `driver/ps5vk_pipeline.c` accepts NIR stages
  (`VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA`) beside
  SPIR-V, takes `VK_PRIMITIVE_TOPOLOGY_META_RECT_LIST_MESA` as a triangle list
  as nvk does, records the vertex input's binding strides and the layout's
  push-constant range, declares the reserved uniform-buffer binding for the
  stages that read push constants, and builds the
  `vk_dynamic_graphics_state` that `vkCmdBindPipeline` will put into the
  command buffer. The draw refusal now allows vertex input, push constants and
  a dynamic viewport and scissor, and still refuses descriptor sets (C3).
- `driver/ps5vk_image.c` reports `R32G32B32A32_UINT` as a vertex-buffer
  format, the rectangle attribute of Mesa's meta draws.
- `driver/ps5vk_private.h` carries the new pipeline, command-buffer and device
  state: the dynamic state, vertex bindings, push-constant bytes, bound vertex
  buffers, the rendering's `vk_meta_rendering_info`, the saved meta state and
  the device's `vk_meta_device`.

### Left to do

1. **`driver/ps5vk_draw.c`**, the bulk of it. Two pieces are done: a draw
   reads its viewport and scissor from
   `cmd_buffer->vk.dynamic_graphics_state`, and `vkCmdBindPipeline` fills that
   from the pipeline with `vk_cmd_set_dynamic_graphics_state`. Because
   `ps5vk_covers_target` still refuses anything but the whole target, the
   streams are unchanged and `golden/b5` and `golden/c1` still match. Left:
   - drop `ps5vk_covers_target`, which the clear's 4096x4096 viewport breaks;
   - `vkCmdBindVertexBuffers2` and `vkCmdPushConstants2`;
   - per draw: a vertex-buffer descriptor table (address low, address high
     with the stride, record count, flags `0x5204`) and the vertex user data
     naming it beside the base vertex and the NGG LDS layout; the
     push-constant buffer, its 16-byte descriptor in a set-0 table and the
     pixel user data naming that table; both written with
     `sceAgcCbSetShRegisterRangeDirect` at SH `0x8c` and `0x0c`, **before
     every draw**, because run pid 117 showed a shader object's tables hold no
     user-data registers;
   - `ps5vk_meta_init` and `ps5vk_meta_finish`, with the flags C1b question 2
     settled and a `cmd_bind_map_buffer` that suballocates from the command
     buffer's GPU-visible chunks;
   - `vkCmdBeginRendering` accepts `loadOp` CLEAR and calls
     `vk_meta_clear_rendering`; `vkCmdClearAttachments` calls
     `vk_meta_clear_attachments`; both save and restore the application's
     pipeline, vertex buffers, push constants and dynamic state around the
     meta draws.
2. **`ps5vk_cmd_buffer_table` gains an alignment argument** (descriptors name
   256-byte aligned addresses), and `ps5vk_device.c` initialises and finishes
   the device's `vk_meta_device`.
3. **PC checks.** `b7_draw` and `b8_groups` will now emit a vertex user-data
   packet their golden frames do not have, because every draw writes its own;
   `golden.py compare-submission` needs to accept that declared difference.
4. The `c1_present` test program and the runner's `c1-triangle` then draw over
   a cleared attachment, and the console run closes C1.

## 2026-09-16: Console run pid 107, the first compute dispatch

Recorded from `jobs/c0-dispatch/queue.txt` (`capture`, `hold 60`,
`c0-dispatch`, `m2-solid`), captured live from klog into
`Klog_Logs/c0-dispatch-klog.log`. One launch, and the first compute workgroup
this console has run.

### The dispatch

- The runner compiled `probes/c0/dispatch.spv` (580 bytes) in-process with the
  patched libpsbc: result 0, `rsrc1` `0x602c0001`, `rsrc2` `0x6`, `rsrc3` `0`,
  16 VGPRs, 108 SGPRs, LDS 0, `user_sgpr_count` 3, the descriptor set in
  user-data dword 2, and 56 bytes of machine code. Undeclared, the same payload
  compiles to 48 bytes and stores through a null descriptor, so the declared
  binding is what puts the descriptor load in the shader.
- The console's 14 ISA words are the ones the host compiler emits for this
  payload, byte for byte: `be830382 f4080001 fa000000` — `s_mov_b32 s3`, the
  compiled `address32_hi` 2, `s_load_dwordx4 s[0:3], s[2:3], 0` through the
  user-data table pointer, then the store, the wait and `s_endpgm`.
- `COMPUTE_PGM_RSRC1`'s VGPR granule (1) and the 16 VGPRs the compiler
  reported agree on wave32, so the initiator carries `CS_W32_EN`.
- The declared binding reports one entry at table byte offset 0, and the probe
  wrote its descriptor there: base low `0x2a000`, base high 2, extent
  `0x1000`, control `0x31016fac`.
- Every dispatch address shares the compiled high word 2 — shader
  `0x200028000`, descriptor table `0x200029000`, storage buffer
  `0x20002a000`, completion marker `0x20002bffc` — which the probe checks
  before it submits.
- The stream is the hand-written sequence `M5_REFERENCE.md` describes:
  SET_SH_REG writes for the grid origin (`0xb810`), the workgroup size
  (`0xb81c`), the program address (`0xb830`), RSRC1/2 (`0xb848`),
  `COMPUTE_RESOURCE_LIMITS` (`0xb854`), `COMPUTE_PGM_RSRC3` (`0xb8a0`) and the
  three user-data dwords (`0xb900`), the reference profile's destination and
  accumulator clears, then `DISPATCH_DIRECT` (`0xc0031500`) with dimensions
  1,1,1 and initiator `0x8041`, `EVENT_WRITE` `CS_PARTIAL_FLUSH`
  (`0xc0004600`, `0x407`) and the completion marker.
- `agc_compute_readback` observed `0xa5a5a5a5`: one workgroup of the shipped
  shader loaded its descriptor from the table and stored the word.

### Checks

- `c0-dispatch` PASS and `m2-solid` PASS. The run's only non-passing entry is
  the known-benign `agc_live_video_unregister` VideoOut-busy warning on close.
- `python3 tools/agc_rules.py Klog_Logs/c0-dispatch-klog.log`: 8 of 8 rules on
  both streams, 0 violations.
- `tools/golden.py extract` refuses this stream: the helper-call trace starts
  at word 50, because every packet before the completion marker is written by
  hand instead of through the AGC helpers the trace wraps. The words are in
  the klog anyway (`agc_live_command_encoding` for the stream and the
  `agc_capture_workspace` chunks for the workspace image), so the capture is
  complete and only the golden file is missing.
- `c0-dispatch` left `kFaultingTests`, the condition `M5_REFERENCE.md`,
  "V0-compute", sets for it to join the default and `all` queues.

### Open

- The golden-capture rule needs a way to record a packet its caller wrote by
  hand: the stream is complete, but no helper call covers its first 50 words,
  and `capture_problems` treats that as an incomplete capture
  (`tools/golden.py`, `tools/ps5vk_log.py`).
- The PC side has no model for a dispatch, so `golden.py rebuild` cannot cover
  this stream yet. The value of the golden file would be the recorded packet
  words, not a replayed result.

## 2026-09-16: Console run pid 107 again, the c0 golden file

The second launch of the c0-dispatch queue, captured into
`Klog_Logs/c0-dispatch-golden.log`, after the probe began recording the packets
it writes by hand. It is the run `golden/c0` is extracted from. The run itself
repeated the first one: `c0-dispatch` PASS with the readback exact, `m2-solid`
PASS, and the same known-benign VideoOut warning on close.

### What the capture needed

- The stream's first 50 words are written by hand, so no AGC helper call covered
  them and `tools/golden.py extract` refused the capture as incomplete. The
  probe now records them as one trace span named `raw_packets`. The name has to
  be one word: `tools/ps5vk_log.py` parses call names as `\w+`, and the first
  name tried, `pm4 packets`, was silently dropped by that pattern.
- `tools/golden.py` treats that name as raw: the words are compared verbatim,
  no model is asked for them, and `check-helpers` counts them apart from the
  calls it replays.
- The PC runner compiles `probes/c0/dispatch.spv` with ps5-opengl's own libpsbc,
  which reports no compute resource words, so it reads the four recorded ones
  (`probes/c0/resources.txt`: `rsrc1` `0x602c0001`, `rsrc2` `0x6`, `rsrc3` `0`,
  16 VGPRs — the values this console's compiler reported in both runs).
  Everything else about the dispatch is the same code on both builds, which is
  what makes the PC rebuild a comparison rather than a replay.

### The golden file and the PC checks

- `golden/c0` holds `c0-dispatch-1.json` (58 words, 2 calls, 12 workspace
  chunks) and `m2-solid-1.json` (114 words, 6 calls, 15 chunks), extracted from
  this run.
- `python3 tools/golden.py rebuild golden/c0`: 2 of 2 frames rebuilt
  identically on the PC, the compute stream included. The dispatch frame has no
  AGC register table of its own, so it replays the defaults another frame of its
  run shows — which is why the queue keeps `m2-solid` in it.
- `python3 tools/golden.py check-helpers golden/c0`: 7 of 7 recorded helper
  calls reproduced exactly, and 50 packet words reported as a raw span and
  compared verbatim.
- `python3 tools/agc_rules.py golden/c0`: 8 of 8 rules on both frames.
- On the PC the readback is NOT_REQUIRED: the host layer completes the
  submission's marker but runs no workgroup, so the word is not there to read.
  The console keeps the exact check.

`V0-compute` therefore meets the acceptance `M5_REFERENCE.md` sets: the console
readback is exact, the stream is a golden frame, `golden.py rebuild` reproduces
it on the PC, and `agc_rules.py` passes on it.

## 2026-09-16: Console run pid 109, the driver clears through vk_meta

Queue `jobs/c1-triangle` (`capture`, `hold 60`, `b7-triangle`, `c1-triangle`,
`m2-solid`), klog `Klog_Logs/c1b-clear-2.log`, runner PPSA99988 from commit
`7f661ab`. Three of three tests passed; the only non-passing record is the
known-benign VideoOut-busy warning on close.

**`c1-triangle`: 4 of 4 frames passed.** Each frame's attachment is cleared by
Mesa's vk_meta through this driver and the corner triangle is then drawn over
it by a second pipeline, all in one stream; the frame is presented, the images
alternate, and each one is read back before its flip.

| Frame | pixels in the triangle's colour, inside the diagonal | pixels in the clear's colour, outside it | pixels in neither |
| --- | --- | --- | --- |
| 0-3 | 4,139,524 of 4,139,524 | 4,139,524 of 4,139,524 | 0 |

Every word of the 8,294,400-word frame was written, so the clear covered the
whole attachment rather than leaving the zero of an untouched image. The clear's
colour appears nowhere the triangle draws and vice versa, and the sampler
records one pixel inside each half of every frame. `b7-triangle` passed before
it -- the driver's draw path with the user data every draw now writes -- and
`m2-solid` passed after it.

The first attempt at this probe (run pid 108, `Klog_Logs/c1b-clear.log`) failed
with every pixel in neither colour, and that was the check's fault, not the
GPU's: a display image is `B8G8R8A8_UNORM`, so its readback words pack as
`colour_word` does (`A<<24, R<<16, G<<8, B`), and both expectations were packed
the way the offscreen `R8G8B8A8` images are. `ps5vk_triangle.h` now names the
display's packing, and `check_split_frame` logs a pixel inside each half so the
next such mismatch is readable from the run.

### What this closes, and what it does not

- The driver's clear renders exactly on the console: C1b's acceptance for the
  clear itself, in the shape the earlier `c1-clear` probe recorded with raw
  AGC. `vk_meta`'s pipeline, the driver's vertex-buffer table, push-constant
  buffer and user data all reached the GPU.
- The PC still cannot compare a clearing frame. The model replays a pipeline's
  register tables from the console capture's workspace image, and the clear's
  pipeline is a second stage mapping that no capture carries, so
  `c1_present`'s PC test draws without a clear and keeps the golden frames from
  pid 142. A model that carries a pipeline the capture never ran is what closes
  that half.

## 2026-09-17: Console run pid 110, the first capture carrying pipelines

Queue `jobs/c1-triangle` (`capture`, `hold 60`, `b7-triangle`, `c1-triangle`,
`m2-solid`), klog `Klog_Logs/pipeline-capture.log`, runner PPSA99988 from commit
`67f9499`. All three tests passed; the only non-passing record is the
known-benign VideoOut-busy warning on close. The clear rendered exactly again:
every frame read back with 4,139,524 pixels of triangle colour inside the
diagonal and 4,139,524 of clear colour outside it.

This is the run steps 1 and 2 were for. The runner logged, for each driver
test, the stage mappings the driver compiled its pipelines into, and the
extractor turned them into golden files:

| Golden | Command words | Pipelines captured |
| --- | --- | --- |
| `golden/c1-triangle/b7-triangle-1.json` | 0 | 1, at `0x20002c000` (64 KiB, 9 chunks) |
| `golden/c1-triangle/c1-triangle-1.json` | 0 | 2, at `0x200070000` and `0x200080000` (8 and 9 chunks) |
| `golden/c1-triangle/m2-solid-1.json` | 114 | none: the runner's own frame, unchanged |

The clearing frame is the second: the corner pipeline and Mesa's vk_meta clear
pipeline, each with its own relocated headers and linked context and uniforms.
A capture before this run could carry only one pipeline, which is exactly why
the PC could not compare a clearing frame.

Two things came out of the extraction:

- A test that drives the Vulkan driver logs no command words, no workspace and
  no helper calls of its own -- the driver submits, not the runner -- so
  `capture_problems` treats a stream whose capture is pipelines and nothing
  else as complete once those pipelines validate, and `golden_document` writes
  a document with an empty command. Before that, the extractor dropped the
  records entirely: the parser only started a stream on a command stream.
- The goldens those tests produce have no context register table, because the
  driver builds its tables, not the runner. `tools/golden.py replay` refuses
  such a document, so replaying a driver capture needs the defaults from
  another frame of the same run -- the same rule `golden.py rebuild` already
  applies. That, and logging the driver's own regions (its swapchain images at
  `0x200400000` and `0x202400000`, which the runner does log), are what a
  replay of this capture needs next.

The comparison itself still needs a reference: `compare-present` builds its
expected stream from a console frame that clears *and* presents, and no such
golden exists yet -- `golden/c1-clear` clears and draws without a flip,
`golden/c1` presents without a clear. That probe is the next step in closing
C1b's PC side.

### Step 2's premise was wrong: a raw-AGC reference cannot pin two pipelines

The plan for closing C1b's PC side was "a console frame that clears *and*
presents", expecting the driver's clearing frames to be compared with a
runner-built one, the way the single-pipeline frames are. The region sizes say
that cannot work:

| Golden | Pipeline layout |
| --- | --- |
| `golden/c1-clear/c1-clear-1.json` | one `stage` region of `0x20000` (128 KiB): both package sets linked into its two halves |
| `golden/c1-triangle/c1-triangle-1.json` | two pipeline regions of `0x10000` (64 KiB) each, at `0x200070000` and `0x200080000` |

The replay hands a captured region to the first allocation *of its size*
(tools/golden.py, replay_text). A driver allocates one 64 KiB mapping per
pipeline stage (`ps5vk_pipeline_create_shaders`), so a 128 KiB reference region
can never pin them: the driver's second stage would land somewhere the
reference does not know, and the SH records that carry each pipeline's program
address would differ. The existing single-pipeline comparisons work only
because there the reference's one 64 KiB stage region *does* match the driver's
one 64 KiB stage, and the replay places the driver's mappings exactly where the
console's were.

Two ways out, and this is the finding that matters for the rest of Phase C:

1. **Capture the driver's own submission** -- its words, the regions they name
   (the command buffer's table chunks as well as the pipeline stages) and their
   images -- and compare the PC driver's dump against the console driver's
   stream. This is a direct comparison of the same code path, needs no proxy
   frame and no address gymnastics, and scales to any number of pipelines. It
   is also what the driver-path tests should have been comparing all along: the
   present comparison currently only works because a driver drawing the M2
   triangle happens to emit the runner's packets at the runner's addresses,
   which is a coupling the driver should not have to keep.
2. Compose a replay from a raw-AGC reference's addresses plus the driver
   capture's pipeline images, and teach the replay to hand one 128 KiB region
   out as two 64 KiB halves. Cheaper (no console run), but it compares the
   driver against a proxy and keeps the addresses as a fixture.

Step 1 is in either case: the driver capture is replayable (see above), which
is what the first option needs for its regions just as much as the second.

## 2026-09-17: Console run pid 112, the driver's own submissions

Queue `jobs/c1-triangle` (`capture`, `hold 60`, `b7-triangle`, `c1-triangle`,
`m2-solid`), klog `Klog_Logs/submission-capture-2.log`, runner PPSA99988 from
commit `aa88d14`. All three tests passed; the only non-passing record is the
known-benign VideoOut-busy warning on close. The clear rendered exactly again.

The driver now reports what it submits and the tables those words name
(`driver/ps5vk_debug.h`, commit `de77845`), and the runner logs them in a
capture. This run produced nine submissions, each complete and each with a
region image that reassembles to the FNV-1a 64 its record claims:

| Submission | Words | Packets | Table region |
| --- | --- | --- | --- |
| `b7-triangle` draw | 42 | 8 | `0x20003c000`, 3 chunks |
| `c1-triangle` frame 0-3 draws | 102 each | 18 each | `0x200030000`, 20 chunks |
| `c1-triangle` frame 0-3 flips | 64 each | 4 each | the same chunk |

The first attempt (run pid 111, `Klog_Logs/submission-capture.log`) captured the
same nine submissions but four of them, the flips, did not walk as type-3
packets: a flip is a stream of its own in the submission buffer
(`ps5vk_queue_flip`) and only the command-buffer path recorded its word count,
so a flip's record claimed the preceding draw's 102 dwords and read past the end
of the flip. `aa88d14` records its own count; the flips are 64 dwords now. The
draws were complete in both runs, and every region image verified in both, which
is what makes the capture comparable at all: the words say which address holds a
table, and the region image says what the table holds.

Why this exists: a *driver* frame cannot be compared with a runner-built
reference. A raw-AGC clearing reference holds both pipelines in one 128 KiB
stage region while a driver allocates one 64 KiB mapping per pipeline, so a
replay cannot pin them (`ca87db4`, the region-size finding). Capturing the
driver's own submission is the way out, and this run is the capture half of it.

Next: carry each submission and its regions in a golden file, and compare the PC
driver's dump against it -- the same code path on both sides, no proxy frame and
no address fixture.

## 2026-09-17: Console run pid 113, the VideoOut handle and what a flip holds

Queue `jobs/c1-triangle`, klog `Klog_Logs/submission-capture-3.log`, runner
PPSA99988 from commit `5da6c12`. All three tests passed. The capture is the
whole run in one directory now: `run-1.json` (the three pipelines, the
swapchain region at `0x200400000`, the VideoOut handle `0x4e100100`, and nine
submissions) plus one document per submission.

The frames walk exactly: `b7-triangle`'s submission is 42 words in 8 packets,
and each clearing frame is 102 words in 18 -- the wait, the meta clear
pipeline's three tables, its user data and its draw, the corner pipeline's
three tables, its user data and its draw, then the barrier and the marker.

### The flips say something about the flip helper

Each flip is 64 words in 4 packets, and all four walked as type-3 packets --
but that is a coincidence of stale memory, not what the helper wrote:

- the flip's own packets are 17 words: `0x79` (3), `0x37` (6) and `0x49`
  (8, the flip's marker);
- words 18-63 are a single `0x10` packet with 44 values, and its payload is
  byte-for-byte the *frame's* tail from word 19 on
  (`[19:] == frame[19:19+45]`).

So the console's `sceAgcDcbSetFlip` advances the command buffer by 64 words
while filling 17, leaving a 46-word region whose header exists and whose values
are whatever the buffer held. Either the real helper takes arguments our call
does not pass -- the driver passes video, buffer, mode and marker -- or it
expects the caller to fill a region it only reserves. It flips correctly on the
console, so this is a *PC-side* problem: the model's flip writes 17 words, so a
replay's flip stream cannot equal the console's until the helper is understood
and the driver fills what it reserves.

The frames are unaffected: the clear lives in the frame's stream, and that is
the stream this whole capture exists for.

### What the comparison does with it

`tools/golden.py compare-run golden/c1-triangle/run-1.json <dump> --test
c1-triangle` walks a PC dump against these submissions, packet by packet and
register record by register record, reading tables from the submission's
regions and the run's pipeline stages. Run against the PC's current C1 present
test it reports the expected differences: the console's frames clear and the
PC's test does not yet, and the console's viewport is the meta clear's 4096
rather than the application's 3840.

## 2026-09-17: Console runs pid 117 and 115, the flip helper out of context

Queue `jobs/c1-triangle`, klogs `Klog_Logs/flip-probe.log` (pid 117, runner
from `0c191db`) and `Klog_Logs/flip-probe-2.log` (pid 115, `2d0cd3c`). Four
tests were queued in both runs, all four reported PASS, and in both the
`c1-flip` probe wrote nothing -- which that verdict called PASS, because a
returned delta of zero and a write count of zero agreed with each other. The
agreement was real and the verdict was still wrong: it accepted "wrote nothing"
as "wrote exactly what it returned". `fedc0c8` asks the pattern pass whether
anything was written at all.

pid 117: `c1-flip` called `sceAgcDcbSetFlip` twice into a **stack** buffer of
its own, once zeroed and once filled with `0xa5a5a5a5`. Both passes:
`returned_delta 0x0`, `up_delta 0`, `words_written 0`, every word still the
pattern. A stack buffer is not a command buffer the helper accepts; it returned
its input unchanged.

pid 115: the same call with the first pass on the stack and the two measured
passes in the stage workspace at `0x4000`, so mapped memory was ruled out as
the reason. All three passes again `0x0`, `0`, `0`, every word untouched.
Nothing was wrong with the buffer: the probe passed the constant `0x4e100100`
(the handle run pid 113 recorded) while **no VideoOut was open in that test**.

What the two runs prove: `sceAgcDcbSetFlip` is not a pure encoder. It writes
nothing -- and returns its input -- unless the calling process holds an open
VideoOut, which is the state `ps5vk_queue_flip` calls it in and the probe did
not. `fedc0c8` moves the call into `c1-triangle`, after the fourth presented
frame and before the device that owns VideoOut is destroyed, passing the
handle `ps5vk_debug_video_handle` reports, and keeps `c1-flip` as the negative
control whose verdict now says FAIL. Both calls run in one queue on purpose,
so one launch answers the paired question: the helper with no VideoOut, and
the same helper with the driver's own.

The probe also stopped pairing each label with the wrong pass: the pass number
is logged before the pass, so the analysis of the next run reads them in order.

## 2026-09-17: Console run pid 109, the flip helper with the driver's VideoOut open

Queue `jobs/c1-triangle`, klog `Klog_Logs/flip-probe-3.log`, runner PPSA99988
from commit `fedc0c8`. `b7-triangle`, `c1-triangle` (4 of 4 frames drawn,
read back and presented) and `m2-solid` passed; `c1-flip` failed, which is the
negative control this run was paired with, and the runner summary says 3 of 4.

`c1-flip`, no VideoOut open, constant handle `0x4e100100`: all three passes
`returned_delta 0x0`, `up_delta 0`, `words_written 0`, every word still what
the pass had put there.

`c1-triangle`, the driver's own VideoOut open, handle `0x4e100100` reported by
`ps5vk_debug_video_handle`, called after the fourth presented frame:

| pass | buffer | `up_delta` | words written |
| --- | --- | --- | --- |
| 0 | stack, filled with the pattern | 64 | 19 |
| 1 | stage workspace, zeroed | 64 | 0 |
| 2 | stage workspace, filled with the pattern | 64 | 19 |

So the helper writes 19 words and advances `up` by 64, and the 45 words it does
not write stay the caller's: with the pattern in the buffer they still held it
after the call, and in run pid 113's flips they held the frame's own tail.
Word for word against `golden/c1-triangle/c1-triangle-2.json` (frame 0 flip),
the probe's 19 words differ only where the call's own arguments and VideoOut's
state go in:

| word | probe | golden | what it carries |
| --- | --- | --- | --- |
| 2 | `c7010109` | `c7010101` | the field that tracks which VideoOut buffer is flipped; this call used buffer 1, the golden's frame 0 buffer 0 |
| 8-9 | `9abcdef0 12345678` | `00000001 00000000` | the marker the caller passes |
| 13 | `800040a8` | `800040a0` | the same buffer's address |
| 17 | `08000107` | `08000101` | VideoOut's own flip counter |

Every other word -- the packet headers, their bodies and the `0x49` marker --
is byte-identical, and the counter walks 5, 6, 7 across the probe's three
calls, one per call, after the four frames' 1 to 4.

Two readings from the earlier entries are wrong, and the stack pass is what
settles them:

- **A stack buffer is accepted.** Words written is 19 in the stack pass with a
  VideoOut open, so pid 117's "nothing was written" was the missing VideoOut,
  not the buffer. The helper wrote the same 19 words into the stack and into
  the stage workspace, so it does not check where the command buffer is.
- **The helper's own words are 19, not 17.** The three packets run pid 113
  saw (`0x79`, `0x37`, `0x49`), with their alignment words, are 18 of them,
  and the 19th is the header of the `0x2c` packet that follows
  (`c02c1000`), whose 1 + 44 words are what fills the 64 the helper reserved.
  Counting only the packet bodies is what made the earlier entry say 17.

What this means for the PC model: `sceAgcDcbSetFlip` encodes a flip only
against an open VideoOut in the calling process, writes 19 of the 64 words it
reserves, and leaves the other 45 as whatever the caller's buffer already
held. Run pid 113's flips are therefore 64 words of which 45 are the frame's
own tail -- the shape the PC comparison has to reproduce before
`compare-run` can pass.

## 2026-09-17: Console run pid 110, and C1b's PC side closes

Queue `jobs/c1-triangle`, klog `Klog_Logs/flip-probe-4.log`, runner PPSA99988
built with the driver change below. The run has the shape of pid 109:
`c1-flip` fails as the negative control, `b7-triangle`, `c1-triangle` (4 of 4
frames read back exactly and presented) and `m2-solid` pass. What the run is
for is the capture, which is reproducible now, and the comparison built on it.

### What had to change

- **The driver zeroes its submission buffer once, at allocation**
  (`ps5vk_queue_init`). The AGC helpers reserve more words than they write --
  the wait packet 32 against its 16 and the flip 64 against its 19 -- and the
  console leaves the buffer's previous content in the rest. Direct memory is
  recycled without being cleared, so run pid 113's first frame carried another
  test's leftovers in words 16-28. One `memset` leaves those words a function
  of this queue's own submissions: zeros for the first, the previous
  submission's words after that.
- **The host model leaves them too** (`host/agc/agc_host.cpp`, both helpers).
  It zeroed the 16 wait words and the 45 flip words the console leaves alone,
  which is why a replay could never equal a driver stream.
- **The replay pins the driver's table chunk** (`tools/golden.py`). The run
  document now carries the table regions its submissions name -- 0x200030000,
  0x40000 for this run -- and `driver_replay_text` lists them like any other
  region, so the model hands the driver's chunk allocation the console's
  address. Without it the PC's chunk landed at 0x200000000 and every user-data
  value that points into the chunk differed by 0x30000. Regions another test's
  command buffer allocated are filtered by `--test`, as the stages are: a
  b7-triangle chunk listed for a c1-triangle replay moved the chunk and made
  pipeline creation fail.
- **The C1 present test clears again**, with the b8-corner set the console's
  c1-triangle draws, and `tools/check-driver.sh` compares its dump with
  `compare-run golden/c1-triangle/run-1.json --test c1-triangle` instead of
  the m2-solid proxy. Both changed together: either half alone breaks the
  other.

### The comparison

`tools/check-driver.sh`: PASS, 27 loader/direct/PS5-link runs plus the two
negative tests, with `c1_present` compared against the console's own driver
run instead of a runner-built proxy -- eight submissions, a frame and a flip
each, word for word. That is the wait packet's reserved words, both pipelines'
three table loads with their records read from the captured chunk, both
stages' user data, the draws, the barrier, the completion marker, and the
flips, which carry the frame's own tail in the words they reserve.

`golden/c1-triangle` is run pid 110 whole now: `run-1.json` with the pipelines,
the framebuffer and the table chunk, plus the eight c1-triangle submissions,
b7-triangle's and m2-solid's frames. This is what C1b's PC side needed: the
same code path on both sides, no proxy frame, and no address that only the
console's allocator could produce.

## 2026-09-17: the proxy comparison is gone

`tools/golden.py compare-present` and the helpers only it used
(`present_documents`, `driver_flip`, `call_packets`) are removed. Nothing has
called them since `tools/check-driver.sh` switched to `compare-run` against the
console's own driver run, and the proxy frame they compared with is the one the
region-size finding ruled out: a raw-AGC clearing reference holds both
pipelines in one 128 KiB region while a driver allocates one 64 KiB mapping
each, so a replay can never pin them (`ca87db4`).

`golden/c1` stays, because it is still the fixture for the models themselves:
`golden.py rebuild golden/c1` rebuilds 9 of 9 frames and
`golden.py check-helpers golden/c1` reproduces 42 of 42 recorded helper calls,
the wait and flip helpers among them -- 16 words of the wait's 32 and 19 of the
flip's 64, with the reserved words left to the buffer the call was made in. No
other tool read `c1-present-*.json`.

## 2026-09-17: the probe SPIR-V moves to Vulkan 1.0, and C1's last finding closes

- **Change.** `tools/build-probe-shaders.sh` and `tools/build-compute-probe.sh`
  compile their GLSL with `--target-env vulkan1.0` instead of `vulkan1.2`, and
  both PROVENANCE lines now name that environment. Every probe set was
  regenerated with the same scripts: the nine graphics sets and `c0`.
- **Why.** The driver's device reports Vulkan 1.0
  (`PS5VK_DEVICE_API_VERSION`, `driver/ps5vk_private.h`), so a SPIR-V 1.5
  module is invalid input for it, which is what
  `VUID-VkShaderModuleCreateInfo-pCode-08737` says. The validation layer's own
  message named the remedy: `spirv-val <input.spv> --target-env vulkan1.0`.
- **What the regeneration moved.** The SPIR-V, the PROVENANCE and the
  `SHA256SUMS` receipts. Nothing else: all 18 `*.bin` and all nine
  `checksums.txt` are byte-identical to their committed values, and so are
  `bindings.txt`, `compile.txt` and `probes/c0/resources.txt`. The receipts
  still record the same machine-code and metadata hashes
  (`build/shaders/<set>/*.ngg.bin`, `*.raw.bin`, `*.hw.json`), so the packages
  the console ran are the packages the scripts still write.
- **What actually differs in the SPIR-V.** Three things, across all ten sets:
  the version header; the vertex stages' `OpDecorate %indexable NonWritable`
  with its initialized `OpVariable` becoming an equivalent `OpStore`; and the
  fragment stages' `OpEntryPoint` interface lists dropping the `Uniform` and
  `UniformConstant` variables they named -- the `m3` and `c1-clear` colour and
  clear blocks and the `m3-texture` sampled image -- while keeping the `Output`
  and `Input` ones. `spirv-val --target-env vulkan1.0` names both rules, run
  against the 1.2 codegen with only its header word changed: "Target of
  NonWritable decoration is invalid: must point to a storage image, tensor
  variable in UniformConstant storage class, uniform block, or storage buffer",
  and "In SPIR-V 1.3 or earlier, OpEntryPoint interfaces must be OpVariables
  with Storage Class of Input(1) or Output(3)". Both forms are what glslang
  emits for the environment it was asked for, and ACO emits identical code from
  each, which is why no package moved.
- **Evidence, before.** The loader build of `c1_present` with
  `VK_LAYER_KHRONOS_validation`, shader-validation caching off and
  `PS5VK_PROBES` pointing at the committed (1.5) sets: one
  `VUID-VkShaderModuleCreateInfo-pCode-08737` per stage, "Invalid SPIR-V binary
  version 1.5 for target environment SPIR-V 1.0 (under Vulkan 1.0 semantics)".
  The program still passed its 11 checks, so the finding was conformance with
  the version the device claims, not a functional break.
- **Evidence, after.** The same run against the regenerated sets reports no
  VUID and no SPIR-V message, and 11 of 11 checks pass. `b7_draw` and
  `b8_groups` are clean too, 3 of 3 and 6 of 6. `b6_pipeline` passes 13 of 13;
  every validation message it produces belongs to one of its own negative
  cases -- a point-list pipeline, entry points the SPIR-V lacks, and modules
  without a shader -- except
  `VUID-VkGraphicsPipelineCreateInfo-renderPass-06041`, which the layer reports
  for the `m4-blend` set (and again for the deliberate `m4-depth`-with-blending
  variant) because `ps5vk_image.c` deliberately leaves
  `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT` out of R8G8B8A8_UNORM's
  features. That is a format-feature advertisement question, not a SPIR-V one,
  and it is recorded as an open finding rather than changed here.
- **Console side.** No console run was taken and none is owed: the packages
  `/golden/c1-triangle` was captured from are byte-identical, so `compare-run`
  still compares the same eight submissions, and the runner's `compile` mode
  recompiles the shipped SPIR-V on the console and must still reproduce those
  packages, which identical machine code keeps true.
- **Gates.** `make lint` PASS (127 attributed files); `make ci` PASS;
  `make inspect` `StaticErrors: 0`; `tools/check-driver.sh` PASS, 27
  loader/direct/PS5-link runs plus the two negatives, `c1_present` 11 of 11
  checks and its eight submissions identical to `golden/c1-triangle` word for
  word; `python3 -m unittest discover -s tests -p 'test_*.py'` 13 tests OK;
  `tools/test_agc_rules.py` 10 tests OK; `golden.py rebuild` 2 of 2 for
  `golden/c0`, 9 of 9 for `golden/c1`, 5 of 5 for `golden/c1-clear`; and
  `golden.py check-helpers golden/c1` 42 of 42.

## 2026-09-17: console runs pid 119, 120 and 121 -- C2's indexed draw, and the three things that had to be true first

**Result.** The driver's indexed draw runs on the console and its command stream
equals the console's own capture word for word: `tools/check-driver.sh` reports
`c2_indexed loader PASS`, `c2_indexed direct PASS` and
`c2-indexed draw: identical to c2-indexed-1.json: 11 packets, 3 register
tables`, with the console's runner summary 2 of 2 (`c2-indexed`, `m2-solid`).
The frame is exact: 2073600 of 2073600 square pixels drawn and none outside,
red `0x80` and alpha `0xff` in every one, and **max green error 0, max blue
error 0**.

Three defects stood between the first launch and that, all of them in this
project's own instruments rather than in the draw:

1. **The readback's channel order.** `check_vertex_frame` was written for the
   runner's B8G8R8A8 VideoOut framebuffer and was handed the driver's
   R8G8B8A8_UNORM image, whose red and blue bytes are swapped (SWAP_STD against
   SWAP_ALT). The record showed exactly that and nothing else: green exact in
   all 2073600 pixels -- bits 8-15 mean the same in both packings -- a maximum
   blue error of 128, which is the constant red `0x80` read from the wrong
   byte, and 7680 matching pixels, the two rows where the blue gradient itself
   passes through `0x80`. The check now takes the target's packing
   (`TargetPacking`: which bits hold red and blue, and which way its rows run).
2. **The row direction.** With the packing right, the samples showed blue
   `0xff` at the top of the square where the runner's own frame has blue `0x00`
   at the top, and the vertical centre exact: the driver programs Vulkan's
   viewport, whose y axis points down in framebuffer space, while the runner
   programs ProsperoLight's, so the same geometry lands turned over. That is
   the difference `tools/check-driver.sh` already declares for the b7 and b8
   draws as `PA_CL_VPORT_YSCALE` `0x111`, and C2's readback now states it
   instead of hiding it.
3. **The capture's regions.** The first capture pinned only the command
   buffer's table chunks, so the driver's own buffers landed elsewhere on the
   PC: 52 of the 54 words of the submission already matched, and the two that
   did not were `INDEX_BASE` and `DRAW_INDEX_2` naming the console's index
   buffer at `0x200030000` against the PC's `0x200004000`. A capture now also
   lists every live bound buffer (`ps5vk_debug_buffers`,
   `driver/ps5vk_buffer.c`), as the **allocation** rather than the buffer --
   the allocator is asked for whole direct-memory pages, so the console's
   12-byte index buffer and 96-byte vertex buffer are 16 KiB regions -- in
   allocation order, because a replay hands a region to the first allocation of
   its size and two buffers of equal size would otherwise swap addresses.
   Address-only regions carry no image, so the klog parser and the golden
   writer learned to accept a region without one.

Point 3 was proven before spending another launch: reordering those two regions
in a scratch copy of the extracted golden made the PC comparison report
`identical ... 11 packets, 3 register tables`, and the last capture then
produced that order natively.

**Runs.** pid 119 is the first launch (`c2-indexed` failed on the colour check
alone, coverage and padding exact); pid 120 is the first passing launch, whose
capture swapped the two buffers; pid 121 is the run this record's numbers come
from (`Klog_Logs/c2-indexed-run4.log`, klog not committed). `m2-solid` passed
after `c2-indexed` in every one of them, so the console renders normally after
an indexed draw.

**What C2 still owes.** Its acceptance also asks for a staging-buffer upload, a
test that writes a buffer in one command buffer and copies it in the next
(copies run on the CPU at submission split points, which needs
`ps5vk_CmdCopyMemoryKHR`), and base vertex and instancing, which stay refused
until a runner probe records them (`docs/M5_REFERENCE.md`, C2). This record
closes the indexed-draw half.

## 2026-09-17: console run pid 122 -- C2's staging upload, and buffers copied at a submission's split point

**Result.** `c2-staging` and `m2-solid` passed 2 of 2 on the console: 2073600 of
2073600 square pixels with none outside, red `0x80` and alpha `0xff` in every
one, and **max green error 0, max blue error 0**. The square's geometry reached
the buffers the draw binds through `vkCmdCopyBuffer` alone -- one command buffer
copied it out of a mapped staging buffer, the frame followed in the next, and
both reached the queue in one `vkQueueSubmit` -- so the readback is proof that
the GPU drew from memory only a CPU copy had written.

The driver's side of that is a submission split: it records the copy's ranges
and where they fall in a command buffer's words, then submits the words up to
the copy, waits for the completion marker, performs the memcpy, and submits the
rest (`driver/ps5vk_queue.c`). Memory is shared, so the copy itself is a
`memcpy` with the CPU cache lines evicted on both sides; what the split buys is
Vulkan's command order, which shared memory alone does not give. A step with no
words submits nothing: a command buffer that only records copies has nothing for
the GPU to do, and the step before it has already completed.

Two details worth recording, because both cost a run or a rewrite:

- A step's barrier and marker packets (16 words) are appended where the next
  step's words begin, so the naive version overwrote them. The queue keeps the
  built words aside and puts each step's range back before that step runs.
- The empty first step was still submitted at first, which made the PC's dump
  hold two submissions where the console's capture -- which reads the driver's
  own last submission -- holds one. Skipping the empty step fixed the
  comparison and removed a pointless DCB submission and marker poll per
  copy-only step.

A submission without copies takes the path it always took, and that is checked
rather than asserted: `c1_present` and `c2_indexed` still compare word for word
with their console goldens, which is where any change to the shared path would
show.

**Evidence.** `golden/c2-staging` is run pid 122 whole, with its sibling
`m2-solid` frame for the register defaults. `tools/check-driver.sh` reports
`c2-staging draw: identical to c2-staging-1.json: 11 packets, 3 register
tables` from both the loader and the direct build, `c2_staging` passes 5 of 5 PC
checks -- among them that the copied bytes equal the geometry the test passed,
byte for byte, read back from the two buffers the copy alone wrote -- and the
driver check is PASS over 33 runs plus the two negatives.

**What C2 still owes.** Base vertex and instancing. Both stay refused with a
message naming the probe that will settle them, and the probe has a starting
point now: the reference runtime programs `sceAgcDcbSetNumInstances` before
every multi-instance draw and resets it to 1 afterwards
(ps5-opengl's `src/platform/ps5_agc_runtime_backend.c`), which is an import this
project has not used yet.

## 2026-09-17: C2 closes -- the tutorial's vertex and index programs, and copies at submission split points

C2's acceptance criteria and where each one's evidence is:

| Criterion | Evidence |
| --- | --- |
| PC: the tutorial's vertex-buffer and index-buffer programs | `driver/tests/vk_c2_indexed_test.c` draws the m3-vertex canary's square through `vkCmdBindVertexBuffers`, `vkCmdBindIndexBuffer` and `vkCmdDrawIndexed`, 3 of 3 checks in the loader and direct builds |
| PC: the stream equals the golden -- vertex-buffer table, index size, index base and count, `DRAW_INDEX_2` | `c2-indexed draw: identical to c2-indexed-1.json: 11 packets, 3 register tables`, in both builds |
| PC: split submissions keep order | `driver/tests/vk_c2_staging_test.c`: the geometry is written into a mapped staging buffer, one command buffer copies it into the buffers the draw binds, the frame follows in the next, and both reach the queue in one `vkQueueSubmit`. The test reads the destination buffers afterwards and their bytes are the geometry it passed, byte for byte -- 5 of 5 checks |
| Console: both programs read back exactly, including a staging-buffer upload | console runs pid 121 (`c2-indexed`) and pid 122 (`c2-staging`), 2 of 2 tests each, 2073600 of 2073600 square pixels with none outside and max green and blue error 0 |

**One documented deviation.** The criterion says the streams "equal golden
`m3-vertex`". They are not compared with that frame, and cannot be: the runner
keeps its vertex and index data inside its 64 KiB stage workspace, while the
driver allocates buffers of its own, and `golden.py compare-submission` has no
address tolerance. C2's references are therefore the console's **own driver
runs** -- `golden/c2-indexed` (pid 121) and `golden/c2-staging` (pid 122) -- the
same shape C1 set with `golden/c1-triangle`, and the packet sequence they are
compared against is the one the m3-vertex canary recorded
(`INDEX_TYPE` `0x20000243` `0x00000400`, `INDEX_BASE`,
`INDEX_BUFFER_SIZE`, `DRAW_INDEX_2`, and no `DRAW_INDEX_AUTO`).

**What C2 does not include: base vertex and instancing.** The step's What column
names them, and its own note says `firstVertex`, `firstInstance` and instance
counts are unrecorded and need a runner probe first. They stay refused, with a
message naming that probe, so no unproven encoding reaches the GPU:
`ps5vk_cmd_draw` refuses a base vertex or an instance count other than 1, and
`ps5vk_CmdDrawIndexed` reaches that through its `vertexOffset`. The probe has a
starting point for whoever picks it up: the reference runtime programs
`sceAgcDcbSetNumInstances` before every multi-instance draw and resets it to 1
afterwards (`ps5-opengl`'s `src/platform/ps5_agc_runtime_backend.c`), an AGC
import this project has not used yet, and observing instancing needs a vertex
shader that reads its instance index, because two instances of one geometry
otherwise land on the same pixels.

C2's closing commit is `feat(vulkan): implement C2 - vertex, staging and index
buffers`.

## 2026-09-17: console run pid 123 -- C3's uniform buffer, from an application descriptor set

**Result.** `c3-uniform` and `m2-solid` passed 2 of 2, and both frames read back
exactly: the m3 canary's colour `0xff4080` in the first, then `0x30d060` after
the buffer was rewritten, with `first_mismatch_word` `0x0` in each. That is an
application-supplied uniform buffer reaching a shader through a descriptor set
the driver built -- layout, pool, allocation, update, binding and the table the
shader reads -- which is what C3 is for. The canary's own run of the same set
(`golden/runner/m3-uniform-1.json`) remains the hardware reference for the
descriptor's shape; what this run adds is that the *driver* produces it.

**Driver.** The six pool and set entry points plus `vkCmdBindDescriptorSets2KHR`
(`driver/ps5vk_descriptor_set.c`), the command buffer's bound sets, and a draw
path that builds each stage's set-0 table from them: the reserved push-constant
descriptor where C1b put it, and one descriptor per metadata binding at the
offset the compiler reads, in the stride form the hardware ran. Everything it
cannot encode is refused by name -- a combined image sampler is C4, more than
one set, a descriptor array or a dynamic offset is D1, an unwritten binding is
C3 -- and `ps5vk_draw_refusal` now refuses only sets beyond 0 instead of every
layout with a set layout.

Two things this run does not settle, both recorded as C3's remaining work:

- **A stage's table holds one set, and the compiler puts the reserved
  push-constant binding at its start**, where an application's first binding
  also begins. The draw refuses a stage that reads both rather than overwriting
  one with the other; the fix is to place the push-constant binding after the
  application's, at the layout's `table_bytes`.
- **C3's console criterion also names a transformed quad** with changing
  uniforms. The run above changes the uniform, and its frame is a full-target
  triangle: the quad, whose vertex stage reads the uniform rather than the pixel
  stage, is still to come.

**Evidence.** `golden/c3-uniform` is run pid 123 whole (two submissions, "draw
1" and "draw 2"), with its sibling `m2-solid` frame for the register defaults.
`tools/check-driver.sh` reports both submissions identical to it -- "c3-uniform
draw 1: identical to c3-uniform-1.json: 8 packets, 3 register tables", and the
same for draw 2, from the loader and the direct build -- and the PC test is 6 of
6, including that a second frame submits the same draw with different uniform
bytes.

## 2026-09-17: console run pid 124 -- C3's transformed quad, and C3 closes

**Result.** `c3-quad` and `m2-solid` passed 2 of 2, and both frames read back
exactly where their transforms put them: frame 0 judged 2067604 pixels of the
target's top-left quadrant with **2067604 matching, 0 mismatches and 0 strays**,
frame 1 the same count in the bottom-right quadrant. The quad's position comes
from a vertex buffer and its transform from a 16-byte uniform buffer at set 0,
binding 0 that the application supplies and replaces between frames, so the
readback proves a uniform read by the *vertex* stage -- the half C3 still owed
after pid 123 proved the pixel stage's.

**A blocker this step found before its run, worth recording.** The shared test
program declared the uniform binding for `VK_SHADER_STAGE_FRAGMENT_BIT`
unconditionally, because the m3 canary's pixel shader was the only reader. A
binding counts for a stage only if the pipeline layout declares it for that
stage (`ps5vk_descriptor_options`), so the c3-quad vertex stage was compiled
with no descriptor binding at all while its machine code still read the set-0
table pointer from a user SGPR: the draw would have written no table and the
shader would have dereferenced a null pointer. The caller now says which stages
read its uniform (`ps5vk_triangle_input.uniform_stages`); c3-uniform passes the
fragment stage, so its pipeline layout, its metadata and its stream are
unchanged and `golden/c3-uniform` still compares identical, which was checked
rather than argued.

**Evidence.** `golden/c3-quad` is run pid 124 whole (two submissions, "draw 1"
and "draw 2"). `tools/check-driver.sh` reports both identical to it -- "c3-quad
draw 1: identical to c3-quad-1.json: 11 packets, 3 register tables" and draw 2
with 10 packets -- from the loader and the direct build, and the PC test is 7 of
7, including that both frames submit with different transforms.

**C3's acceptance, where each part's evidence is:**

| Criterion | Evidence |
| --- | --- |
| PC: the tutorial's uniform-buffer program | `driver/tests/vk_c3_uniform_test.c`, 6 of 6 |
| PC: descriptor tables and pixel-stage user data equal the golden | `c3-uniform draw 1/2: identical ... 8 packets, 3 register tables` against `golden/c3-uniform` (the console's own driver run, as C1 and C2 do -- the runner frame cannot serve, for the reasons recorded under C2) |
| PC: push-constant data reaches the state the compiler expects without overwriting other user data | The reserved binding is placed at the set-0 layout's `table_bytes`, after the application's bindings, and the draw refuses two metadata bindings at one table entry rather than overwriting one (commit `4e5e947`) |
| Console: the uniform colour across frames with changing uniforms | console run pid 123: 2 of 2, `0xff4080` then `0x30d060`, `first_mismatch_word` `0x0` |
| Console: a transformed quad read back exactly across frames | console run pid 124: 2 of 2, 2067604 judged pixels matching in each frame's quadrant, 0 mismatches, 0 strays |

C3's closing commit is `feat(vulkan): implement C3 - descriptor sets, uniform
buffers and push constants`.

## 2026-09-17: console run pid 126 -- C4's texture, sampled through the driver

**Result.** `c4-texture` and `m2-solid` passed 2 of 2, and both frames read back
through the canary's own checker:

| Frame | Filter | Pixels | Max error |
| --- | --- | --- | --- |
| 0 | nearest | 2073600 of 2073600 within 0 | **0** |
| 1 | bilinear | 2073600 of 2073600 within 2 | **1** |

That is C4's console criterion for sampling: nearest exact texel for texel, and
bilinear within the tolerance the canary recorded (2 levels). The texels reached
the image through a mapped staging buffer and `vkCmdCopyBufferToImage`, which
the driver performs at a submission split point as it does the buffer copies;
the image's view and one of two samplers are named by a combined image sampler
descriptor the driver builds word for word as the M3 canary had it, and the
second frame changes the sampler the descriptor names rather than the texels.

**Evidence.** `golden/c4-texture` is run pid 126 whole (two submissions, "draw
1" and "draw 2"). `tools/check-driver.sh` reports both identical to it -- 11
packets and 3 register tables, then 10 -- from the loader and the direct build,
and the PC test is 10 of 10, the canary's pattern in the image byte for byte
among them. Every earlier console golden still compares identical, so the
texture path is additive: the samplers, the 48-byte descriptor and the image
copy changed nothing the C1, C2 and C3 runs had established.

**What C4 still owes: render to texture.** Its criterion names it, and the
driver refuses the case rather than guessing: sampling a tiled image the *same*
command buffer rendered into needs the colour barrier (`RELEASE_MEM` event 45)
between the render and the sample, and no stream carries that packet yet -- the
queue writes it once at the end of a submission, so a render in an *earlier*
submission is ordered and one in the same command buffer is not. The M4 canary
proved the hardware samples a target it drew earlier in the same stream
(`HARDWARE_FINDINGS.md`), so the packet's shape is recorded; putting it into the
draw's own words before a sampling draw is the remaining work. A residual hole
is recorded with it: a render in one command buffer and a sample in another of
the *same* submission is neither refused nor ordered.

## 2026-09-17: C4's render to texture, on the console (runs pid 143-148)

**Result.** `c4-rtt` and `m2-solid` passed 2 of 2 (`runner_summary`), and the
frame that renders the canary's texels into a colour image and then samples that
image in the same command buffer read back exact:

| Half | Pixels | Max error |
| --- | --- | --- |
| The image the fill pass rendered | 8294400 of 8294400 hold the texel the fill pass sampled | 0 |
| The frame sampling it (the case's criterion) | 2073600 of 2073600 square pixels, 0 outside | 0 |

That is C4's console criterion complete: the render half is a CPU readback of
the image itself through the tiled RGBA8 layout, and the sample half is the
canary's own checker over the square, unchanged from `c4-texture`.

**The failure the runs found.** The first three runs agreed on the image and
disagreed on the sample: the image held every texel exactly, while the frame
read *zero* for 40661 of its 2073600 square pixels (run 1), then 38325 (run 2),
then 36006 (run 3). Every non-zero pixel matched exactly (max error 0) and
nothing was drawn outside the square, so the missing pixels were not a
displaced sample but a sample that read nothing. Their shape named the cause:
they sat in a band at the *top* of the square -- which the driver's Vulkan
viewport turns into the image's *last* rows, across its full width -- so the
draw that samples the image read its most recently written rows before they had
reached memory. The mid-stream colour barrier (`RELEASE_MEM` event 45, control
12) flushes the colour buffers; it does not wait for the flush, and the sample's
first texture fetches overtook it.

**The fix.** A draw that samples a target an earlier draw in the same command
buffer rendered into now splits the submission where it starts, with no copy at
the split (`ps5vk_cmd_buffer_split`, `driver/ps5vk_cmd_buffer.c`): the queue
submits the words before it, waits for that step's completion marker -- the same
step boundary its CPU copies already rely on, written after the step's own
colour flush -- and only then runs the draw
(`driver/ps5vk_queue.c`, `ps5vk_queue_run_copy_steps`). The barrier stays in the
draw's own words; the split is what makes the sample wait for it.

**Evidence.** `golden/c4-rtt` is run pid 148: two submissions, "draw 1, step 1
of 2" (44 words, the fill pass) and "draw 1, step 2 of 2" (63 words, the
sampling draw, which opens with the colour barrier), and one pipeline. Both
compare identical on the PC from the loader and the direct build
(`tools/check-driver.sh c4_rtt`), and the PC test is 13 of 13: among them that
the submission reached the GPU as those two steps, that the second opens with
the barrier, and that each step's words end with its own barrier and completion
marker whole. The run before it, pid 147, is the same run without the split and
failed; pid 146 with the fix but the old driver archive in the deployed title
failed identically, which is how the prebuilt-archive trap below was found.

**Runs 4 and 5 failed; run 6 passed.** Run 4 tested a stale title: the runner
links `build/driver/ps5/libps5vk.ps5.a`, a prebuilt archive that `make deploy`
does not rebuild, so the fix was not in the deployed eboot and the numbers were
run 3's. `docs/DEPLOYMENT.md` now says to run `tools/build-driver.sh` after any
driver change. Run 5 had the fix and passed, but its capture showed the split's
first step mixed with the second's words: a step's end packets land on the first
words of the step after it, so a capture that reads the stream after the whole
submission sees two steps' words interleaved. The queue now keeps its own copy
of every step as it was submitted (`ps5vk_queue.c`, `ps5vk_debug_submission_steps`),
and the console logs each step as its own submission.

**A faster gate.** `tools/check-driver.sh` accepts test names:
`tools/check-driver.sh c4_rtt` builds and runs one test's three arms in about
six seconds instead of every test's -- a driver change no longer costs the full
three and a half minutes per turn.

**Still open.** A render in one command buffer and a sample in another of the
*same* submission is neither refused nor ordered: `ps5vk_sampled_image` sees
only the sampling command buffer's own targets. A submission of more than
`PS5VK_MAX_SUBMISSION_STEPS` steps keeps the first eight in its capture.

## 2026-09-17: C5's depth work, and what two console runs say (pids 149, 150)

**In progress, not proven.** C5's acceptance is "overlapping geometry resolves
by depth exactly, and cleared depth reads back as the clear value". The driver
now has the depth path that criterion needs, and no console run has yet read a
depth value back.

**What is implemented.** `ps5vk_CmdBeginRendering` accepts a D32_SFLOAT depth
attachment the size of the target (tiled like a colour one), a rendering may
have no colour attachment at all when it has a depth one (vk_meta's depth clear
is exactly that pass), and `ps5vk_cmd_draw` gives the draw's table the 16 DB
registers from the M4 canary's own values plus DB_DEPTH_CONTROL built from the
pipeline's depth state (`ps5vk_depth_control`: Z_ENABLE bit 1, Z_WRITE_ENABLE
bit 2, ZFUNC bits 4-6, where Vulkan's compare op order is the register's, so
LESS is 0x16 and ALWAYS 0x76 -- the two words the canary ran). A pipeline that
writes no colour zeroes CB_TARGET_MASK and CB_SHADER_MASK in its table, which is
what the vk_meta depth-clear pipeline asks for. All of it is additive: every
draw without a depth attachment records exactly the words it recorded before,
and the whole gate still passes (48 arms plus the two negatives). The harness
grew the depth attachment (`ps5vk_triangle_input.depth` and the fields beside
it), `driver/tests/vk_c5_depth_test.c` asserts the recorded registers against
the canary's values, and the console probe `c5-depth` draws the canary's two
rectangles over such an attachment and reads both targets back.

**What the console says.** Run pid 149 (`Klog_Logs/c5-depth-run1.log`) recorded
the tables exactly as intended -- `DB_Z_INFO` 0x80000183, the Z read and write
bases from the depth image, `DB_DEPTH_SIZE_XY` 0x086f0eff, `DB_DEPTH_CONTROL`
0x76 for the vk_meta clear and 0x16 for the frame's draw, the masks zeroed for
the clear and RGBA for the draw -- and yet nothing landed: the colour stayed
zero everywhere the geometry covers and every depth sample read 0. Run pid 150
bisected it with two more variants:

| Variant | Colour | Depth |
| --- | --- | --- |
| `c5-depth` (clear 1.0, test and write, LESS) | 5572800 of 8294400 | 0 of 8294400 |
| `c5-depth-notest` (test off, ALWAYS, write on) | 5961600 of 8294400 | 0 of 8294400 |
| `c5-depth-noclear` (test on, no clear) | 8294400 of 8294400 (nothing drawn, as predicted) | 8294400 of 8294400 |

The third row is the prediction that names the failing half: with the
attachment at its allocated zeros and LESS, every fragment is rejected, which is
what a depth buffer the clear never wrote looks like. The second row rules out
the test as the only fault -- with ALWAYS nothing can be rejected -- and shows
the geometry reaching the target only partly (one half of the far rectangle) and
still writing no depth at all, so the write path or the attachment itself is
also implicated.

**Next.** A control probe: the proven b7-corner triangle with a depth attachment
bound and both the depth test and the write off, which separates "the depth
attachment's presence changes what the frame draws" from "this probe's geometry
or pipeline is wrong". Then the write path (the Z bases and `DB_Z_INFO` against
the canary's own frame) if the control draws correctly.

## 2026-09-17: C5's depth test and write work; the clear does not (pids 151-153)

Three more console runs, one of them lost to a bug of mine. Their result is
that the depth attachment, the test and the write are all correct, and the only
part of C5's criterion still failing is the depth clear:

| Run | What it says |
| --- | --- |
| pid 151 | the title stopped inside my own checker: the no-attachment control passes a null depth pointer and the checker flushed it as if it were 33 MB. Fixed (a null check with the load-bearing comment) |
| pid 152 | the coarse colour map showed the far rectangle drawn *vertically mirrored*: the geometry is the M4 canary's, whose ndc_y puts target row 0 at the geometry's -y edge, and the driver programs Vulkan's y-down viewport, so it lands turned over -- the C2 checkers' rows_bottom_up, which this probe's checker lacked. My bug, not the driver's |
| pid 153 | with that fixed the colour is **8294400 of 8294400 exact**, and the depth is **2721600 of 2721600 exact inside the geometry** -- the near rectangle's 0.25, the far one's 0.75, the test rejecting the far where the near drew first. Outside the geometry every sample reads 0 where the clear's 1.0 belongs |

So the DB registers, DB_DEPTH_CONTROL, the tiled depth layout and the readback
are all proven on the console. What is left is vk_meta's depth clear: the run
that records it (its table carries DB_DEPTH_CONTROL 0x76, test and write with
ALWAYS, the colour masks zeroed, and its own 6-vertex rect buffer) leaves the
attachment at zero outside the geometry, and the frame's LESS draws then fail
against it -- which is exactly what the first run's bisection predicted.

**The lead.** The capture's region list holds the meta clear's rect buffer (96
bytes, the 6x16-byte rect record) at `0x200034000`, the address of a live table
chunk whose records the same capture shows intact. Both are allocated through
`sceKernelAllocateDirectMemory`, which cannot hand out one address twice, so one
of the two entries is stale -- most likely the debug buffer list carrying a
freed rect buffer from an earlier frame whose memory the chunk later took. The
next step is to settle that (what ps5vk_debug_buffers reports, and whether the
live chunk list is pruned) before instrumenting the clear itself: if the rect
buffer's live address is the chunk's, the clear's vertices are garbage and the
fix is in the allocator bookkeeping; if it is not, the clear's own draw needs
the next probe.

## 2026-09-17: C5 closes -- depth through the driver, proven on the console

**Result.** `runner_summary` 3 of 3 (pids 157 and 158; `Klog_Logs/c5-depth-run7.log`,
`golden/c5-depth`), and the criterion's own frame read back
**8,294,400 of 8,294,400 colour pixels and 8,294,400 of 8,294,400 depth samples
exact**: the near rectangle's 0.25 where it draws, the far one's 0.75 where only
it draws, the far rejected where the near drew first, and the clear's 1.0
everywhere else. That is C5's console criterion complete -- "overlapping
geometry resolves by depth exactly, and cleared depth reads back as the clear
value" -- and `tools/check-driver.sh c5_depth` reports the submission identical
to the golden from the loader and the direct build (17 packets, 6 register
tables), with the test 5 of 5 on the PC.

**What the driver records.** A D32_SFLOAT attachment the size of the target,
tiled like a colour one; every draw over such a rendering carries the M4
canary's 16 DB registers (`DB_Z_INFO` 0x80000183, the Z read and write bases
from the image, `DB_DEPTH_SIZE_XY`) and `DB_DEPTH_CONTROL` from its pipeline's
depth state (Z_ENABLE, Z_WRITE_ENABLE and ZFUNC, whose values are Vulkan's
compare-op order: LESS 0x16, ALWAYS 0x76); a rendering may have no colour
attachment when it has a depth one, which is exactly vk_meta's depth-clear pass,
and a pipeline that writes no colour zeroes the colour masks in its table. All
of it is additive: every draw without a depth attachment records the words it
recorded before C5, and the whole gate passes with it.

**Three bugs stood in the way, all of them ours, none of them the hardware's.**

1. The bisection's no-attachment control handed the checker a null depth pointer
   that it flushed as if it were 33 MB; the title stopped in the checker
   (pid 151). Fixed with the null check its comment explains.
2. The checker lacked the vertical flip every C2-style checker carries as
   `rows_bottom_up`: the geometry is the M4 canary's, whose `ndc_y` puts target
   row 0 at the geometry's -y edge, so under the driver's y-down viewport it
   lands turned over. The probe's coarse colour map is what showed it -- the far
   rectangle drawn mirrored -- and it also showed the driver's geometry,
   viewport and scissor were right all along (pid 152).
3. The harness gave `vkCmdBeginRenderPass` ONE clear value for a pass with TWO
   CLEAR attachments, so the depth clear drew whatever the stack held instead of
   the value the probe asked for: invalid Vulkan usage in the harness, and the
   reason the attachment stayed zero outside the geometry. With the depth value
   at index 1 the clear lands exactly (pid 158). The bisection had already
   proved the rest: with the test off, the geometry's own depths read back
   exact, and with no clear every LESS fragment is rejected as a zero-filled
   attachment says.

**Coverage kept.** The console's test table also holds the three bisection
variants -- `c5-depth-notest`, `c5-depth-noclear`, `c5-depth-nodepth` -- whose
expectations differ only in what the variant changes, so the next regression in
the depth test, the write or the clear names itself in the summary.

**Tooling.** A run that holds more than one driver test lost the later tests'
table-chunk regions: they were keyed by address alone, so the first test's entry
replaced the others' and a replay of the later test pinned nothing (the
c5_depth arm's first comparison). `golden.py extract` now keys them by test and
address, and `driver_replay_text` already filtered by test, so a run of several
driver tests replays each one.

## 2026-09-17: V0-formats -- the reported tables audited, and observed (pid 161)

**Result.** `runner_summary` 2 of 2 (`Klog_Logs/v0-formats-run3.log`), and the
run's `v0-formats` probe read **14 of 14 formats reporting exactly the features
`docs/V0_FORMATS_AUDIT.md` records**: the seven this driver has proved carry
their bits (R8G8B8A8_UNORM's sampled, linear, transfer and colour-attachment
bits among them), and the seven the audit records as gaps report nothing at all.
That is V0-formats' console evidence: the same question
`tools/format_audit.py` asks the driver's source, asked of the device.

**The audit.** `tools/format_audit.py` reads the vendored
`formats-v1.4.354.adoc`'s three "Mandatory Format Support" tables and the table
`driver/ps5vk_image.c` reports through `vkGetPhysicalDeviceFormatProperties`,
and prints every required feature no entry carries. When it was written: **179
formats are required, 7 are reported, 55 miss a required feature**, and 55 more
have only conditional requirements. `docs/V0_FORMATS_AUDIT.md` records that, the
seven entries with what proved them, and every gap family with the reason its bit
is off and what closes it -- the descriptor's format word, the AGC
`CB_COLOR0_INFO` word with its export format, C7's blit path, the DB format
words and the stencil registers, D2's storage path. `python3
tools/format_audit.py --check` exits non-zero while a gap remains, so the claim
and the record cannot drift apart silently.

**What this step does not claim.** The reported claim is still false for those 55
formats: a Vulkan 1.0 device must support them and this one does not. The step's
acceptance is the audit -- "every required format must either carry the correct
feature bits or be recorded as a deliberate gap with its tile-layout reason" --
and that is now a list instead of an unknown, with the driver reporting no bit
that no probe has proved.

**Two probe bugs, both mine, both in the runner.** The first entry named no
package set, and the runner stages a graphics package set for every test, so its
own setup reported INCONCLUSIVE and stopped the test before the probe ran
(pid 160). The second built its own instance through
`vk_icdGetInstanceProcAddr(NULL, ...)`, which this driver answers with nothing;
the probe now takes its instance and device from the harness every other driver
probe uses, whose create path the console has proved.

## 2026-09-17: V0-query, and a console that runs batteries by itself (pids 162, 111)

**What the step needed.** Vulkan 1.0 requires occlusion queries, and the only
GPU-to-CPU signal this project had recorded was a `RELEASE_MEM` completion
marker -- a write, not a counter. The probe therefore writes the packet
ps5-opengl's hardware-validated runtime emits for a query by hand: `ZPASS_DONE`
as `EVENT_WRITE` (`PKT3(0x46,2)`, event `0x115`) of the GPU's z-pass counter to
an address in the stage workspace, which the CPU reads back. No AGC helper emits
one, so the four words per sample are recorded as a `raw_packets` span and
compared word for word.

**Run pid 163, two regions.** The first battery (`Klog_Logs/v0-query-run1.log`)
sampled around a draw covering the whole 4K target and around the same draw
under a zero-area scissor. `masked_region` was **0**, exactly as a masked region
must be; `drawn_region` was **518,400** for 8,294,400 samples -- a clean number,
sixteen samples per count, which is what the console's occlusion counter counts
in. The counters themselves read as large negative 64-bit values in the log
because the low 32 bits carry the count and the high half a constant; the
difference is the measurement.

**Run pid 111, three regions.** The probe now measures that unit instead of
assuming it: the same triangle under a scissor covering none, all and half of
the target. `runner_summary` 2 of 2, and the three differences were **0, 518,400
and 259,200 counts for 0, 8,294,400 and 4,147,200 samples** -- one count per
sixteen samples at all three points:

```
PASS agc_query_region  0 of 8294400 pixels covered: 0 counts of the 0 a 16-sample unit predicts
PASS agc_query_region  8294400 of 8294400 pixels covered: 518400 counts of the 518400 a 16-sample unit predicts
PASS agc_query_region  4147200 of 8294400 pixels covered: 259200 counts of the 259200 a 16-sample unit predicts
PASS v0_query          the masked, full and half regions counted 0, 518400 and 259200 counts, one count per 16 samples
```

`golden/v0-query` is pid 111: three query frames (42 words, 8 helper calls each)
and `m2-solid`, whose 114 words `tools/golden.py rebuild` reproduces
identically on the PC. The three query frames have no PC rebuild because the
host runner builds without `AGC_VULKAN_DRIVER`, so driver probes are not in its
table; their PC comparison is the driver-side one, which is the driver's own
query path and its test (`docs/VULKAN_PROBE_ACTIVE.md`, Next 1).

**The console now runs a battery by itself.** Every battery costs one launch,
and a title cannot launch itself: the system refuses to launch a running
application, and a title a probe has just faulted the GPU inside cannot be
trusted to close itself. `payload/ps5vkctl` is a resident agent
(`tools/build-ps5vkctl.sh`, port 9111, logs to `/data/ps5vkctl.log`) that
launches, closes and restarts a title through
`sceLncUtilLaunchApp`/`sceLncUtilKillApp` -- the calls AirPSX and
ps5-payload-manager already make from a payload -- and refuses any title id but
the one the console is really running. A queue whose last line is `exit` makes
the runner leave the console when its batch is done, so the next deploy's eboot
is what the next launch loads. One command is the whole loop
(`tools/ps5_console.py battery`), and pid 111 ran through it: queue uploaded,
klog armed, title restarted by the agent, run captured, and the agent reporting
`ok idle` afterwards because the runner exited itself.

**Two launch facts, each found by a reload.** The user service must be
initialized before a launch is asked for: without `sceUserServiceInitialize` the
launch returns `0x2018` and the foreground user reads as 0 (pid 163's battery
never started a title, which is how the failure was found -- the agent logged
`sceLncUtilLaunchApp(PPSA99988, user 0) -> 0x00002018`). With it initialized the
foreground and logged-in user both read `515310723`, and the launch that worked
was `sceLncUtilLaunchApp` as that logged-in user (`ok restarted PPSA99988
result=0x8094000c via lncUtilApp/login as user 515310723`); `0x8094000c` is
"already running", which the agent treats as success and which the run
immediately after it confirms.

**One build fix on the way.** `tools/build-host-runner.sh` had not compiled
since C3: the host runner builds without `AGC_VULKAN_DRIVER`, so the driver
probes' definitions are unreferenced there and `-Werror` failed on five of them
(`run_occlusion_query_frames` among them). They are marked `[[maybe_unused]]`
now, and the rebuild goldens are unchanged: `golden/c0` 2 of 2, `golden/c1`
9 of 9, `golden/c1-clear` 5 of 5, `check-helpers golden/c1` 42 of 42.

**Addendum: the loop's other half, and a launch report the console decides.**
Run pid 112 ran the C5 battery through the same agent -- `runner_summary` 3 of 3
(`c5-depth-notest`, `c5-depth`, `m2-solid`, `Klog_Logs/c5-depth-run8.log`) --
and its queue has no `exit`, so the title stayed up and the kill path could be
asked for: `ps5vkctl` reported `ok running app=24600 title=PPSA99988` before,
then `ok killed PPSA99988: PPSA99988 is gone (app id 24600, 1 processes killed,
result 0x00000000)` -- suspend, `SIGKILL` to pid 112, `sceLncUtilKillApp` -- and
`ok idle` after. Both halves of the loop are therefore proven from the PC: the
agent started pids 111 and 112, and closed 112.

That run also showed the agent's launch report to be one step off: the reply
named the *second* attempt as the winner, which means the first attempt had
already started the title while answering with a code that is neither `0` nor
`already running` (etaHEN's own launcher treats a non-negative answer as
success for the same reason). The agent now asks the console which application
is running after each attempt -- waiting up to five seconds, because a launch is
asynchronous -- and reports the attempt that answer belongs to. The change is
built and uploaded (`build/ps5vkctl/ps5vkctl.elf`, 151,360 bytes) and takes
effect at the next agent load; the running agent (pid 110) still starts and
closes titles correctly.

## 2026-09-17: V0-query closes -- the driver's own occlusion queries (pids 113-115)

**What was missing.** The AGC-level probe (previous entry) proved the packet and
the counter; the driver could not answer a query at all. Vulkan 1.0 requires
`vkCreateQueryPool`, `vkCmdBeginQuery`, `vkCmdEndQuery`,
`vkCmdResetQueryPool` and `vkGetQueryPoolResults` for
`VK_QUERY_TYPE_OCCLUSION`, so the device's 1.0 claim was still false for them.

**The implementation** (`driver/ps5vk_query.c`, wired through the generated
entry points and `DRIVER_SRC`). A pool is one direct-memory mapping the GPU
writes and the CPU reads: two eight-byte counters per query. `vkCmdBeginQuery`
records one GFX10 `ZPASS_DONE` (`EVENT_WRITE`, packet `0xc0024600`, event
`0x00000115`, then the counter's address -- the words ps5-opengl's
`ps5_agc_emit_occlusion_sample` emits and the AGC probe measured) into the
first counter, `vkCmdEndQuery` one into the second, and the sample registers the
pool with the command buffer so submission evicts the counters' cache lines
before the CPU reads them. `vkGetQueryPoolResults` subtracts the two and
multiplies by sixteen, because the console measured one count per sixteen
samples; the count is therefore coarse, so the device keeps reporting
`occlusionQueryPrecise` false, and a result is exact for any region whose
samples are a whole number of counts -- every region whole fragments cover.
`ps5vk_triangle` grew the two knobs this needed: a pipeline scissor
(`use_scissor`/`scissor`, the default still the whole target) and the query a
frame records inside (`query_pool`/`query`, reset before the render pass that
carries the samples), plus `ps5vk_triangle_create_query_pool`,
`ps5vk_triangle_set_query` and `ps5vk_triangle_query_samples` for callers.

**Three console runs, one per region at the end.** Pid 113 ran the first
version -- one test drawing three regions under three scissors -- and passed:
`0`, `8,294,400` and `4,147,200` samples through `vkGetQueryPoolResults`, which
is the target's own sample count and half of it, one sample per pixel. The
golden could not be extracted from that run: a driver test's golden holds one
program's pipeline stages, and the three regions shared a test name, so the
capture read "3 of 1 pipeline stages logged". Pid 114 repeated it with the
stages logged, and pid 115 split the regions into three tests of their own
(`v0-query-masked`, `v0-query-full`, `v0-query-half`), which is what the golden
holds: `runner_summary` 4 of 4, `golden/v0-query-driver` with one submission and
one pipeline stage per region plus the `m2-solid` frame. Every run was started
and closed by the control payload's loop with no hand on the console
(`tools/ps5_console.py battery`).

**Why the numbers are the check, not the packet.** A driver that returned the
counter's own counts would answer 0, 518,400 and 259,200 -- the numbers the
AGC-level probe measured -- and fail here; returning 0, 8,294,400 and 4,147,200
is the scaling to samples that Vulkan's occlusion queries promise. Because
every region is a whole number of sixteen-sample counts, the coarse unit does
not blur these three numbers; a region of a few pixels would be.

**The PC side.** `driver/tests/vk_v0_query_full_test.c` runs the full region
through both of `tools/check-driver.sh`'s PC arms and the PS5 link: the direct
arm passes 9 of 9 checks -- including that the queued stream carries this
hardware's `ZPASS_DONE` sample and that the driver's own debug API sees the
submission -- the loader arm 7 of 7, and the link/converter arm accepts 168
imports. A replayed submission runs no GPU, so the counters keep the zero the
pool was created with and the PC reads 0 samples; the console's counts are the
golden's.

**Still open: the stream comparison.** `golden.py compare-run` against
`golden/v0-query-driver` differs in exactly two words per submission: the query
counters' addresses. On the console the pool's mapping took `0x2c000`, while the
PC's replay handed its own mapping `0x0`, because the pool's 16 KiB
direct-memory mapping is not an allocation the *runner* logs -- the capture's
regions are the runner's buffers, images and table chunks. Every other word of
the three submissions matches. The fix is to carry the driver's pool mapping in
the capture (a region the replay hands out, as `ALLOCATION_PROBES` already does
for the runner's own), after which `v0_query_full` can compare like C5's depth
test does; `tools/check-driver.sh` runs its arms against the console's replay
until then, and says so where the comparison would be.

**Addendum: V0-query closes completely -- the PC stream compares identical.**
The one difference the previous entry left open was two words per submission:
the query counters' addresses. On the console the pool's 16 KiB direct-memory
mapping took `0x20002c000`, while the PC replay handed its own mapping `0x0`,
because the pool's mapping is the driver's allocation and no *runner* probe
logged it -- the capture's regions are the runner's buffers, images and table
chunks, and `tools/ps5vk_log.py`'s region kinds name only those.

The driver now reports the mapping (`ps5vk_debug_query_pool_storage`,
`driver/ps5vk_debug.h`), the full-region probe logs it as the `agc_query_pool`
region (address and size in bytes) when it captures, and `ALLOCATION_PROBES`
learned that kind. A replay hands a region to the first allocation of its size
(`host/ps5/ps5_host.cpp`), so the PC's pool now takes the console's own
`0x20002c000` and the two streams agree word for word. Only the full region
logs it: that is the test `tools/check-driver.sh` compares, and a region name
shared by three tests would keep only the last of them.

Console run pid 116 (`Klog_Logs/v0-query-driver-run4.log`) re-ran the three
region tests and `m2-solid` -- `runner_summary` 4 of 4 -- and
`golden/v0-query-driver` is that run. `tools/check-driver.sh v0_query_full` now
reports, for the direct arm, **`identical to v0-query-full-1.json: 10 packets,
3 register tables`**, with 9 of 9 of the test's own checks passed, the loader
arm passing and the PS5 link accepting 168 imports. V0-query therefore has what
the objective asks of a step: console runs (pids 111 and 162 for the AGC-level
probe, 113-116 for the driver's own queries), captured goldens
(`golden/v0-query`, `golden/v0-query-driver`) and a PC comparison that is
byte-identical word for word (`compare-run`, plus the `m2-solid` frame inside
each golden rebuilt identically by `golden.py rebuild`).

## 2026-09-17: C7's mip probe, and what the console does with a mip chain (pids 117-134)

**What C7 needed.** Mip levels in descriptors and sampling, and `vkCmdBlitImage`
as draws or as CPU copies at split points (docs/M5_REFERENCE.md, C7). The
driver already sized a chain's levels by ps5-opengl's rules; what had never run
was a descriptor that names more than one level.

**What was built.** The sampler encodes the mip filter (bits 26-27 of word 10,
ps5-opengl's own encoding, replacing the canary's bits rather than OR-ing into
them -- OR-ing left 3, which the first run read as a wrong level) and the LOD
range (word 9, `min | (max << 12)` with ps5-opengl's 1/256 packing); the
descriptor carries the view's first and last level in word 3 and word 5; the
copy path takes `imageSubresource.mipLevel` and lays a level out the way
`ps5vk_image_level_layout` computes; `ps5vk_triangle` gained a chain's upload
(one staging buffer, one copy per level, a solid colour per level, a view of
the whole chain or of one level), mip-filter samplers and a LOD-range pin. A
probe (`run_vulkan_mip_frames`, four designs deep) and six shader sets
(`probes/c7-mip`, `-linear`, `-diag`) came with it.

**What the console answered: level 0, always.** Four independent ways of naming
a level -- ps5-opengl's descriptor fields, a pinned LOD range, an explicit
`textureLod`, and a view of the level alone -- all read level 0's grey, while
the chain's storage was measured byte for byte and is correct (pids 117-134;
the table and the offsets are in `HARDWARE_FINDINGS.md`). The probe's own
history is worth keeping: bands read level 0 whatever the shader computed; a
frame's occupancy showed the *target was never cleared*, so stale pixels from
the previous program were being measured as the frame's own (pids 117-121,
fixed by clearing with `loadOp = CLEAR`); and the level's storage, its upload
and the vertex data the draw fetches were each verified before the sampling
question was called (pids 122-130).

**What stands.** The per-level storage, the level layout and the per-level
upload are proven; the sampler's and descriptor's level fields are recorded but
this console does not act on them for a linear texture. The driver now refuses
a view of more than one level, or a base level above 0, by name -- an honest
refusal instead of silently answering every sample with level 0 -- and the
probe's two tests are out of the runner's table until an encoding works,
`jobs/c7-mip/queue.txt` included. **C7 is therefore open**: mip *sampling* on a
tiled chain (where each level's tiles could carry the level) and
`vkCmdBlitImage` are what remain, and `docs/VULKAN_PROBE_ACTIVE.md` says so.

## 2026-09-17: C2's base vertex, and instancing still open (pids 141-143)

**Base vertex: proven.** An indexed draw's `vkCmdDrawIndexed` takes a
vertexOffset the hardware adds to every index it fetches, and the driver
already writes it into the vertex stage's base-vertex user data -- RADV's NGG
ABI, the same slot the corner draws pass 0 through (ps5vk_draw.c,
ps5vk_cmd_draw). What was missing was the proof, and the driver refused the
value until it existed. The probe puts **two quads in one vertex buffer**, the
second the first with another red (0x80 then 0x20), and draws the same indices
twice with vertexOffset 0 and 4: `runner_summary` 2 of 2 (pid 141), both frames
read back the quad their base vertex names -- exact red, green and blue
gradients included, through the C2 checker unchanged apart from which red it
expects -- and `golden/c2-base-vertex` holds the two submissions and the
program's pipeline stages. The refusal is gone; a first *instance* and a first
vertex on a *non-indexed* draw are still refused, because nothing has probed
either.

**Instancing: implemented, not yet proven.** The driver asks the hardware for a
draw's instance count with the AGC library's own packet,
`sceAgcDcbSetNumInstances`, around the draw, exactly as ps5-opengl's runtime
does (`ps5_agc_draw_index_instanced`), and puts it back to one afterwards; the
symbols are declared in ps5vk_private.h and stubbed for the host link and the
four system link stubs, with the host model emitting nothing until the packet's
words are recorded from a console capture. `probes/c2-instance` colours the
quad by its instance builtins so a frame says how many instances ran. It does
not yet: with one instance the square reads red, with three it reads red again,
and the last run -- colouring red by gl_InstanceID and green by
gl_InstanceIndex, a quarter scale each -- read (1.0, 0.0) in both frames, a
colour neither the shader's formula nor the clear explains (pids 142, 143).
Two candidate causes are left to separate: whether the count reaches the
hardware, and whether the compiler provides either instance builtin. The
probe's test is out of the runner's table, `jobs/c2-base-vertex/queue.txt` names
it in a comment, and the driver keeps refusing a first instance.

**Addendum: the instancing probe's frames do not answer to its shader.** Five
console runs (pids 142, 143 and 144-146) read back a byte-identical frame from
`c2-instancing` -- a yellow (1, 1, 0) band of 26,880 pixels inside the check
region, zeros elsewhere -- while the shader package it draws with changed three
times: the instance-builtin colours, a constant yellow, and back to the
instance builtins; a *fresh* probe directory (`probes/c2-inst2`, whose
`vertex.bin` hashes to the same current build) drew the same yellow band; and
the deployed file was verified over FTP to be the current build byte for byte
(`sha256 5810ffce...`, the file the runner logs loading from
`/app0/probes/c2-instance/vertex.bin`). The kernel clear (`loadOp = CLEAR`) is
not the cause either: with `DONT_CARE` the same band appears, and a subsequent
draw's explicit `vkCmdSetViewport`/`vkCmdSetScissor` changes nothing.

What that leaves is not a rendering question but a *tooling* one: the frame
does not come from the shader file the probe reads, so the next round must
establish where it does come from -- the capture's pipeline stage images name
the shader the AGC object was created from, and a shader that writes magenta
under a *renamed test* is the experiment that separates a stale stage from a
stale draw. Until then `c2-instancing` stays out of the runner's table,
`jobs/c2-base-vertex/queue.txt` names it in a comment, and C2's instancing
remains open (the base vertex is proven, pid 141).

**Addendum: the instancing probe's shader never compiled.** The five runs whose
frames "did not answer to the shader" had a cause in this repository: the
probe's vertex shader named **`gl_InstanceID`**, which Vulkan GLSL does not
declare -- glslang refuses the shader with `'gl_InstanceID' : undeclared
identifier (Did you mean gl_InstanceIndex?)` and exits 2. The build then
packaged the *previous* build's SPIR-V, because `tools/build-probe-shaders.sh`
touched the output file and left a stale module in place when the compile
failed, and because every invocation in that work was piped through `tail`,
which is what hid the non-zero exit. `compile_spirv` now removes the output
before compiling, aborts with glslang's own first errors, and requires a
non-empty module, so a stale package cannot be shipped again.

With `gl_InstanceIndex` the package rebuilds (the SPIR-V carries
`OpDecorate %gl_InstanceIndex BuiltIn InstanceIndex`) and the frame finally
holds the shader's colour: instance 0's `(1, 0, 0.5)` reads back as `0x8000ff`
(run15, 236,160 of the check region's 432,000 pixels, the rest outside the
square). What is still open is the *count*: a frame drawn with
`instanceCount = 1` and one drawn with `instanceCount = 3` come back the same --
one square, in instance 0's colour, at a place the shader's own arithmetic does
not put instance 0 (run17) -- so C2's instancing stays out of the runner's
table, and whether `sceAgcDcbSetNumInstances` reaches the hardware is the next
round's first question.

Two things the C7 finding does **not** owe to this: its sampling runs used the
m3-texture package, whose shaders were never touched, and its pinned-LOD and
level-view tests measured the *colour* a sample returns, which is
geometry-independent. The C7 runs that measured *where* squares landed did use
the c7 packages and are therefore worth re-reading with the fixed build.

## 2026-09-17: C2 closes for real -- instancing, and two defects it found

**Result.** Console run pid 124 (`Klog_Logs/c2-instancing-run19.log`, queue
`jobs/c2-instancing/queue.txt`) passed **2 of 2**: `c2-instancing` and the
`m2-solid` control after it. `golden/c2-instancing` is that run: two
submissions, one pipeline stage, and the `c2-instance` program's tables.

**What the step was waiting for.** C2's instancing probe draws the m3-vertex
square twice through `probes/c2-instance` -- whose vertex shader scales the quad
to a quarter and moves instance *i* to `(i-1)*0.6` of NDC space, colouring it by
`gl_InstanceIndex` -- once with `vkCmdDrawIndexed`'s `instanceCount` 1 and once
with 3. The driver asks the hardware for the count with the AGC library's own
packet, `sceAgcDcbSetNumInstances`, the import ps5-opengl's runtime uses
(`ps5_agc_set_instances`), and C2's acceptance criteria for the step name it.

**Defect 1: the count packet's place.** Run 17 asked for three instances and
drew one. The driver programmed the count **before** the register tables and put
it back to one **before** the draw, so the reset that followed the *previous*
draw was what the draw read. The count is the packet the draw that follows it
reads, so it now sits immediately before the draw and the reset immediately
after it -- the order radeonsi and RADV use, and the order ps5-opengl's runtime
describes. The console's own stream now says so, word for word:

```
one instance    55 dwords: tables, user data, INDEX_BASE, INDEX_COUNT, DRAW_INDEX_2
three instances 56 dwords: tables, user data, 0xc0002f00 0x3 (NUM_INSTANCES),
                           INDEX_BASE, INDEX_COUNT, DRAW_INDEX_2,
                           0xc0002f00 0x1 (back to one)
```

`host/agc/agc_host.cpp` emitted nothing for that helper until now; it emits
`pkt3(0x2f, 0)` and the count, which is the console's header and body exactly,
so the PC comparison is word for word rather than documented.

**Defect 2: the frame's readback was row-major.** The target a driver test draws
into is the console's **tiled** RGBA8 image (128x128 blocks of 0x10000 bytes
with an XOR pixel map), which every other readback already reads through
`FramebufferView`/`kTiledRgba8Layout`. The first version of the instancing check
read `words[row * kOutputWidth + column]`. Run 18 failed all six of its boxes
while the *colour counts* were exactly right -- 129,600 pixels of each instance
colour, which is a 480x270 square -- because a tiling is a permutation: the
counts survive it and the coordinates do not. The check now reads through the
view, and a cleared pixel is the zero word rather than the `0xff8040` an older
probe's target held.

**Three boxes, and what they prove.** With the count honoured and the layout
read correctly, each frame holds exactly the boxes the shader's own maths puts
the instances in. Each square is 480x270 px on rows 945..1214, and the check
reads a 120x66-sample box at each instance's centre (allowing one 8-bit step per
channel, because 0.75 arrives as 191 or 192 and 0.5 as 127 or 128):

| Frame | instance 0 (columns 528..1007) | instance 1 (1680..2159) | instance 2 (2832..3311) |
| --- | --- | --- | --- |
| one instance | 7,920 of 7,920 of `0x8000ff` | 7,920 of 7,920 of the clear | 7,920 of 7,920 of the clear |
| three instances | 7,920 of 7,920 of `0x8000ff` | 7,920 of 7,920 of `0x8040bf` | 7,920 of 7,920 of `0x808080` |

and the whole-frame counts are 129,600 px of each instance's own colour: exactly
`(kOutputWidth/8) * (kOutputHeight/8)`. One instance leaving the other boxes
clear is what says the count reached the hardware; three boxes each with their
own colour is what says every instance read its own `gl_InstanceIndex`.

**PC comparison.** `driver/tests/vk_c2_instancing_test.c` draws the same two
frames through the driver on the PC against `golden/c2-instancing`, and
`tools/check-driver.sh c2_instancing` reports the loader arm PASS, the direct arm
PASS and the PS5 link accepting 169 imports. It is in the test list, the replay
set and the `compare-run` table of `tools/check-driver.sh`.

**A third defect, in the PC model, found by that comparison.** The host layer's
replay parser read a region name into a 32-byte buffer with `%31s`. Region names
are `tables-<test>-<address>` -- 32 characters for this golden -- so the name was
truncated into the address that followed it and the region parsed as address 0
with a size of `0x200044000`. Strict replay then handed *no* region to the
vertex, index or table allocations: the PC test's index buffer landed at
`0x200004000` where the console's is `0x200030000`, and the comparison reported
exactly those two packets. `golden/v0-query-driver`'s replay has the same
32-character names and had been replayed the same way -- its comparison passed
anyway, so the defect was invisible there. `host/ps5/ps5_host.cpp` now holds 64
bytes and scans `%63s`, and `check-driver.sh c2_instancing v0_query_full` passes
both.

**What C2 still refuses.** A first instance (`firstInstance != 0`) and a first
vertex on a **non-indexed** draw are refused by name: no recorded stream sets
either. An indexed draw's base vertex is not refused any more -- pid 141 proved
it.

C2's closing commit is `feat(vulkan): prove C2's instancing and read the tiled target`.

## 2026-09-17: C7's mip sampling, bisected to the descriptor and past it

**Result.** Console runs pid 125, 126 and 127 (`Klog_Logs/c7-mip-run25.log` to
`run27.log`) all read **level 0's texel in every band of every frame**, with the
descriptor's level fields verified word for word in the capture. C7's sampling
half is still open, but the space of explanations is now small: nothing the
descriptor can say about levels moves this console's sample off the texel at the
descriptor's own base address.

**The probe is now one draw per filter, five bands.** `run_vulkan_mip_frames`
draws five horizontal bands through `probes/c7-mip` (or `c7-mip-linear`); the
pixel shader takes its LOD from the fragment's own row (`floor(5 * y / 2160)`),
so one draw measures all five levels of the chain. Frames 1 to 5 then pin the
sampler's LOD range to one level each (`vkCmdSetSampler`-free: the descriptor's
word 9 is the range), which is the bisection: a frame that cannot read level *k*
with the range pinned to *k* cannot read it at all. The readback goes through
`FramebufferView`/`kTiledRgba8Layout` like every other one, and each band's box
is judged on its middle pixel's grey plus nine tenths of the box, because a
band's box can clip a quad edge the rasteriser rounds.

**What the console's descriptor held.** `golden.py extract` of pid 126's
submissions shows the 12 descriptor words the driver wrote, and they are
ps5-opengl's own encoding: word 3 `0x90040fac` (type 2D, first level 0, last
level 4), word 5 `0x00400040` (MAX_MIP 4, which is the image's last level), word
9 `0x00400000` (LOD range 0 to 4) for the band frame and `0x00400400` (range 4 to
4) for the frame pinned to level 4, word 10 with the mip filter in bits 26-27,
and word 2's `RESOURCE_LEVEL` bit set. The chain's storage holds each level's
grey at its own per-level offset (the `agc_mip_storage` record: 0xa0a0a,
0x646464, 0xc8c8c8, 0x282828, 0x3c3c3c).

**What the hardware did with it.** Every band of every frame read the grey at
the descriptor's base address: 10, level 0's. That is true

- for a view of the whole chain (first level 0, last level 4, MAX_MIP 4),
- for an explicit `textureLod(band)` in the shader (the SPIR-V carries
  `OpImageSampleExplicitLod ... Lod`, checked with `spirv-dis`),
- for a **linear** mip filter as well as a nearest one,
- and for the LOD range pinned to each level in turn, minLod = maxLod = k,
  where word 9 held `k * 256` in both fields,
- and for a view of one level alone (the earlier runs, pid 134).

**One driver change came out of it.** Word 5's MAX_MIP field carried the
*view's* last level; ps5-opengl writes the **image's** last level there
(`descriptor[5] = 0x00400000 | (texture->base.last_level << 4)`,
`ps5_opengl_sdk/src/gallium/ps5/ps5_screen.c`), and Mesa's GFX10 descriptor
builder does the same (`S_00A014_MAX_MIP(state->num_levels - 1)`,
`.deps/native/mesa/.../ac_descriptors.c`). `ps5vk_write_image_descriptor` now
writes the image's last level, which is what a view of one level of a five-level
image needs to leave the chain's length intact. The console still read level 0,
so this is correctness against both references rather than the fix.

**What is left.** The one property the probe has not varied is the storage: this
chain is **linear** (SW_MODE 0). The next probe is a **tiled** chain -- an image
whose levels are the driver's 64 KiB tiles, written by the CPU through the tile
map the target readbacks already use -- because on this console level selection
may exist only for tiled storage. `vkCmdBlitImage` is untouched, and the two
tests stay in the runner's table so a queue can name them: they fail with the
bands' greys in the log, which is the measurement.

## 2026-09-17: C7's tiled chain selects levels -- the layout is what is wrong

**Result.** Console runs pid 128, 129, 130 and 131 measured a **tiled** mip chain and
found what the linear one could not do: the LOD selects the storage the sample
reads. The chain is the same five solid levels, but stored the way the driver
stores a colour attachment -- 64 KiB tiles of 128x128 texels, one level after
another (`ps5vk_image.c`, `ps5vk_image_storage`), created with the
colour-attachment usage that asks for that storage, and filled by the probe
itself through the tile map the target readbacks decode, because the driver
records no copy into tiled storage.

**The measurement.** The probe's last form writes **every 4 KiB page of the
chain's storage with its own index** in the target's low two bytes
(`fill_tiled_pages`, `run_vulkan_page_mip_frames`), so the two bytes a frame
reads name the page the hardware addressed, and logs an 8x8 grid of those pages
per frame. With the sampler's LOD range pinned to level k, the frame read:

| Pinned LOD | Page (4 KiB) | Byte offset |
| --- | --- | --- |
| 0 | 0x50 | 0x50000 |
| 1 | 0x1c | 0x1c000 |
| 2 | 0x08 | 0x08000 |
| 3 | 0x06 | 0x06000 |
| 4 | 0x01 | 0x01000 |

Five distinct pages for five LODs, and the band frame -- whose LOD comes from
the fragment's own row -- read exactly the same five pages in the same order
(0x50, 0x1c, 0x08, 0x06, 0x01). So on this console:

- **A tiled chain's LOD reaches the sampler's address.** The linear chain's
  five failed encodings were not a hardware dead end: with tiled storage the
  same descriptor fields address five different places.
- **The addresses are not the driver's layout.** The driver puts level 0 at 0,
  level 1 at 0x40000, level 2 at 0x50000, level 3 at 0x60000 and level 4 at
  0x70000 (each level's tiles after the last); the hardware reads level 4's
  texels at 0x1000, level 3's at 0x6000, level 2's at 0x8000, level 1's at
  0x1c000 and level 0's at 0x50000, with the 8x8 page grids showing the intra-
  level swizzle too. The regions the grids confine each level to are 0x4000,
  0x4000, 0x8000, 0x10000 and 0x40000 bytes from 0 up -- the small levels
  first, each rounded to a power of two, and the whole chain inside 0x60000
  bytes where the driver's own layout needs 0x80000.
- That is a real hardware layout rule, not noise: it is what AddrLib computes
  (`gfx9addrlib.cpp`, `Gfx9Lib::ComputeMipChainInfo` with `GetMipTailDim` and
  `MipTailOffset256B`), and `.deps/native/mesa/mesa-26.2.0/src/amd/addrlib/`
  holds its source. C7's next step is to implement that rule for the driver's
  tiled chains -- the per-level bases and the swizzle the grids measured -- and
  then a chain whose every texel is distinct proves it, which is what the
  five-solid-level probe has been building towards.

The two tests stay in the runner's table (`c7-mip-tiled`, `c7-mip-pages`) so a
queue can name them: they fail with the map in the log, which is the
measurement. The page grid is logged as `agc_mip_page_grid` rows, the pinned
pages as `agc_mip_pages`, and `c7-mip-pages` passes when the five pinned LODs
read five different pages -- which run 130 did.

## 2026-09-17: C7's mip sampling works -- the tiled chain reads all five levels

**Result.** Console run pid 134 (`Klog_Logs/c7-mip-run34.log`, queue
`jobs/c7-mip/queue.txt`) passed `c7-mip-tiled` **and** `c7-mip-pages`:

```
PASS agc_mip_levels  a nearest mip filter read every level's own grey
     agc_mip_levels  read 5 of 5, uniform_bands 5
PASS agc_c7_mip      the tiled chain read 5 of 5 levels from the fragment's own
                     LOD, and 5 of 5 with the sampler's range pinned to one
                     level each
PASS agc_mip_addresses  the address map read 5 of 5 addresses, one per pinned LOD
```

The two **linear** chain tests still fail, and that is the finding: level
selection needs tiled storage (pid 125-127 measured the linear chain reading
level 0 under every descriptor encoding).

**How the chain is stored.** The address map did the work: every 4-byte word of
the chain's storage holds its own byte offset in its low 24 bits, so the colour
a frame reads IS the address the sampler fetched, to the byte. Run 33's map
(pids 131-133's 32x32 grids) shows, for the five-level 256x256 RGBA8 chain:

| Level | Side | Base | Region the map confines it to |
| --- | --- | --- | --- |
| 0 | 256 | 0x20000 | 0x20000..0x5ffff |
| 1 | 128 | 0x10000 | 0x10000..0x1ffff |
| 2 | 64 | 0x8000 | 0x8000..0xbfff |
| 3 | 32 | 0x4800 | 0x4800..0x7bfc |
| 4 | 16 | 0x800 | 0x800..0x19fc |

and the texel map **inside** each level is the driver's own
`tiled_rgba8_offset` map exactly: 1024 of 1024 sampled texels of the 16x16 level
and 1024 of 1024 of the 32x32 level land where that map says, with the larger
levels matching to within the grid's own texel rounding. The bases are *not*
the driver's tiled sizing (which puts each level's tiles after the last: 0,
0x40000, 0x50000, 0x60000, 0x70000): this console reads the small levels first,
at the bases above, and the whole chain inside 0x60000 bytes where the driver's
layout needs 0x80000. That ordering is AddrLib's mip chain rule for a 64 KiB
swizzle mode (`Gfx9Lib::ComputeMipChainInfo`, `GetMipTailDim`,
`MipTailOffset256B`, whose source is in `.deps/native/mesa`), and deriving the
general rule for any chain is the driver work C7 leaves behind.

`fill_tiled_mip_chain` therefore writes each level at the base the map named
(`kMipLevelBases`) and fills its texels through `tiled_level_offset`, the
driver's map. The frames then read the five greys, one band each and one pinned
LOD each -- 10, 100, 200, 40 and 60 -- which is the sampling half of C7 proven
on the console. `golden/c7-mip-tiled` is that run: six submissions (the band
frame and the five pinned ones) and the program's two stages.

**What this changes for the driver.** Its tiled image sizing still lays a chain
out its own way, so an application's tiled mip chain would sample the right
level of the wrong bytes; the driver records no copy into tiled storage (so
nothing can fill one through the API) and C7's remaining driver work is the
AddrLib layout plus the tiled upload. The probe fills its chain itself through
`ps5vk_debug_image_storage`, which is what a probe is for.

**The PC comparison.** `driver/tests/vk_c7_tiled_mip_test.c` draws the same six
frames on the PC -- the five bands, then one frame per level with the sampler's
LOD range pinned to it -- through the library's tiled chain
(`ps5vk_triangle_input.texture_tiled`), filling the storage at the bases the
address map measured, and `tools/check-driver.sh c7_tiled_mip` reports the
loader arm PASS, the direct arm PASS and the PS5 link accepting 169 imports,
with every submission *identical to the console's* (`16 packets, 6 register
tables` each, against `golden/c7-mip-tiled`, the six submissions of run pid
134). The test takes its device functions from the device
(`vkGetDeviceProcAddr`), because a NULL instance answers only for the
instance-level ones.

## 2026-09-17: V0-robust -- 1.0's one required feature is on

**Result.** Console run pid 140 (`Klog_Logs/v0-robust-run6.log`, queue
`jobs/v0-robust/queue.txt`) passed `v0-robust` and `m2-solid`, and pid 141
re-ran it with the feature reported. The device now reports
`robustBufferAccess` true, which is the only feature Vulkan 1.0 requires
(`docs/M5_REFERENCE.md`, "1.0: make the current claim true") -- and the claim is
true for it.

**The probe.** Two frames of C2's indexed square, through the same program and
the same index buffer, differing in one thing: the count the draw asks for.
Frame 0 asks for the six indices the buffer holds; frame 1 asks for **twelve**,
twice the bound. Both frames read back the square exactly -- 2,073,600 of
2,073,600 pixels inside it and none outside -- and the console's own stream
shows why the second is safe:

```
frame 0 (six indices):    INDEX_COUNT 6, DRAW_INDEX_2 count 6
frame 1 (twelve indices): INDEX_COUNT 6, DRAW_INDEX_2 count 6
```

The count the application asked for never reaches the hardware: the driver
clamps an indexed draw's count to the indices the *binding* covers
(`ps5vk_CmdBindIndexBuffer3KHR` records the range the runtime hands it,
`ps5vk_cmd_draw` clamps to it), so nothing outside the bound is fetched. That is
what robustBufferAccess requires -- no fault, and no bytes from outside the
buffer -- and `golden/v0-robust` holds the two submissions.

**What the compiler cannot do.** `docs/M5_REFERENCE.md` expected "shader
compiler option or NIR bounds lowering" for this step, and the AGC compiler has
neither: its CLI takes `--address32-hi`, `--descriptor-binding`,
`--vertex-attribute`, `--ngc`-family flags and nothing about robustness, and it
refuses any uniform block larger than 16 bytes ("Shader compilation failed:
internal error"), so a shader-side out-of-bounds *uniform* read is not
expressible through it. The fixed-function paths are where robustness is
reachable on this console, and the index count is the one the step's own note
named ("a runner check with out-of-range indices").

**PC comparison.** `driver/tests/vk_v0_robust_test.c` draws the same two frames
on the PC and `tools/check-driver.sh v0_robust` reports the loader arm PASS, the
direct arm PASS and the PS5 link accepting 169 imports, with both submissions
identical to the console's (11 and 10 packets, 3 register tables each).
`driver/tests/vk_b2_device_test.c` now asserts both halves of the report:
`robustBufferAccess` is on, and every other 1.0 feature is off.

What 1.0 still owes is the Required Format Support tables: the 55 missing
features `docs/V0_FORMATS_AUDIT.md` lists, family by family.

## 2026-09-17: D1's dynamic uniform buffer

**Result.** Console run pid 144 (`Klog_Logs/d1-dynamic-ubo-run3.log`, queue
`jobs/d1-dynamic-ubo/queue.txt`) passed `d1-dynamic-ubo` and `m2-solid`. The
driver used to refuse dynamic offsets by name ("dynamic descriptor offsets need
the dynamic uniform buffers of D1"), while still reporting
`maxDescriptorSetUniformBuffersDynamic = 8` -- a 1.0 requirement -- so this closes
a claim the limits table was already making.

**The probe.** One 32-byte buffer holding two 16-byte colours, one descriptor
whose range is the shader's 16 bytes, and the layout's binding declared
`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`. The two frames bind the same set
with dynamic offsets 0 and 16 through
`VkBindDescriptorSetsInfo::pDynamicOffsets`, and each reads back the half its
offset chose (both frames exact, `check_solid_frame`). The console's own
descriptors say it is the offset that changed and nothing else:

```
frame 0 (offset 0):  address 0x20002c000, stride 16, record count 1
frame 1 (offset 16): address 0x20002c010, stride 16, record count 1
```

**What changed in the driver.** `ps5vk_CmdBindDescriptorSets2KHR` records one
dynamic offset per set (`cmd_buffer->descriptor_set_offsets`), the descriptor set
layout accepts the dynamic type with the same 16-byte table entry, the
descriptor write records it the same way, and `ps5vk_cmd_draw` adds the set's
offset to the descriptor's address while leaving the bound range -- and so the
record count -- alone. The compiler option mapping needed the same care as the
layout: a dynamic uniform buffer is `PSBC_DESCRIPTOR_UNIFORM_BUFFER` to the
shader compiler, and passing it as a combined image sampler is what the first
run's "fragment stage: internal error" was.

**PC comparison.** `driver/tests/vk_d1_dynamic_ubo_test.c` draws the same two
frames on the PC and `tools/check-driver.sh d1_dynamic_ubo` reports the loader
arm PASS, the direct arm PASS and the PS5 link accepting 169 imports, with both
submissions identical to the console's (8 packets, 3 register tables each);
`golden/d1-dynamic-ubo` holds them.

## 2026-09-17: V0-formats' sampled families (seventeen required formats)

**Result.** Console run pid 152 (`Klog_Logs/v0-formats-sampled-run8.log`, queue
`jobs/v0-formats-sampled/queue.txt`) passed `v0-formats-sampled` and
`m2-solid`: **17 of 17** required formats fetched the colour their texel decodes
to, every one of 38,400 samples in the box for every format, with the linear
filter. The run before the fix (pid 148, `...-run4.log`) is the other half of
the record: 9 of 17 passed, and the eight failures were the one- and two-channel
16- and 32-bit families.

**What the probe draws.** One frame per format: the m3-texture canary's indexed
square over a 256x4 image of that format whose every texel holds one value
chosen so the decode is exact (`0x80` for `R8_UNORM`, `0x8000` for
`R16_UNORM`, half floats `0x3800`, full floats `0x3f000000`, and the sRGB and
SNORM forms offset so the linearisation lands on the same colour), sampled
through a combined image sampler whose descriptor is the driver's own words.
The check reads the square back through `FramebufferView`/`kTiledRgba8Layout`.

**The defect the first run found.** With the identity DST_SEL word (`0xfac`)
the hardware does not fill a format's missing channels with zero and one: it
**aliases** them. `R8_UNORM`'s single component came back in all four channels
(alpha included) and `R8G8_UNORM`'s two came back cyclically, R, G, R, G, so a
0.5, 0.25 texel landed as `(128, 64, 128, 64)` where Vulkan requires
`(128, 64, 0, 255)`. The fix is the rule RADV uses: the descriptor's DST_SEL
selectors carry the fill, `0x204` for a one-channel format, `0x22c` for two,
`0x3ac` for three and `0xfac` for four, chosen per format-table entry
(`driver/ps5vk_private.h`, `PS5VK_FORMAT_SWIZZLE_R001`). The measurement and the
two-channel table are in `docs/HARDWARE_FINDINGS.md`.

**What changed in the driver.** The format table gained the sixteen formats and
their features, the FORMAT word per entry (the register database's
`GFX10_FORMAT_*`) and the DST_SEL word per entry; `ps5vk_sampled_image` carries
both into the descriptor, and the WORD field it replaced is gone. Sampling a
format whose words no probe has recorded is still refused, by name.

**Capture.** The run that fixed the capture (pid 150,
`Klog_Logs/v0-formats-sampled-run6.log`) and the acceptance run (pid 152) are
the same stream: `golden/v0-formats-sampled` holds seventeen submissions
(one per format, 11 packets and 3 register tables each), the pipeline stage of
each format's own device (seventeen, because the probe draws through one device
per format), and `m2-solid`. Two tool fixes came out of capturing it. A capture
that logs its stages in more than one call now sums their counts
(`tools/ps5vk_log.py`, `capture_problems`), and the host replay names up to 24
regions instead of 8, which is what seventeen stages plus a frame's allocations
need (`host/ps5/ps5_host.cpp`, `kMaxRegions`).

**A host-stub defect, found by the PC comparison.** The first compare run had
every submission after the first differing in one word: the geometry buffer's
address, `0x200030000` on the console and a first-fit `0x200004000` on the PC.
The replay hands a captured region to the first allocation of its size, but the
free allocator never gave the region back when the application released the
memory, so the second device's allocations of that size found no region left.
`sceKernelReleaseDirectMemory`'s free path now clears the region it handed out
(`host/ps5/ps5_host.cpp`, `free_release`), which is what the console's own
allocator does, and a program that allocates the same sizes again -- seventeen
devices, one per format -- lands on the captured addresses.

**PC comparison.** `driver/tests/vk_v0_formats_test.c` draws the same seventeen
frames on the PC, in the same order, with the same texels, and
`tools/check-driver.sh v0_formats` reports the loader arm PASS, the direct arm
PASS with all seventeen submissions **identical to the console's** (11 packets,
3 register tables each) and the PS5 link accepting 169 imports.

**The audit.** `docs/V0_FORMATS_AUDIT.md` moves from 7 reported formats to 21,
and every one of the seventeen is still counted as missing a required feature --
now for `BLIT_SRC`/`BLIT_DST`, which needs C7's blit path, rather than for the
sampled fetch, which is proved.

## 2026-09-17: C7's image copies and the one-to-one blit

**Result.** Console run pid 155 (`Klog_Logs/c7-copy-run3.log`, queue
`jobs/c7-copy/queue.txt`) passed `c7-copy`, `v0-formats` and `m2-solid`:
**2 of 2** frames read back the texels their copy or one-to-one blit moved, and
the driver reports exactly the format features the audit records (30 of 30
formats queried). `golden/c7-copy` holds the run.

**What the frames draw.** The m3-texture canary's square, its 64x36 texels
uploaded into the row-layout image C4 samples, and then -- in the frame's own
upload command buffer, before the render pass -- `vkCmdCopyImage` in the first
frame and the one-to-one `vkCmdBlitImage` in the second move them into a
second, tiled image the frame samples. Each frame is checked twice: the
destination's mapped bytes against the measured tile map, and the square's
readback against the canary's pattern. Both frames passed both, exactly:
**2304 of 2304 texels** at the addresses the map names, and 2,073,600 of
2,073,600 readback pixels with a maximum error of **0** (nearest sampling).

**What changed in the driver.** `ps5vk_CmdCopyImage2KHR` and
`ps5vk_CmdBlitImage2KHR` (the KHR spellings; the two names of each share one
dispatch slot, which the runtime asserts) record CPU copies at a submission
split point, like the buffer-to-image copy C4 already used. A row-layout side
is linear with its level's padded pitch; a tiled side is the measured RGBA8 map,
16 bytes -- four texels -- at a time, because that is the longest run the XOR
map keeps contiguous (docs/HARDWARE_FINDINGS.md, measured this round). Images
whose placement no probe has measured are refused by name: a tiled chain (the
levels sit at AddrLib's bases, which C7's remaining piece implements), a tiled
image whose texels are not four bytes, a depth or stencil aspect, a layer other
than the first, and a *scaled* or mirrored blit region -- which becomes C7's
blit draw. `VK_FORMAT_R8G8B8A8_UNORM` now reports `BLIT_SRC` and `BLIT_DST`,
the two bits its frames proved, and nothing else does.

**The stale probe this found.** `v0-formats` -- the query probe that checks the
reported features against `docs/V0_FORMATS_AUDIT.md` -- still expected the
pre-V0-formats table (it wanted nothing reported for `R8_UNORM` and
`R16G16B16A16_SFLOAT`, which the previous step had proved), so it would have
failed had any queue run it. Its table now carries all seventeen sampled
formats with their four bits, the two blit bits on `R8G8B8A8_UNORM`, and the
recorded gaps as gaps; the console run reports 30 of 30.

**PC comparison.** `driver/tests/vk_c7_copy_test.c` draws the same two frames
-- two programs, two devices -- and checks the copied bytes against the tile map
on the PC too; `tools/check-driver.sh c7_copy` reports the loader arm PASS, the
direct arm PASS with both submissions **identical to the console's** (11
packets, 3 register tables each) and the PS5 link accepting 169 imports.

## 2026-09-17: C7's scaled blit

**Result.** Console run pid 157 (`Klog_Logs/c7-copy-run5.log`, queue
`jobs/c7-copy/queue.txt`) passed `c7-copy`, `v0-formats` and `m2-solid`:
**4 of 4** frames read back the texels their copy or blit moved, and the two
scaled ones were exact -- 2,073,600 of 2,073,600 square pixels, maximum error
**0**, for the nearest blit and for the filtered one. `golden/c7-copy` holds the
run (four submissions, four stages).

**What the new frames draw.** The same tiled destination the copy fills, but
with `vkCmdBlitImage` scaling: the canary texture's middle half (a 32x18
rectangle) over the whole 64x36 destination, so each source texel covers a 2x2
block. One frame asks for `VK_FILTER_NEAREST` and one for `VK_FILTER_LINEAR`,
and the frame then samples the destination with the nearest sampler, so the
readback is the destination's texels.

**What changed in the driver.** `ps5vk_CmdBlitImage2KHR` now tells the two
shapes apart: rectangles that match are the copy C7 already proved (any filter,
since no sample lands between texels), and rectangles that differ -- scaled or
mirrored -- are recorded as one *resample* the queue runs on the CPU at the
submission split point (`struct ps5vk_memory_copy`, `ps5vk_blit_execute`). A
destination texel's sample point maps onto the source rectangle the way Vulkan's
blit defines it -- destination texel centres onto the rectangle's edges -- and a
sample outside the rectangle clamps to its edge, which is the clamp-to-edge
sampling the driver's descriptors use. The destination rectangle clips to the
image with the transform unchanged, so a rectangle that reaches outside keeps
its scale.

Refused by name, each because a probe has not proved it: a blit whose regions
mix the two shapes in one call, a scaled blit whose texels are not four bytes,
and a *filtered* blit whose images are not both `R8G8B8A8_UNORM` (the blend is
on the texel bytes, which are the colour channels there and ordinary UNORM
values only there; an sRGB or SNORM blend would need the decode Vulkan's filter
names).

**How it is checked.** The probe computes the expected colour of every square
pixel on the CPU, independently of the driver's arithmetic: the destination
texel that pixel samples, and the source texels the 2x upscale puts there --
the exact source texel for a nearest blit, and the ideal bilinear blend, with
the rectangle's clamp, for a filtered one, compared within the bilinear
tolerance (which the exact-zero result does not even need). The same reference
is in `driver/tests/vk_c7_copy_test.c`, which checks the destination's *bytes*
on the PC: 13 of 13 checks pass, including "the filtered blit's destination is
the ideal 2x blend of the middle half", and
`tools/check-driver.sh c7_copy` reports the loader arm PASS, the direct arm PASS
with all four submissions **identical to the console's** (11 packets, 3 register
tables each) and the PS5 link accepting 169 imports.

## 2026-09-17: C7's per-format blits (BLIT_SRC for every sampled format)

**Result.** Console run pid 163 (`Klog_Logs/c7-blit-formats-run6.log`, queue
`jobs/c7-copy/queue.txt`) passed all five queued tests: `c7-copy`,
`c7-blit-formats` (**16 of 16** reported sampled formats blitted into a tiled
destination and read back), `v0-formats` (30 of 30 formats as audited) and
`v0-formats-sampled` (**16 of 16** formats fetched the colour their texel holds,
now with distinct channels in every texel). `golden/c7-blit-formats` holds the
new run, and `golden/v0-formats-sampled` and `golden/c7-copy` are re-captures of
it.

**What changed in the driver.** The resampler decodes the source texel into the
destination's four bytes: the sampler's fetch for that format -- Vulkan's
fill-in rule for the channels it does not have, which V0-formats measured --
converted to eight-bit UNORM, with an sRGB value linearised, a SNORM one taken
as its signed byte over 127 (the console's own fetch of 0x20 lands at 0x40,
which is what the destination's clamp does with it) and a float one clamped
(`ps5vk_texel_to_rgba8`). A scaled blit may therefore have a source of any
reported format with a four-byte destination, and a filtered one still needs
`R8G8B8A8_UNORM` on both sides. Every format that probe proves now reports
`BLIT_SRC`; `BLIT_DST` stays `R8G8B8A8_UNORM`'s alone, since that is the one
destination any probe has written.

**Two defects the step found and fixed.** The tiled-side check asked for a
four-byte *source* where the tiled image was the destination, so a small
source format was refused outright. And `A8B8G8R8_*`'s reversed memory order was
not being handled at all: the descriptor's DST_SEL had to become `W, Z, Y, X`
(`0x977`) for those formats, which the console's own fetch then confirmed for
the UNORM and SNORM forms (docs/HARDWARE_FINDINGS.md).

**One format dropped, with the hardware reason.** `A8B8G8R8_SRGB_PACK32` is no
longer claimed: the sRGB curve is applied to the *first three fetched*
components, so the byte-reversed sRGB format linearises the wrong three bytes
and no DST_SEL can move the curve with the channel (the run's own
`v0-formats-sampled` frame read 0x89, 0x80, 0xc0 where 0x40, 0x80, 0xc0 was
required). It needs a shader-side swizzle; the audit records it as a gap, and
the query probe expects nothing for it.

**PC comparison.** `driver/tests/vk_c7_blit_formats_test.c` draws the same
sixteen frames over sixteen devices and checks each destination texel against
the colour its format's fetch decodes to -- the expectation V0-formats measured
on the hardware, not the driver's own arithmetic.
`tools/check-driver.sh c7_blit_formats` reports the loader arm PASS, the direct
arm PASS with all sixteen submissions **identical to the console's** (11
packets, 3 register tables each) and the PS5 link accepting 169 imports; the
format query probe, `c7_copy` and the sampled-format test all pass beside it.

## 2026-09-17: C8's four-sample target

**Result.** Console run pid 169 (`Klog_Logs/c8-msaa-run6.log`, queue
`jobs/c8-msaa/queue.txt`) passed `c8-msaa` and `m2-solid`: the M2 solid frame
renders into a **four-sample** R8G8B8A8_UNORM target, and the probe's
measurement of that target's storage is **8,294,400 of 8,388,608 words holding
the frame's colour, the first of them at word 0** -- four words a texel for the
99% of the 32 MiB window the frame covers, the rest being the padding of the
aligned tail. `golden/c8-msaa` holds the run.

**What changed in the driver.** Two refusals and one register. The graphics
pipeline takes `VK_SAMPLE_COUNT_4_BIT` as well as one sample (nothing in the
compiled shader depends on it), `vkCmdBeginRendering` accepts a four-sample
colour attachment of the one target size, and the colour-target registers carry
the count: `CB_COLOR0_ATTRIB`'s `NUM_SAMPLES` (bits 12-14) and `NUM_FRAGMENTS`
(15-16) take the 4x encoding `2 | 2 << 3` for a four-sample target and the AGC
default for a one-sample one (`ps5vk_target_registers`, ps5vk_draw.c). The four
sample words a texel are what a four-sample tile holds: 64x64-texel tiles of
0x10000 bytes are sixteen bytes a texel, which is what `ps5vk_tile_extent`
already sized for four samples.

**What the probe measures.** It draws the fullscreen M2 frame, counts the words
of the target's storage that hold the frame's colour (`0xffffa020`, red in the
low byte as the driver's RGBA8 packing stores it), logs the count, the target's
texel count, the first matching word and a page of word addresses, and requires
the count to sit between one word per texel and four -- a band any sample
placement satisfies. It passed at the top of the band, which is the measurement
the resolve needs: the samples are in the storage, four to a texel, and the next
step is a frame whose samples differ (a `gl_SampleID` shader) to place them
inside the tile, then the resolve itself as a CPU pass over that map.

**PC comparison.** `driver/tests/vk_c8_msaa_test.c` draws the same four-sample
frame and checks what a PC can: that the four-sample pipeline is accepted, that
the frame records, submits and signals its fence, and that the target the driver
allocated is the four-sample one it sized.
`tools/check-driver.sh c8_msaa` reports the loader arm PASS, the direct arm PASS
with the submission **identical to the console's** (8 packets, 3 register
tables) and the PS5 link accepting 169 imports.

## 2026-09-17: C8's sample positions cannot come from the shader

`tools/build-probe-shaders.sh c8-sampleid` builds a fragment shader that writes
`vec4(gl_SamplePosition, float(gl_SampleID) / 8.0, 1.0)`, which would name each
of a four-sample target's four sample words in one frame. It fails, by design
and with the reason kept in the script: the compiler's SPIR-V front end rejects
`SpvCapabilitySampleRateShading` ("Unsupported SPIR-V capability:
SpvCapabilitySampleRateShading (35)"), so no shader of this toolchain can read a
sample's position or index. The set stays for the record the way `v0-robust`'s
does, and exits 2.

The layout therefore has to be measured by **coverage**: a geometry edge covers
some samples of the pixels it crosses and not others, so a frame drawn with a
clear load over a known edge leaves the covered sample words in one colour and
the rest in another, and the boundary between them in the target's storage is
the sample positions. That is the next measurement, and the resolve does not
need it: averaging a texel's four words is commutative, so
`vkCmdResolveImage`'s arithmetic is the same whatever order the samples are in.

## 2026-09-18: level 2's base is 0x8400, and AddrLib is the layout's oracle

Offline round: the console has been off the network since the 2026-09-17 runs
(no ping answer, 2121/9111/3232 closed, no address in a full scan of
<lan>/24), so nothing was deployed and no klog was captured. What the
round did have is run 33's own address map, and it turned out to be enough to
settle C7's chain layout.

**The correction.** `Klog_Logs/c7-mip-run33.log`'s five `pinned_address` records
are 0x2f0fc, 0x13cfc, 0xb4fc, 0x58fc and 0x8fc. Each is a level's base plus the
map of the texel that frame sampled, and the map's own fitted texel for each
address is the level's centre for all five -- so the bases are 0x20000, 0x10000,
**0x8400**, 0x4800 and 0x800. The five-level table in `src/diagnostics.cpp`
(`kMipLevelBases`) and in `driver/tests/vk_c7_tiled_mip_test.c` (`kLevelBases`)
held 0x8000 for level 2, which is the lowest address that level's texels reach
rather than the level's start, and no texel of the level could have produced
run 33's 0xb4fc from it. `docs/HARDWARE_FINDINGS.md` has the arithmetic.

**AddrLib as the oracle.** AddrLib's rule was transcribed twice before and
disagreed with the console once; this round compiled it instead.
`tools/mip-layout-oracle.cpp` links the pinned AddrLib
(`.deps/native/mesa`) for the host and asks `Addr2ComputeSurfaceInfo` -- the call
radv makes -- for the chain, and `tools/check-mip-layout.sh` builds it and holds
its five-level 256x256 answer against the console's measurement: bases
0x20000, 0x10000, 0x8400, 0x4800 and 0x800 in 0x60000 bytes, `firstMipIdInTail`
2. It passes, and the shape matrix it prints (six-level 256x256, 128x128x4,
512x512x7, 1024x1024x8, 256x128x4, 64x64x3, 32x32x2) is the reference the
probe's `c7-mip-pages-six` frames and the driver's tiled upload will be read
from. A tail level is at the swizzle of its tail coordinate, not at its
`mipTailOffset`.

**What changed in the tree.** The two tables hold 0x8400 for level 2.
`driver/tests/vk_c7_tiled_mip_test.c` now pins the table to run 33's raw
addresses -- each base plus the map of the centre texel has to be the address
the frame fetched -- and checks that every texel of every level has its own
address inside the chain's measured 0x60000 bytes. Both checks were run against
the old table: the first fails with 0x8000 (5 of 6 checks), the second passes
(0x8400 and 0x8000 both keep the levels apart), so the raw-address check is what
holds the table to the console.

**Gates.** `tools/check-driver.sh c7_tiled_mip` 6 of 6 in the loader and direct
arms, its six submissions identical to `golden/c7-mip-tiled`, PS5 link 169
imports; `tools/check-mip-layout.sh` PASS; the full `make lint`, `make test`,
`make ci` and `tools/check-driver.sh` runs are recorded with the round's commit.

**Still open, console-bound.** C8's resolve (the parked `c8-resolve-wip.patch`
and its queue), C7's tiled upload (the rule above, once a run can prove it), and
the driver's own tiled sizing, which still lays each level's tiles after the
last -- larger than the hardware's 0x60000 for this chain, so nothing overflows,
but not the hardware's layout.

## 2026-09-18: a view whose component mapping is not the identity is refused

Offline round again (the console has not answered since the 2026-09-17 runs).
This one came out of an audit of a class of bug rather than of a measurement:
state the driver accepts and then ignores, which returns a wrong result with no
error. The class is short. Array layers are already refused in all three paths
that would use them -- a sampled view whose image has more than one layer or
whose view starts past layer 0 (`driver/ps5vk_draw.c`), a colour or depth
attachment of an array image, and a copy or blit with `layerCount != 1` -- and
the pipeline path refuses everything `ps5vk_draw_refusal` has not seen run
(culling, polygon modes, blending, logic ops, write masks, stencil and
depth-bounds, dynamic state outside the viewport, scissor and depth trio, sets
past 0). `vkCreateSampler` refuses every field the canary did not run.

**The hole was the view's component mapping.** `VkImageViewCreateInfo::components`
is applied to what a shader reads through a combined image sampler, and the
driver's descriptor carries only the format's own DST_SEL selectors
(`ps5vk_image.c`), so a view with, say, `{B, G, R, A}` sampled the format's own
channels: the application asked for a swap and got none, with no error. The
runtime resolves `VK_COMPONENT_SWIZZLE_IDENTITY` to R, G, B and A whatever the
format is (`vk_image_view_init`), which is what every frame the driver has run
passes, so the check is a plain four-way comparison. Vulkan requires the
identity for framebuffer attachments, storage images and input attachments
("This remapping **must** be the identity swizzle for any `VkImageView` used
with ... input attachment descriptors, framebuffer attachments, and storage
image descriptors", VkImageViewCreateInfo), and the driver has no storage or
input attachments, so the sampled path is the only place that has to check it --
which is what `ps5vk_sampled_image` now does, refusing by name and pointing at
the probe that would settle the composition.

**The check.** `driver/tests/ps5vk_triangle.c` takes the mapping in its input
struct and passes it to the view's `VkImageViewCreateInfo.components` (zeroed is
the identity, so every existing frame records exactly what it recorded); the C4
texture test adds a frame whose view maps `{B, G, R, A}`, created after its two
proven frames so the addresses the golden holds are untouched, and asserts that
the create succeeds (a mapped view is legal), that the draw fails, and that the
driver's message is the one that names the component mapping. The test's step
recorder keeps the first driver message of a refusal, because the result's name
follows the reason.

**Verified on this host.** `tools/check-driver.sh c4_texture`: 12 of 12 checks
in the loader and the direct arms, both frames still identical to
`golden/c4-texture`, PS5 link 169 imports. `tools/build-driver.sh` 18 host and
18 PS5 sources with no warning; the full `tools/check-driver.sh` (every test,
three arms, both negatives), `make lint`, `make ci`, `make test`,
`make inspect`, `tools/check-psbc-link.sh`, `tools/check-vulkan-runtime.sh`,
`tools/build-all-titles.sh` (five titles, no warning) and the golden rebuilds
are recorded with the round's commit. The console is off the network for the
seventeenth round running, so nothing was deployed.

## 2026-09-18: C2's two transfer commands, implemented and read back

The two commands the probes never needed and the driver never had:
`vkCmdFillBuffer` and `vkCmdUpdateBuffer`. They were refused in the two rounds
that swept the core command surface, because their entry points crash -- and the
refusal was the right place for them only while nothing implemented them. Both
are **CPU work at a submission split point**, which is what this driver's copies
already are (`driver/ps5vk_queue.c`, Phase C2): the console's memory is shared,
the queue runs the words recorded before the transfer, waits for that step, does
the work, and only then runs the words after it, so Vulkan's command order holds.
A fill and a memcpy are that same work, so no probe was needed and neither
command changes a stream: they add a record to the command buffer's copy list
(`struct ps5vk_memory_copy` gained a `fill` flag with its word, and an
`owned_source` for the update's snapshot).

**What the test proves, on the PC.** `driver/tests/vk_c2_transfers_test.c` (14 of
14 checks in the loader and direct arms) reads every write back from the mapped
buffer, which is the memory the GPU would read:

- a fill's own range holds its word and nothing outside it changed;
- a fill followed by an update of two of its bytes leaves the update's bytes --
  which only holds if the queue really splits the submission between them;
- `VK_WHOLE_SIZE` fills from the offset to the end of the buffer and leaves the
  bytes before it alone;
- the update's data is **snapshotted**: the test overwrites its source array
  after recording and before submitting, and the buffer still holds the original
  bytes, which is what Vulkan allows the application to rely on.

`driver/ps5vk_refusals.c` no longer refuses either command, so the refused set is
thirteen (`driver/tests/vk_b2_commands_test.c` now checks those and calls these
two only to see them not crash). What the console still owes them is what it owes
every transfer: a run that writes and reads the bytes back on the console, which
the next battery can do with a probe frame. The console is off the network for the
eighteenth round running, so nothing was deployed.

## 2026-09-18: the image readback, implemented and compared byte for byte

`vkCmdCopyImageToBuffer` was the last transfer still refused. It is the same CPU
work at a submission split point -- a run-by-run memory copy -- that every
transfer here is, over the image's own texel map, so it needed no probe either:
the image side is the side a copy already builds (`ps5vk_image_copy_side`, whose
tiled case wants four-byte texels and one level), and a buffer is a linear side
with a row pitch of its own, `bufferRowLength` texels when the application names
one and the region's width otherwise (`bufferImageHeight` only pads between
layers, and a region names one layer). The entry point is
`ps5vk_CmdCopyImageToBuffer2KHR`: the runtime forwards the core name to the 2
form and the two share a dispatch slot, as they do for `vkCmdCopyImage`.

**What the test proves, on the PC.** `driver/tests/vk_c7_readback_test.c` reads
the bytes back from the mapped buffer, 11 of 11 checks in the loader and direct
arms:

- a round trip: the pattern uploaded through `vkCmdCopyBufferToImage` and read
  back into a second buffer, **byte for byte**;
- a padded readback: `bufferRowLength` 80 texels for a 64-texel region and a
  `bufferOffset` of 64, so each row's 256 bytes land where the pitch names them
  and the padding *and* the bytes before the offset stay untouched;
- a region: the 16x8 block at (8, 4), which the readback is compared against;
- a **tiled** source: the colour attachment's storage, filled by the test through
  the tile map (`tiled_offset`, the map `ps5vk_tiled_texel_offset` and
  `src/diagnostics.cpp`'s `tiled_rgba8_offset` share) and read back through the
  copy's own decoding of it, byte for byte -- which is what says the run-by-run
  walk of a tiled row is right.

`driver/ps5vk_refusals.c` no longer refuses it, so the refused set is twelve
(`driver/tests/vk_b2_commands_test.c` now checks those, 13 of 13). What the
console still owes the readback is what it owes every transfer: a run that writes
and reads the bytes back there, which the next battery's probe frames can do. The
console is off the network for the eighteenth round running, so nothing was
deployed.

## 2026-09-18: the colour clear, implemented and read back

`vkCmdClearColorImage` was the last CPU-writable refusal: an image clear is the
same work at a submission split point the transfers are, writing one encoded
texel over every texel of the ranges' regions through the image's own map (the
padded rows of a linear image, the measured tile map of a colour attachment).
Two pieces make it: an encoder for the clear value and a record the queue runs.

**The encoder** (`driver/ps5vk_image.c`, `ps5vk_format_encode_clear`) is the
inverse of the decode a blit does (`ps5vk_texel_to_rgba8`): UNORM8 in R8G8B8A8
and A8B8G8R8 byte order, sRGB (the clear value is linear and the texel is the
encode), SNORM8, UNORM16, SFLOAT16 and SFLOAT32, one to four channels. A format
the encoder does not carry is refused by name rather than written wrongly.

**The record** is one `struct ps5vk_memory_copy` with a `clear` flag, the
destination side, the region and the sixteen-byte texel; the queue walks the
region through `ps5vk_image_copy_address` and flushes the range it touched once.
One ordering detail is worth recording: a clear has no bytes of its own, so the
executor dispatches it *before* the empty-copy check -- with the check first,
every clear was silently skipped, which is exactly what the first run of the
test showed (the readback came back zero).

**What the test proves, on the PC** (`driver/tests/vk_c7_clear_image_test.c`, 25
of 25 checks in the loader and direct arms). Each case clears an image and reads
it back with `vkCmdCopyImageToBuffer`, comparing the bytes:

- R8G8B8A8_UNORM (0.25, 0.5, 0.75, 1) lands as 40 80 bf ff;
- R8G8B8A8_SRGB at a linear 0.5 lands as bc -- the sRGB encode, not the value;
- R8_UNORM, R8G8_UNORM, R16_UNORM and R32_SFLOAT land as their own encodings;
- a **tiled** colour attachment clears through the tile map, which the readback
  then decodes;
- level 1 of a two-level image, so both the clear and the readback name the
  level's own layout;
- a range that names two array layers is refused by name (D1).

`vkCmdClearDepthStencilImage` stays refused, and for a reason worth recording: the
format table reports no `TRANSFER_DST` for D32_SFLOAT, which that command's valid
usage needs, so no valid call exists until a probe settles a depth transfer. The
proven depth clear is the attachment clear (Phase C5). The refused set is eleven
(`driver/tests/vk_b2_commands_test.c` now checks those, 12 of 12). The console
still owes the clear what it owes every transfer: a run that writes and reads the
bytes back there. The console is off the network for the eighteenth round running,
so nothing was deployed.

## 2026-09-18: the indirect draws, read on the CPU and compared with the console's frame

`vkCmdDrawIndirect` and `vkCmdDrawIndexedIndirect` were refused because their
parameters would come from the PM4 `INDIRECT_BUFFER` packet that B8 measured as
faulting the GPU. They do not have to: a buffer is host memory here, so the
driver **reads the parameters when the draw is recorded** and records the packets
a direct draw of the same parameters records -- no `INDIRECT_BUFFER` packet
anywhere. Both the indexed and the non-indexed structure are Vulkan's own
(`VkDrawIndirectCommand`, `VkDrawIndexedIndirectCommand`), the read is a memcpy
plus a cache flush, and `drawCount` records that many draws in order.

**One case is refused by name, and it is the one that would be wrong.** Vulkan
reads an indirect draw's parameters at execution time, and this driver reads them
at record time, so a command buffer that *writes* the indirect buffer itself
would have its draw recorded from the values before that write. The driver knows
what its own command buffer records: `ps5vk_cmd_buffer_writes_range` walks the
copy records made so far, and a draw whose parameters they touch is refused with a
message that names the split-point read as the later step. A write in an earlier
submission has already run (submissions are synchronous here), and one recorded
after the draw does not affect it, so the check is exactly as wide as the hole.

**Verified against the console's own frame.** `driver/tests/vk_c2_indirect_test.c`
draws the M2 triangle through an indirect buffer -- the harness fills it with the
parameters it would otherwise pass -- and `tools/check-driver.sh` compares the
submission against the console's b4-headless capture: *identical, 6 packets, 3
register tables*, apart from the viewport orientation and the user data the driver
writes, which every draw comparison here already expects. The same run's second
case records a fill of the indirect buffer followed by an indirect draw and checks
the refusal. Five of five checks in the loader and direct arms; the PS5 link
accepts the test with 170 imports.

The refused set is ten (`driver/tests/vk_b2_commands_test.c` checks those, 10 of
10), and what is left in it is either console-bound (the dispatch pair, events,
pipeline caches, buffer views, timestamps, the depth clear) or needs its own step
(secondary command buffers). The console is off the network for the eighteenth
round running, so nothing was deployed.

## 2026-09-18: C8's four-sample map and the resolve, measured on the console

The console is back and this round finished C8's colour half: a four-sample
frame now renders four samples a texel and `vkCmdResolveImage` reproduces the
one-sample reference frame **word for word** on the console (pid 119,
`golden/c8-resolve`, `golden/c8-msaa`).

**Three faults, each named by a measurement.** The resolve had failed at every
attempt, and the reasons were not the ones the parked patch assumed.

1. *The sample bits were cleared after being set.*
   `ps5vk_target_registers` cleared `CB_COLOR0_ATTRIB`'s NUM_SAMPLES and
   NUM_FRAGMENTS twice -- once before the 4x encoding was written and once after
   -- so every four-sample target was programmed as a one-sample one. The
   console said so directly: a frame that covers the whole target left exactly
   **8,294,400 = one word a texel** in a 128 MiB four-sample image (pids 110-116),
   and the captured `CB_COLOR0_ATTRIB` record was `0x00000000`.
2. *The rasterizer needs its own MSAA state.* `CB_COLOR0_ATTRIB` alone is not
   enough: `PA_SC_AA_CONFIG` (MSAA_NUM_SAMPLES and MSAA_EXPOSED_SAMPLES =
   log2(4), MAX_SAMPLE_DIST 6), `PA_SC_MODE_CNTL_0.MSAA_ENABLE`, the four
   standard sample-location registers (`0x622ae6ae`), the centroid priorities,
   both coverage masks and `DB_EQAA`'s sample counts join a four-sample draw's
   context table (`ps5vk_multisample_registers`, driver/ps5vk_draw.c). They are
   written out rather than derived from AGC's defaults because the PC's replay
   only carries the colour target's defaults, and both sides have to record the
   same table (tools/golden.py, register_defaults).
3. *The four-sample map is four sample planes a tile.* With (1) and (2) fixed
   the storage held **33,177,600 = four words a texel** and a tile's four planes
   compared identical, word for word, which is what a per-pixel fragment shader
   promises. Inside a plane the texel is at the same measured swizzle the
   one-sample map uses, at 64-texel granularity, and the *block's* place turns
   it: bit 5 of the block-local y is XORed with bit 0 of the block's column and
   bit 5 of the local x with bit 0 of the row (thirteen blocks fitted, each on
   all 2048 words of its plane). `ps5vk_image_copy_address` now addresses one
   sample, `ps5vk_resolve_execute` averages the four planes' words and
   `ps5vk_clear_execute` writes all four, and the `ps5vk_image_copy_side` a
   multi-sample linear image gets is refused by name (its layout is unmeasured).
   docs/HARDWARE_FINDINGS.md has the tables.

**The measurement itself is part of the case.** `c8-resolve` draws the
m3-vertex canary's quad over the *whole* viewport with a green/blue gradient, so
a word's value names the pixel it came from and its address says where the map
put it: the case walks the four-sample image's storage once, counts clear,
untouched and drawn words, logs the addresses of one known pixel's words and a
spread of tiles' first 8 KiB (`agc_resolve_map`, 672 chunks). That is what
fitted the planes and the block turn -- and it is why the case's two frames are
now drawn over the viewport rather than as the quarter-area square: a
DONT_CARE load leaves the pixels outside a smaller quad holding another
allocation's contents, which differ between the resolved and the reference frame
whatever the resolve does (that, not the arithmetic, is what the 747 and 752
differing rows of pids 111-116 were).

**Evidence.** Console **pid 119**: `m2-solid`, `c8-msaa` and `c8-resolve` PASS,
263 PASS records, one benign VideoOut WARN; `agc_resolve` reports
`resolved_checksum` = `reference_checksum` = `0xb2b5b08ff1f18000`, zero differing
rows of 2160 and zero differing columns of 3840. `tools/golden.py extract`
wrote `golden/c8-msaa` and `golden/c8-resolve` from that run, each with a
runner-driven sibling (`m2-solid-1.json`) because a driver capture logs no AGC
helper calls and only a frame of the same run carries the process's register
defaults. `tools/check-driver.sh c8_msaa` and `c8_resolve` both PASS all three
arms, with the PC's submissions **identical to the console's** for every frame
(`c8_msaa`: 8 packets, 3 register tables; `c8_resolve`: four frames, 11 packets,
3 register tables each). `tools/check-driver.sh` (full) passes every test, and
`tools/build-all-titles.sh` builds five titles with no warning.

`driver/tests/vk_c8_resolve_test.c` now mirrors the console case's four frames
(one-sample control, four-sample control, resolved, one-sample reference) rather
than driving one, so the PC comparison covers the whole run. The parked patch is
gone: the work is committed.

**The committed tree's whole battery.** `jobs/c8-controls/queue.txt` carries
`c8-resolve` now too, and run **pid 120** (`Klog_Logs/c8-controls-run2.log`; the
earlier pid 120 of this log is a C2 run of another boot) passed all thirteen
cases (899 PASS records, the one benign VideoOut WARN) on these sources:
the C7 chains, the geometry controls, the four-sample frame and the resolve.

## 2026-09-18: D2, compute behind the Vulkan API

`vkCreateComputePipelines` and `vkCmdDispatch` are implemented and proven on the
console: run **pid 125** (`Klog_Logs/d2-compute-run5.log`) PASSed `d2-compute`
with the readback `0xa5a5a5a5`, the word the dispatched workgroup wrote, and
`m2-solid` PASSed after it. The PC's submission is **identical** to the
console's (`tools/check-driver.sh d2_compute`, three arms: 14 packets,
`golden/d2-compute`).

**What the driver does.** `ps5vk_compute.c` is one file: `vkCreateComputePipelines`
compiles the application's SPIR-V with libpsbc's compute stage, keeps the ISA in
a GPU-visible direct mapping and the words the compiler reported (RSRC1/2/3, the
VGPR count, the descriptor binding, the user-data dword), reads the shader's
local size out of its SPIR-V (`OpExecutionMode ... LocalSize`) and derives the
wave size from RSRC1's VGPR granule. `vkCmdDispatch` writes the bound storage
buffer into the table at the offset the compiler named, then records the packets
the V0-compute probe had already proved on this console: the COMPUTE_* SH
registers (start, thread counts, program address, resources, limits, user data,
destinations, accumulators), PM4 `DISPATCH_DIRECT` and a CS partial flush. The
queue submits them like any draw. `vkCmdBindPipeline` takes the compute bind
point, the descriptor-set layout gives a storage buffer a 16-byte entry, and the
descriptor write records its address and extent.

**The harness and the two comparisons.** `driver/tests/ps5vk_compute.c` is an
ordinary compute program (instance, device, one host-visible storage buffer, a
shader module, a descriptor set, a pipeline, one command buffer and a fence),
shared by the runner case `d2-compute` and the PC test `vk_d2_compute_test.c`:
the console proves the readback, the PC proves the stream. One ordering matters
and is now documented in the harness: the pipeline is created *before* the
buffer, because a run's replay lists the driver's stage regions before the
buffers' regions and hands a region to the first allocation of its size -- with
the buffer first, the PC's code mapping and buffer swapped addresses and the
`COMPUTE_PGM_LO` word differed by one page (the first form of this case failed
exactly there).

`jobs/d2-compute/queue.txt` is the battery (`capture`, `hold 60`, `d2-compute`,
`m2-solid`), and the command audit now reports **85 driver, 4 refused, 0 gap**:
`vkCreateComputePipelines` left the refusal list. `vkCmdDispatchIndirect` and
the compute-queue breadth Vulkan 1.0 asks for (the dispatch-base command family
is 1.1) come with the next D2 step, and `vkCreateBufferView` (texel buffers)
stays refused for now.

## 2026-09-18: D2's indirect dispatch

`vkCmdDispatchIndirect` is implemented and proven: run **pid 128** of this
boot (`Klog_Logs/d2-compute-run7.log`; the C7 record's pid 128 is another boot's) PASSed `d2-compute` with **both** dispatches --
`vkCmdDispatch` and `vkCmdDispatchIndirect` -- reading back the shader's word
`0xa5a5a5a5`, and `m2-solid` PASSed after them. The PC's two submissions are
identical to the console's (`tools/check-driver.sh d2_compute`, three arms,
`golden/d2-compute`: two submissions, 14 packets each).

The command shares the whole dispatch path: `ps5vk_dispatch` (driver/ps5vk_compute.c)
is what both entry points call, and the indirect one only adds the parameter read
-- the three `VkDispatchIndirectCommand` dwords from the bound buffer, read when
the dispatch is recorded, exactly as `vkCmdDrawIndirect` reads its parameters. A
command buffer that writes those parameters itself is refused by name
(`ps5vk_cmd_buffer_writes_range`, now shared with the draw path).

One lesson from the first form: the harness wrote only the *x* count into the
parameter block, so the console faithfully dispatched nothing and the readback was
the buffer's own contents. A dispatch reads all three (1, 1, 1 in the case), and
its submission now has the same 14 packets as the direct one -- which is also the
check `tools/check-driver.sh` compares (identical, both frames).

The command audit reads **86 driver, 47 runtime, 4 refused, 0 gap**;
`jobs/c8-controls` passed all thirteen cases on this build (899 PASS records,
`Klog_Logs/c8-controls-run4.log`).

## 2026-09-18: vkCmdCopyQueryPoolResults

The fourth refusal on the 1.0 list is closed: `vkCmdCopyQueryPoolResults` is
implemented and proven. Run **pid 132** of this boot
(`Klog_Logs/v0-query-copy-run3.log`) PASSed `v0-query-full` with the copy's
result `0x7e9000` -- 8,294,400 samples, exactly the number
`vkGetQueryPoolResults` answered for the same query -- and `available` 1, and
pid 135 repeated it on the final build (`Klog_Logs/v0-query-copy-run5.log`).
`tools/check-driver.sh v0_query_full` passes all three arms with the PC
submission identical to the console's (`golden/v0-query-driver`).

The implementation is the CPU-work-at-a-split-point pattern the other transfer
commands use: a query's counters are memory the GPU wrote, so the copy is
recorded as a `ps5vk_memory_copy` with the query kind and run by the queue at
its split point, where the GPU has finished the words before it
(`ps5vk_query_execute`, driver/ps5vk_query.c). It writes Vulkan's layout --
`query_stride` bytes a result, 32 or 64 bits as `VK_QUERY_RESULT_64_BIT` asks,
and the availability value `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` asks for --
and `vkGetQueryPoolResults` now shares the same arithmetic. The pool joins the
submission's targets so the queue evicts its counters' cache lines.

Two test bugs stood between the first attempt and the proof, and both are worth
the record: the *availability* word is what tells "the copy ran and wrote zero"
from "the copy did not run" (the console's first run showed 0 for both the
result and availability), and the runner read the harness's buffer *after*
`ps5vk_triangle_finish` had cleared the program, so the accessor saw a null
mapping. The harness's accessor is now read before the finish.

The command audit reads **87 driver, 47 runtime, 3 refused, 0 gap**;
`jobs/c8-controls` passed all thirteen cases on this build
(`Klog_Logs/c8-controls-run5.log`).

## 2026-09-18: vkCmdClearDepthStencilImage

The third refusal is closed: `vkCmdClearDepthStencilImage` is implemented and
proven. Run **pid 138** of this boot
(`Klog_Logs/c5-depth-clear-run3.log`) PASSed the whole C5 battery, and the new
case `c5-depth-clear` reports **8,294,400 of 8,294,400** texels holding exactly
the float bits of the value the command was given -- read through the depth
image's own tiled map (`tiled_depth_offset`, the map C5's readbacks proved) --
from an image whose only other writer is the pass that *loads* it. The PC
submission is identical to the console's (`tools/golden.py compare-run
golden/c5-depth-clear/run-1.json`, 17 packets, 6 register tables; the replay is
written from the same run).

The driver walks the depth map with `ps5vk_tiled_depth_offset`
(driver/ps5vk_image.c), the transcription of ps5-opengl's
`ps5_tiled_depth_offset` the runner's checker uses, and `ps5vk_image_copy_side`
marks a depth side so `ps5vk_image_copy_address` picks it. The clear itself is
the colour clear's shape -- CPU work at a split point, one encoded texel over
the region -- with the depth value stored as its own float bits
(`ps5vk_format_encode_clear`'s D32F case), and a stencil aspect is refused by
name because the driver's depth targets carry no stencil.

Two mistakes of mine are worth the record: the case's first check walked the
depth *storage* as if it were row-major (the counts then meant nothing), and its
second expected the near rectangle to be *rejected* by a LESS test against the
cleared 0.5 when LESS passes it. Both were the check's, not the driver's: with
the proven map the storage says the command wrote its value everywhere.

The command audit reads **88 driver, 47 runtime, 2 refused, 0 gap**.

**The B2 bookkeeping that follows.** `vkCmdClearDepthStencilImage` left the
refusal list `driver/tests/vk_b2_commands_test.c` holds, which now names
`vkCmdWriteTimestamp` alone, and the query-result copy stays there for a
different reason worth keeping: a call with no pool refuses rather than crashes,
which is what the B2 device test records every 1.0 command to see. Both
implemented clears also refuse a NULL image or buffer by name now.

## 2026-09-18: the console's GPU clock, recorded (V0-query's timestamp probe)

Vulkan 1.0 requires `vkCmdWriteTimestamp`, and the driver has refused it by name
with `timestampValidBits` 0 because no probe had asked the console whether it has
a readable clock at all (`docs/M5_REFERENCE.md`, V0-query). It does.

**The packet.** A RELEASE_MEM (PKT3 opcode 0x49) with the BOTTOM_OF_PIPE_TS
event (40) and an EOP selector that names the timestamp writes the GPU's
64-bit clock, which is how radv writes one (`radv_write_timestamp`:
`EVENT_TYPE(V_028A90_BOTTOM_OF_PIPE_TS)` with `EOP_DATA_SEL_TIMESTAMP`). The
word the probe adds is the selector: `EOP_DST_SEL(MEM) |
EOP_INT_SEL(SEND_DATA_AFTER_WR_CONFIRM) | EOP_DATA_SEL(TIMESTAMP)` =
**0x63000000**, and the same with `EOP_INT_SEL(NONE)` = **0x60000000**. Every
other word is the console's own completion marker, read out of a captured
frame's stream, so only the selector is new: header `0xc0064900`, op
`0x0030c528` (event 40, event index 5, the cache actions), selector, address lo,
address hi, two immediate words and the trailing word AGC emits. `sid.h` in the
pinned Mesa has the field encodings (DST_SEL bits 16-17, INT_SEL 24-26, DATA_SEL
29-31) and the event number (BOTTOM_OF_PIPE_TS 40, ZPASS_DONE 21 -- the 0x115 the
occlusion sample already writes is event 21 with event index 1).

**What the console answered** (run **pid 143**, `Klog_Logs/v0-timestamp-run5.log`,
`jobs/v0-timestamp/queue.txt`). Two frames: the first writes the clock four
times, twice with the write-confirm selector and twice without, and the second
writes both again one sleep later. The first frame's pair advanced 72 ticks and
the second frame's readings came 108,536,495 ticks after it over a 1,087,692 us
sleep: **99,786,056 ticks a second, a period of about 10.02 ns**, so the clock is
the 100 MHz reference a GPU timestamp is expected to be, and the ~0.2% below
nominal is the submit and marker latency the wall-clock interval carries rather
than the clock. Both selectors write; the driver takes radv's write-confirm form.
The clock's magnitude (about 1.036e12 ticks, 2.9 hours) is the console's own
uptime, so it is not reset per title.

**Three mistakes of mine are the record.** The first packet was **seven** words
and the submission's own PM4 framing check refused the frame before it ran
(`validate_indirect_register_tables` counts a type-3 packet as its count plus
two); the console's marker is eight. The second was reading the slots after both
frames, which let the second read overwrite the first frame's clock values with
the zeros it had cleared them to. The third was clearing only the probe's own six
slots between frames: a **ZPASS_DONE write leaves counter words across a wider
range** (the console's second-frame capture showed them from 0x130 to 0x1fc of
the scratch block), and a capture holds the workspace as it stands when the frame
is built, so those leftovers were a workspace difference no PC rebuild could
replay. Clearing the whole 0x200-byte block after the first frame fixed it.

**Evidence.** `golden/v0-timestamp` holds the run's three frames (the probe's two
and the `m2-solid` sibling), and `tools/golden.py rebuild golden/v0-timestamp`
reports **3 of 3 frames identical** on the PC: command words, helper calls,
addresses and workspace image. That rebuild is also the first one that works for
a query probe: `v0-timestamp` is registered with the AGC-level cases rather than
inside `#ifdef AGC_VULKAN_DRIVER`, and the plain host runner's build was broken
until this step -- `run_vulkan_compute_dispatch` defined driver types outside the
guard, so `tools/build-host-runner.sh` (without `--driver`) failed and
`tools/golden.py rebuild` ran a stale binary.

**Next**: `ps5vk_CreateQueryPool` takes `VK_QUERY_TYPE_TIMESTAMP`,
`vkCmdWriteTimestamp` records this packet, `timestampValidBits` becomes 64 and
`timestampPeriod` 10.0, and the command leaves the B2 refusal list.

## 2026-09-18: vkCmdWriteTimestamp, the clock behind the Vulkan API

The probe measured the console's GPU clock (the entry above); this is the
driver's half. `ps5vk_CmdWriteTimestamp` records the packet the probe proved --
RELEASE_MEM `0xc0064900`, event word `0x0030c528` (BOTTOM_OF_PIPE_TS, index 5,
the cache actions), EOP selector `0x63000000` -- writing the clock into the
query's first counter, and registers the pool with the submission so the
counter's cache line is evicted before the CPU reads it
(`ps5vk_cmd_buffer_timestamp_sample`, driver/ps5vk_query.c).
`ps5vk_CreateQueryPool` takes `VK_QUERY_TYPE_TIMESTAMP` beside the occlusion
type, `ps5vk_query_result` returns a timestamp's counter raw and an occlusion
query's difference scaled by sixteen, and a new availability rule tells a query
no command wrote (zero, availability 0) apart from one that ran and read zero.

**The reporting changes with it**: the queue family's `timestampValidBits` is 64
(all of the counter is valid; unlike the z-pass counter there is no valid bit),
`timestampComputeAndGraphics` is true and `timestampPeriod` is 10 ns, the
measured 100 MHz reference clock. `vkCmdWriteTimestamp` leaves the refusal list
and the command audit reads **89 driver, 1 refused, 0 gap** -- `vkCreateBufferView`
is the last one.

**The console run** (run **pid 144**, `Klog_Logs/v0-timestamp-driver-run1.log`,
`jobs/v0-timestamp-driver/queue.txt`): one frame records `vkCmdWriteTimestamp`
into query 0 before its draws and query 1 after them, and
`vkCmdCopyQueryPoolResults` for the second write. The two reads came back
1,090,469,126,918 and 1,090,469,138,750 ticks -- 11,832 apart, about 118 us of
frame -- and the copy carried the second write's value with availability 1, so
the two API paths agree. `golden/v0-timestamp-driver` holds that run's
submission and the `m2-solid` sibling its register defaults come from.

**The PC arms** (driver/tests/vk_v0_timestamp_test.c) see no clock -- the host
layer runs no GPU -- so what they check is the path and the availability rule: a
pool is created, the frame records both writes and the copy, the device accepts
the submission, both results read zero, and the copy's availability word is 0,
which is exactly what tells an unwritten timestamp from one that ran and read
zero. The direct arm also reads the queued stream and finds the packet. With the
console's golden wired into `tools/check-driver.sh` (a replay for the pool's
mapped counters, then `compare-run`), all three arms pass and the direct arm's
submission is **identical to the console's frame**: 10 packets, 3 register
tables.

## 2026-09-18: V0-formats' integer vertex positions: two rows closed

The audit's gap list had eight rows that missed `VERTEX_BUFFER` alone, and two of
them -- `VK_FORMAT_R32G32B32_SINT` and `VK_FORMAT_R32G32B32_UINT` -- are the last
the shader compiler can express: libpsbc's `PsbcVertexFormat` has both, while the
sixteen-bit unorm/snorm families it has no entry for, so no probe can draw with
them whatever the driver does. The driver's table has carried the four formats
M3, M4 and C1b fetched with since those steps; these two had no probe.

**The probe.** Two new shader sets, `probes/v0-vertex-sint` and
`probes/v0-vertex-uint` (`shaders/v0/vertex_sint.vert`, `vertex_uint.vert`, the
m3-vertex colour pixel stage), drawn by the runner cases `v0-vertex-sint` and
`v0-vertex-uint` (`jobs/v0-vertex-int/queue.txt`). The geometry is the m3-vertex
canary's square with its own colours, the position an integer triple and the
records 32 bytes wide: the signed set holds ±1 and scales by exactly 0.5, the
unsigned one holds 0 and 1 and subtracts a half, because an unsigned component
has no negative value to hold. Both put the square exactly where the float frames
put it, and **the third component is carried out as the fragment's alpha**: it is
0 on the left vertices and 1 on the right ones, so the readback has an alpha
gradient to check. A format the hardware fetched wrongly would move the square,
flatten a gradient or leave the alpha at one value, and the log names which.

**The console run** (run **pid 146**, `Klog_Logs/v0-vertex-int-run2.log`):
`v0-vertex-sint` and `v0-vertex-uint` both PASS, and for each of them the whole
square -- 2,073,600 pixels -- is inside the drawn region with **matching 2,073,600,
green exact 2,073,600, blue exact 2,073,600, alpha exact 2,073,600 and max error 0
on all three**. The two frames are `golden/v0-vertex-sint` and
`golden/v0-vertex-uint`, each with the run's `m2-solid` sibling, and
`tools/check-driver.sh v0_vertex_sint v0_vertex_uint` passes all six arms with
the PC's submissions identical to the console's (11 packets, 3 register tables
each).

**What the driver reports now.** `VK_FORMAT_R32G32B32_SINT` and
`VK_FORMAT_R32G32B32_UINT` join the format table with `VERTEX_BUFFER` alone, and
the `v0-formats` self-report case asserts them. The audit reads **22 formats
reported and 53 missing a required feature** (was 20 and 55), and
`tests/test_tools.py` gates the new counts and rows -- which is what failed first
when the table changed, as it should.

**First attempt's mistake, kept for the record.** The first run (pid 145) failed
both cases: the unsigned vertex records were written as *signed* ±1, so the
unsigned fetch read 0xffffffff and the square left the target, and the alpha
gradient rose the wrong way because z was 1 on the left vertices while the check
expects the gradient to rise left to right like green. Both were the probe's, not
the driver's: the second run's numbers above are the whole square exact.

## 2026-09-18: C7's tiled upload: the driver fills a mip chain itself

The driver refused "a copy into an image stored in 64 KiB tiles" by name: the
TiledMip cases proved the chain's layout with the probe writing the tiles itself
(C7's address map, run 33) and the level bases were measured and pinned to the
pinned AddrLib by `tools/check-mip-layout.sh` -- but nothing carried a
application's texels into a chain. It does now.

**What the driver does.** A tiled image's levels are not "each level's tiles
after the last": the sampler walks a chain from its base address by the
hardware's own rule, so the driver places each level where that rule puts it.
`ps5vk_tiled_chains` (`driver/ps5vk_image.c`) holds the rule's answer for the
eight shapes the oracle has been run for, `ps5vk_image_tiled_level_base` reads
it, and `ps5vk_image_storage` sizes such an image with the chain's measured
length instead of the sum of its tiles. An upload into a tiled image is one
`ps5vk_memory_copy` a region with `image_write` set: the queue walks the region's
texels, reading the source's row pitch and writing each at
`ps5vk_image_copy_address` of the level's tiled side
(`ps5vk_image_write_execute`, driver/ps5vk_queue.c), because a tiled row is not
a run of bytes. A shape the oracle has not been run for still refuses by name,
which is the point of keeping the rule measured rather than guessed.

**The console run** (run **pid 148**, `Klog_Logs/c7-mip-upload-run2.log`,
`jobs/c7-mip-upload/queue.txt`): the runner's new `c7-mip-upload` case records one
`vkCmdCopyBufferToImage` a level out of the harness's mapped staging buffer into
a 256x256 five-level chain, then draws the c7-mip probe's six frames -- five
bands, one per level, and five frames with the sampler's LOD range pinned to one
level each. Verdicts: **"a nearest mip filter read every level's own grey"** PASS
and all five pinned frames PASS. A level placed at the wrong base reads another
level's grey, which the bands would name.

**The PC arms.** `driver/tests/vk_c7_mip_upload_test.c` mirrors the case;
`tools/check-driver.sh c7_mip_upload` passes all three arms with the PC's
submissions identical to the console's six frames (16 packets, 6 register tables
each). `tools/check-mip-layout.sh` now also holds the driver's own chain table
against the oracle, shape by shape, so the layout cannot drift from AddrLib's
rule in the driver either.

## 2026-09-18: vkCreateBufferView, the last command with no path

The command audit had one refusal left: `vkCreateBufferView`, held in
`driver/ps5vk_refusals.c` because a buffer view exists for uniform and storage
texel buffers and those are Phase D2. It has its path now
(`ps5vk_CreateBufferView`, `driver/ps5vk_buffer.c`), and the audit reads **90
implemented by the driver, 0 refused by name, 0 unaccounted**.

**What the command owns.** Not the sampling -- that is a texel-buffer binding,
which the descriptor-set layout still refuses by name -- but the reporting rule
an application can check for itself: the format must carry the texel-buffer
feature the buffer's usage asks for (`VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT`
or `VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT`). No format reports either bit
yet, so every valid-shaped call refuses and the message names the format and the
feature: the same answer as "this device samples no texel buffer", reached
through the format table rather than by the command having no path. A missing
buffer and a range that leaves it refuse too, and destroying no view does
nothing.

**The evidence.** `driver/tests/vk_b2_buffer_view_test.c` checks the rule from
both sides on the PC: `vkGetPhysicalDeviceFormatProperties` reports no
texel-buffer feature for `R8G8B8A8_UNORM`, and a view of that format is refused
with no handle written; a view of no buffer and one whose range leaves the buffer
are refused; a destroy of no view is a no-op. `tools/check-driver.sh
b2_buffer_view` passes all three arms, and `b2_commands` and `b2_device` were
updated to say what the refusal is for now.

**The console** (run **pid 149**, `Klog_Logs/v0-formats-run1.log`): the
`v0-formats` self-report case reads **32 of 32 formats report exactly the
features docs/V0_FORMATS_AUDIT.md records**, so the device's own reporting is
what the new command's rule is built from -- including the two integer vertex
formats of the entry above, whose `bufferFeatures` the console reads as
`0x40`, `VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT`. `m2-solid` PASSes after it.

## 2026-09-18: D1's texture arrays, confirmed on the console

The array work had two halves and neither needed a search: the oracle's slice
rule (a slice is a chain of its own, layers consecutive) and the register
database's descriptor fields (`SQ_IMG_RSRC_WORD4.DEPTH` at bits 0-12,
`BASE_ARRAY` at 16-28, word 3's TYPE 13 for a 2D array; docs/HARDWARE_FINDINGS.md).

**What the driver does now.** `ps5vk_write_image_descriptor` writes word 4 from
the view's layers and word 3's kind from the view's type (2D array 13, cube 11,
2D 9), `ps5vk_set_sampled_image` accepts a 2D-array view instead of refusing it,
and the transfers place a layer's texels at the slice the oracle measured:
`ps5vk_image_layer_bytes` is one slice's bytes and
`ps5vk_image_tiled_layer_level_base` is the layer's slice plus the level inside
it, used by the byte-level upload and by the copy sides. A single-layer view and
a single-layer upload write exactly the words they wrote before, which is why the
existing goldens still compare identical. Two shapes joined the measured chain
table for the probe (`256x256x1` at 0x40000, `64x64x1` at 0x10000), both checked
against the oracle by `tools/check-mip-layout.sh`.

**The console run** (run **pid 153**, `Klog_Logs/d1-array-run2.log`,
`jobs/d1-array/queue.txt`): the runner's new `v0-array-layers` case uploads two
layers of a 256x256 tiled image through the driver's own per-layer copies, draws
two bands through a `sampler2DArray` shader (probes/v0-array, a new set), and the
readback holds **0xffc08040 in one band and 0xff7030d0 in the other -- the two
layers' own colours -- with 0 and 0 pixels outside them**. A descriptor that
ignored `DEPTH`/`BASE_ARRAY` would read one layer twice and the log would show
one colour in both bands.

**One refusal had to go first**: `ps5vk_set_sampled_image` refused any view that
was not `VK_IMAGE_VIEW_TYPE_2D` ("a 2D view is what the console's canaries
sampled"), which the first console attempt (pid 152) hit before the descriptor
was ever written. It now takes 2D, 2D array and cube.

**Evidence.** `golden/v0-array-layers` is pid 153's submission and its
`m2-solid` sibling; `driver/tests/vk_v0_array_test.c` is the PC arm and
`tools/check-driver.sh v0_array` passes all three of its arms with the PC's
submission identical to the console's. The full driver check passes with it.

## 2026-09-18: D1's cubemaps, confirmed on the console (arrays and cubes done)

A cube is six slices of a cube-compatible image -- the oracle's six-slice array
row is byte-identical to it, because AddrLib's ADDR2 API has no cube flag -- and
the descriptor says cube with word 3's TYPE 11, the same `DEPTH`/`BASE_ARRAY`
fields the array case confirmed. The driver already wrote TYPE 11 for six
cube-compatible layers when the array step added the kinds, so the cube's work
was the probe and two refusals that would have blocked any cube:

- `ps5vk_set_sampled_image` refused every view that was not
  `VK_IMAGE_VIEW_TYPE_2D`; it now takes 2D, 2D array and cube (the array step's
  first console attempt hit this, pid 152).
- `ps5vk_image_supported` required `flags == 0`; it now accepts
  `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` and nothing else. This one **aborted the
  runner on the console** (pids 160 and 161: the klog stops right after the
  geometry buffers, with no run record) and the PC arm reproduced it exactly as
  the assertion it was -- `ps5vk_CreateImage: Assertion
  ps5vk_image_supported(...) failed` -- which is what the PC test is for.

**The console run** (run **pid 168**, `Klog_Logs/d1-cube-run3.log`,
`jobs/d1-cube/queue.txt`): the runner's new `v0-cube-faces` case uploads six faces
of a 256x256 cube through the driver's per-layer copies and draws six horizontal
bands through a `samplerCube` shader (probes/v0-cube, a new set), each band
sampling one face's direction in Vulkan's order (+X, -X, +Y, -Y, +Z, -Z). The
verdict: **0 of the 8,294,400 pixels are outside their band's face colour**, and
each band's word is logged beside the face's expected word. A descriptor left at
TYPE 9, or with the layer fields at zero, would have sampled one face everywhere.

**Evidence.** `golden/v0-cube-faces` is pid 168's submission and its `m2-solid`
sibling; `driver/tests/vk_v0_cube_test.c` is the PC arm and
`tools/check-driver.sh v0_cube` passes all three of its arms with the PC's
submission identical to the console's. That closes D1's array and cube breadth:
the remaining D1 work is what arrays enable elsewhere, not a layout unknown.

## C8's four-sample depth clear, and the layout behind it

Five batteries of the depth question (`jobs/unknowns-depth4x/queue.txt`,
`Klog_Logs/unknowns-depth4x-run15.log` .. `run19.log`) and one of the clear
(`jobs/c8-depth4x/queue.txt`, `Klog_Logs/c8-depth4x-run1.log`, `run2.log`).

* `-ramp` logged the storage's raw words for the first time
  (`agc_depth4x_raw`, run15): the target's first eight words are pixel 0's four
  samples at x = 0.375, 0.875, 0.125, 0.625 and pixel 1's next, which is the
  MSAA pattern's own sample x offsets. The four samples of a texel are therefore
  its four consecutive words, and the earlier "columns advancing word by word"
  reading was those samples, not four texels.
* `unknowns-depth4x-rects` (new in run16, completed in run19) draws nested strips
  at one origin, one 1x8 and one 8x1 strip per low bit, and 64x64-texel tile
  probes, and logs each rectangle's word count, its element set and the tile
  bitmap. One run's worth of it: a 1x2 strip is **eight words in two elements**
  (a texel is one sixteen-byte element), a 64x64-texel block is 16,384 words in
  64 KiB tiles that step +1 per 64 texels of x and -60 per 64 of y, and the
  element masks read x 1, 4, 2048, 16, 560, 144 and y 2, 8, 1024, 272, 96 with
  y bit 5 moving the storage sixty tiles (docs/HARDWARE_FINDINGS.md has the
  numbers and the family each came from).
* `-single` found no depth word and no colour word in three runs (run15 among
  them): a one-texel quad does not rasterize at four samples. Every measurement
  above uses two-texel strips or larger. The case claims nothing and is reported
  as the negative finding it is.
* `ps5vk_image_copy_side` no longer places a four-sample depth sample with the
  one-sample map's four 0x4000 planes: a CPU copy of one is refused by name
  (docs/HARDWARE_FINDINGS.md, "Boundary"). `vkCmdClearDepthStencilImage` on a
  four-sample image is now one 32-bit fill of the image's storage -- the measured
  layout makes the clear layout-free -- and a partial-range clear of one is
  refused by name.
* `c8-depth-clear` (new case, `run_vulkan_depth_clear_4x_frames`) is that
  command's console proof: a 4K four-sample depth image, the command via the
  harness, then every word of the target read back. Run 1 failed the case's own
  arithmetic (the check compared the 128 MiB allocation with the image's
  33,177,600 words); run 2 passed it: **words 33,554,432, cleared 33,554,432,
  texels 8,294,400, sample words 33,177,600**, runner summary 2 of 2
  (`Klog_Logs/c8-depth4x-run2.log`). A clear that wrote one sample in four, or
  walked the one-sample map's four-byte texels, leaves three quarters of the
  target at other values and fails there.

## V0-formats: the packed families' fetch

V0-formats' sampled probe grew from sixteen formats to twenty-two
(`jobs/v0-formats-sampled/queue.txt`, `Klog_Logs/v0-formats-sampled-packed-run3.log`,
run pid 263): `R5G6B5`, `A1R5G5B5`, `B4G4R4A4`, `E5B9G9R9`, `B10G11R11` and
`A2B10G10R10_UNORM`, whose components are not byte-aligned. The output is
**22 of 22 formats fetched the colour their texel holds** -- so the hardware
decodes each packed word the way the format's table entry says, with the linear
sampler the case has always used.

* The format words are the pinned register database's, chosen by component size
  (`GFX10_FORMAT_5_6_5` 133, `_1_5_5_5` 134, `_4_4_4_4` 136,
  `_5_9_9_9_FLOAT` 132, `_10_11_11_FLOAT` 36, `_2_10_10_10_UNORM` 50), which is
  what Mesa's `ac_formats.c` picks for the same formats.
* The channel order is the descriptor's `DST_SEL`, and the console fitted it:
  `ZYX1` (0x52e) for R5G6B5's B, G, R memory, `ZYXW` (0xf2e) for A1R5G5B5's,
  `YZWX` (0x9f5) for B4G4R4A4's A, R, G, B, `XYZ1` (0x5ac) for the two
  shared-exponent forms and `RGBA` for the ten-bit one -- five of the six
  passed on the first run.
* The sixth, B4G4R4A4, failed on its **own texel bytes**, not on the hardware:
  the word was written 0xF8F4 instead of 0x48FF, and the console faithfully
  reported the colour that word means (B 255, A 68 instead of B 68). The
  expectation was right and the texel was wrong, which is why the PC now
  decodes every packed entry from the source before a console run can read one
  as a hardware result: `tools/packed-format-check.py`, gated by `make test`.
  Run 2's log is the run that failed that way.
* The audit moves: **28 formats are reported** (was 22) and the six rows lose
  their `SAMPLED_IMAGE` and `SAMPLED_IMAGE_FILTER_LINEAR` features, which the
  upload also proves for `TRANSFER_DST`; what they still miss is their blit
  frames, their colour-attachment words and a readback (`TRANSFER_SRC` became
  required the moment the fetch was claimed: 197 probe-reachable features are
  191, 80 features on 44 rows stay parked on libpsbc's enums).

## V0-formats: the packed families' blit source

The per-format blit case iterates the same twenty-two-format table the sampled
probe does, so the six packed formats joined it the moment their fetch was
proved -- once the driver's resampler could decode them. That decode
(`ps5vk_texel_to_rgba8`) is per format, and the packed ones need the
specification's own rules: R5G6B5's 5/6/5 fields, A1R5G5B5's single alpha bit,
B4G4R4A4's A, R, G, B nibbles, the ten-bit fields of A2B10G10R10, and the two
shared-exponent forms (E5B9G9R9's one exponent over three 9-bit mantissas,
B10G11R11's 5-bit exponent over a 6- and a 5-bit mantissa a channel). The
console proved all six: **22 of 22 reported sampled formats blitted into the
destination and read back** (Klog_Logs/c7-copy-packed-run1.log,
`agc_c7_blit_formats`). The same run re-passed the copy case's four frames, the
sampled probe's 22 fetches and `m2-solid`.

So the six rows now carry `BLIT_SRC` too, and the driver's own report says so:
the first run of `jobs/v0-formats` after that (Klog_Logs/c7-copy-packed-run1.log)
failed 27 of 33 -- the packed formats were reporting 0x9401 where the runner's
table still expected the three features the fetch alone proved -- and the
re-run with the four-feature expectation passes **33 of 33**
(Klog_Logs/v0-formats-packed-run3.log). The audit reads **185 probe-reachable
features** (was 191): the packed rows now miss only their colour-attachment
words, their destination blits and a readback.

## V0-formats: the unsigned integer families' fetch

The first of V0-formats' integer families is closed. A UINT texel needs a
`usampler` and a fetch whose value is the integer itself, so the probe is its
own: `probes/v0-texture-uint` (the m3 texture shaders with
`shaders/v0/texture_uint.frag`, which writes `vec4(value) / 255.0` so the RGBA8
target keeps the integer the texel held) and the case
`v0-formats-sampled-uint`, which runs the same frame loop the sampled probe does
through a shared helper (`run_sampled_format_frames`). Three formats, one driver
entry each, with the register database's own word: `R8_UINT` `8_UINT` 5,
`R8G8_UINT` `8_8_UINT` 18, `R8G8B8A8_UINT` `8_8_8_8_UINT` 60, each claimed with
`SAMPLED_IMAGE` and `TRANSFER_DST` and deliberately **not**
`SAMPLED_IMAGE_FILTER_LINEAR`, which an integer format never has.

The console proved it, first try: **3 of 3 sampled formats fetched the colour
their texel holds** (`Klog_Logs/v0-formats-sampled-uint-run1.log`, run pid 119,
the same battery re-passing the twenty-two-format float probe and `m2-solid`),
and `golden/v0-formats-sampled-uint` is that run's three submissions. The audit
moves to **31 formats reported**. The signed families, the 16- and 32-bit ones
and `A2B10G10R10_UINT` follow the same recipe (a second `isampler` probe set and
one entry a format), which the active file's Next list carries.

## V0-formats: the wider unsigned integer families

The unsigned probe's table grew from three formats to eight
(`jobs/v0-formats-sampled`, `Klog_Logs/v0-formats-sampled-uint-run4.log`, run
pid 127): `R16_UINT` (`16_UINT` 11), `R16G16_UINT` (`16_16_UINT` 27), `R32_UINT`
(`32_UINT` 20), `R32G32_UINT` (`32_32_UINT` 62) and `R32G32B32A32_UINT`
(`32_32_32_32_UINT` 75) joined the eight-bit three, and the console read
**8 of 8 sampled formats fetched the colour their texel holds**. The same
battery re-passed the twenty-two-format float probe and `m2-solid`, and
`golden/v0-formats-sampled-uint` is the run's eight submissions.

The first attempt at these five failed for a reason worth keeping: the shader
writes the fetched integer over 255 into an RGBA8 target, so a texel wider than
a byte has to hold a value a byte can carry. The table held 64 x 255 and
128 x 255, and the target clamped both to 1.0 -- the console read 255, 255, 0
(run 2 and run 3 of the same battery, docs/HARDWARE_FINDINGS.md). With texels of
64, 128 and 192 the wider formats fetch the value itself, exactly as the 8-bit
ones do. The audit reads **35 formats reported**; the signed families still need
their selectors fitted, which the active file's Next list carries first.

## V0-formats: the ten-bit unsigned format

`A2B10G10R10_UINT_PACK32` joined the unsigned integer probe (`2_10_10_10_UINT`
54, the UNORM twin's `RGBA` selectors, texels of 64, 128 and 192 in its 10-bit
fields) and the console read **9 of 9 sampled formats fetched the colour their
texel holds** (`Klog_Logs/v0-formats-sampled-uint-run5.log`, run pid 120;
`golden/v0-formats-sampled-uint` is that run's nine submissions, and the same
battery re-passed the twenty-two-format float probe and `m2-solid`). The audit
reads **36 formats reported**; that closes every unsigned integer family the
audit lists. What remains of the integer work is the signed half, whose
selectors the console showed are not the unsigned ones'.

## V0-formats: the signed integer families, and the pair's word

The signed half of the integer work is closed. `R8G8_SINT`'s two-channel word
was fitted a candidate a battery through the isampler probe
(`jobs/v0-formats-sampled`): the register database's `8_8_SNORM` **15** came
back saturated -- **0 of 2 frames**, red 255 in every pixel, which is what the
SNORM decode does to a probe whose shader divides the integer fetch by 255
(`Klog_Logs/v0-formats-sampled-sint-run6.log`, pid 141) -- and `8_8_SINT`
**19** with the unsigned pair's `RG01` selectors passed both frames: the
positive pair (`0x20, 0x40` -> `32, 64, 0`) and the negative second byte
(`0x40, 0x80` -> `64, 0, 0`, `0x80` being -128 read as signed)
(`Klog_Logs/v0-formats-sampled-sint-run7.log`, pid 142, **2 of 2**).

The probe itself had to be fixed first: the expectation the earlier candidates
carried (`0x80` -> +128 in green) cannot be met by any signed decode, so runs
1-3's readings -- red 64 and green 0 whatever the selectors -- were consistent
with a correct signed fetch and had not refuted the words they tried
(docs/HARDWARE_FINDINGS.md re-reads them). A battery run in between repeated the
mislabelled word-20 attempt after a fresh driver build and reproduced run 4's
reading exactly (red, green, blue and alpha all 64 -- `32_UINT` 20, not
`8_8_SNORM`), which is the reading that had already been discarded as a
mislabel.

The rest of the family followed through the same isampler, the register
database's SINT word of each field width and the same selectors the unsigned
entries use: `R8_SINT` `8_SINT` 6 `R001`, `R8G8B8A8_SINT` and
`A8B8G8R8_SINT_PACK32` `8_8_8_8_SINT` 61 (`RGBA` and `WZYX`), `R16_SINT`
`16_SINT` 12, `R16G16_SINT` `16_16_SINT` 28, `R16G16B16A16_SINT`
`16_16_16_16_SINT` 70, `R32_SINT` `32_SINT` 21, `R32G32_SINT` `32_32_SINT` 63
and `R32G32B32A32_SINT` `32_32_32_32_SINT` 76, every texel a value a byte
carries. Run 8 read **11 of 11 frames fetched the colour their texel holds**
(`Klog_Logs/v0-formats-sampled-sint-run8.log`, pid 145) with the float probe and
`m2-solid` re-passing beside them; `golden/v0-formats-sampled-sint` is that
run's eleven submissions and its `run-1.json` the runner frame a later driver
arm compares against. No PC driver arm runs the case yet, exactly as none runs
the unsigned case: the golden is the record.

The audit moves with them. The ten signed rows lose `SAMPLED_IMAGE` and gain
`TRANSFER_SRC` from the maintenance1 rule the moment the driver reports them
sampled, so the counts read **46 formats reported**, `SAMPLED_IMAGE` open on 7
rows instead of 17, `TRANSFER_SRC` on 25 instead of 15, the parked split
unchanged and the probe-reachable count unchanged at 173. What the integer probe
has left is the colour-attachment and blit paths every sampled row still waits
for, which the active file's Next list carries.

## V0-formats: the four-byte colour targets

The colour half of V0-formats has its machinery and its first family. The driver
now carries a colour-target table (`ps5vk_find_colour_format`,
driver/ps5vk_image.c): the `CB_COLOR0_INFO` FORMAT, NUMBER_TYPE and COMP_SWAP
values ps5-opengl's `sceGnmCreateRenderTarget` writes and Mesa's
`ac_get_cb_format` / `ac_get_cb_number_type` / `ac_translate_colorswap` pick. The
render-pass, secondary-command-buffer and pipeline paths take any format that
table names instead of the hardcoded `R8G8B8A8_UNORM`/`B8G8R8A8_UNORM` pair, and
the harness gained `target_format` (driver/tests/ps5vk_triangle.h), which its
target image, view, render passes and pipelines all follow.

The new `v0-targets` case draws the whole 3840x2160 target once per format
through the m4-blend geometry -- a full-target rectangle in the 28-byte-record
layout those shaders were compiled for -- and reads the target's storage back
through the tiled word map. Every format is four bytes a texel, which is what
lets one map serve them all, and each colour is one the format quantises
exactly. Run 1 (`Klog_Logs/v0-targets-run1.log`, pid 169) read **5 of 5**; run 2
(`Klog_Logs/v0-targets-run2.log`, pid 172) repeated it with the capture armed, so
`golden/v0-targets` holds the five submissions and the `m2-solid` frame a later
driver arm compares against. The texels the frames held:

| Format | Texel | Word |
| --- | --- | --- |
| `R8G8B8A8_UNORM` | R 0x40, G 0x80, B 0xc0, A 0xff | `0xffc08040` |
| `B8G8R8A8_UNORM` | the same colour in B, G, R, A bytes | `0xff4080c0` |
| `A8B8G8R8_UNORM_PACK32` | the same colour in A, B, G, R bytes | `0x4080c0ff` |
| `R8G8B8A8_SRGB` | the sRGB encodings 0x89, 0xbc, 0xe1 | `0xffe1bc89` |
| `A2B10G10R10_UNORM_PACK32` | R 256, G 512, B 768 of 1023, A 3 | `0xf0080100` |

The audit moves with them: `A8B8G8R8_UNORM_PACK32`, `R8G8B8A8_SRGB` and
`A2B10G10R10_UNORM_PACK32` carry `COLOR_ATTACHMENT` now, so the feature is open
on 30 rows instead of 33 and the probe-reachable count falls from 173 to 170 at
46 formats reported. Blending is the next step in this family: it needs the
driver's blend registers (`CB_BLEND0_CONTROL`, `CB_COLOR_CONTROL`, which the M4
canary recorded) and the harness's blend state -- the driver refuses a blending
pipeline by name today, so no draw blends silently -- with the pixel shader
exporting
what Mesa's `ac_choose_spi_color_formats` picks for the target (FP16_ABGR for
these 8-bit formats, M4 step 2).

## V0-formats: the colour targets' blending

The driver blends. `ps5vk_pipeline.c` turns an attachment's colour-blend state
into `CB_BLEND0_CONTROL` (0x1e0) -- the factor and equation fields AMD's
register headers name, with Vulkan's factor numbering mapped through a table
because it is not AMD's, and the constant and second-source factors refused by
name -- and picks FP16_ABGR for the pixel stage where an attachment blends, the
export Mesa's `ac_choose_spi_color_formats` chooses for these formats. A blending
draw records that word and `CB_COLOR_CONTROL` (0x202) `0x00cc0011` beside its
colour target's registers; a draw whose pipeline does not blend records neither,
so every earlier frame's words are unchanged. The harness grew the frame's blend
state and its clear colour (`ps5vk_triangle_input.blend*`, `.clear_colour`), so a
frame can clear its attachment to a destination colour and draw a source over it.

`v0-targets` now draws two frames a format: the solid one, and one that clears
to a destination colour and adds a source over it (`ONE`, `ONE`, `ADD`). Both
colours are exact in the format's own units, so the expected words are exact
integers -- including the sRGB one, whose blend happens in linear space and whose
result 0x89 + 0x89 = 0xbc is the re-encoding of 0x40 + 0x40 of 255 linear, a
pair V0-formats' sampling probe measured from the other side. Run 3
(`Klog_Logs/v0-targets-run3.log`, run pid 186) read **10 of 10**: the five solid
words (`0xffc08040`, `0xff4080c0`, `0x4080c0ff`, `0xffe1bc89`, `0xf0080100`) and
the five blended ones (`0xff5a4632`, `0xff32465a`, `0x32465aff`, `0xffe1bcbc`,
`0xe80c0180`). `golden/v0-targets` is that run's ten submissions, three frames
of it the `m2-solid` frame beside them.

The audit moves: `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `A8B8G8R8_UNORM_PACK32`,
`R8G8B8A8_SRGB` and `A2B10G10R10_UNORM_PACK32` report
`COLOR_ATTACHMENT_BLEND`, so the feature is open on 7 rows instead of 12 and the
probe-reachable count falls from 170 to 165 at 46 formats reported. What the
blend work has left is the other texel widths and the integer and float exports:
`R5G6B5`, `A1R5G5B5`, `R8_UNORM`, `R8G8_UNORM`, the `R16_*` float family, and the
UINT/SINT targets Mesa exports as UINT16_ABGR/SINT16_ABGR.

## V0-formats: the integer and half-float colour targets

The colour-target table grew ten four-byte entries: the unsigned five
(`R8G8B8A8_UINT`, `A8B8G8R8_UINT_PACK32`, `A2B10G10R10_UINT_PACK32`,
`R16G16_UINT`, `R32_UINT`), the signed four (`R8G8B8A8_SINT`,
`A8B8G8R8_SINT_PACK32`, `R16G16_SINT`, `R32_SINT`) and `R16G16_SFLOAT`. The CB
word carries the three bits the number type decides, as Mesa's
`ac_build_cb_state` sets them (`BLEND_CLAMP` clear and `BLEND_BYPASS` set for the
integer types, `ROUND_MODE` set for everything not normalized), and a format
whose entry names an export takes it whatever the blend state: the integer
targets export `UINT16_ABGR` or `SINT16_ABGR`, the single-channel 32-bit ones
`32_R`, and the half-float pair `FP16_ABGR` when it blends. The driver also
refuses a blending pipeline on an integer attachment, which Vulkan forbids and
the CB bypasses.

`v0-targets-uint` is new: the m2 fullscreen triangle with a constant
`uvec4(0x40, 0x80, 0xc0, 0xff)` and one frame a format, read back through the
tiled map. Run 5 (`Klog_Logs/v0-targets-run5.log`, run pid 205) read **5 of 5**
-- `0xffc08040`, `0x4080c0ff`, `0xcc020040`, `0x00800040`, `0x00000040` -- and
the same run's `v0-targets` read **12 of 12**, its `R16G16_SFLOAT` frames the new
pair (`0x4d004900` solid, `0x46004400` blended). `golden/v0-targets` is that
run's seventeen submissions.

**The signed half did not get a reading.** `v0-targets-sint`'s shader compiles
once -- its first frame, `R8G8B8A8_SINT`, passed -- and the second compile in
the same process aborts the runner inside ACO's register allocator
(`register_allocation` -> `lower_to_hw_instr` -> `add_entry`), deterministically
over four runs (`Klog_Logs/v0-targets-run4.log`, `-run5.log`,
`Klog_Logs/v0-targets-sint-run2.log`, `-run3.log`; the last with `reason: abort
is called`). That is a compiler fault, not a hardware limit, so the signed rows
claim nothing: the case, its queue line and the four feature bits are out of the
tree, the colour entries stay with a comment saying so, and the signed package
(`probes/v0-target-sint`, `shaders/v0/target_sint.frag`) is built and waiting.
Finding and fixing that fault is the next step's first job; the second is the
`R16_*` and packed families, which this machinery now reaches.

The audit moves with the proved half: `R16G16_SFLOAT` reports
`COLOR_ATTACHMENT` and `COLOR_ATTACHMENT_BLEND`, the five unsigned integer rows
report `COLOR_ATTACHMENT`, and `A8B8G8R8_UINT_PACK32` is reported at all for the
first time, so the counts read **47 formats reported**, `COLOR_ATTACHMENT` open
on 25 rows instead of 30, `COLOR_ATTACHMENT_BLEND` on 6 instead of 7, and the
probe-reachable count **158** on 44 rows.

## 2026-09-19 — RetroArch multi-input fragment shader correction

The user's RetroArch rendering defect took precedence over the active format
ladder. A host compile of its two-input fragment shader reproduced slot aliasing:
both NIR loads had base 0; metadata had zero input semantics and unresolved AGC
linkage despite successful compilation. The regression in
`tools/check-fragment-inputs.sh` failed before the change. Adding RADV's existing
input-location assignment to the compiler work copy resolves both dense (0/1)
and sparse (3/7) locations. Build with `tools/build-psbc-ps5.sh` before
`tools/build-driver.sh`; changing the patch alone does not update a prebuilt
compiler archive. No dependency, version pin or compiler flag was added.

The full driver gate initially exited 141 before its first test: `ldconfig -p`
received SIGPIPE when awk exited after finding the loader. The lookup now consumes
the complete stream while retaining only the first match. This changes host gate
orchestration only.

RetroArch's full 30-second console run completed, was closed by its script, and
records zero refusals and no observed Vulkan API failure. The owner confirms
correct colours with no flicker or triangles. Pixel readback changes opaque-white
text correctness from 0/909 to 909/909 and consecutive frames become identical.
Commands, archive/build identities and results: `evidence/fragment-inputs/`.

Validation completed: `tools/check-driver.sh` PASS (all loader, direct, PS5-link
and negative arms); `make test`, `make lint`, and `make` PASS. The dense/sparse
regression is green. RetroArch also passes its five gates after retiring the
capture hooks. A final upload of that capture-free build was skipped because the
shared console held a running title; the owner-confirmed diagnostic build remains
installed. No running title was interrupted for this cleanup.

## 2026-09-19 — the three format items on the console (pid 244, `jobs/format-items`)

Queue: `capture`, `hold 60`, `c8-depth4-copy`, `v0-transfer-16`,
`v0-blend-constant`, `m2-solid`, `exit`. Log:
`Klog_Logs/format-items-run5.log`. Result: all four tests PASS, 195 PASS
records, one known-benign `agc_live_video_unregister` WARN (VideoOut busy
0x80290009).

`c8-depth4-copy` PASS. Two four-sample D32 images, 256x128 (eight 64x64-texel
tiles), one cleared to 0.5 and one to 0.25, one `vkCmdCopyImage` between them:
the allocation is 2097152 bytes (524288 words) each, the clear fills all of
them, the copy writes the tiles (131072 words) with the source's word, and the
first word the destination's own clear survives in is 131072, the first word
past the tiles. The case is the console proof of the transfer usage
`VK_FORMAT_D32_SFLOAT` now carries and of the region-record copy the driver
replaced its one-record-a-run expansion with.

`v0-transfer-16` PASS. A 500x128 `R16_UNORM` image: `tight_bytes` 128000,
`stored_bytes` 131072 (the driver's rows pad each 1000-byte row to 1024),
filled through `vkCmdCopyBufferToImage` and read back through
`vkCmdCopyImageToBuffer`, 64000 of 64000 words equal and no mismatch. The case
also records why it is a transfer round trip and not a render target: no
two-byte format can be an attachment, so no two-byte image is ever tiled.

`v0-blend-constant` PASS. The m4-blend rectangle over the harness's offscreen
clear (red 0x40, green 0x80, blue 0xff, opaque) with
`CONSTANT_COLOR`/`ONE_MINUS_CONSTANT_COLOR` and
`CONSTANT_ALPHA`/`ONE_MINUS_CONSTANT_ALPHA` and the constants {0.25, 0.5, 0.75,
1.0}: all 8294400 pixels hold `0xff88586c`, the exact blend
(`expected_word` = `first_word`, `wrong` 0, `max_channel_error` 0). The first
attempt at this case drew with the `b8-corner` set and failed, which is the
finding that a blending 8-bit target needs the FP16_ABGR export the m4-blend
set's shaders carry.

`m2-solid` PASS. The console's own canary still renders after the three cases.

Two earlier runs of this queue are part of the record: `run2` (the first
deploy) aborted in `ps5vk_CreateImage`'s assert for the two-byte target and
again for the four-sample depth destination, and `run3` ran to completion with
the depth copy failing on `no memory to record an image copy` (the record
explosion) and the blend case failing on the shader-export mismatch. Both
causes and their fixes are in `docs/HARDWARE_FINDINGS.md`.

## 2026-09-19 — rung round 1: the 25 TRANSFER_SRC rows (pid 266, `jobs/v0-transfer-formats`)

Queue: `hold 60`, `v0-transfer-formats`, `m2-solid`, `exit`. Log:
`Klog_Logs/v0-transfer-formats-run1.log`. Result: **both tests PASS**, 133 PASS
records, one known-benign `agc_live_video_unregister` WARN.

`v0-transfer-formats` PASS, 25 of 25 rows. The case creates a 64x16 image per
format with sampled and transfer usage and no attachment usage (the row layout),
fills it from a staging buffer through `vkCmdCopyBufferToImage`, reads it back
through `vkCmdCopyImageToBuffer`, and requires every tight byte back, the row
padding to be the driver's (1024 tight bytes stored as 4096 for the one-byte
format, 2048 as 4096 for a packed two-byte one, 4096 and 16384 exact for the
four- and sixteen-byte ones), and the format to report `TRANSFER_SRC` at all.

The driver change is the bit on 25 table entries (`driver/ps5vk_image.c`); the
audit moved with it: 25 features closed, `TRANSFER_SRC` gone as an open kind,
the two rows whose only missing feature it was (`B4G4R4A4_UNORM_PACK16`,
`E5B9G9R9_UFLOAT_PACK32`) out of the row table, and the summary at 51 missing
rows and 133 probe-reachable features on 41 rows. `tests/test_tools.py` carries
the new row count with the run id, as its message convention asks.

Gates at this round's commit: `tools/build-driver.sh`, `tools/check-driver.sh`,
`tools/check-runner-cases.sh`, `make test`, `make lint` and `tools/build-all-titles.sh`.

## 2026-09-19 — rung round 2: the vertex rows are the enum's, not the prober's (offline)

The audit counted 32 of the 38 `VERTEX_BUFFER` rows as probe-reachable work, on
the reading that only the sixteen-bit unorm/snorm families had no
`PsbcVertexFormat`. Reading the pinned enum in full
(`.deps/native/psbc/include/psbc_compile.h`) and ps5-opengl's own
`ps5_vertex_format` mapping (`src/gallium/ps5/ps5_screen.c`) shows the enum has
twenty-two values and every one is a **32-bit-component or packed** format: `R32`
float/signed/unsigned with one to four components, the 8888 `B8G8R8A8` and
`R8G8B8A8` UNORM layouts, and the eight 10-10-10-2 forms. No 8-bit or 16-bit
component of any kind, no 16-bit float, no 8888 SNORM or integer layout, has a
value at all.

So **29 of the 38 vertex rows are parked on the enum**, not reachable: their
attribute types have no format word to build, however much a probe shows about
their texels. The nine expressible rows stay reachable work -- `R32_UINT`,
`R32_SINT`, `R32G32_UINT`, `R32G32_SINT`, `R32G32B32A32_SINT`, `R8G8B8A8_UNORM`,
`A8B8G8R8_UNORM_PACK32`, `B8G8R8A8_UNORM`, `A2B10G10R10_UNORM_PACK32` -- and need
their driver entry plus a probe drawing with each (the shader sets for an
attribute *shape*: one `vec4` covers the normalised and packed floats, and
`uvec`/`ivec` sets the integers).

The split line moves with the recount that `tests/test_tools.py` performs:
`PsbcDescriptorType` 74 features on 38 rows (unchanged), `PsbcVertexFormat`
**29 features on 29 rows**, the hardware's fixed fetch order 12 on 2, and
probe-reachable work **110 features on 39 rows**, down from 133 on 41. The
documents quote the enum and ps5-opengl's mapping as the blocker's evidence.

No console run this round: nothing new is claimed about the GPU, and the driver's
table is unchanged (reporting a bit before a run proves it is what this project
does not do). Gates: `make test`, `make lint`, `tools/format_audit.py`,
`tools/command_audit.py`, `tools/limits_audit.py`, `tools/check-driver.sh`,
`tools/check-runner-cases.sh`, `tools/check-mip-layout.sh`,
`tools/build-all-titles.sh`.

## 2026-09-19 — rung round 3: the vertex case is built, the console was busy (offline)

The nine expressible vertex rows (`R32_UINT`, `R32_SINT`, `R32G32_UINT`,
`R32G32_SINT`, `R32G32B32A32_SINT`, `R8G8B8A8_UNORM`, `A8B8G8R8_UNORM_PACK32`,
`B8G8R8A8_UNORM`, `A2B10G10R10_UNORM_PACK32`) now have their probe: three new
shader sets -- `v0-vertex-bytes-float`, `v0-vertex-bytes-uint` and
`v0-vertex-bytes-sint`, one per attribute *class* rather than one per format --
whose vertex shader draws a full-screen triangle from `gl_VertexIndex` and
carries the single attribute to `m3/vertex_colour.frag`; and
`v0-vertex-formats`, one frame a row, binding the row's own format as attribute
0 with the format's size as the stride. The unsigned class writes each component
as a byte (`value / 255`), the signed class adds 128 so a negative component is
visible, and the float class writes the fetched `vec4` out, so a normalised or
packed format's frame is the colour its decode names; the check requires every
pixel of the target within one count a channel and logs the middle pixel's four
channels, which is where a padded component that is not Vulkan's fill rule (zero,
and one for w) would show. The driver's `VkFormat` to `PsbcVertexFormat` table
grew the nine entries, the words taken from ps5-opengl's own `ps5_vertex_format`.

**The console run is pending.** The battery could not start: `ps5vkctl` reports
`the running application is PPSA99169`, the owner's own title, and this
repository does not close a title it did not start. Because a feature may not be
*claimed* before a run proves it, the nine `VERTEX_BUFFER` bits are not in the
driver's format table and the audit still lists them: the claim, the
`kFormatQueries` entries and the audit rows go in with the battery next round,
which is a two-minute edit plus the run. Everything that carries no claim stayed:
the probe sets, the shaders, the case, the queue and the driver's vertex format
mapping.

Gates in this state: `tools/build-driver.sh`, `tools/check-driver.sh`,
`tools/check-runner-cases.sh`, `make test`, `make lint` and
`tools/build-all-titles.sh`.

## 2026-09-19 — rung round 3 finished: the nine vertex rows (pid 283, `jobs/v0-vertex-formats`)

Queue: `hold 60`, `v0-vertex-formats`, `m2-solid`, `exit`. Log:
`Klog_Logs/v0-vertex-formats-run3.log`. Result: **both tests PASS**, 393 PASS
records, one known-benign `agc_live_video_unregister` WARN. Run pid 281 and 282
are the two attempts before it: the first measured seven of nine rows, and the
second was a rebuild that missed its edit.

`v0-vertex-formats` PASS, nine of nine. The frames measured, from the log's
middle-pixel channels: `R32_UINT` (64, 0, 0, 1), `R32G32_UINT` (64, 128, 0, 1),
`R32_SINT` (64, 128, 128, 129), `R32G32_SINT` (64, 192, 128, 129),
`R32G32B32A32_SINT` (64, 192, 96, 0), `R8G8B8A8_UNORM` (64, 128, 192, 255),
`A8B8G8R8_UNORM_PACK32` (64, 128, 192, 255), `B8G8R8A8_UNORM` (64, 128, 192, 255)
and `A2B10G10R10_UNORM_PACK32` (1, 32, 128, 255).

The one- and two-component rows are where the console contradicted a first
reading of the case rather than the driver: an absent component fetches as
Vulkan's fill rule says (zero, and **one** for w), the unsigned shader writes it
back as `1 / 255`, and the frame therefore holds the byte **1** in alpha -- the
first run's expectation of 0xff was the case's error, and the signed rows' 129
in the same position is the same fill read through their +128. The ten-bit row's
4, 128, 512 and 3 land on 1, 32, 128 and 255 within the check's one-count
tolerance, so its decode and export both round rather than truncate.

The nine `VERTEX_BUFFER` rows are closed: the driver reports the bit, the audit's
row table drops it, and the split moves to `PsbcDescriptorType` 74 features on 38
rows, `PsbcVertexFormat` 29 on 29, the hardware's fixed fetch order 12 on 2, and
**101 features on 38 rows probe-reachable**, from 158 on 44 when the rung's
rounds began.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh`,
`tools/check-runner-cases.sh`, `make test`, `make lint`,
`tools/build-all-titles.sh`.

## 2026-09-19 — rung round 4: the blit sources (pid 160, `jobs/c7-blit`)

Queue: `hold 60`, `c7-blit-formats`, `m2-solid`, `exit`. Log:
`Klog_Logs/c7-blit-formats-run3.log`. Result: **both tests PASS**, 1280 PASS
records, one known-benign `agc_live_video_unregister` WARN. `c7-copy` and
`v0-formats` passed in the run before it (`c7-blit-formats-run1.log`), which is
where the driver's query table and the audit were checked together on the
console.

`c7-blit-formats` now loops 44 formats: the 22 sampled families it proved
before, plus the integer families, the byte order VideoOut scans out
(`B8G8R8A8_UNORM`) and the ten-bit unsigned form, each blitted scaled-nearest
into a tiled R8G8B8A8_UNORM destination the frame samples. The driver's
resampler gained a decode for the integer families -- an integer component
becomes eight-bit UNORM by scaling its value to the range its width has, so 0x40
of a byte, 0x4000 of a short and 0x40000000 of a word all land on 0x40, and a
signed value's negative half clamps to zero -- and `VK_FORMAT_FEATURE_BLIT_SRC`
on 21 of the rows the audit listed as reachable.

Two rows keep the bit off: `R16G16B16A16_UINT` has no entry in the driver's
format table at all (its row lists six other missing features), and the one- and
two-channel SNORM forms are the same case. Their audit rows stay open, and the
case's table does not carry them.

Three runs and four builds went into this round, and two of the failures were
mine rather than the hardware's: a helper inserted between a function's return
type and its name left `ps5vk_texel_to_rgba8` untyped (its four wrong frames were
the symptom), and the case's table had been trimmed against a count that did not
match the initialiser, which the compiler refused. Run 2's six failed rows were
that first fault, not the decode.

## 2026-09-19 — rung round 5: the blit destinations (pid 290, `jobs/c7-blit-dst`)

Queue: `hold 60`, `v0-blit-dst`, `m2-solid`, `exit`. Log:
`Klog_Logs/c7-blit-dst-run3.log`. Result: **all three tests PASS** (`v0-blit-dst`,
`v0-formats`, `m2-solid`), 183 PASS records and no FAIL record at all; the new
case's own summary reads 39 rows, 39 written, 39 distinct formats, and
`v0-formats` reads **51 of 51 formats report exactly the features
`docs/V0_FORMATS_AUDIT.md` records** -- the audit's record asked back from the
device, on the same driver that reports the 34 new bits.

The driver gained the write half of the resampler. `ps5vk_rgba8_to_texel` is
`ps5vk_texel_to_rgba8` read backwards -- one case a format, so a blit *into* a
format writes exactly the bytes the sampler's fetch of that format decodes --
and `ps5vk_rgba8_texel_bytes` gives the recording the byte count it refuses on.
The scaled blit's gate now names the destination as well as the source: a blit
into a format with no encode is refused by name rather than written as raw
source bytes (which is what the first run's second row showed, and what the
audit would have had to record as an unproved bit). 38 rows gained
`BLIT_DST`, and the two rows that carried `BLIT_SRC`/`BLIT_DST` without the
transfer pair (`B8G8R8A8_UNORM`, `A8B8G8R8_UINT_PACK32`) gained
`TRANSFER_SRC|TRANSFER_DST`: Vulkan does not let an application create an image
whose usage the format's features do not allow, and the driver's own
`ps5vk_CreateImage` assert is what enforced it.

`v0-blit-dst` (39 rows) fills one `R8G8B8A8_UNORM` source with a single colour,
blits it scaled-nearest into a linear image of each row's format, reads that
image back with `vkCmdCopyImageToBuffer` and compares the bytes against the
encode's rule. Each row logs its format and both words, so a failure names the
format and the disagreement instead of just a count. 39 of 39 match, including
the byte-reversed `A8B8G8R8_*` forms (A first in memory, which is the order
their fetch names) and the integer families.

The round moved the audit's record in two places, and the second one is the
case table `v0-formats` compares the device against: the 38 rows that gained
`BLIT_DST` and the two that gained the transfer pair had to gain them there too,
or the device and the record disagree. `tools/check-runner-cases.sh` caught that
on the PC (13 of 51 formats as audited) before the battery did, and the final
run reads 51 of 51.

**The case is a PC gate as well as a console one.** Its work is the CPU
resampler's, so `tools/check-runner-cases.sh` now runs it whole on the host, and
that run found two driver faults before the console did: `ps5vk_scale8` widened
a 32-bit channel's maximum through a 32-bit product, so every 32-bit integer
encode wrapped to zero, and the integer encode derived its component width from
the block size, which reads an `R8G8_UINT` texel as one sixteen-bit component
instead of two eight-bit ones. Both are fixed; the host run reads 39 of 39.

**Run 1 (pid 287, `Klog_Logs/c7-blit-dst-run1.log`) is the round's own
misreading.** It failed row 2 with the source's identity bytes and then aborted
without a case record. The abort's backtrace pointed into ACO, which sent the
search after a compiler fault; the real cause was two faults at once. The build
had linked a driver archive **older than the source** (the app build reads
prebuilt archives, `tools/build.sh`, and only the app's own objects were
rebuilt), so the deployed driver had no encode at all -- hence the identity
bytes -- and `vkCreateImage`'s assert then aborted the case on `B8G8R8A8_UNORM`,
whose entry still lacked the transfer bits. The ACO frames in that backtrace were
stale stack from the one compile the run did make. `tools/build.sh` now refuses
to link a driver archive older than `driver/`'s sources, which is the guard that
would have caught it.

Audit movement: 51 required rows open before the round, 50 after; the 34
`BLIT_DST` features the reachable list carried are closed and
`VK_FORMAT_R8G8B8A8_SRGB` left the table entirely. The parked/reachable split
reads **74 features on 38 rows parked on `PsbcDescriptorType`, 29 on 29 rows on
`PsbcVertexFormat`, 12 on 2 rows blocked by the hardware's fixed fetch order, and
47 features on 30 rows probe-reachable** -- down from 80 on 38. The largest
family left is `COLOR_ATTACHMENT` (24 rows), then `SAMPLED_IMAGE` (7), the
blends (6), `BLIT_SRC` (5), the linear fetches (3), `D16_UNORM`'s depth
attachment and `R16G16B16A16_UINT`'s `BLIT_DST` (1 each).

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh` (five cases and
two drawing submissions, 201 PASS records), `make test` (23 tests, which recount
the audit's numbers and the parked/reachable split), `make lint`,
`tools/check-mip-layout.sh`, `tools/build-all-titles.sh` (five titles, 0
warnings, 0 misaligned) and the audits -- command 137 required: 90 driver, 0
refused, 47 runtime, 0 gap; limits 106 members, 97 compared, 0 missing; format
47 reported, 50 missing -- are green on this commit. `tools/format_audit.py
--check` still exits 1 by design: it fails while *any* required feature is
missing, and the 50 rows the compiler enums and the hardware's fetch order park
are exactly the ones this rung cannot close.

## 2026-09-19 — rung round 6: the fetch rows (pid 296, `jobs/v0-snorm`)

Queue: `hold 60`, `v0-formats-sampled`, `c7-blit-formats`, `v0-formats`,
`m2-solid`, `exit`. Log: `Klog_Logs/v0-snorm-run4.log`. Result: **all four tests
PASS**, 4171 PASS records and not one FAIL: the sampled case reads **25 of 25**
frames, the blit case **52 of 52**, and `v0-formats` **53 of 53 formats report
exactly the features docs/V0_FORMATS_AUDIT.md records**.

Four rows were left in the audit's reachable list for want of a driver entry or
a claim, and this round closed them:

- `R8_SNORM` and `R8G8_SNORM` had no entry in the table at all: they have one
  now, the register database's `GFX10_FORMAT_8_SNORM` (2) and `_8_8_SNORM` (15)
  with the R001 and RG01 selectors, so they report `SAMPLED_IMAGE`,
  `SAMPLED_IMAGE_FILTER_LINEAR`, the transfer pair and `BLIT_SRC`.
- `B8G8R8A8_UNORM` had never been *sampled* -- it is VideoOut's format and has
  been a colour target since M2 -- and its sampler word is the 8_8_8_8 layout
  with the ZYXW selectors, which is what Mesa's own `radv_compose_swizzle` picks
  for a B,G,R,A description and what the colour target's `COMP_SWAP` ALT says on
  the CB side. It reports the sampled fetch and the linear filter now.
- `A8B8G8R8_UINT_PACK32` had a colour word but no sampler word; it takes
  `8_8_8_8_UINT` (60) with the straight RGBA selectors, because the console's
  integer fetch of that format reads R first -- the byte order round 5's blit
  destination run measured -- and not the reversed one its UNORM twin has.

Nine reachable features closed in all: `SAMPLED_IMAGE` 7 -> 3, the linear
filter 3 -> 0 and `BLIT_SRC` 5 -> 3, leaving `D16_UNORM`, `D32_SFLOAT` and
`R16G16B16A16_UINT` in each. The split reads **74 features on 38 rows parked on
`PsbcDescriptorType`, 29 on 29 rows on `PsbcVertexFormat`, 12 on 2 rows blocked
by the hardware's fixed fetch order, and 38 features on 26 rows
probe-reachable** -- down from 47 on 30 -- and the driver reports **49 formats**
now instead of 47.

Three faults of the round, all in the case machinery rather than the driver:

- The runner's texture helper (`driver/tests/ps5vk_triangle.c`) has a texel-size
  table of its own, and the two rows the round claimed were not in it, so the
  first two batteries stopped with `b7_create_texture_image`
  `VK_ERROR_FORMAT_NOT_SUPPORTED` (runs 1 and 2). It carries them now.
- **The main sampled case had been failing on the table's integer rows ever
  since the table grew past its float families**: a UINT texel fetched through a
  *float* sampler comes back as its raw value (an R8_UINT 0x40 fetches as 64.0),
  so those rows can only pass the two typed cases. The main case now skips them
  (`integer_sampled_format`); the blit case keeps them, because a blit's decode
  is the driver's own whatever a sampler would return. The earlier batteries
  never showed this because they ran a table of 22 rows, and the table has since
  grown to 52 without the case ever being run whole.
- The three signed 32-bit rows carried the *typed* texels (0x20000000 for a
  0x20 colour) while the blit case's decode divides by 0x7fffffff, half the
  unsigned twin's range, so the same colour needs half the value: the rows use
  0x10000000, 0x20000000 and 0x60000000 now and the blit case reads 52 of 52.
- The first battery also died of a **SIGFPE inside ACO's scheduler** after the
  second case: the main case's early FAIL had left its device and pipeline in
  place, and the next case's compile was the second one in that process. With
  the helper fixed the main case completes, and no later battery hit it. It is
  a compiler fault of the same family as the second-compile abort of the signed
  pixel shader, and the open findings note it.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh` (five cases and
two drawing submissions), `make test` (23 tests, which recount the audit's
numbers and the parked/reachable split), `make lint`,
`tools/check-mip-layout.sh`, `tools/build-all-titles.sh` and the three audits
(command 137 required: 90 driver, 0 refused, 47 runtime, 0 gap; limits 106
members, 97 compared, 0 missing; format 49 reported, 50 missing) are green on
this commit.

## 2026-09-19 — rung round 7: the eight-byte unsigned row (pids 320 and 327, `jobs/v0-u16` and `jobs/v0-u16-blit`)

`R16G16B16A16_UINT` was the one row the audit listed as having **no driver entry
at all**, with eight missing features, four of them probe-reachable. It has the
entry now: the register database's `GFX10_FORMAT_16_16_16_16_UINT` (69) with the
RGBA selectors (its signed twin's word 70 with the same selectors), and the
features a row-layout image reaches -- `SAMPLED_IMAGE`, the transfer pair,
`BLIT_SRC` and `BLIT_DST`. Three reachable features closed, the driver reports
**50 formats** now, and the split reads **35 probe-reachable features on 26
rows**. Its colour attachment stays open: an eight-byte texel needs its own tiled
colour map, which the AddrLib oracle already names
(`mip-layout-oracle swizzle 8 1 64kb_r_x`: tile 128x64, ten terms) and no probe
has applied yet.

The round's evidence is two batteries, because the console's compiler would not
let it be one. The *first* queue (`v0-formats-sampled-uint`, `c7-blit-formats`,
`v0-blit-dst`, `v0-formats`, `m2-solid`) died twice with the round-6 **SIGFPE in
`aco::schedule_program`** (runs 1 and 2, pids 308, 317): the unsigned case's
frames all passed -- including format 95, `R16G16B16A16_UINT`, 38400 of 38400
pixels at the colour its texel holds -- and the *next* case's compile then
faulted, so the same shader's compile leaves the compiler in a state the next
one cannot survive. Running the case **last** in its own queue still faults
after its last frame, in the runner's teardown or the post-queue probe, but the
frames are all logged first, which is the evidence the audit needs. The other
half ran clean: `Klog_Logs/v0-u16-blit-run1.log` (pid 327) reads **53 of 53**
blit frames (the table's new eight-byte row among them), `v0-blit-dst`'s
destinations and **54 of 54** formats as the audit records them, with
`m2-solid` after them.

The compiler fault is the third shape of one family and is quoted as such: the
signed-integer pixel shader's *second* compile aborts in ACO's register
allocator, a backtrace can walk stale ACO frames, and now the unsigned texture
case's compile leaves ACO unable to compile again in that process. None of the
three is a hardware limit, and none keeps a row from being *proved* -- only from
sharing one process with another case.

Offline work the round needed: the sampled table grew to 53 rows and the
unsigned table to 10 (its call site's array size with it), the blit destination
table to 40, the runner's texel-size helper learned the 8-byte unsigned form,
and `kFormatQueries` -- the mirror `v0-formats` reads back -- one row.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh` (five cases and
two drawing submissions), `make test` (23 tests), `make lint`,
`tools/check-mip-layout.sh`, `tools/build-all-titles.sh` and the three audits
(command 137 required: 90 driver, 0 refused, 47 runtime, 0 gap; limits 106
members, 97 compared, 0 missing; format 50 reported, 50 missing) are green on
this commit.

## 2026-09-19 — rung round 8: the eight-byte colour target (pid 337, `jobs/v0-wide-target`)

Round 7 left `R16G16B16A16_UINT`'s attachment on one piece: an eight-byte texel
needs a tiled colour map, and `ps5vk_tile_extent` already had the geometry
(128x64 texels a tile) with no row to place a texel by. The driver has it now:
`ps5vk_tiled_8b_terms` is AddrLib's own row, which the oracle prints on demand
(`mip-layout-oracle swizzle 8 1 64kb_r_x`: ten terms) and
`tools/check-mip-layout.sh` compares the driver against, so the map is a gate
before it is a console run. The copy and transfer checks accept an eight-byte
element's one-sample map, and the colour table has the wide rows: 16_16_16_16
and 32_32 with the number types their formats name and the exports Mesa's
`ac_choose_spi_color_formats` picks (UINT16_ABGR for the unsigned integer,
FP16_ABGR for the half-float, 32_AR for the two 32-bit-channel forms,
32_ABGR for the four). Only `R16G16B16A16_UINT` carries `COLOR_ATTACHMENT`: the
others' rows are words without a claim, which the audit keeps as open.

Queue: `hold 60`, `v0-targets-uint`, `v0-formats`, `m2-solid`, `exit`. Log:
`Klog_Logs/v0-wide-target-run1.log`. Result: **all three tests PASS**, 305 PASS
records and no FAIL -- and the same again on the commit's build, pid 338,
`Klog_Logs/v0-wide-target-run2.log`. The integer target case reads **6 of 6** targets, the
eight-byte one through a new two-word check (`check_wide_frame`, the case's own
copy of the eight-byte map): every one of the target's **8294400 texels** holds
both words, 0x00800040 and 0x00ff00c0, the four 16-bit channels of the colour
the uint shader writes. `v0-formats` reads 54 of 54 formats back as the audit
records them and `m2-solid` follows.

Audit movement: `COLOR_ATTACHMENT` 24 -> 23 rows, the driver still reports 50
formats, and the split reads **34 probe-reachable features on 25 rows** (was 35
on 26) with 74 / 29 / 12 parked. Nineteen rows have nothing left but parked
features now.

What the round leaves ready for the next one: the same map, the same two-word
check and the same export table serve the *sixteen*-byte rows (the oracle prints
their row too: 64x64 texels, eight terms) and the *signed* and *float* eight-byte
rows, which wait on their own probes -- the signed ones on the ACO fault, the
float ones only on the case.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh` (five cases and
two drawing submissions), `make test` (23 tests), `make lint`,
`tools/check-mip-layout.sh` (now with the eight-byte row), `tools/build-all-titles.sh`
and the three audits (command 137 required: 90 driver, 0 refused, 47 runtime, 0
gap; limits 106 members, 97 compared, 0 missing; format 50 reported, 50 missing)
are green on this commit.

## 2026-09-19 — rung round 9: the wide float target (pid 109, `jobs/v0-wide2`)

Round 8's map work left the wide attachment rows to their probes. The eight-byte
half-float one is proved now: `R16G16B16A16_SFLOAT` carries
`COLOR_ATTACHMENT` and `COLOR_ATTACHMENT_BLEND`, with the colour entry round 8
wrote (16_16_16_16 with the FLOAT number type and Mesa's FP16_ABGR export) and
the eight-byte colour map.

Queue: `hold 60`, `v0-targets-uint`, `v0-targets`, `v0-formats`, `m2-solid`,
`exit`. Log: `Klog_Logs/v0-wide2-run2.log`. Result: **all four tests PASS**, 851
PASS records and no FAIL. `v0-targets` reads **14 of 14** frames: the 8-byte
half-float row's solid frame holds 0x38003400 and 0x3c003a00 in every one of the
target's 8294400 texels -- the halves of 0.25, 0.5, 0.75 and 1.0, values chosen
exact so the 16-bit storage carries them bit for bit -- and its blended frame
0x38003400 and 0x40003a00, the same three quarters with the clear's and the
source's exact sums and alpha 2.0. The integer case reads 6 of 6, `v0-formats`
54 of 54 formats as the audit records them, and `m2-solid` follows.

Audit movement: `COLOR_ATTACHMENT` 23 -> 22 rows and the blend 6 -> 5; the split
reads **32 probe-reachable features on 24 rows** (was 34 on 25) with 74 / 29 /
12 parked and 50 formats reported.

**The sixteen-byte target did not pass, and that is this round's other result.**
`R32G32B32A32_UINT` and `R32G32B32A32_SFLOAT` were drawn with the sixteen-byte
map (`ps5vk_tiled_16b_terms`, AddrLib's own 64x64-texel row, which
`tools/check-mip-layout.sh` compares the driver against) and read back: 8263680
of 8294400 texels hold the expected words, 99.63 per cent, with the first
mismatch at (x 64, y 2128) -- a tile-column boundary eight rows from the bottom
of the target (`Klog_Logs/v0-wide2-run1.log`). The map is not the suspect: the
oracle derives it and the driver's copy passes the gate. What a sixteen-byte
target needs and the four- and eight-byte ones did not is the **CB pitch**: the
target registers keep AGC's default in the pitch field, which was tuned for a
four-byte target, and a tile row of sixteen-byte elements is half as wide in
texels. The rows' claims are therefore **reverted** -- their colour words stay in
the table without a feature bit, and the case's rows are out of the tables -- so
nothing is claimed that a run did not prove. The next round starts from that
mismatch: log the pitch register, work out the sixteen-byte target's own value,
and re-run the two rows.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh`, `make test` (23
tests), `make lint`, `tools/check-mip-layout.sh` (the eight- and sixteen-byte
rows), `tools/build-all-titles.sh` and the three audits (command 137 required: 90
driver, 0 refused, 47 runtime, 0 gap; limits 106 members, 97 compared, 0 missing;
format 50 reported, 50 missing) are green on this commit.

## 2026-09-19 — rung round 10: the two-channel 32-bit targets (pid 112, `jobs/v0-wide3`)

The eight-byte `32_32` data format is proved now: `R32G32_UINT` and
`R32G32_SFLOAT` carry `COLOR_ATTACHMENT`, and all 8294400 texels of each hold
their two 32-bit channels -- 0x00000040 and 0x00000080 for the unsigned integer
one, the float words 0x3e800000 and 0x3f000000 for the other, solid and blended
(console run pid 112, `Klog_Logs/v0-wide3-run3.log`: 7 of 7 integer targets, 16
of 16 colour targets, `v0-formats` 54 of 54 formats as the audit records them,
`m2-solid`, 4 of 4 and no FAIL).

**The export is what a two-channel 32-bit target needs, not the pitch.** Round
9 left `R32G32B32A32_*` failing at 99.63 per cent and guessed a CB pitch; the
target register block has no pitch register at all -- `ps5vk_target_offsets`
lists CB_COLOR0_BASE, VIEW, INFO, ATTRIB, DCC_CONTROL, CMASK, FMASK,
CLEAR_WORD0/1, DCC_BASE, the four EXT bases and ATTRIB2/3, and round 9's
"records[13]" is CB_COLOR0_DCC_BASE_EXT. What the first attempt at these two
rows showed instead is the *export*: with Mesa's 32_AR the console wrote the
first channel of every texel and left the second untouched (word 0 correct, word
1 zero in all 8294400 texels). With 32_ABGR -- the four-slot export the
32_32_32_32 rows already use -- both channels are written and every texel
matches. The round's own reading of its first failure is the record of that.

The round also grew the failing probe's diagnostics: `check_wide_frame` logs the
first mismatching texel's two words, its coordinates *and* the bounding box of
every mismatch, which is what tells a shifted layout from a hole (the 16-byte
rows' mismatches covered the whole target in the earlier run, which is why the
pitch reading did not hold).

Audit movement: `COLOR_ATTACHMENT` 22 -> 20 rows; the split reads **30
probe-reachable features on 22 rows** (was 32 on 24) with 74 / 29 / 12 parked and
50 formats reported.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh`, `make test`,
`make lint`, `tools/check-mip-layout.sh`, `tools/build-all-titles.sh` and the
three audits are green on this commit.

## 2026-09-19 — rung round 11: the narrow targets (pid 117, `jobs/v0-narrow`)

The one- and two-byte colour targets are proved: `R8_UNORM`, `R8G8_UNORM` and
`R16_SFLOAT` render and blend, `R8_UINT` and `R8G8_UINT` render, and every one of
each target's 8294400 texels holds the value its storage must -- 0x40 for the
one-byte ones, 0x8040 for the two-channel 8-bit pair, 0x3400 for the half-float
(console run pid 117, `Klog_Logs/v0-narrow-run5.log`: 9 of 9 integer targets, 22
of 22 colour targets, `v0-formats` 54 of 54 formats as the audit records them,
`m2-solid`; 4 of 4, 1208 PASS records, no FAIL).

The driver gained the **one-byte element's tiled map**: AddrLib's own row for it
(256x256-texel tiles, thirteen terms, which `tools/check-mip-layout.sh` now
compares the driver against), where the driver had been using the four-byte row
for every element size that was not two, four, eight or sixteen. The CB words
came from the register database (COLOR_8, COLOR_8_8 and COLOR_16 with the UNORM,
UINT, SINT and FLOAT number types) and Mesa's `ac_choose_spi_color_formats`
exports (FP16_ABGR for the normalized and half-float targets, UINT16_ABGR for
the integer pair). Nothing new was needed for the storage: one- and two-byte
elements are single-sample maps the copy and transfer checks accept now.

The case machinery grew a **narrow readback**: a one- or two-byte texel is one
8- or 16-bit value, read through that element size's map, so `check_narrow_frame`
replaces the word-at-a-time walk for those rows, and the target tables route by
element size (four-byte rows keep the RGBA8 map, narrow ones the 8/16-bit maps,
wide ones the wide maps).

Three of the round's five batteries were spent on its own mistakes, each worth
recording because the console's silence looked like a hardware answer:

- **A readback that never ran is not a failure of the target.** The target cases
  gate their check on `triangle.target_bytes >= kFramebufferBytes`, the *four-byte*
  target's size; a one-byte target is a quarter of that, so every narrow row
  failed with no readback record at all. The floor is the target's own size now.
- **A patch that failed an assertion wrote nothing.** Twice a multi-part edit
  script asserted partway and left the file untouched, so the battery ran the
  previous binary and repeated the previous result -- the second time after a
  `Build complete` that was real but of unchanged source. The fixes are applied
  as separate, verified edits now.
- **A failed route shows as the wrong check.** The narrow rows first reached
  `check_wide_frame` with one word and an expectation of zero, because the float
  case's call site passed `target.texel_word` (the four-byte field) while the
  narrow rows carry their value in `texel_words`. Its readbacks logged
  0x40404040 and 0x80408040 at the wrong addresses -- the *right* bytes read
  through the *wrong* map, which is what named the fault.

Audit movement: `COLOR_ATTACHMENT` 20 -> 15 rows and the blend 5 -> 2; the split
reads **22 probe-reachable features on 17 rows** (was 30 on 22) with 74 / 29 / 12
parked and 50 formats reported. Twenty-eight rows have nothing left but parked
features.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh`, `make test`,
`make lint`, `tools/check-mip-layout.sh` (seven map rows now, the one-byte one
among them), `tools/build-all-titles.sh` and the three audits are green on this
commit.

## 2026-09-19 — rung round 12: the packed 16-bit targets (pid 118, `jobs/v0-packed`)

`R5G6B5_UNORM_PACK16` and `A1R5G5B5_UNORM_PACK16` render **and blend**, and
`R16_UINT` renders, on the round-11 narrow machinery: a two-byte texel read
through the two-byte map. The CB words are the register database's COLOR_5_6_5
(16) and COLOR_1_5_5_5 (17) with the UNORM number type, and COLOR_16 (2) with
UINT for the integer row; the exports are Mesa's FP16_ABGR for the packed pair
and UINT16_ABGR for `R16_UINT`. The colours are exact in the storage -- 8 of 31,
16 of 63 and 8 of 31 are the frames' five- and six-bit channels -- so the solid
word is 0x4208 and the blended one 0x8410 for R5G6B5, 0xA108 and 0xC210 for
A1R5G5B5, and 0x0040 for `R16_UINT`.

Queue: `hold 60`, `v0-targets-uint`, `v0-targets`, `v0-formats`, `m2-solid`,
`exit`. Log: `Klog_Logs/v0-packed-run1.log` (pid 118). Result: **all four tests
PASS**, 1373 PASS records and no FAIL: 10 of 10 integer targets, **26 of 26**
colour targets, 54 of 54 formats as the audit records them, and every one of the
new rows' 8294400 texels matching on the first battery of the round.

**The blend family is closed.** `COLOR_ATTACHMENT_BLEND` is now missing only on
the two rows the hardware's sRGB fetch order parks, so the audit's blend row
leaves the reachable table. Two rows left the missing list entirely:
`R5G6B5_UNORM_PACK16` and `A1R5G5B5_UNORM_PACK16` had nothing else missing, so
the audit counts **48 formats missing a required feature** (was 50) and the
split reads **17 probe-reachable features on 14 rows** (was 22 on 17) with
74 / 29 / 12 parked.

What is left of rung 1.0's reachable work is now two families: the **depth rows**
(`D16_UNORM`'s `DEPTH_STENCIL_ATTACHMENT` and the `SAMPLED_IMAGE`/`BLIT_SRC` pair
`D16_UNORM` and `D32_SFLOAT` share -- five features, all needing the sampler's
depth path and, for D16, its `DB_Z_INFO` word), and the twelve
`COLOR_ATTACHMENT` rows of which **ten are the signed integers** and are quoted
against the ACO second-compile abort, and `R32G32B32A32_UINT`/`_SFLOAT` wait on
the sixteen-byte target's 99.63 per cent match (the probe now logs the
mismatches' bounding box).

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh`, `make test` (23
tests, the missing-row count moved with the two rows), `make lint`,
`tools/check-mip-layout.sh`, `tools/build-all-titles.sh` and the three audits
(command 90 driver / 0 refused / 0 gap; limits 0 missing; format 50 reported /
48 missing) are green on this commit.

## 2026-09-19 — rung round 13: the depth rows are the compiler's, not the hardware's (pids 119-121, `jobs/v0-depth`)

The round set out to prove `D16_UNORM`'s and `D32_SFLOAT`'s `SAMPLED_IMAGE` and
`BLIT_SRC` -- five reachable features including D16's depth attachment. It proved
none of the four, and what it found is worth more than the claims would have
been: **a shader whose descriptors include a depth image aborts inside ACO**, so
neither feature is reachable by any probe this driver can run.

Three batteries, three shapes of the same abort:

- run 1 (pid 119) put the two depth rows in the sampled table: the case's frames
  ran to the row before them and the abort took the process at the depth frame's
  pipeline (`Klog_Logs/v0-depth-run1.log`).
- run 2 (pid 120) ran the per-format blit case, whose *source* is the depth image
  and whose frame samples the RGBA8 destination: the abort came at its sixteenth
  frame -- the depth row (run2, pid 120).
- run 3 (pid 121) gave the depth pair a two-row case of its own
  (`c7-blit-depth`, the blit case's body with a table parameter): the abort came
  at the *first* frame (run3, pid 121).

Every backtrace ends the same way --
`radv_nir_lower_descriptors` -> `aco::lower_branches` -> `lower_to_hw_instr` --
which is the compiler, not the GPU. Run 2's sixteenth frame is the second
reading of the same round: the case's whole fifty-five-row table no longer fits
one process either, because the same fault ends a run after roughly fifteen pixel
stages. That is why `run_blit_format_frames` is a helper with a table of its own
now: a family can be proved alone while the fault stands.

So the round's changes are *removals and quotes*, not claims: the two depth
entries lost the sampled and blit-source bits (D32 keeps its measured depth
attachment and transfer pair), the `c7-blit-depth` case is out of the tree, the
depth rows are out of the sampled table, and the audit's split gained a class for
the compiler fault -- **4 features on 2 rows are blocked by the compiler fault,
and 13 features on 13 rows are probe-reachable work** (74 / 29 / 12 parked; 13
was 17 before the round). The four depth features are now quoted against the
fault exactly as the ten signed attachment rows are.

What is left of rung 1.0's reachable work is therefore: `D16_UNORM`'s
`DEPTH_STENCIL_ATTACHMENT` (one feature: its `DB_Z_INFO` word is `Z_16` = 1 in
the word the console measured for D32F, and the depth-target path already exists
from C5), and the twelve attachment rows (ten signed, quoted against the same
fault; two sixteen-byte, waiting on that target's 99.63 per cent match).

The reverted tree then ran a clean battery of its own (pid 122,
`Klog_Logs/v0-depth-run4.log`): `v0-formats` 54 of 54 formats as the audit records
them and `m2-solid` PASS, 108 PASS records and no FAIL -- the console is left
rendering with no claim made that a run did not prove.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh`, `tools/check-runner-cases.sh`,
`make test` (23 tests, the split now five classes), `make lint`,
`tools/check-mip-layout.sh`, `tools/build-all-titles.sh` and the three audits are
green on this commit.

## 2026-09-19 — rung round 14: the D16 depth attachment's groundwork (pid 125, `jobs/v0-depth2`)

The last reachable feature rung 1.0 has that the compiler fault does not block
is `D16_UNORM`'s `DEPTH_STENCIL_ATTACHMENT`. The round did the driver half of it
and proved the half it touched did not break the measured path; the probe and
the claim are the next round's.

What the driver gained:

- `ps5vk_depth_registers` builds `DB_Z_INFO`'s FORMAT field from the image's
  format instead of hardcoding D32F: the register database's `ZFormat` has
  `Z_16` = 1 and `Z_32_FLOAT` = 3, so a D16 target's word is **0x80000181**
  where the console measured **0x80000183** for D32F, with the same
  NUM_SAMPLES bits (C8's four-sample half).
- `ps5vk_tiled_depth_offset` takes the element size, and a two-byte element
  reads through **AddrLib's own Z_X row for it** -- a 256x128-texel tile with
  thirteen terms, `ps5vk_tiled_depth2_terms`, which `tools/check-mip-layout.sh`
  now compares with the oracle beside the four-byte and four-sample rows.
- `ps5vk_tile_extent` keeps a two-byte depth element's own tile instead of
  forcing every depth element to four bytes.

The D16 image also carries no feature bit yet: `ps5vk_draw.c` still refuses a
depth attachment that is not D32_SFLOAT by name, which is the check the probe
relaxes when it renders into D16 and reads the depth back through the two-byte
map. Nothing is claimed that a console run has not proved.

The round's battery is the regression that matters for groundwork: `hold 60`,
`c5-depth`, `c5-depth-clear`, `v0-formats`, `m2-solid`, `exit`
(`Klog_Logs/v0-depth2-run1.log`, pid 125). **All four tests PASS**, 216 PASS
records and no FAIL: the D32 frames still draw and read their depth through the
map that now takes its element size, the clear case still clears, and the audit
mirror still reads 54 of 54 formats as the document records them.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh`, `tools/check-runner-cases.sh`,
`make test`, `make lint`, `tools/check-mip-layout.sh` (eight map rows now, the
two-byte depth one among them), `tools/build-all-titles.sh` and the three audits
are green on this commit.

## 2026-09-19 — rung round 15: the D16 depth attachment (pid 129, `jobs/v0-d16`)

`D16_UNORM`'s `DEPTH_STENCIL_ATTACHMENT` is proved, and with it the last
reachable feature rung 1.0 had that the compiler fault does not block. The M4
canary's own frame -- the same geometry, the same shaders and the same clear of
1.0 -- renders through a D16 depth attachment: the depth test resolves the
overlapping rectangles exactly as it does for D32F, and all **8294400 of 8294400
depth samples** read back through the two-byte element's own map hold the depth
the geometry and the clear wrote (console run pid 129,
`Klog_Logs/v0-d16-run2.log`: `c5-depth-16` PASS, with `c5-depth`, `v0-formats`
54 of 54 and `m2-solid` after it -- 4 of 4, 218 PASS records, no FAIL).

What the round changed:

- `ps5vk_draw.c` accepts a D16 depth attachment where it refused anything but
  D32_SFLOAT by name, and carries the image's format into the registers, so
  `DB_Z_INFO`'s FORMAT field is `Z_16` (1) and the word is **0x80000181**;
- `ps5vk_triangle` takes the attachment's format (`input.depth_format`, D32
  unless a caller names another) for the render pass, the image and the view;
- the case reads a two-byte depth target through the case-side copy of AddrLib's
  2-byte Z_X row and compares 16-bit values, and a `c5-depth-16` case runs the
  criterion's frame with it;
- the audit's split reads **74 / 29 / 12 parked, 4 features on 2 rows blocked by
  the compiler fault, and 12 features on 12 rows probe-reachable**, the driver
  reports **51 formats** (D16's attachment word is reported now), and the only
  reachable features left are the twelve `COLOR_ATTACHMENT` rows: ten signed
  (quoted against the ACO fault) and `R32G32B32A32_UINT`/`_SFLOAT` (the
  sixteen-byte target's 99.63 per cent match).

**The first run of the round is its own lesson.** It read 7646400 of 8294400
samples and named the fault exactly: the first mismatch at (480, 1080) held
0xbfff where the expectation said 0xc000 -- the far rectangle's 0.75. The
console converts a depth to sixteen bits by rounding to nearest, and 0.75 x
65535 is 49151.25, so the stored value is 49151 (0xbfff), not 49152. The
expectation was the round's arithmetic, not the hardware's; one constant moved
and the next run read every sample.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh`, `tools/check-runner-cases.sh`,
`make test`, `make lint`, `tools/check-mip-layout.sh` (eight map rows),
`tools/build-all-titles.sh` and the three audits (format 51 reported / 48
missing) are green on this commit.

## 2026-09-20 — rung round 16: the sixteen-byte rows' first compile, and the ten signed probes (pids 133, 135, 139; `jobs/v0-wide16`)

The round went after rung 1.0's last two reachable rows. `R32G32B32A32_UINT` and
`R32G32B32A32_SFLOAT` were open on round 9's measurement -- drawn last in the two
family cases they rendered, 8263680 of 8294400 texels holding their words, the
first mismatch at (64, 2128) (pid 109, `Klog_Logs/v0-wide2-run1.log`) -- and the
round's job was to re-read that mismatch with `check_wide_frame`'s bounding box.
It did not get there: every shape that put the row **first** died inside ACO at
the process's first pipeline compile.

| run | shape | result |
| --- | --- | --- |
| pid 133, `Klog_Logs/v0-wide16-run2.log` | the unsigned family with the sixteen-byte row in it, its own cases after | eight frames passed (`R8G8_UINT` last) and the process aborted while the next frame's pipeline was compiled; 308 PASS records, no FAIL |
| pid 135, `Klog_Logs/v0-wide16-run3.log` | the case `v0-target-wide16-uint`, the row alone | `abort is called(system)` at the case's first compile, right after `b7_create_device`; 45 PASS records |
| pid 139, `Klog_Logs/v0-wide16-run4.log` | the family cases with the rows moved to the front of their tables | died before its first frame was logged, the same 45 PASS boundary; the battery relaunched the title three times and all four processes died the same way |

All three backtraces walk ACO's lowering: `aco::lower_branches` ->
`aco::lower_to_hw_instr` -> `aco::reindex_ssa` (pid 139's frames 0x449383,
0x439549, 0x438598, 0x4157df, 0x4045e3, 0x435f26 and 0x40016b resolved by nearest
symbol over the deployed build's `build/llvm-pie.elf`; the other two runs show the
same chain at addresses 0x50 lower). The console's own deployed queue is what
identifies the probe the third run's binary carried: it names
`v0-target-wide16-uint` and `v0-target-wide16-sfloat` and records round 16's
first attempt, with the probe `agc_v0_targets_wide16` visible in that build's
strings.

**The rows stay reachable.** Round 9's reading is the counter-evidence: the same
target's pixel stage lowered fine when it was the process's seventh or sixteenth
compile, and the driver's words for both rows are the ones that rendered then. So
the fault is a property of the *first* compile of a process, not of the row's
words, and quoting the rows against the compiler fault would have hidden work a
console run has already done. The rows keep round 9's place at the end of
`kTargetFormats`/`kUnsignedTargets`, the table comments carry the arrangement, and
`jobs/v0-wide16/queue.txt` draws them mid-case, where `check_wide_frame` logs the
mismatch box. `docs/HARDWARE_FINDINGS.md` has the reading in full.

**The round's other half is the signed family's probes** -- the last ten rows rung
1.0 can reach at all. The signed pixel stage compiles once in a process and
aborts ACO's register allocator on the second compile (round 4), so each row gets
a case that draws exactly one frame: `kSignedTargets` holds the ten rows with the
`ivec4(0x20, 0x40, 0x60, 0x7f)` words `shaders/v0/target_sint.frag` writes
(`R8G8_SINT` 0x4020, `A8B8G8R8_SINT_PACK32` 0x2040607f, `R32G32B32A32_SINT` four
32-bit words), the ten cases `v0-target-sint-r8` through
`v0-target-sint-r32g32b32a32` use the `v0-target-sint` package, and
`jobs/v0-sint-*/queue.txt` carries one case each, so one battery proves one row and
the process never sees a second signed compile. `driver/ps5vk_image.c` gained the
three signed colour words the table lacked (`R16G16B16A16_SINT` 16_16_16_16 with
SINT16_ABGR, `R32G32_SINT` 32_32 and `R32G32B32A32_SINT` 32_32_32_32 with
32_ABGR), each the twin of its unsigned row's word per Mesa's
`ac_choose_spi_color_formats`; the other seven were already there.

**The console's control payload went down with the last battery.**
`Klog_Logs/v0-wide16-run4.log`'s four crashing processes are the last thing it
launched: `ps5vkctl` answered `ok idle` before the battery and refuses connections
after it, so the ten signed batteries wait for `build/ps5vkctl/ps5vkctl.elf` to be
loaded on the console again.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full, B2's three
requirement programs included), `tools/check-runner-cases.sh`, `make test` (23
tests), `make lint` (183 files), `tools/check-mip-layout.sh` (the eight map rows
and the four-sample depth map), `tools/build-all-titles.sh` (5 titles, 0 warnings)
and the three audits (command 137 required: 90 driver, 0 refused, 47 runtime, 0
gap; limits 106 members, 97 compared, 0 missing; format 51 reported, 48 missing)
are green on this commit.

## 2026-09-20 — rung round 17: the "compiler faults" were one assert in `vkCreateImage` (rounds 4, 13 and 16 re-read)

The round set out to reproduce the console's ACO aborts on the host. It could not
-- the host build of the same compiler (`.deps/work/psbc-ps5/third_party/opengnm-psbc/libpsbc.pic.a`)
compiled the signed and the sixteen-byte pixel stages twelve times over, with the
driver's own options and every export combination the colour table uses, and
returned `PSBC_RESULT_OK` every time (`/tmp/psbc_pair.c`'s recipe: vertex NGG plus
five pixel stages a pass, twelve passes). That negative result is what forced the
question the backtrace had been answering too easily, and the answer is the call
in flight:

| run | last [PS5VK] record before `abort is called(system)` | the call it died in |
| --- | --- | --- |
| pids 133, 135, 139 (`Klog_Logs/v0-wide16-run2..4.log`) | `b7_create_device` | `b7_create_image` of `R32G32B32A32_UINT`, whose entry carries no `COLOR_ATTACHMENT` bit |
| pids 119, 120, 121 (`Klog_Logs/v0-depth-run1..3.log`) | `b7_create_device`, `b7_create_copied_view` | the depth image the case samples: `D16_UNORM` had **no entry in the driver's table at all** then, and neither depth row carried `SAMPLED_IMAGE` |
| round 4's signed runs (`Klog_Logs/v0-targets-sint-run2.log`, `-run3.log`, `v0-targets-run4.log`, `-run5.log`) | `b7_create_device` | `b7_create_image` of the case's second signed format, whose entry the attempt had not claimed |

`b7_create_image` cannot reach the shader compiler. `ps5vk_CreateImage` asserted
`ps5vk_image_supported` instead -- and **an assert's abort backtrace walks
whatever ACO frames the stack still held**, which is why rounds 4, 13 and 16 all
read the same `abort() -> aco::...` chain as a compiler fault. The compiler had
just run for the case's earlier frame or for the runner's own startup probes, and
its frames were still the newest complete chain on that stack.

**What the round changed.**

- `driver/ps5vk_image.c`: that assert is a **named refusal** now --
  `VK_ERROR_FORMAT_NOT_SUPPORTED` with the format, tiling, usage and flags in the
  message -- so a case whose row the driver's table does not carry yet fails
  loudly in the log instead of killing the title. The extent/mip/layer assert
  stays, since no probe can reach it by accident.
- `driver/tests/vk_b3_image_test.c` checks the refusal for
  `R32G32B32A32_UINT` and a colour attachment, which is the exact call the three
  rounds died in (`tools/check-driver.sh`: "a colour attachment whose format has
  no COLOR_ATTACHMENT bit is refused by name", PASS).
- `src/diagnostics.cpp`: the signed probe is one family case again
  (`v0-targets-sint`, one frame a format, `jobs/v0-sint`) -- the one-case-a-battery
  design existed only because the "second signed compile" was read as fatal -- and
  the sixteen-byte rows keep round 9's place at the end of
  `kTargetFormats`/`kUnsignedTargets` with their queue's comment corrected to the
  real blocker.
- The audit's **compiler-fault class is empty**: the ten signed attachments, the
  two sixteen-byte attachments and `D16_UNORM`'s and `D32_SFLOAT`'s
  `SAMPLED_IMAGE`/`BLIT_SRC` are reachable work, so the split reads **74 / 29 / 12
  parked, 0 features on 0 rows blocked by the compiler fault, and 16 features on
  14 rows probe-reachable**. `tests/test_tools.py`'s `blocked_by_compiler` set is
  empty with the diagnosis beside it, and its signed-probe tests now check the ten
  words against their unsigned twins, the one case and its queue.

**What round 4 already proved, and what the next batteries are.** Its first signed
frame, `R8G8B8A8_SINT`, passed -- image, pipeline, draw and every texel -- before
the second format's unclaimed entry aborted the process, so the signed words and
the `ivec4` shader are measured, not speculative; the ten-row case needs the ten
`COLOR_ATTACHMENT` bits in the same commit as its battery. The sixteen-byte rows
need their two bits and then round 9's mismatch box re-read. The depth pair needs
its two bits plus the sampler's depth word, which no attempt has reached yet. The
console's control payload went down with round 16's last battery and every one of
those batteries waits for it.

## 2026-09-20 — rung round 18: nine signed colour attachments proved, and the sixteen-byte map's one band (pids 110, 111; `jobs/v0-sint`, `jobs/v0-wide16`)

The console's control payload came back, so the round ran the two batteries its
claims needed. The claims were the twelve `COLOR_ATTACHMENT` bits round 17's
diagnosis said the cases were missing -- all twelve rows already had their
`CB_COLOR0_INFO` words in `ps5vk_colour_formats` -- and the readings split the
twelve cleanly.

**Nine passed.** `v0-targets-sint` is one case again (round 17's collapse), and
its ten frames drew the signed `ivec4(0x20, 0x40, 0x60, 0x7f)` into
`R8_SINT`, `R8G8_SINT`, `R8G8B8A8_SINT`, `A8B8G8R8_SINT_PACK32`, `R16_SINT`,
`R16G16_SINT`, `R16G16B16A16_SINT`, `R32_SINT` and `R32G32_SINT`: every one of
each target's 8294400 texels held the word its storage carries -- 0x4020 for the
two-byte one, 0x7f604020 for the 8888 layout, 0x2040607f for the byte-reversed
packed one, 0x00400020 and 0x007f0060 for the eight-byte one, and the four
32-bit words for the rest (`agc_v0_targets_int` 9 of 10, console run pid 110,
`Klog_Logs/v0-sint-run1.log`). Those nine rows' `COLOR_ATTACHMENT` is proved, so
their claims stay and their rows left the reachable list; `src/diagnostics.cpp`'s
audit mirror moved with them and the next run read `v0-formats` **54 of 54**
(pid 111, `Klog_Logs/v0-sint-run2.log`).

**The three sixteen-byte rows failed identically, and that is the round's other
result.** `R32G32B32A32_SINT` (the signed case's tenth frame) and
`R32G32B32A32_UINT`/`_SFLOAT` (the family cases with the rows last) each read
back 8263680 of 8294400 texels -- 99.63 per cent, round 9's number exactly -- with
the *same* first mismatch (64, 2128) and the *same* box: x 64..3839, y 2128..2143
(pid 110, `Klog_Logs/v0-sint-run1.log`; pid 111,
`Klog_Logs/v0-wide16-run5.log`). So the sixteen-byte map is one bug and not three:
thirty-two mismatching texels a row over sixteen rows, the odd 64-texel tile
columns of the last tile row's tile-local rows 16 to 31, and the stored word is
0x0 -- a hole in the read, not a neighbour's colour. Their claims are out again
and the rows stay reachable work; the driver's new refusal names them instead of
aborting (`format 108: type 1, tiling 0, usage 0x10 ... is not a supported
image`, `Klog_Logs/v0-sint-run2.log`), which is exactly what round 17's change
was for. `docs/HARDWARE_FINDINGS.md` carries the box and the next probe's shape:
a gradient frame, so a displacement shows as a colour and not as a hole.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full), the runner-case
and mip-layout gates, `make test`, `make lint`, `tools/build-all-titles.sh` and
the three audits are green on this commit, with the audit at **51 formats
reported, 48 missing, 55 conditional** and the split at **74 / 29 / 12 parked, 0
compiler-blocked, 7 features on 5 rows probe-reachable**.

## 2026-09-20 — rung round 19: the sixteen-byte map's twist, measured by a ramp frame (pids 113-115; `jobs/v0-wide16-ramp`)

Round 18 left the three sixteen-byte rows failing identically -- 8263680 of
8294400 texels, first mismatch (64, 2128), box x 64..3839, y 2128..2143 -- and
this round measured why. Two diagnostics went into the failing readback first
(`check_wide_frame`: the mismatches' count per tile-local row and per tile
column, then a walk of the **whole allocation without the map**), which returned
the shape: 1920 mismatches in each of tile-local rows 16..31, 1024 in each **odd**
tile column, and the allocation itself holding exactly 8294400 words of the
expected colour in 1980 full tiles and the last tile row's sixty partial ones
(pid 111, `Klog_Logs/v0-wide16-run6.log`). A solid frame can only see holes, so
the round added the measurement that sees displacement too.

**The ramp frame.** `v0-wide16-ramp` draws the colour family's rect into
`R32G32B32A32_SFLOAT` with a colour that encodes the fragment's own position: `R
= x/4096`, `G = y/4096`, dyadic rationals a 32-bit float holds exactly, evaluated
at the pixel centre so the decode is exact. The case then walks the mapped
storage with **no map at all**: every word names the texel that wrote it (8294400
of 8294400 decoded, none foreign; pids 113-115), and the raw dump records the ramp
value at each texel's *mapped* address for three tiles -- the image's second tile,
a tile of a full tile row, and the band's tile -- sixteen columns and all
sixty-four tile-local rows a tile.

**What it says.** The driver's `in_y` bit 5 is inverted for odd tile columns: the
word at the address the driver's map gives for `(in_x, in_y)` belongs to the
texel at `in_y ^ 32` whenever `x / 64` is odd, and in the last (partial) tile row
the twisted rows 48..63 do not exist, so those addresses are unwritten -- exactly
the 30720 holes and the odd columns only. That is the block-level XOR the driver
already applies to four-sample images (`in_y ^= (x / tile_width & 1u) *
(tile_height / 2u)`), and the sixteen-byte colour map needs it too, in
`driver/ps5vk_image.c`'s term application and in the case-side copy
(`tiled_wide16_offset_for`) that reads the target back.
`docs/HARDWARE_FINDINGS.md` carries the dump and the fix; the x-axis half of the
twist is still unmeasured, and the ramp frame's verification pass (decode each
probe tile's words and compare them with the texel that should have written them)
is what the next run reads before the three `COLOR_ATTACHMENT` bits are claimed
again. The claims are out of the driver's table on this commit: the rows are
still unproved, and the audit's split is unchanged at **7 features on 5 rows
probe-reachable**.

## 2026-09-20 — rung round 20: the sixteen-byte twist applied, three more rows proved (pids 116-118; `jobs/v0-wide16-ramp`, `jobs/v0-wide16`, `jobs/v0-sint`)

Round 19 measured the twist; this round confirmed it and closed the rows. The
ramp case's new candidate check applies two candidate block XORs to the term table
independently of the driver's map and compares each candidate's word with the
texel that should have written it: the y-half candidate matched 8192 of 12288
probe texels, the two-half candidate **11264 of 11264** of those that exist, with
no first mismatch (pid 116, `Klog_Logs/v0-wide16-ramp-run4.log`). The 1024 the
first candidate could not match are the probe rows past the target's height, which
the check now skips -- the last tile row's unrendered rows, the very place the
holes were.

**The fix is two lines, twice.** `driver/ps5vk_image.c`'s
`ps5vk_tiled_texel_offset` applies the whole block XOR to sixteen-byte elements
(the same one four-sample images get), and the case-side copy
`tiled_wide16_offset_for` -- the map the frames' readback walks -- got the same
two lines. With them the readings are clean: `v0-targets-uint` **11 of 11**
integer targets, `v0-targets` **28 of 28** colour targets (the sixteen-byte
float row's solid and blended frames among them), `v0-targets-sint` **PASS** (the
ten signed rows and the sixteen-byte signed one), `v0-formats` **54 of 54** after
the audit mirror moved for the three formats, `m2-solid` -- 1469 PASS records and
no FAIL in the wide battery (pid 117, `Klog_Logs/v0-wide16-run8.log`), 409 and no
FAIL in the signed one (pid 118, `Klog_Logs/v0-sint-run4.log`).

**So the three sixteen-byte `COLOR_ATTACHMENT` rows are proved and reported.**
Their claims stay in the driver's table, the audit's row table lost
`COLOR_ATTACHMENT` for all three, and the split reads **74 / 29 / 12 parked, 0
features on 0 rows blocked by the compiler fault, and 4 features on 2 rows
probe-reachable** -- the two-byte depth pair's `SAMPLED_IMAGE` and `BLIT_SRC`,
the only rows left that a probe this driver can run has not reached. What closes
them is the depth sampler's own word and path (round 13 never got past image
creation to compile a depth-descriptor shader) and then one console battery with
the bits claimed, exactly as these three went.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full), the runner-case
and mip-layout gates, `make test`, `make lint`, `tools/build-all-titles.sh` and
the three audits are green on this commit (format 51 reported / 48 missing / 55
conditional).

## 2026-09-20 — rung round 21: the depth pair proved, and rung 1.0 closed (pids 126, 128; `jobs/v0-depth-pair`, `jobs/v0-transfer-formats`)

The last family. `D16_UNORM`'s and `D32_SFLOAT`'s `SAMPLED_IMAGE` and `BLIT_SRC`
were the only features left that a probe had never reached: round 13 died at
`vkCreateImage` -- the assert round 17 turned into a named refusal -- so no
depth-descriptor shader had ever compiled, and the audit's four features stayed
open through sixteen rounds of colour work.

**The round's first attempt refused itself, loudly and usefully.** The sampled
case's texture image asks for `SAMPLED | COLOR_ATTACHMENT | TRANSFER_DST`, which
a depth format cannot be, and the driver's new refusal named it: `format 124:
type 1, tiling 0, usage 0x6 ... is not a supported image`. The harness learned
that a depth texture is nobody's colour attachment (its image stays the row
layout the sampler reads) and that its view, upload and readback regions name the
depth aspect; the driver's upload path, which asserted the colour aspect, now
refuses any other by name.

**Then the two readings.** `v0-formats-sampled-depth` uploads a 0.5 depth into a
256x4 image of each format and samples it: **2 of 2 sampled formats fetched the
colour their texel holds** -- 0x80 in red, the R001 single channel their fetch
words (16_UNORM 7, 32_FLOAT 22) name -- so a depth image needs no descriptor
field of its own. `c7-blit-depth` blits each row's texels, one to one, into a
second depth image of the same format -- a depth-to-colour blit is not a blit of
compatible formats and the driver refuses it by name -- and checks the
destination's own bytes: **1024 of 1024 texels for both**, D16 at a 512-byte row
pitch and D32 at 1024, first bad texel none (pid 126,
`Klog_Logs/v0-depth-pair-run4.log`, 344 PASS records and no FAIL). The last piece
was `vkCmdCopyImageToBuffer`, which had refused a depth source since C7: it now
reads one back through the depth map the C5 rows measured, and
`v0-transfer-formats`' new D16 row round-trips a two-byte depth image through a
linear one and reports the bit (pid 128, `Klog_Logs/v0-transfer-formats-run3.log`).

**Rung 1.0 is closed.** The audit reads **74 / 29 / 12 parked, 0 features on 0
rows blocked by the compiler fault, 0 features on 0 rows probe-reachable**, 51
formats reported and 46 missing a required feature -- every one quoted against
the libpsbc `PsbcDescriptorType` or `PsbcVertexFormat` enum or the hardware's
fixed sRGB fetch order, which is what the rung's finish line asked for.
`tests/test_tools.py` asserts that finish line itself now: the compiler and
reachable counts must be zero, so a change that opens a probe-reachable row has
to close it or quote it before the suite passes.

Gates: `tools/build-driver.sh`, `tools/check-driver.sh` (full), the runner-case
and mip-layout gates, `make test` (26 tests, the closure assertion included),
`make lint`, `tools/build-all-titles.sh` and the three audits (command 137: 90
driver / 0 refused / 47 runtime / 0 gap; limits 106/97/0; format 51 reported /
46 missing / 55 conditional) are green on this commit.

## 2026-09-20 — blocker round 1: the eight-bit vertex formats (pid 110; `jobs/v0-vertex-formats`)

Rung 1.0's rows are either proved or quoted, and the biggest quoted family is the
libpsbc enums. This round took the first mechanism of it: the eight-bit component
vertex layouts, which the driver refused because `PsbcVertexFormat` named no
eight-bit format at all. Public reference: KytyPS5's
`src/graphics/guest_gpu/gpu_defs.h` carries the GFX10 hardware format numbering
-- `k8UNorm` 1, `k8SNorm` 2, `k8UInt` 5, `k8SInt` 6, `k8_8UNorm` 14,
`k8_8SNorm` 15, `k8_8UInt` 18, `k8_8SInt` 19 -- the same space this repository's
fetch words come from, so the hardware fetches all eight and only the enum and
its pipe format were missing.

**The mechanism.** `tooling/psbc/patch-vertex-formats.py` appends the eight enum
values to the compiler's header and their `PIPE_FORMAT_*` cases to the compiler,
in the same idempotent, anchor-checked style the fragment-input patch uses; the
compiler's `gfx_state.vi` takes a pipe format a vertex attribute and Mesa's
vertex-element emission writes the descriptor word from it. `tools/build-psbc-ps5.sh`
runs the patch (both the PS5 and the host archive come from the same work copy),
the driver's `ps5vk_vertex_formats` maps the eight VkFormats onto the new values,
and their format entries claim `VERTEX_BUFFER`.

**The proof.** `v0-vertex-formats` grew from 9 rows to 17 and every one read back
8294400 of 8294400 pixels: `R8_UNORM` 0x40, `R8_SNORM` 0x81 (64/127 of 255 is
128.5, which the target stores as 129 -- the decode is exact), `R8_UINT` 0x40,
`R8_SINT` 0xc0, and the two-channel rows 0x40/0x80, 0x81/0x40, 0x40/0x80 and
0xc0/0xa0, with Vulkan's fill rule's zero and one in the components the formats do
not have (pid 110, `Klog_Logs/v0-vertex-formats-run4.log`: three cases, 701 PASS
records, no FAIL; `v0-formats` 54 of 54 after the mirror moved). One mirror bug
was caught on the way -- the four integer rows' `VERTEX_BUFFER` first landed in
their optimal-features slot instead of the buffer one, which the console's
`v0_formats_different` named exactly -- and `tests/test_tools.py` gained a
three-test gate tying the patch's enum values, its switch cases and the driver's
mapping table together, so a drift between the two trees fails the suite.

**Audit movement.** The eight rows lost `VERTEX_BUFFER`; the split reads
**74 features on 38 rows parked on `PsbcDescriptorType`, 21 features on 21 rows
on `PsbcVertexFormat`, 12 on 2 blocked by the hardware's fetch order, 0 on 0
blocked by the compiler fault and 0 probe-reachable**; 46 formats still miss a
required feature, the eight included -- their remaining gaps are the texel-buffer
and storage-image rows the descriptor enum parks. `docs/BLOCKERS.md` is the new
plan and mechanism log for the work after the rung, with the sixteen-bit
components next.

Gates: `tools/build-psbc-ps5.sh` (1 source compiled, the patch in place),
`tools/build-driver.sh`, `tools/check-driver.sh` (full), the runner-case and
mip-layout gates, `make test` (29 tests), `make lint`, `tools/build-all-titles.sh`
and the three audits (command 137: 90 driver / 0 refused / 47 runtime / 0 gap;
limits 106/97/0; format 51 reported / 46 missing / 55 conditional) are green on
this commit.

## 2026-09-20 — blocker round 2: the sixteen-bit vertex formats (pid 111; `jobs/v0-vertex-formats-16`)

The same mechanism as round 1, one family further: the fifteen sixteen-bit
component layouts (`R16_UNORM`, `R16_SNORM`, `R16_UINT`, `R16_SINT`,
`R16_SFLOAT`, their `R16G16_*` twins and the four-channel
`R16G16B16A16_*` family), whose hardware words are 7, 8, 11, 12, 13, 23..29 and
65..71 in the same GFX10 space. `tooling/psbc/patch-vertex-formats.py` became a
table-driven script (the `FORMATS` list, inserted one value at a time before the
enum's closing brace and the switch's `default:`), which made it idempotent on an
already-patched work copy and let round 3 add to the same table.

**Three of the fifteen had no format entry at all** -- `R16_SNORM`,
`R16G16_SNORM` and `R16G16B16A16_SNORM` -- so the round added them with
`VERTEX_BUFFER` alone: the spec's sampled and blit requirements for a SNORM
format are conditional and no probe has claimed them, so the entries report only
what the console proved. `R16_SNORM` and `R16G16B16A16_SNORM` (and
`R16G16_UNORM`'s twins) had nothing else missing, which took six rows out of the
audit's list: **54 formats are reported now and 40 miss a required feature.**

**The proof.** `v0-vertex-formats-16` is a new case carrying the fifteen rows
through the same probe (the runner takes a table now), and every row read back
8294400 of 8294400 pixels: the normalised classes 0x80/0x40/0xbf (0xc000 of 65535
is 0.50001, which the target stores as 191) and 0x80, the half-float ones the same
values, the unsigned ones the integers themselves and the signed ones plus 128
(pid 111, `Klog_Logs/v0-vertex-formats-16-run1.log`: three cases, 633 PASS
records, no FAIL; `v0-formats` 54 of 54 after the mirror moved).

**Audit movement.** The split reads **74 features on 38 rows parked on
`PsbcDescriptorType`, 6 features on 6 rows on `PsbcVertexFormat`, 12 on 2 blocked
by the hardware's fetch order, 0 on 0 blocked by the compiler fault and 0
probe-reachable**; the six vertex rows left are the byte-reversed 8888 layouts and
the 8888 SNORM/SINT/UINT pair -- round 3's mechanism. `docs/BLOCKERS.md` carries
the log.

Gates: `tools/build-psbc-ps5.sh` (the patch applied, 15 values and 15 cases),
`tools/build-driver.sh`, `tools/check-driver.sh`, `tools/check-runner-cases.sh`,
`make test` (30 tests), `make lint`, `tools/check-mip-layout.sh`,
`tools/build-all-titles.sh` and the three audits (command 137: 90 driver / 0
refused / 47 runtime / 0 gap; limits 106/97/0; format 54 reported / 40 missing /
55 conditional) are green on this commit.

## 2026-09-20 — blocker round 3: the 8888 SNORM, SINT and UINT layouts, and the vertex enum closed (pid 112; `jobs/v0-vertex-formats-8888`)

The last six rows the vertex enum parked: `R8G8B8A8_SNORM`, `R8G8B8A8_SINT`,
`R8G8B8A8_UINT` and their `A8B8G8R8_*_PACK32` twins. Three enum values close all
six, because Vulkan's packed `A8B8G8R8_*_PACK32` names the same memory order as
its `R8G8B8A8_*` twin -- the packed form counts its bytes from the most
significant end, which round 3's UNORM pair had already shown -- so one pipe
format serves both rows and the case proves the pair with the same vertex word.
The hardware's words are 57 (`8_8_8_8_SNORM`), 61 (`_SINT`) and 60 (`_UINT`).

**The reading, and one expectation of the round's own that the console
corrected.** `v0-vertex-formats-8888`'s six rows all read back 8294400 of 8294400
pixels: the SNORM pair 0x40/0x81/0xc1 (0x20, 0x40 and 0x60 of 127 are 0.252,
0.504 and 0.756), the UINT pair 0x40/0x80/0xc0, and the SINT pair
0xa0/0xc0/0xe0/0xff. The first run failed the SINT pair with 0 of 8294400
because the row's expectation had its first two channels transposed -- the
console's answer, 160, 192, 224, was exactly the byte order the word carries plus
the signed set's 128, so the fetch was right and the case's arithmetic was not
(pid 112, `Klog_Logs/v0-vertex-formats-8888-run1.log`, `-run2.log`: three cases,
327 PASS records, no FAIL).

**The vertex blocker is closed.** The split reads **74 features on 38 rows parked
on `PsbcDescriptorType`, 0 features on 0 rows on `PsbcVertexFormat`, 12 on 2
blocked by the hardware's fixed fetch order, 0 on 0 blocked by the compiler fault
and 0 probe-reachable**; 54 formats reported, 40 missing, every remaining row
quoted against the descriptor enum or the fetch order.
`tests/test_tools.py`'s `parked_on_vertex` set is empty with the reason beside it,
and `docs/BLOCKERS.md` records the three rounds that emptied it: every layout the
required formats name is now expressible, because the hardware fetches them (the
GFX10 words KytyPS5's `BufferFormat` numbers) and the compiler's enum names their
pipe formats.

Gates: `tools/build-psbc-ps5.sh` (26 values and 26 cases in the table),
`tools/build-driver.sh`, `tools/check-driver.sh`, `tools/check-runner-cases.sh`,
`make test` (30 tests), `make lint`, `tools/check-mip-layout.sh`,
`tools/build-all-titles.sh` and the three audits are green on this commit.

## 2026-09-20 — blocker round 4 groundwork: the texel-buffer descriptor mechanism (no claims)

The descriptor blocker's first mechanism, landed as plumbing: the compiler half
and the driver half, with the probe and the claims left to the next round because
nothing is reported before a console run proves it.

**What the compiler needed was small, because it is already RADV's.** The
compiler builds a real `radv_descriptor_set_layout` from the caller's
`PsbcDescriptorBinding` list and maps each entry onto a Vulkan descriptor type
(`UNIFORM_BUFFER`, `STORAGE_BUFFER`, else `COMBINED_IMAGE_SAMPLER`), so a texel
buffer is an enum value and its type: `tooling/psbc/patch-descriptor-types.py`
adds `PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER` and `_STORAGE_TEXEL_BUFFER`, their
`VK_DESCRIPTOR_TYPE_*` arms in the layout's type expression and their names in
the binding validation, in the same idempotent, anchor-checked style as the
vertex-format patch (which now lives in the same build script).

**What the driver needed was the V# encoding.** A texel buffer's entry is a
buffer descriptor (16 bytes, the stride the buffer types already use), whose
word 3 carries the *fetch format*:

```
word 0: the view's buffer address low
word 1: address high | (element stride << 16)
word 2: the view's range in elements
word 3: 0xfac | (GFX10 format word << 12) | ADD_TID_ENABLE
```

The format word is the register database's combined GFX10 numbering
(`S_008F0C_FORMAT_GFX10(x) = x << 12`, `amdgfxregs.h`) -- the same numbering this
repository's `ps5vk_formats` fetch words already use (7 for 16_UNORM, 22 for
32_FLOAT, 56 for 8_8_8_8_UNORM, 75 for 32_32_32_32_UINT) -- and `ADD_TID_ENABLE`
(bit 23) is what makes the fetch add the thread's index times the stride, exactly
as Mesa's `ac_build_buffer_descriptor` does for a typed buffer. So
`ps5vk_descriptor_stride` gives the two types 16 bytes,
`ps5vk_descriptor_set_write` records the write's `pTexelBufferView`, and the
draw's table writer emits the words above from the view's buffer, offset, range
and format. Nothing claims a feature yet: the format entries still report no
texel-buffer bit, `vkCreateBufferView` still refuses by name for want of one, and
the audit's 74 parked features are unchanged.

**Next round** is the probe that turns this into claims: a `samplerBuffer` fetch
shader (a `texelFetch` per texel, one row a format) and an `imageBuffer`
load/store one, a case each with the formats' fetch words as the only variable,
the bits claimed in the same commit as the battery, and the audit and mirror
moved with the result.

Gates: `tools/build-psbc-ps5.sh` (the descriptor patch applied: 2 enum values, 1
type mapping, 1 validation), `tools/build-driver.sh`, `tools/check-driver.sh`,
`tools/check-runner-cases.sh`, `make test`, `make lint`,
`tools/check-mip-layout.sh`, `tools/build-all-titles.sh` and the three audits are
green on this commit; no format's reported features moved.

## Blocker round 5: the uniform texel buffer, proved (pid 130)

The descriptor blocker's first mechanism, finished. Round 4 left the machinery in
place and nothing reported; this round is the probe that proves it, and the two
failures it took to get there -- both of them the driver's, not the hardware's.

**The probe.** Three sets, one per sampler class: `probes/v0-texel-buffer`
(`samplerBuffer`), `probes/v0-texel-buffer-uint` (`usamplerBuffer`) and
`probes/v0-texel-buffer-sint` (`isamplerBuffer`), each a full-target triangle
whose pixel stage fetches from a buffer view at set 0, binding 0. The buffer
holds four texels and the shader fetches the one its fragment's x lands in
(`int(gl_FragCoord.x) / 960`, a quarter of the target's width), so a fetch that
ignores the index, reads the wrong stride or runs off the end of the view shows
as a failed quarter. The row tables are `texel_buffer_{float,uint,sint}_formats`
in `src/diagnostics.cpp`: 16, 11 and 10 rows, one per format the audit parks
`VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT` on, each row carrying the class its
texel encoding belongs to and the channels it has.

The shader writes three signals, which is what made the two failures readable:
red is the quarter's own element's first component, green is element 0's first
component fetched with a **constant** index, and blue is a constant. A black
frame with the right blue is a fetch that returned nothing; a right blue with a
red of the first level everywhere is an index the shader computed wrongly; an
all-zero frame is a frame that drew nothing.

**Failure 1: `ADD_TID_ENABLE`.** Round 4's V# writer set bit 23 because
`ac_build_buffer_descriptor` sets it for some buffer descriptors. It is what a
*scratch* buffer's descriptor uses (ACO's `p_init_scratch`: `add_tid = true`,
`index_stride = 2`), not what a texel buffer's is: the hardware then adds the
thread's id to the element index, which walks a four-texel view's fetch off its
end and returns zero. The reference is RADV's own
`radv_make_texel_buffer_descriptor`: word 3 is the format entry's `DST_SEL`
selectors, `S_008F0C_FORMAT_GFX10` of its format word, `OOB_SELECT` 0 and
`S_008F0C_RESOURCE_LEVEL` 1 -- and **no** `ADD_TID_ENABLE`. Both console runs
before this round measured it: `Klog_Logs/v0-texel-buffer-run1.log` (pid 129,
`ADD_TID_ENABLE` set) and `Klog_Logs/v0-texel-buffer-run2.log` (pid 129, clear
but see failure 2), every row 0 of 38400 pixels and every quarter's middle pixel
zero.

**Failure 2: the table's size did not count the texel-buffer entry.** The
validation loop that sizes a stage's set-0 table ended with
`table_bytes = MAX2(table_bytes, offset + array_size * stride)`, and the
texel-buffer arm of its type checks `continue`d past that line -- so a stage
whose only binding was a texel buffer reserved `ALIGN_POT(0, ...)`, and the entry
the writer put there was written past what the draw had reserved and shared an
address with the next stage's table. Moving the accounting to the top of the
loop, before any check can skip it, is the whole fix
(`driver/ps5vk_draw.c`). The second failure's own evidence is the run's V# dump
(`agc_texel_buffer_table`), which is now part of the case's log: the words were
already the ones designed (`0x0002c000 0x00010002 0x00000004 0x1001204` for
`R8_UNORM`), and the frame still fetched nothing.

**The result.** `v0-formats-texel-buffer` 16 of 16 rows, `-uint` 11 of 11 and
`-sint` 10 of 10, every row 38400 of 38400 checked pixels of its three signals
(`Klog_Logs/v0-texel-buffer-run3.log`, pid 130); `v0-formats` 54 of 54 after the
mirror moved; `m2-solid`; 1411 PASS records and no FAIL. The driver claims
`VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT` on the 37 rows that wanted it, so 18
rows left the audit's missing list (40 -> 22, 74 -> 37 parked features, 38 -> 20
rows) and `docs/V0_FORMATS_AUDIT.md`'s split line and tables moved with them.

**The gates this round adds.** `driver/tests/vk_v0_texel_buffer_test.c` draws the
same program on the PC and reads the table chunk back through
`ps5vk_debug_table_chunks`: the V# must name a live bound buffer, stride 4, count
4, the format entry's `DST_SEL`, format word 56 and `RESOURCE_LEVEL`, and must not
set `ADD_TID_ENABLE` -- the one part of the path a console frame cannot show the
words of, run by `tools/check-driver.sh` against a replay of the console run
(`golden/v0-texel-buffer`, capture pid 132) so the PC has the console's register
defaults. `vk_b2_buffer_view_test.c` now checks both sides of the reporting rule
(a claimed format's view is created; `R32_SFLOAT`, whose buffer features are
empty, is still refused by name), `vk_b3_image_test.c` and
`vk_b6_pipeline_test.c` carry the new bits, and `tests/test_tools.py` recounts
the moved split (22 rows in the missing list, `UNIFORM_TEXEL_BUFFER` out of the
parked set, the quote now about the storage family).

## Blocker round 6: the storage texel buffer, proved (pid 135)

The descriptor blocker's second mechanism. Its compiler half was already in place
-- `PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER` and its CLI name came with round 4 --
and a `writeonly imageBuffer` at set 0, binding 0 compiles to
`buffer_store_format_xyzw` against the same 16-byte entry the uniform fetch loads
(`PSBC_DEBUG_DISASM`, one `s_load_dwordx4` from the set pointer at offset 0). The
driver needed **no new descriptor code**: one V# serves both types, the layout
gives `VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER` the same 16-byte stride, and
`vkCreateBufferView` already picks the bit a buffer's usage asks for. What the
round added is the probe and the claims.

**The probe.** `probes/v0-texel-buffer-store` (and `-uint`, `-sint`) draw the same
full-target triangle and *store* one level into the texel the fragment's quarter
names, then the case reads the buffer's own memory back through the mapping the
harness made: every texel must hold its level in the format's own encoding -- the
bytes the fetch probe's rows upload, from the same encoder -- so a store that
ignores the index, writes the wrong stride or runs off the view's end leaves a
texel at the zero it started from. The frame's own readback carries the level in
red and 0.5 and 0.25 in green and blue, so a frame that drew shows even when a
store wrote nothing. The rows are the nineteen formats the audit parks
`VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT` on: seven float and normalised ones,
six unsigned and six signed (7 + 6 + 6).

**Two runs of rounding, both on the 32-bit float rows.** The first run stored
`0x3E008080` where the case expected `0x3E008081` for level 0x20: the shader's
decimal literal was a digit short of 32/255, so it parsed to one ULP below the
correctly rounded value, and a 32-bit float texel keeps that (`run1`). Replacing
the literal with a division, `float(level) / 255.0`, moved the error instead of
removing it: the compiler turns the division into a multiply by the float32
reciprocal, which lands one ULP *above* for 96 and 127 (`run2`, texels 2 and 3 of
both 32-bit float rows). The shader now carries correctly rounded decimal
literals for all four levels, which parse to exactly the float32 the case's own
`(float)level / 255.0f` produces. The 16-bit float row is insensitive to that
ULP: the four levels' half encodings agree either way, which is why it passed all
three runs.

**The result.** `v0-formats-texel-buffer-store` 7 of 7 rows,
`-uint` 6 of 6 and `-sint` 6 of 6, every row four of four texels byte-exact
(`Klog_Logs/v0-texel-buffer-store-run3.log`, pid 135); `v0-formats` 54 of 54 after
the mirror moved; `m2-solid`; 818 PASS records and no FAIL. The driver claims
`VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT` on the nineteen rows that wanted it,
so four rows left the audit's missing list (22 -> 18, 37 -> 18 parked features,
20 -> 16 rows).

**The gates this round moves.** `driver/tests/vk_v0_texel_buffer_test.c` now draws
*both* halves -- the uniform fetch and the storage store, each with its own probe
set -- and asserts the same V# shape for each (stride four, count four, the
entry's `DST_SEL`, format word 56, `RESOURCE_LEVEL`, no `ADD_TID_ENABLE`, a live
bound buffer address), which is also what checks that a storage usage gets past
`vkCreateBufferView`'s reporting rule. `tests/test_tools.py` recounts the moved
split (18 rows in the missing list, `STORAGE_TEXEL_BUFFER` out of the parked set)
and `vk_b3_image_test.c`'s expected feature words carry the new bit through the
audit's own table. `make test`, `make lint` and `tools/check-driver.sh
v0_texel_buffer` are green on this commit; the full driver, runner-case and
title gates could not run when it was written, because the SDK this repository
pins was replaced mid-round by the 0.3.0 release -- whose layout is not the one
`tools/*.sh` expect -- and running them needs the SDK-tree adapter the next round
adds (docs/BLOCKERS.md, the SDK section).

## Blocker round 7, groundwork: the SDK tree adapter (0.3.0, without moving the compiler)

The pinned `ps5-opengl-sdk-0.2.0` was replaced by 0.3.0 mid-round-6, whose release
layout (`sdk/`, `sources/*.tar`, `ps5-opengl/`) is not the layout `tools/*.sh`
read, so the full driver, runner-case and title gates could not run. This lands
the adapter, and it keeps the compiler **frozen**: the tree's
`third_party/opengnm-psbc` is copied from the work copy
`tools/build-psbc-ps5.sh` last built -- the 0.2.0-era fork with
`tooling/psbc/patch-*.py` applied -- so nothing a proof depends on moves. The
compiler archive that comes out of it is byte-for-byte the one every probe
package was built with (`484fe8ed6365948de6927c5d011b10ccd618fc5b55886145125959cf5f873c38`,
the digest the earlier builds printed), which the host gates then confirm
against the committed packages.

- `tools/adapt-opengl-sdk.sh` writes the pinned layout to
  `.deps/native/opengl-sdk` from a ps5-opengl **checkout** (a clone of the
  repository, at `../ps5-opengl-sdk-0.3.0` here, revision `6cb291a`) or from a
  release bundle: the frozen compiler tree, its include dependencies, the PS5
  makefiles and platform sources from the work copy, and the package writer,
  host build configuration and `dependencies.json` from the checkout. It builds
  the host compiler (`libpsbc.a`, `opengnm-psbc`) into the tree, and writes
  `PROVENANCE.txt`, `SHA256SUMS` and a `psbc_compat` identity into
  `dependencies.json`.
- `tools/sdk-root.sh` resolves which tree the tools read: `PS5_OPENGL_SDK`,
  else the adapted tree, else the pinned sibling; the eleven scripts that read
  the SDK source it now, so the resolution lives in one place.
- `tooling/sdk/fetch-sources.py` is the verifier the build calls in place of
  ps5-opengl's own: it recomputes the frozen compiler tree's hash and fails
  loudly when its contents moved, which is the same guard the pinned revision
  check gave.
- Two smaller fixes the adapter surfaced: `patch-compute-metadata.py` is
  idempotent now (the frozen tree already carries its fields, so a second run
  must skip rather than duplicate them), and `build-psbc-ps5.sh`'s sysctl-length
  check accepts the file both ways -- the two `int` sites it fixes in a
  downloaded tree (three sites in that file, one already `size_t`) or the three
  `size_t` sites of a tree that has been fixed.
- Two round-6 expectations had never run under the full gate, because the SDK
  went missing in that round: `vk_b3_image_test.c` and
  `vk_b6_pipeline_test.c` now expect `STORAGE_TEXEL_BUFFER` where round 6
  claimed it (R8G8B8A8_UNORM, R32G32_SFLOAT and R32G32B32A32_SFLOAT).

Gates, all green on this commit: `tools/build-psbc-ps5.sh` (identity verified,
same archive digests), `tools/build-psbc-cli.sh`, `tools/build-driver.sh`,
`make test` (30), `tools/check-driver.sh` (loader, direct, PS5 link and the
negatives, with every golden comparison against the committed packages),
`tools/check-runner-cases.sh` (202 PASS), `make lint` (189 files),
`tools/check-psbc-link.sh`, `tools/check-vulkan-runtime.sh`,
`tools/check-mip-layout.sh` and `tools/build-all-titles.sh` (five titles, no
warnings). The runner title's ELF digest moves (`221a76405e03fa2b` ->
`882482fc1b6271a5`) because the linked archive's debug information records a
different SDK path; its behaviour is what the driver and runner gates compare.

Next, and the reason the adapter was worth building this way: the storage image
(the remaining 18 features on 16 rows) as a normal round, with 0.3.0's
`PSBC_DESCRIPTOR_STORAGE_IMAGE` and its console-validated 8-word descriptor as
the encoding reference, added to the frozen tree by a patch script in the
rounds 4-6 style -- the fork's own compiler stays for the round that decides
whether its ACO changes fix the SIGFPE.

## Blocker round 7, groundwork: the storage image's descriptor path (no claims)

The third mechanism's plumbing, landed the way rounds 4 and 6 landed theirs:
everything but the probe and the claims, with nothing advertised.

**The compiler.** `tooling/psbc/patch-descriptor-types.py` now owns all three
types the fork cannot name -- the two texel buffers (16-byte entries) and the
storage image (32) -- and rewrites what they appear in *canonically* rather than
appending arms: the enum values, the CLI names, and the whole of the validation's
type list, its expected stride and the layout's type mapping, built from one
table. Two scripts appending to the same expression fail each other's anchors,
which is exactly what the first attempt did (the storage image ran first and the
texel-buffer validation anchor was gone); a canonical rewrite is idempotent
whatever the tree already carries, which matters because the reconstructed SDK
tree is a copy of a work copy this script has run on. The type, its 32-byte
stride and its `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` mapping are ps5-opengl 0.3.0's
own fork (`libpsbc/psbc_compile.c` there).

**The driver.** `PS5VK_STORAGE_IMAGE_DESCRIPTOR_BYTES` is 32 and
`ps5vk_descriptor_stride` returns it; a storage image's write records the view it
names and no sampler; the pipeline maps `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` onto
`PSBC_DESCRIPTOR_STORAGE_IMAGE`; and the draw validates a 32-byte entry as a
storage image and writes it as the combined image sampler's own image descriptor
-- the first eight words, the sampler's three left out -- which is the same shape
ps5-opengl's console-validated `ps5_storage_image_view_descriptor` builds. The
sampler in `ps5vk_sampled_image` is optional now: a storage image's write names
none, and everything else about the validator (format word, DST_SEL, levels,
layers, the 256-byte-row rule) is the sampled path's.

**One gate earned its keep.** `tools/check-fragment-inputs.sh`, which
`tools/check-driver.sh` runs, compiles a `sampler2D` shader: the first canonical
stride statement dropped the fork's own combined sampler's 48-byte arm (the
table only listed the types this repository adds), so a 48-byte entry was
suddenly invalid and the check failed with `result=6, count=0`. The stride list
leads with the combined sampler now, and the check passes on both varyings.

Gates, all green and nothing reported: `tools/build-psbc-ps5.sh` (identity
verified), `tools/build-driver.sh`, `tools/check-fragment-inputs.sh`, `make test`
(30), `tools/check-driver.sh` (loader, direct, PS5 link, negatives and every
golden comparison against the committed packages), `make lint` (189 files) and
`tools/build-all-titles.sh` (five titles, no warnings). Next: the probe -- an
`imageStore` into a row-layout image the case reads back, one row a format --
the claims in the same commit as that battery, and the audit and mirror moved
with them.

## Blocker round 7: the storage image, proved (pid 161)

The descriptor blocker's third mechanism, and the last one whose type the fork
could not express. The compiler half is `tooling/psbc/patch-descriptor-types.py`'s
third table row -- `PSBC_DESCRIPTOR_STORAGE_IMAGE`, its 32-byte entry and its
`VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` mapping, taken from ps5-opengl 0.3.0's own
fork -- and the driver half is the entry, the write record, the pipeline mapping,
the parser and the 32-byte writer, all sharing the sampled path's image
descriptor: words 0-5 with the sampler's words 6-10 left out, which is the shape
ps5-opengl's console-validated `ps5_storage_image_view_descriptor` builds.

**The probe.** `probes/v0-image-store` (with its `-uint` and `-sint` twins) draws
the full-target triangle and stores through a `writeonly image2D` at set 0,
binding 0: the fragment's quarter writes its own level into texel (quarter, 0) and
the next quarter's into texel (quarter, 1) of a 64x2 row-layout image, so the two
rows hold the same four levels in different places. The case then reads the
image's own memory back -- row 1 at the padded stride the driver stores it with --
and compares every texel with its level in the format's own encoding, the bytes
the fetch and store probes' rows use. A descriptor whose row pitch, format word or
`DST_SEL` selectors are wrong puts a store in the wrong texel, row or channel. The
rows are the sixteen formats the audit parks
`VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` on: six float and normalised, five unsigned
and five signed.

**The first run's two expectation bugs.** Six of sixteen rows passed. Both causes
were the case's, not the hardware's: its readback used a fixed 256-byte row for
every format, so an eight- or sixteen-byte texel's row 1 was read out of row 0
(`Klog_Logs/v0-storage-image-run1.log`), and the float shader computed
`float(level) / 255.0`, which the compiler turns into a multiply by the float32
reciprocal -- the same trap round 6 measured for the 32-bit float store rows,
which lands one ULP high for 96 and 127. The row stride is
`storage_image_row_bytes(row)` now (the driver's 256-byte row rule) and the float
shader carries correctly rounded literals, the fix round 6 found.

**The result.** `v0-formats-storage-image` 6 of 6 rows, `-uint` 5 of 5 and
`-sint` 5 of 5, every row eight of eight texels byte-exact over the image's two
rows (`Klog_Logs/v0-storage-image-run2.log`, pid 161); `v0-formats` 54 of 54 after
the mirror moved; `m2-solid`; 713 PASS records and no FAIL. The driver claims
`VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` on the sixteen rows that wanted it, so
fourteen rows left the audit's missing list (18 -> 4, 18 -> 2 parked features, 16
-> 2 parked rows) -- what is left there is the two sRGB rows the hardware's fixed
fetch order blocks and `R32_SINT`/`R32_UINT`'s storage image *atomics*, for which
no public implementation records an instruction.

**The gates this round adds or moves.**
`driver/tests/vk_v0_texel_buffer_test.c` now draws all three mechanisms -- the
uniform fetch, the storage store and the storage image -- and asserts the storage
image's eight descriptor words on the PC: the format word at bits 20-26, the low
width bits at 30-31, the width field, the entry's `DST_SEL`, the untiled 2D kind,
the single-level word and words 6-7 clear, which is what checks that a storage
image's entry is the image descriptor without the sampler. `vk_b3_image_test.c`
carries the new optimal-tiling bit, and its "storage usage is refused" case moved
to a format without the bit now that R8G8B8A8_UNORM reports it;
`tests/test_tools.py` recounts the moved split (4 rows in the missing list, the
parked set down to the atomics, the parked quote about them). Gates, all green:
`tools/build-psbc-ps5.sh`, `tools/build-psbc-cli.sh`, `tools/build-driver.sh`,
`make test` (30), `tools/check-driver.sh` (loader, direct, PS5 link, negatives,
goldens), `tools/check-runner-cases.sh`, `make lint` and
`tools/build-all-titles.sh`.

## Blocker round 8: ACO stability -- the three shapes, re-measured

The ACO objective has three recorded shapes. Two were already measured clean in
earlier rounds and are quoted rather than re-run; the third reproduced this round,
twice and differently, and is now isolated to the console's compiler build.

**The depth-descriptor compile is clean.** The shape -- a shader whose descriptors
name a depth image -- was never reached before round 17 removed the `vkCreateImage`
assert, which is what the aborts had been. Round 21 then compiled *and ran* it:
`v0-formats-sampled-depth` sampled `D16_UNORM` and `D32_SFLOAT` through the
m3-texture set, 2 of 2 formats, the 0.5 depth fetched as `0x80` in red
(Klog_Logs/v0-depth-pair-run4.log, pid 126), and `c7-blit-depth` blitted both
(1024 of 1024 destination texels). So the depth shape is not a compiler fault at
all: it was the driver's assert, and the driver refuses an unsupported image by
name now.

**The signed shader's second compile is clean.** Round 4 recorded the
signed-integer pixel shader's *second* compile aborting in ACO's register
allocator, and the signed batteries were split because of it. They are not split
now: `v0-targets-sint` compiled and drew **all ten signed rows in one process**
(round 18, pid 110, `Klog_Logs/v0-sint-run1.log`), `v0-formats-sampled-sint` runs
its 11 rows in one case, and the three signed descriptor probes (texel-buffer
`sint`, storage-image `sint`) all compile and pass in the batteries of rounds 5 to
7. Nothing in the current driver or compiler shows that shape.

**The compile-order fault reproduces, and it is the console build's.** The
recorded SIGFPE in `aco::schedule_program` "after the unsigned texture case's
compile" was probed with a bounded queue of exactly that shape
(`jobs/aco-state/queue.txt`): every row of `v0-formats-sampled-uint` passed and the
process then died with signal 8 (integer divide fault, `rip 0x430d54`) before any
later case's first frame, the battery's retries dying with SIGSEGV instead. The
same case *alone* (`jobs/aco-state/queue-unsigned-alone.txt`) died earlier and
differently -- signal 11, a write to an unmapped stack page during the case's
setup, before any frame -- so the fault is not a deterministic function of the
shader sequence. The host does not show it: `tools/check-aco-state.sh` compiles
the recorded sequence and then every probe set's pixel stage three times over
(**105 compiles**) in one process against the same compiler archive the driver
links, with `result=0` for every one. The console's compiler build is the
difference, and the next step is the 0.3.0 fork's ACO, whose `num_waves == 0`
guard is of this fault's class (docs/BLOCKERS.md, the SDK section). The full
finding, with both logs and the addresses, is in docs/HARDWARE_FINDINGS.md.

**The round's gates.** `tooling/psbc/compile-sequence.c` is a new diagnostic: it
compiles a queue of shaders in one process with the probes' options and prints
each step before the next, so a fault names the pair of compiles in flight.
`tools/check-aco-state.sh` builds it against the work copy's host archive and runs
the two sequences above; it is part of `tools/check-driver.sh` now, so the host
half of the ACO question is a gate. Nothing this round touches a driver path, so
no row's claim moves: the split stays at 2 features on 2 rows parked, 12 on 2
blocked by the hardware, 0 compiler-blocked and 0 probe-reachable, and the format
audit at 54 reported and 4 missing a required feature.

## Blocker round 9: the storage image atomics, proved -- the descriptor blocker closed

The last two parked features. The round turned out to need no compiler and no
driver change at all: `imageAtomicAdd` already lowers -- `PSBC_DEBUG_DISASM` shows
`image_atomic_add %s[0-7], s4, v[addr], v[data], v[result] dmask:x 2d unrm
storage:image semantics:volatile,atomic,rmw` against the same 32-byte storage
image entry round 7 proved -- so what was missing was the probe and the claim.

**The probe.** `probes/v0-image-atomic` (with its `-sint` twin) draws the
full-target triangle over a 64x2 `R32_UINT`/`R32_SINT` storage image. The case
zeroes the two rows' first four texels in the image's own memory before the draw,
and every fragment of the frame adds its quarter's texel: one to texel
(quarter, 0) and two to texel (quarter, 1). Every pixel of the target is covered
exactly once by the fullscreen triangle, so each texel's count is the quarter's
960x2160 pixels times its addend, and an atomic add's sum is order-independent: a
lost update leaves the texel short, which is the shape a read-modify-write
instruction would produce under millions of concurrent writers. The case reads the
counts back out of the same mapping (row 1 at the padded row stride the driver
stores it with) and compares each with the count it must have accumulated.

**Why the atomics did not need a new descriptor.** The atomic reads the same
storage image SRD an `imageStore` does: its format word tells the hardware the
data format, the kind tells it the image is the untied 2D row layout, and `dmask:x`
selects the single 32-bit channel the atomic returns and updates. That is why the
round is small -- and it is the last thing the descriptor blocker needed.

**The result.** `v0-formats-storage-image-atomic` 1 of 1 and `-sint` 1 of 1, every
one of the eight texels holding its quarter's count exactly -- 2073600 in row 0 and
4150720 in row 1, the quarter's 960x2160 pixels times the row's addend
(`Klog_Logs/v0-storage-image-atomic-run2.log`, pid 186); `v0-formats` 54 of 54 and
`m2-solid`; 208 PASS records, no FAIL. The first run passed 0 of 1 for a small
reason worth recording: the atomic shader exported a constant colour where every
storage image probe's frame check expects the quarter's level in red, so the
frame was refused before the count was read; the shader exports the levels now,
the same export the store probe uses. The driver claims
`VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT` on `R32_UINT` and `R32_SINT`, so the
audit's missing list is down to **two formats**: `A8B8G8R8_SRGB_PACK32` and
`B8G8R8A8_SRGB`, the two rows the hardware's fixed sRGB fetch order blocks. The
split's `PsbcDescriptorType` class reads **0 features on 0 rows** -- the blocker
the workstream opened with is closed, from 74 parked features on 38 rows to none.

**The gates.** `driver/tests/vk_v0_texel_buffer_test.c` draws a fourth program,
the atomic set, and asserts the same storage image descriptor words for it, so all
four descriptor paths have a host gate. `tests/test_tools.py`'s parked set is
empty now, and the check that used to forbid a parked family's bit is replaced by
the count each battery proved -- 37 uniform texel buffers, 19 storage texel
buffers, 16 storage images, 2 storage image atomics -- so a claim on one format
too many, or a dropped one, fails the suite. `make test` (30),
`tools/check-driver.sh` (with the ACO state probe and the new fourth program),
`make lint` and `tools/build-all-titles.sh` are green on this commit.

## 2026-09-20 - blocker round 11: the audit's conditional cells, and the clause they hid

R1's report (a PS5 vkQuake port's `PS5_VULKAN_REQUESTS.md`) found a hole in the
audit rather than in the driver, and the hole is worth the round because it made a
false claim unfalsifiable: `tools/format_audit.py`'s loop read a mandatory table
cell's marker, and for anything that was not `{sym1}` it appended the feature to a
`conditional` list **without ever consulting the driver's table**. That is right
for a cell like `{sym3}`'s "if the extension is enabled", which is a caveat, but
wrong for a footnote clause: the depth/stencil table's second row group says
`VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT` **must: be supported for at least
one of** `D24_UNORM_S8_UINT` and `D32_SFLOAT_S8_UINT`, and every one of those two
cells is the same `{sym2}`. The requirement is over the *set*: it holds if either
format reports the bit. The driver reports it for neither, and the audit could not
see that at all -- the row appeared as a conditional requirement, `--check` stayed
silent about it, and the summary line said **0 features on 0 rows are
probe-reachable work**, which is why the rung read as closed. The first clause of
the same footnote (at least one of `X8_D24_UNORM_PACK32` and `D32_SFLOAT`) was
satisfied all along by `D32_SFLOAT`, which is what makes the pair a clean test of
the mechanism: one clause met, one violated, both invisible.

**The fix is the mechanism, not the row.** `tools/format_audit.py` now reads the
tables' footnote rows whole (`footnote_paragraphs`: a `NN+|` row and the unmarked
lines that continue it), splits each paragraph at `must: be supported for at least
one of`, carries the feature a `, and must:` continuation inherits, and returns
`(feature, [format])` pairs (`required_clauses`). Each clause is evaluated against
the driver's whole table: met when any named format reports the feature, unmet
otherwise. Unmet clauses are printed as their own section -- one row per format
the clause names, then the clause itself -- and `--check` exits 1 on them exactly
as on a missing `{sym1}` feature. Both clauses of the depth/stencil footnote parse
(verified: `DEPTH_STENCIL_ATTACHMENT_BIT` -> `[X8_D24_UNORM_PACK32, D32_SFLOAT]`
met, `-> [D24_UNORM_S8_UINT, D32_SFLOAT_S8_UINT]` unmet), and the constraint the
`{sym2}` comment in the suite always had -- "the driver's format reporting and the
audit must move together" -- now has a gate over set-level requirements too.

**What it discloses.** The audit's counts gain two rows: 1 `must:` clause no
reported format satisfies, 2 formats named by one. The row-by-row list gains the
clause's two rows in their own table, and the split line is corrected from **0
features on 0 rows probe-reachable** to **2 features on 2 rows**: the depth/stencil
clause is the only requirement in the specification that this device violates and
a probe can close. Nothing else moved -- 179 required, 54 reported, 2 missing
(the two sRGB rows), 55 conditional -- and the parked classes stay empty. The
suite no longer asserts the probe-reachable count is zero; it asserts the counts
against the row table (so the clause's rows have to be listed, and a new unmet
clause cannot hide) and keeps the compiler-fault class at zero.

**The result.** `python3 tools/format_audit.py --check` exits 1, printing the
unmet clause and its two rows; `python3 tools/format_audit.py` prints the three
counts, the missing list, the unmet clause and the conditional list; `make test`
(30) is green with the new counts, the clause's rows parsed out of the audit and
compared with the document, and the split's corrected arithmetic.
`docs/M5_PHASE_C.md` (this entry), `docs/V0_FORMATS_AUDIT.md` (counts, the family
table's depth/stencil row, the clause's table and the corrected split) and
`docs/BLOCKERS.md` (mechanism rounds 11 and 12) record it. Round 12 is the driver
half R1 also asked for: the stencil path, whose register reference is ps5-opengl
0.3.0's `append_depth_target_state`.

## 2026-09-20 - blocker round 12 groundwork: the stencil path's machinery, no claims

R1's report asked for two things and round 11 answered the tooling half. This is
the driver half's machinery, landed without a claim because the two formats'
entries arrive with the console probe that proves them (the precedent is round
4's texel-buffer groundwork).

**The layout.** A depth format that carries a stencil gets a second plane, and
the derivation is AddrLib's own: `ac_surface.c`'s `gfx9_compute_surface` computes
the depth miptree first and then, for the stencil, sets `flags.stencil = 1`,
`bpp = 8` and computes a second miptree whose base is
`align(surf_size, baseAlign)` in the same allocation -- one byte a texel, in the
swizzle mode the depth surface asked for, which is 64 KiB Z_X. So the plane is a
256x256-texel 64 KiB tile grid (`tools/mip-layout-oracle swizzle 1 1 64kb_z_x`
prints that row: fifteen terms, `verified 65536`), it starts at a 64 KiB
boundary, and that is what `ps5vk_image_stencil_plane` and
`ps5vk_image_storage` place. ps5-opengl 0.3.0's runtime asks for the same 64 KiB
alignment of the separate stencil buffer its hardware-run
`append_depth_target_state` binds.

**The words.** `DB_STENCIL_INFO` (0x011) is `0x20000181` for such an attachment:
FORMAT = STENCIL_8 (1), SW_MODE = 24 (the depth surface's own 64 KiB Z_X) and
TILE_STENCIL_DISABLE (bit 29) set, the word ps5-opengl's validated path programs
for a separate stencil plane; RADV's combined-plane path leaves that bit clear
and is the candidate to fall back on if the console refuses this one. The
stencil read, write and high bases (0x013, 0x015, 0x01b, 0x01d) take the plane's
address, and a depth-only format records zeros there exactly as the console's own
recorded depth frames do -- the golden comparison is what caught the first
version of this, which left the Z plane's address in the stencil bases.

**The state.** `DB_DEPTH_CONTROL` gains STENCIL_ENABLE (bit 0), BACKFACE_ENABLE
(bit 7) and the two faces' STENCILFUNC (bits 8-10 and 20-22); three more
registers carry the six face operations (DB_STENCIL_CONTROL, 0x10b) and each
face's reference, compare mask, write mask and op value (DB_STENCILREFMASK
0x10c, _BF 0x10d). Every one of them is RADV's own emission
(`radv_emit_depth_stencil_state`) with its op translation: Vulkan's order is not
the register's, so REPLACE is the register's STENCIL_REPLACE_TEST (3) and the
clamping and wrapping operations are 5, 6, 8 and 9. The pipeline's static
stencil state lands in the dynamic state the draw reads, per face, each member
static unless the pipeline declares that state dynamic -- and the dynamic
stencil setters are accepted now.

**What Vulkan's ignore rule costs.** A rendering whose attachment carries no
stencil plane must program *nothing* of this: Vulkan ignores the stencil test
there. The first version guarded only the enable bits, so a pipeline with
`stencilTestEnable` and a D32_SFLOAT attachment still wrote its compare function
into `DB_DEPTH_CONTROL` (0x00200216 where the console's own frame records
0x00000016) -- the golden comparison refused it, and the fix makes every stencil
bit conditional on the bound plane. The gate is
`driver/tests/vk_c5_depth_test.c`: its frame's pipeline now enables the stencil
test against a depth-only attachment, the recorded stream must stay identical to
`golden/c5-depth`'s, and the direct build checks that not one stencil register is
recorded.

**What is not here.** No format entry yet, so no claim: `D24_UNORM_S8_UINT` and
`D32_SFLOAT_S8_UINT` land with the probe. A stencil image is refused unless it is
the single-level, single-layer, one-sample attachment the plane layout was
derived for, and a stencil aspect's transfer is still refused by name -- no
readback or upload of a stencil plane has been measured. The console battery that
proves the path is the next step.

**Gates.** `tools/build-driver.sh` (0 warnings), `tools/check-driver.sh` (the
golden comparison above and the new register check), `make test` (30),
`make lint` (191 files), `tools/check-runner-cases.sh` (202).

## 2026-09-20 - blocker round 12, the claim: the stencil path, proved

The machinery landed first (`58bfc70`, no claims). This is the part that makes a
claim true: the two formats' entries, the runner's `v0-stencil` case and the
console battery that decided it.

**The case.** One full-target quad drawn by two pipelines into a
`VK_FORMAT_D32_SFLOAT_S8_UINT` attachment. The setup pipeline has the stencil
test on with FUNC ALWAYS and PASS REPLACE, so every fragment it rasterises stores
the reference into the plane, and its fragment shader writes `gl_FragDepth` 0.75
into the depth plane; it exports red. The test pipeline has the stencil test on
with FUNC EQUAL against the frame's own reference, PASS and FAIL both KEEP, and
`gl_FragDepth` 0.25, which passes LESS against the 0.75 the setup pass wrote; it
exports green. Four frames, one signal each: the matching reference must leave
the whole target green, a reference the plane does not hold must leave it red,
a test pass whose depth is behind the plane's must leave it red, and a control
frame with no stencil test must paint -- so a green frame needs *both* planes of
a combined format to work, and a red one names which half rejected it.

**The runs, and the two bugs they found.** The first run passed 0 of 4: every
frame was red. The case's own diagnostic -- the raw bytes of the attachment's two
planes, read out of the mapped image memory after each frame -- then said the
write half was fine (the stencil plane's first 64 KiB tile held 0x5a in all 65536
bytes, and the depth plane's tile held 0.75 in all 16384 texels) while nothing
the test pass drew landed. The cause was the case's own: it drew with
`PS5VK_TRIANGLE_ONE_DRAW`, which records **one** draw, so the test pipeline never
ran at all. The second run's control frame (the same pass with no stencil test)
is what made that unmistakable -- a red control cannot be the stencil test's
doing. With `PS5VK_TRIANGLE_ONE_COMMAND_BUFFER` all four frames pass. What the
diagnostic measured on the way is worth keeping: the stencil plane is where the
AddrLib derivation places it, one byte a texel, and the hardware's own writes
land in the driver's map of it.

**The result.** `v0-stencil` 4 of 4 frames, `v0-formats` 54 -> 56 rows and
`m2-solid`, 3 of 3 tests and no FAIL (pid 192, `Klog_Logs/v0-stencil-run5.log`;
the capture run is `golden/v0-stencil` with four submissions and twelve
pipelines). The driver claims `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT`
for `D32_SFLOAT_S8_UINT` and `D24_UNORM_S8_UINT` -- the two formats the
depth/stencil table's second `must:` clause names -- and nothing else: a stencil
aspect's transfer, fetch and blit stay unproved and are refused by name, and both
formats report no buffer features. The audit's clause is satisfied, its count
table reads 56 reported and 0 clauses unmet, and the split's probe-reachable
class is **0 / 0 / 12 / 0 / 0** -- the state R1's report said the audit could not
see.

**The gates.** `driver/tests/vk_c5_stencil_test.c` is the focused host gate: the
same frame through the same harness, judged by the words the draw records
(DB_STENCIL_INFO 0x20000181, the stencil read and write bases at the plane's
address, DB_Z_INFO unchanged at D32F's measured word, DB_DEPTH_CONTROL's
STENCIL_ENABLE/BACKFACE_ENABLE and two STENCILFUNCs, DB_STENCIL_CONTROL with the
setup pass's REPLACE and the test pass's KEEPs, and both DB_STENCILREFMASK
words), 13 of 13 checks in the direct build and 4 of 4 through the loader.
`driver/tests/vk_b3_image_test.c` checks the two formats' reported bits
exactly; `golden/v0-stencil` and `tools/check-driver.sh`'s replay carry the
console run into the host run. `make test` (30), `make lint`, and
`python3 tools/format_audit.py --check` now refuses only the two sRGB rows.

## 2026-09-20 - blocker round 13: the byte-reversed sRGB fetch, and the row the curve does not block

Round 12 closed the stencil clause; this is the first of the objective's two
remaining sRGB rows, and it began by reading the measurement again rather than
the audit's summary of it. pid 160's probe is of `A8B8G8R8_SRGB_PACK32`: memory
bytes A, B, G, R, of which bytes 1 and 2 took the curve and the red byte did not
-- because the console applies the curve to the first three *fetched* components,
before any selector, and that format's first three are A, B and G.
`B8G8R8A8_SRGB`'s memory bytes are B, G, R, A, so its first three fetched
components are its whole colour triple and the `ZYXW` selectors the driver has
used for `B8G8R8A8_UNORM` since round 6 are exactly what places them. The two
rows had been quoted against the hardware as one case (docs/V0_FORMATS_AUDIT.md,
docs/BLOCKERS.md); only one of them is.

**The mechanism.** The format's entry is the register database's `8_8_8_8_SRGB`
word (130) with `PS5VK_FORMAT_SWIZZLE_ZYXW`, and the CPU paths gained their own
branches: `ps5vk_texel_to_rgba8`'s sRGB case treats `B8G8R8A8_SRGB` like the
packed form with the blue-first order, and `ps5vk_rgba8_to_texel`'s writes the
three encoded channels at bytes 2, 1 and 0. The harness's `texture_texel_bytes`
learned the format's four-byte texel -- the first console run failed before
drawing because of exactly that, which is the harness's own table and not the
driver's.

**The result.** Six of the six cases pass on the console: `v0-formats-sampled`
(the fetch and the linear filter, 26 frames), `c7-blit-formats` (54 of 54 blit
sources), `v0-blit-dst`, `v0-transfer-formats`, `v0-formats` (57 of 57 rows, the
audit mirror) and `m2-solid`, 6 of 6 tests and no FAIL (pid 194,
`Klog_Logs/v0-srgb-run2.log`). The row's own frame fetched a texel stored
0xe1, 0xbc, 0x89, 0xff as 0x40, 0x80, 0xc0 -- the same colour its
`R8G8B8A8_SRGB` twin fetches from the same red, green and blue -- so the driver
reports `SAMPLED_IMAGE`, `SAMPLED_IMAGE_FILTER_LINEAR`, both transfers and both
blit directions for it. Its colour-attachment pair is not claimed: the
`CB_COLOR0_INFO` word and the export format are their own mechanism, and the
round after this one carries them.

**The audit.** 57 formats reported, the hardware class **8 features on 2 rows**
where it was 12 on 2, and `A8B8G8R8_SRGB_PACK32` alone carries the packed
format's six. `tests/test_tools.py` recounts the split from the row table, so the
family table's `BLIT_DST` row and the closing line moved with it.

**The gates.** `driver/tests/vk_v0_formats_test.c` now frames twenty-six formats
-- the console case's own float rows, which it had been three rows behind on --
with the new row's texel bytes among them, and its frames are compared with the
refreshed `golden/v0-formats-sampled` capture (26 submissions) through the
replay `tools/check-driver.sh` builds from it. `tools/check-driver.sh`,
`make test` (30), `make lint` and `tools/check-runner-cases.sh` are green.

## 2026-09-20 - blocker round 14: the byte-reversed sRGB target -- the audit's list is one row

Round 13 gave `B8G8R8A8_SRGB` its fetch, blits and transfers and left its
colour-attachment pair, because a target is a different mechanism from a fetch:
the `CB_COLOR0_INFO` word and the export path rather than a descriptor selector.
This round carried it.

**The mechanism.** The format's colour-format row is `{10 /* 8_8_8_8 */,
6 /* SRGB */, 1 /* SWAP_ALT */, 0}`: the same data format and number type
`R8G8B8A8_SRGB`'s row uses with the component swap the swapchain's `B8G8R8A8`
byte order has carried since M2, so the hardware encodes a linear export into
bytes 2, 1 and 0. The CPU clear encode gained the matching branch
(`ps5vk_format_encode_clear`: the sRGB curve with the blue-first order), and the
format's entry claims the pair.

**The result.** `v0-targets` holds both of the row's frames on the console: the
solid frame's `0xff89bce1` (the encodings of 0x40, 0x80 and 0xc0 written B, G, R)
and the blended frame's `0xffbcbce1` (the additive sum 0x80, 0x80, 0xc0's
encodings, the same arithmetic the `R8G8B8A8_SRGB` row measured in round 8).
`v0-formats` 57 of 57 -- the audit mirror -- and `m2-solid`, 3 of 3 tests and no
FAIL (pid 195, `Klog_Logs/v0-srgb-target-run1.log`).

**What is left of the objective's format work.** One row: the packed
`A8B8G8R8_SRGB_PACK32` and its six features, quoted against the fetch order pid
160 measured. The audit's counts read 179 required, 57 reported, **1** missing a
required feature, 55 conditional, and its split 0 / 0 / 6 features on 1 row / 0 /
0 -- no parked feature, no unmet clause, no probe-reachable row, and the one
remaining row is a hardware behaviour rather than a missing driver word.

**The gates.** `driver/tests/vk_c7_clear_image_test.c` gained the clear-encode
case for the row (28 of 28 checks in both builds), and the runner's target table
is the probe the console run decides. `tools/check-driver.sh`, `make test` (30),
`make lint` and `tools/check-runner-cases.sh` are green.

## 2026-09-20 - blocker round 15: the ACO fault's mechanism, from one case and a missing guard

The objective's ACO item asks to distinguish driver-state bugs from genuine
compiler bugs and to reduce the latter to minimal cases. Round 8 had reproduced
the compile-order fault and shown the host compiling the same 105 shaders
cleanly; this round reduced it to one case and found its arithmetic in the
pinned compiler's source.

**The reduction.** `jobs/aco-min/queue.txt` queues `v0-formats-sampled-uint`
alone -- the unsigned texture case whose compile followed the first recorded
fault -- and the fault reproduces without anything after it: every row passes
(the last, `format 107`, fetches its colour), the device goes idle, and the
process takes signal 8, SIGFPE, "integer divide fault", with `rax = rcx = rdx =
0` (Klog_Logs/aco-min-run1.log, pid 211, `rip 0x4306e4`). Round 8's shape needed
a case queued behind it; one case is enough, so the fault belongs to that case's
own compiles and teardown.

**The mechanism.** The driver links the SDK's frozen `opengnm-psbc`. Its
`aco_live_var_analysis.cpp` sets `program->num_waves = 0` on the path where "this
won't compile, register pressure reduction necessary", and the ordinary path can
reach zero through `max_suitable_waves`; `get_addr_regs_from_waves` in the same
file then divides by it (`program->dev.physical_sgprs / waves`). A caller that
arrives with `waves == 0` faults exactly as measured: a divide with a zero
divisor and no memory access, which is why the signal is SIGFPE and why the
register window holds zeroes rather than an address.

**The fix is already in the fork this project cloned.** ps5-opengl 0.3.0's
`toolchain/opengnm-psbc-ps5.patch` adds the guard at that point -- `if
(!program->num_waves) { program->max_reg_demand = temp_demand; return; }`, with
the comment that fixed registers can lower occupancy below the workgroup minimum
after the initial pressure check -- and the pinned tree has no such guard. So
the fault is a genuine ACO bug of the occupancy class with a *named* fix, and the
SDK fork migration is now justified by a specific bug rather than by metadata
alone. Nothing the driver does triggers it beyond asking the compiler to compile
the shaders it must compile; no driver-side workaround is honest, and the
migration is the fix's route.

**What did not change.** No driver code, no claim, no format row: the audit still
reads 179 required, 57 reported, 1 missing a required feature, 0 clauses unmet,
and the split 0 / 0 / 6 features on 1 row / 0 / 0. The queue is a fault-capable
probe (bounded to the case plus the regression, `hold 60`, the runner's own
cleanup) and its log is the evidence above.

## 2026-09-20 - blocker round 16: the SDK fork migration's work list, verified

Round 15 identified the ACO fault as a missing guard in the pinned compiler and
named ps5-opengl 0.3.0's fork as the fix. This round assembled that fork's
compiler from the SDK's own pins and measured what the move costs, so the
migration's remaining work is a list rather than a prediction.

**The check.** `tools/check-sdk-fork-migration.sh` reads the SDK's
`dependencies.json` for its pins, fetches upstream
`PS4-OpenGNM/opengnm-psbc` at
`a92a1228ea3a64e4be9f0e61c2a65a5aa7ffed92` into `build/sdk-fork` (nothing in the
repository is touched), verifies the compiler patch's hash
(`a7c73aef3f1d51c47107976b03e68de5e99f044a97601a4adf3b89144fb2e848`), applies it,
and then runs this project's four compiler patches against the result.

**What it found.**

- The patch applies cleanly, and the assembled tree's own hash --
  `git write-tree` after `git add --all` -- is
  `a27cbecc8c11761da04af6b8e089905b93252e65`, which is the SDK's recorded
  `psbc_patch.patched_tree`: the assembly is byte-for-byte the compiler the SDK
  built, not merely a tree that accepts the patch.
- `patch-vertex-formats.py` holds unchanged (26 enum values and 26 switch cases).
- `patch-descriptor-types.py` holds unchanged (2 enum values, 3 CLI names, the
  type mapping, the validation and the entry strides) -- the fork already carries
  `PSBC_DESCRIPTOR_STORAGE_IMAGE`, so this adds the two texel-buffer types.
- `patch-fragment-inputs.py` does not hold: its anchor is reformatted in the fork
  (`expected one fragment-input anchor`).
- `patch-compute-metadata.py` does not hold and is obsolete: its anchor is
  `#define PSBC_SHADER_METADATA_VERSION 8u`, and the fork defines `14u` with the
  compute fields already in the struct.
- The fork's `PSBC_MAX_DESCRIPTOR_BINDINGS` is 128, and its metadata version is
  14.

**The work list.** Move one anchor (`patch-fragment-inputs.py`), drop one patch
(`patch-compute-metadata.py`), carry two over unchanged, and take the metadata
version from 8 to 14 -- which rebuilds every probe package, provenance file and
golden and re-runs every battery before a claim stands, as the 0.3.0 section
already says. The driver reads the metadata through the header's struct (no
hardcoded offsets), so it recompiles against the new schema; the check itself is
not a gate (it exits non-zero while the work list is non-empty) but a report, and
`make lint` covers it as an attributed tool.

**Nothing moved.** The pinned compiler is untouched (metadata version 8 and its
three descriptor types are still what the build uses), no driver code changed and
no claim moved: the audit still reads 179 required, 57 reported, 1 missing a
required feature, 0 clauses unmet, split 0 / 0 / 6 features on 1 row / 0 / 0.

## 2026-09-20 - blocker round 17: the packed sRGB row, and a format audit with no gaps

The objective's last row was `VK_FORMAT_A8B8G8R8_SRGB_PACK32`'s six features, and
the reason it was open is a property of the texture unit: the console applies the
sRGB curve to the first three *fetched* components, before any selector, and this
format's Vulkan layout is A, B, G, R -- so the curve reaches the alpha byte and
two colour bytes and the red it must linearise is the fourth, out of the curved
set for any selector or format word.

**The mechanism: the image's layout is the driver's to choose.** Every image this
driver creates is `VK_IMAGE_TILING_OPTIMAL`, so the order its texels are stored
in is the implementation's business as long as every defined operation presents
and accepts Vulkan's values. `ps5vk_format` gained a `storage_reversed` property
-- true for this one format -- and the driver stores its texels in the R, G, B, A
order its `R8G8B8A8_SRGB` twin has, swapping the four bytes at every boundary
where an application's bytes meet the image's: the row and tiled uploads
(`ps5vk_CmdCopyMemoryToImageKHR`), the readbacks
(`ps5vk_CmdCopyImageToBuffer2KHR`) and the blit paths (whose decode and encode
now treat it as the identity order). Everything inside works in the stored order,
so the descriptors are the twin's: the fetch is the register database's
`8_8_8_8_SRGB` word (130) with the straight RGBA selectors, and the colour
target's word is 8_8_8_8 with the sRGB number type and `SWAP_STD`. The clear
encode writes the same encode in the storage's order, and a readback presents the
format's own A, B, G, R.

**The result.** Seven of seven cases pass on the console, 0 FAIL: the fetch and
linear filter 27 of 27 rows (`v0-formats-sampled`), the blit source 55 of 55
(`c7-blit-formats`), the blit destination (`v0-blit-dst`), the transfer pair
(`v0-transfer-formats`), the attachment pair (`v0-targets`, both of the row's
frames), the audit mirror 58 of 58 (`v0-formats`) and `m2-solid` (pid 235,
`Klog_Logs/v0-srgb-packed-run1.log`). **`python3 tools/format_audit.py --check`
exits 0**: 179 formats required, 58 reported, **0 missing a required feature**,
55 conditional, 0 `must:` clauses unmet, and the split 0 / 0 / 0 / 0 / 0.

**The gates.** `driver/tests/vk_v0_formats_test.c` frames the console case's
twenty-seven rows and its upload check knows the storage order;
`vk_c7_clear_image_test.c` gained the row's clear case, which proves both the
encode in the storage's order and the readback's swap. `vk_c7_blit_formats_test.c`
now carries the console case's whole sampled set (55 rows, generated from
`sampled_formats()` with the count the capture's submissions are compared
against), so the two tables cannot drift again -- the refresh this round also
gave `golden/c7-blit-formats` (55 submissions) and `golden/v0-formats-sampled`
(27). `tools/check-driver.sh`, `make test` (30), `make lint` and
`tools/check-runner-cases.sh` are green.

**A note on the addresses above.** The network addresses this log recorded -- the
console's, the build host's and a neighbouring device's with its MAC -- are
replaced by placeholders (`<console-lan-ip>`, `<host-lan-ip>`, `<lan>`,
`<neighbour-lan-ip>`) when the repository was published under its new home. The
runs themselves are unchanged; only those values are.

## 2026-09-20: the compiler migration to the SDK's 0.3.0 fork

The driver now links ps5-opengl 0.3.0's own compiler instead of the frozen
0.2.0-era work copy. `tools/adapt-opengl-sdk.sh` assembles the tree through
`tooling/sdk/assemble-psbc-fork.sh`, which applies the release's
`toolchain/opengnm-psbc-ps5.patch` to the revision `dependencies.json` pins
(a92a1228), checks the result against the `psbc_patch.patched_tree` the manifest
records (a27cbecc), materializes the generated Mesa sources the standalone
Makefile omits, and writes the view; the AGC package writer comes from the
release with it, because 0.3.0's copy validates the version-14 schema the fork
reports. The host compiler, the PS5 compiler, the driver archives, the 55 titles
and all 39 probe sets plus the compute probe were rebuilt; `make test`,
`make lint`, the three requirement audits, `check-mip-layout.sh`,
`check-psbc-link.sh`, `check-vulkan-runtime.sh` and `check-runner-cases.sh` are
green.

**The compiler patches.** `patch-fragment-inputs.py`'s anchor moved to the fork's
call signature (`radv_nir_shader_info_pass` now takes a `radv_shader_stage *` and
picks the pipeline kind from the Mesa stage); `patch-compute-metadata.py` was
dropped rather than moved, because the fork's `PsbcShaderMetadata` version 14
carries every field it added (`user_sgpr_count`, the `ngg_lds_layout` trio, the
`compute_*` group, with `compute_lds_bytes` in place of `compute_lds_size`) and
the compiler programs COMPUTE_PGM_RSRC1/2 itself; `patch-vertex-formats.py`'s
anchor was not merely moved but fixed -- it was an eight-space `default:`, a
*substring* of a deeper switch's `                default:` in the fork, so it had
inserted the 26 cases into `psbc_tess_input_supported`, where no `format`
variable exists, instead of the fork's `psbc_vertex_pipe_format` function;
`patch-aco-min-waves.py` is new (below). `tools/check-sdk-fork-migration.sh` is
green for all four.

**What the driver had to change.** The fork removed the fields the old patch
added, so `driver/ps5vk_compute.c` reads the dispatch's words where the fork
reports them -- the shader register table at the relative offsets 0x212, 0x213
and 0x228 (`(register - 0xb000) / 4`, the table 0.3.0's writer reads too) -- takes
the wave size from `compute_wave_size` instead of inferring it from RSRC1's VGPR
granule, and keeps a cross-check by requiring the compiler's reported workgroup
shape to equal the module's own local size. The compute probe records the same
words the old compiler did (`rsrc1 0x602c0001`, `rsrc2 6`) and now also
`wave_size 32`, `workgroup 1 1 1` (probes/c0/resources.txt).

**The console.** The migrated build was deployed as `PPSA99988` and every case of
the golden re-capture battery passed (the packages carry one more shader-program
register, which the console reproduced exactly): `m2-solid`, `c1-triangle`,
`c2-indexed`, `c2-instancing`, `c2-staging`, `c3-quad`, `c3-uniform`, `c4-rtt`,
`c4-texture`, `c5-depth`, `c7-blit-formats`, `c7-copy`, `c7-mip-tiled`,
`c7-mip-upload`, `c8-msaa`, `c8-resolve` in `Klog_Logs/capture-migrated.log`, and
the second batch in `Klog_Logs/capture-migrated-b2.log`.

**The compile-order fault is not fixed by the fork.** `jobs/aco-min` run pid 261
(`Klog_Logs/aco-min-fixed.log`) faults exactly as before: `signal: 8 (SIGFPE)`,
`integer divide fault`, `rax = rcx = rdx = 0`, `rip = 0x430a14`. The round's
instrumentation is what makes that a finding rather than a repeat: the patch put
a witness on `get_addr_regs_from_waves`'s divide (and restored
`program->workgroup_size`'s "unknown is UINT_MAX" invariant before
`calc_min_waves`), and the witness never fired, so rounds 15-16's attribution does
not hold for this tree. The host cannot reproduce the fault in any configuration
built here, including the console's own defines (docs/HARDWARE_FINDINGS.md,
2026-09-20), so the next step is console-side instrumentation of the ACO path.

## 2026-09-20 — the ACO round's fault was the runner's zero divisor, and the migration's goldens are re-captured

The instrumentation round never ran, because the fault had no ACO in it. Reading
the crash report's own load base (`# /app0/eboot.bin`, `#  xotext:
0000000000400000:...`) puts the three "ACO frames" of every previous round in
`run_linked_agc_canary`, `main` and `_start`, and puts `rip 0x430b34` on
`div %r13d` in `run_sampled_format_frames` -- the test runner's own division of
a sampled row's packed buffer by that row's texel size. `sampled_unsigned_formats()`
declared ten rows and initialized seven, so the eighth ran `0 / 0`; the signed
table (declared eleven, seven rows) was the same fault one case later
(docs/HARDWARE_FINDINGS.md has the mechanism, the register dump that matches it,
and the retired attributions).

**The repair.** Both tables declare their seven rows, every `sampled_*` table
carries a `static_assert(sampled_formats_filled(...))` that makes a
declared-but-unfilled row a build error -- the only direction the compiler
cannot catch, since too many initializers are already an error -- and the two
cases take the table's own `size()` through `auto` instead of repeating the
count. The driver's and the runner's 32 MiB compile stacks stay as robustness;
their comments no longer claim this fault.

**Console proof.** `jobs/aco-min` (four unsigned-sample runs plus `m2-solid` and
`v0-formats` regressions) on the migrated, repaired build: pid 287,
`Klog_Logs/aco-min-tablefix.log`, seven of seven tests PASS, each
`v0-formats-sampled-uint` reporting **"7 of 7 sampled formats fetched the colour
their texel holds"**, 1581 PASS records, and the klog holds **no `signal:`
record at all**. The same case through the driver-enabled host runner
(`build/host/runner_host_driver --cases driver`) no longer takes a signal
either, which is where the reproduction was hiding: the host had only ever been
asked to compile, never to run the case.

**Batch B's goldens.** The nine cases whose goldens the migration moved and
which Batch A did not cover were captured with `jobs/capture-b/queue.txt`
(`Klog_Logs/capture-migrated-b3.log`, pid 289: `m2-solid`, `d1-dynamic-ubo`,
`v0-array-layers`, `v0-cube-faces`, `v0-formats-sampled`, `v0-query-full`,
`v0-robust`, `v0-timestamp-driver`, `v0-vertex-sint`), and distributed into
`golden/`. Two of the migrated goldens turned out to be *incomplete*, and were
re-captured alone: `v0-vertex-uint`, which the capture tool's own stream cut off
inside while the run itself finished (`runner_summary` "11 of 11" and `run_end`
are in the klog; `jobs/capture-b2/queue.txt`, `Klog_Logs/capture-vertex-uint.log`,
pid 290), and `c8-resolve`, whose batch A capture holds three of its four frames
and no `runner_test` record (`jobs/capture-b3/queue.txt`,
`Klog_Logs/capture-c8-resolve.log`, pid 295: four submissions, both cases PASS).
Batch A's fifteen goldens were re-extracted from `Klog_Logs/capture-migrated.log`
with the same code, so every batch carries the replay fixes below.

**A NIR stage has no module, and the driver dereferenced it.** The deep-stack
wrapper this session added replaced `nir ? psbc_compile_nir(nir, ...) :
psbc_compile_shader(module->words, ...)` with an eager
`ps5vk_compile_shader_deep(nir, module->words, module->size, ...)`, and the
ternary had been what kept `module->words` from being read for the NIR stages
Mesa's meta operations hand the driver. Every meta clear, blit and resolve
faulted in `ps5vk_compile_stage` -- eight of `tools/check-driver.sh`'s host
tests died with SIGSEGV there, and the console's own capture batteries predate
the wrapper, so no console run had reached it. `driver/ps5vk_pipeline.c` now
passes the words only when there is a module. Verified on the console with the
meta-clear path in the queue: `c7-clear`, `v0-formats-sampled-uint`, `c5-depth`
and `m2-solid` all PASS, 528 PASS records and no `signal:`
(`Klog_Logs/verify-compile-stage.log`, pid 294).

**The host model's one-record difference, fixed rather than declared.** After
the migration, `tools/check-driver.sh` failed on exactly one record per affected
test: register `0x318` (`CB_COLOR0_BASE`) read one 64 MiB block higher on the PC
than on the console (`0x02044000` against `0x02004000`; c4-rtt's
render-to-texture step `0x02064000` against `0x02024000`). It was not a driver
difference. A driver-run document merged the run's regions by name, so
`framebuffer` -- c1-triangle's 64 MiB swapchain allocation at `0x200400000` --
was pinned for *every* test's replay, while the console had freed it long before
c2-indexed allocated its own target at that same address. Dropping the region
from c2-indexed's replay makes its PC submission byte-identical to the console's
(11 packets, 3 register tables), so `driver_run_document` now keys an allocation
region by the test that recorded it (`framebuffer-c1-triangle`) and
`driver_replay_text`'s existing test filter drops it for the others. No
`--expect-record` was needed and none was added.

**The same scope error, twice more.** A run document is process-wide and three
of its lists were being read as if they were the replayed test's:

- the *stages*: `compare_run` handed `golden_table` every test's pipeline
  mapping, and a table address can lie in several of them -- c2-staging's uconfig
  table at `0x200042000` lies in c2-staging's stage, c4-texture's and a third,
  and the first match read a shader header out of the wrong image. The
  comparison now selects the submission's own test's stages, as the replay
  always has.
- the *VideoOut handle*: `driver_run_document` kept the last stream's handle, so
  a run whose first test presented recorded `video: -1` from the last headless
  one, and the C1 present test's replay opened no VideoOut at all
  (`sceVideoOutOpen failed: 0xffffffff`). The document now carries a per-test
  handle map and the replay takes the replayed test's own.

With those, `tools/check-driver.sh`'s comparisons are clean: no `DIFFERENT`
record anywhere and no segfault, where the working tree began at 148 failing
comparisons and 8 host-test faults. The gate's `query_run` also moved from
`golden/v0-query-driver/run-1.json` -- a pre-migration capture whose submission
has one user-data word fewer -- to the case's own re-captured
`golden/v0-query-full/run-1.json`.

**A captured flip carried the draw's length.** `ps5vk_queue_flip` updated
`ps5vk_debug_last_submission`'s count but not the step list
`ps5vk_debug_submission_steps` reads, so the runner's capture of a flip logged
the *previous draw's* 104 words over the flip's 64 -- the last 40 being the
draw's buffer behind them, which no packet parser can walk
(`ValueError: word 64 (0x80000000) does not start a complete type-3 packet`).
The flip is a submission of its own and the driver now records it as one; the
re-capture shows 104-word draws and 64-word flips, matching the PC's dumps word
for word (`Klog_Logs/capture-c1-triangle3.log`, pid 298). That capture is also
where the golden's flip marker had to come from: the marker the wait and flip
packets name is the swapchain's own present counter, and a run whose queue had
already presented -- Batch A ran `m2-solid` first -- recorded markers one higher
than a PC process's, so `jobs/capture-b4/queue.txt` runs c1-triangle first and
m2-solid after it, which is enough because the register defaults a capture needs
are the same run's m2-solid frame, not an earlier one.
`tools/check-driver.sh` is green with all of it: the whole table of loader,
direct and PS5-link checks PASS, no `DIFFERENT` comparison and no fault.

**Console proof of the final artifact.** The `jobs/aco-min` queue ran once more
on the build this commit lands (pid 299, `Klog_Logs/aco-min-final.log`): seven of
seven tests PASS, 1581 PASS records, no `signal:` record. The compile-stage fix
is proved by the same artifact in `Klog_Logs/verify-compile-stage.log` (pid 294),
whose queue is the meta-clear path that faulted on the host: `c7-clear`
PASS.

**One more host limit.** `tools/check-runner-cases.sh` was red before any of
this: the re-captured goldens' replays carry one `stage` line per pipeline per
frame, and `golden/v0-formats-sampled`'s runs to 142 region lines against the
host's `kMaxRegions = 64`, so `parse_replay` rejected a valid replay and five
cases reported NO RECORD. The cap is 256 now, sized from the measured largest
capture rather than the previous one, and the gate's seven cases pass again.
`make lint` had four clang-format violations at the two `compile_deep` call
sites, which `tools/run_clang_format.sh` fixed.

## 2026-09-20 — R2: the sampler's address modes, and the default one

`vkCreateSampler` accepted exactly one configuration: clamp-to-edge on all three
axes, the state the M3 texture canary ran. `VK_SAMPLER_ADDRESS_MODE_REPEAT` is
what a zeroed `VkSamplerCreateInfo` holds, so the driver refused the *default*
sampler and the first `vkCreateSampler` an application makes
(PS5_VULKAN_REQUESTS.md, R2).

The mode is per-sampler state in Vulkan but three 3-bit fields of word 8 of the
combined image-sampler descriptor this driver writes, and the encoding comes
from ps5-opengl's own `ps5_texture_descriptor_wrap`: `PIPE_TEX_WRAP_REPEAT` 0,
`MIRROR_REPEAT` 1, `CLAMP_TO_EDGE` 2 -- and 2 is what the canary's descriptor has
always carried, which is what made the change a widening rather than a
rewrite. `v0-sampler-address` proves it in pixels: one frame per mode, each
sampling a 256-texel-wide image whose four 64-texel groups are red, green, blue
and white, with u running 0 to 4 across the whole target and nearest filtering,
reading two pixels whose texel lands in the image's odd 256-texel period, where
the modes differ. The console (`Klog_Logs/r2-sampler-address2.log`, pid 315):

| mode | group fetched | pixel |
| --- | --- | --- |
| repeat | 1 | green |
| mirrored repeat | 2 | blue |
| clamp to edge | 3 | white |

Three modes, three different fetches at the same pixel: a driver that ignored
the state would draw the clamp frame for all three, which is exactly what the
case fails on. `m3-texture` and `m2-solid` regress, and the case's golden is
`golden/v0-sampler-address` with `jobs/v0-sampler-address/queue.txt`.

Two modes core Vulkan 1.0 requires are still refused, and each names its own
gap rather than sharing the address-mode message: `CLAMP_TO_BORDER` needs the
border colour word (word 11) no probe has varied, and `MIRROR_CLAMP_TO_EDGE` is
`VK_KHR_sampler_mirror_clamp_to_edge` rather than core 1.0. The rest of the
sampler state -- mixed filter pairs, anisotropy, comparison sampling, LOD bias
-- stays refused: the request says each is its own probe, and the application
that filed R2 asks for anisotropy in the same block it asks for repeat, which
its own report already notes is the application's call to make (a device
reporting `maxSamplerAnisotropy` 1.0 makes the flag a semantic no-op, and the
report names asking only above 1.0 as the correct application-side fix).

## 2026-09-20 — R1: culling and rasterizer discard, and depth bias left named

`ps5vk_draw_refusal` refused five pieces of `VkPipelineRasterizationStateCreateInfo`
in one condition, and three of them are core Vulkan 1.0 with no feature bit to
gate them: `cullMode`, `rasterizerDiscardEnable` and `depthBiasEnable`
(PS5_VULKAN_REQUESTS.md, R1). The other two -- `polygonMode` beyond FILL and
`depthClampEnable` -- are gated by `fillModeNonSolid` and `depthClamp`, which
this device reports false, so refusing them is the specification's own answer and
they keep their refusal (reworded to name the feature bit). A pipeline that culls
is created successfully and refused later, at its first draw, which is why
nothing in the command audit could see it.

Two of the three are closed, and the registers are the register database's own
(Mesa's `gfx10.json`; ps5-opengl's runtime programs the same bits):
`PA_SU_SC_MODE_CNTL` (context 0x205) carries `CULL_FRONT` bit 0, `CULL_BACK`
bit 1 and `FACE` bit 2, and `PA_CL_CLIP_CNTL` (0x204) carries
`DX_RASTERIZATION_KILL` bit 22. `v0-cull` proves both in pixels: the same quad
four times, its two triangles wound opposite ways -- a quad wound one way is
culled whole or not at all, which would prove nothing about the *face* -- with
each frame's pixels read back. On the console
(`Klog_Logs/r1-cull-run4.log`, pid 323):

| frame | samples drawn (of 132) |
| --- | --- |
| cull none | 132 |
| cull back | 65 |
| cull front | 67 |
| rasterizer discard | 0 |

`65 + 67 = 132`: the two cull modes remove complementary halves, which is what
tells a driver that programmed the wrong `FACE` winding bit from one that
programmed no culling at all. And which half goes is the specification's: with
`frontFace = COUNTER_CLOCKWISE` and this driver's viewport carrying Vulkan's
orientation, the `(0,1,2)` triangle of the quad is clockwise in framebuffer
coordinates, so it is the back face Vulkan names and `cull back` removes it
(that triangle's columns read clear in the cull-back frame).

**Depth bias is the third state and stays refused, by name.** The offset words
are known (`PA_SU_POLY_OFFSET_DB_FMT_CNTL` at 0x2de through
`PA_SU_POLY_OFFSET_BACK_OFFSET` at 0x2e3, the six-word block ps5-opengl's
runtime writes, with the scale as the slope factor times sixteen and
`DB_FMT_CNTL`'s 0x1e9 the 32-bit float depth format's own), but a bias is
observable only in a depth readback, and no probe has read a biased depth back:
the refusal names that probe rather than accepting the state on a reading. The
case's golden is `golden/v0-cull` with `jobs/v0-cull/queue.txt`, and `m2-solid`
regresses.

## 2026-09-20 — R3: the stencil clear's value, and the workaround that names its own retirement

Mesa's `vk_meta_clear` unwraps a stencil attachment's clear value with
`clearValue.depthStencil.depth` where the value it wants is the `.stencil`
member (`src/vulkan/runtime/vk_meta_clear.c`), so a render pass that clears a
combined depth-stencil attachment with depth 1.0 and stencil 0 leaves the
stencil plane holding the depth clear's own byte. The driver's `v0-stencil` case
never saw it: it clears and then *writes* the stencil with a pipeline before
reading it, so the cleared value is never the thing read (PS5_VULKAN_REQUESTS.md,
R3).

`v0-stencil-clear` clears with depth 1.0 and then 0.5, draws nothing -- R1's
rasterizer discard, so the clear is the only thing that writes -- and reads the
stencil plane's whole first tile back. Measured on the console before the fix
(`Klog_Logs/r3-stencil-clear-before.log`, pid 327):

| depth clear | stencil tile bytes equal to 0 (of 65536) |
| --- | --- |
| 1.0 | 0 |
| 0.5 | 65536 |

which is the request's discriminating follow-up: the plane follows the *depth
member's* value rather than being clamped or defaulted, and with depth 1.0 it
holds no zero at all.

**The workaround, at the driver's boundary.** `ps5vk_CmdBeginRendering` hands
`vk_meta_clear_rendering` a copy of the rendering info whose *stencil*
attachment carries the stencil's own value in the `depth` member -- the member
`vk_meta` reads -- and leaves the depth attachment's copy alone, so the depth
clear still takes the depth. It is marked `WORKAROUND(R3)` with the upstream
line that retires it, and it is deliberately not a patch to Mesa: the driver
links a prebuilt runtime whose sources this repository does not own, and the
defect is upstream's and already reported as a measurement rather than a
request. After the fix the same run reads 65536 zero bytes for both depths
(`Klog_Logs/r3-stencil-clear-after.log`, pid 329), with `v0-stencil` (round 12's
write-then-read case) and `m2-solid` regressing. The case's golden is
`golden/v0-stencil-clear` with `jobs/v0-stencil-clear/queue.txt`.

## 2026-09-20 — R5 and R4: the resolve refusal's sentence, and what the audits cannot see

**R5, reworded rather than tiled.** `vkCmdResolveImage` refuses unless both
images are tiled, and `ps5vk_image_storage` chooses tiled storage from the usage
bits -- `COLOR_ATTACHMENT` or `DEPTH_STENCIL_ATTACHMENT`. An application that
follows the specification asks for a destination with `TRANSFER_DST` (and
`SAMPLED`), which is stored in rows, so it is refused by a sentence about tiling
with no path from the sentence to the usage bit that decided it
(PS5_VULKAN_REQUESTS.md, R5). The refusal now names the bit, the storage rule and
the workaround in one sentence.

The request offers tiling a transfer destination as the alternative, and this
round took the sentence deliberately: tiling is chosen from the usage bits, and
a texture upload declares `TRANSFER_DST` too, so tiling every transfer
destination would move every sampled image's descriptor (the tiled kind bit) and
with it every golden that samples a texture. The sentence is one line; the tiling
is a repository-wide re-capture for the same behaviour. Both are the request's
own two options, and it makes no recommendation between them.
**R4, the audits' blind spots, stated.** The three audits now print what they
cannot see beside the case that covers it, which is the request's own point --
a command list says nothing about pipeline state (R1 lived there), a limit is
not sampler state (R2), and a clear that writes the wrong value is still a clear
(R3):

| audit | what it cannot see | the case |
| --- | --- | --- |
| `command_audit.py` | pipeline state, sampler state, cleared pixels, a submission's steps | `v0-cull`, `v0-sampler-address`, `v0-stencil-clear`, `v0-two-passes` |
| `limits_audit.py` | sampler state | `v0-sampler-address` |
| `format_audit.py` | the pixels a clear wrote, a resolve destination's usage | `v0-stencil-clear`, the R5 refusal |

Each of those cases is a console-proved runner case with a queue under `jobs/`
and a golden under `golden/`, not a one-off test: `v0-two-passes` (R6),
`v0-sampler-address` (R2), `v0-cull` (R1) and `v0-stencil-clear` (R3) were added
by the rounds above, each with the console run that proves it recorded in its own
section. The audits' `--check` modes exit 0 unchanged: the statements are
printed, not counted.
