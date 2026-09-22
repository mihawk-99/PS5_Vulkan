# Milestone 5 reference

Milestone 5 in full: the hardware-behaviour workflow, the phase A-E step tables
with their acceptance criteria, the Vulkan version ladder, and the PC
accelerator. Top-level plan: [VULKAN_PROBE_PLAN.md](VULKAN_PROBE_PLAN.md).
Current state and next actions: [VULKAN_PROBE_ACTIVE.md](VULKAN_PROBE_ACTIVE.md).

_Reference file: read it when working on the driver._

## Milestone 5 roadmap

Decided on 2026-09-14. The Vulkan layer is built on Mesa's common Vulkan
runtime (`src/vulkan/runtime`, MIT, present in the SDK's opengnm-psbc tree)
over the AGC backend these milestones established. RADV's state translation is
borrowed function by function where it matches. The Vulkan Tutorial's chapter
order is the test ladder, because each chapter adds one concept.

The map below is a specification, not a status board: it lists every step with
its acceptance criteria. What has passed, with run ids and digests, lives in
`docs/VULKAN_PROBE_ACTIVE.md` and the phase logs (`docs/M5_PHASE_A.md`,
`docs/M5_PHASE_B.md`, `docs/M5_PHASE_C.md`). "PC" steps need no console time;
"console" steps need a build and at least one launch.

### Handling hardware behaviour not yet recorded

The PC checks only know what golden captures recorded, and they fail loudly
instead of guessing:
- a helper model refuses arguments no capture covered
- an AGC call without a model does not link
- changed shader or link inputs no longer match their replay

A new behaviour therefore costs one console build and one launch with
`jobs/capture/queue.txt`:
1. The launch proves the hardware result with an exact readback and records
   golden files.
2. The models or replay data are extended from the recording.
3. From then on, every iteration on that feature is PC-only.

The PC checks verify what is sent to the GPU, not what it draws. Pixel
results, GPU faults, deadlocks and timing are only visible on the console.
Lavapipe reference images (accelerator step 5) predict expected pixels, but
the console decides.

### Development accelerator

| Step | What | Where |
|---|---|---|
| 1 | klog parser and PM4 decoder | PC |
| 2 | Runner capture mode and golden files | console, once |
| 3 | Helper models and whole-frame rebuild from the runner's source | PC |
| 4 | Checks for every hardware rule, each proven on a deliberately broken frame | PC |
| 5 | Vulkan programs through the driver on the PC, compared with golden files and lavapipe reference images | PC |
| - | Restore `jobs/regression/queue.txt` on the console before normal runs | seconds |

What of each is built: `docs/VULKAN_PROBE_ACTIVE.md`.

### Phase A: foundation gate (shader compilation on the console)

| Step | What | Where |
|---|---|---|
| A1 | Build Mesa's `util` library and the NIR/ACO compiler (libpsbc) with the PS5 toolchain, filling gaps in the console's libc (threads, mappings). `tools/check-psbc-link.sh` links it | PC |
| A2 | A C/C++ shader package writer that reproduces the probe packages byte for byte | PC |
| A3 | Console test: compile SPIR-V on the console for the target's export format, package it, draw the M2 triangle with an exact readback, and require a package byte-identical to the PC-compiled one (runner queue keyword `compile`) | console |
| A4 | Gate: if A3 fails, fall back to pipelines precompiled on the PC | decision |

Phase A's results, run ids and package digests: `docs/M5_PHASE_A.md`.

### Phase B: first end-to-end Vulkan program (headless triangle)

| Step | What | Where |
|---|---|---|
| B1 | Generate the Vulkan dispatch and entry-point files from `vk.xml` with Mesa's generators; build the common Vulkan runtime for the PS5 and the PC. Mesa 26.2.0 is the source release; a PC smoke test must pass and the PS5 link must be accepted by the converter | PC |
| B2 | Driver skeleton: instance, physical device (features and limits), device, queue. The reported features and limits are the specification's own: every optional feature is off until a probe proves it, every limit is the Required Limits table's value, and `tools/limits_audit.py` holds the two tables together member by member (docs/M5_PHASE_B.md). The test must pass through the Khronos loader and directly, and its PS5 link must be accepted by the converter | PC |
| B3 | Memory, buffers and images on direct memory, keeping 32-bit shader pointers at high word `0x2`. B3a device memory: one mapped direct-memory allocation per `VkDeviceMemory`, refused outside the high-word-2 window, with a negative test. B3b buffers: 256-byte aligned, GPU address = mapping + bind offset. B3c: RGBA8 and D32 formats, and image storage from ps5-opengl's row and tile layouts (the 4K colour and depth attachments size to the runner's 32 MiB allocations) | PC |
| B4 | Probe: a submission that completes without a flip, the completion signal headless Vulkan needs. A `RELEASE_MEM` event 40 marker is written after the stream's last packet, in the shape ps5-opengl's non-presenting frames use, and must arrive after the last draw has rendered | console |
| B5 | Command buffers and submission through the frame encoder; fences and semaphores on the B4 marker. Command buffers submit synchronously, each submission ending in its own completion marker; fences and semaphores use a CPU sync type; a marker that never arrives is device loss | PC, then console |
| B6 | Pipelines: SPIR-V to package, one pixel-shader variant per attachment export format (the M4 blend rule). `vkCreateGraphicsPipelines` compiles both stages with psbc and the packages of every probe set (M2 to M4) must be byte-identical to the hardware-run ones. The compiler archive's five runtime stubs are renamed in a driver copy; SPIR-V without the named entry point is refused before the compiler, which crashes on it | PC |
| B7 | Render pass or dynamic rendering to target registers; draw and read back. Mesa render passes run over `vkCmdBeginRendering`, AGC shader objects are created on the first draw, and the submitted frame must equal the console's golden `b4-headless` frame apart from the Vulkan viewport orientation (y scale +h/2) | PC, then console |
| B8 | Probe: several command buffers in one submission (`INDIRECT_BUFFER` chaining). A PM4 `INDIRECT_BUFFER` call into the title's memory faults the GPU (unmapped page at the buffer address, system context; about 2 s stall, no reset), so the driver copies command buffers into one stream instead; `b8-indirect` is not to be queued again | console |

### Phase C: Vulkan Tutorial ladder (one driver step per chapter)

Adopted on 2026-09-15. Phase B's lessons shape the workflow:
- B7's console run taught the viewport orientation and the culling defaults,
  although the registers were "recorded".
- B8's indirect-buffer probe froze the console.

#### Workflow

1. **PC first.** The driver and each step's Vulkan test program are written
   and cross-compiled on the host. Every test runs on the PC through the
   Khronos loader, with the Khronos Validation layer when it is installed, and
   directly.
2. **What PC checks verify.** They check what the driver submits:
   - the host layer records each submission (`PS5_HOST_SUBMISSION_DUMP`);
   - `tools/golden.py compare-submission` compares it, packet by packet and
     table by table, with golden frames recorded on the console.
   
   The PC renders nothing. Where pixels are hard to predict (filtering,
   mipmaps, MSAA), lavapipe renders the same test program as a reference
   image, and the console decides.
3. **Every step ends on the console.** A step's Vulkan program runs through
   the driver in the test runner (`AGC_VULKAN_DRIVER`) with an exact readback,
   or a stated tolerance where filtering makes exactness meaningless. That is
   a normal run, not a probe.
4. **New hardware behaviour needs a runner probe first.** A behaviour no
   console run has recorded is first proven by a runner test written for it
   (as B4, B5c and B8a were). The runner test uses raw AGC, not the driver,
   and runs with capture mode.
   - Its golden frames extend the host models and replays.
   - Driver submissions are then compared with them on the PC, and the rest
     of the step is PC-only until its closing console run.
   
   PC checks that meet an unrecorded behaviour fail loudly instead of
   guessing. Before queuing anything, a failing PC check is diagnosed as
   driver logic or missing hardware knowledge.
5. **Risk.** A probe that may fault the GPU or freeze the console is named,
   with its likely effect, before the user launches it. It is never queued by
   default: the runner's `kFaultingTests` keeps known offenders out of the
   default and `all` queues.
6. **Commits.**
   - Every build that passes its checks gets a detailed commit, as in
     Phases A and B.
   - Each step's closing commit is titled
     `feat(vulkan): implement C<N> - <feature>`, with a detailed body.
   - Commits stay local; the user pushes.
7. **Toolchain.** Scripts in `tools/` gain no Android or other non-PS5
   toolchain flags.

#### Steps

| Step | What | New hardware behaviour | Acceptance criteria |
|---|---|---|---|
| C1 | Swapchain and present. `VK_KHR_surface`, `VK_KHR_display` (VideoOut as the display, the standard for a screen without a windowing system, which `vkcube --display` uses) and `VK_KHR_swapchain`. Driver-owned B8G8R8A8 swapchain images registered with VideoOut. `vkAcquireNextImageKHR`, `vkQueuePresentKHR` ending the stream in the wait packet and flip, FIFO. Colour clears (`VK_ATTACHMENT_LOAD_OP_CLEAR`). Out-of-date handling and presentation pacing. | The flip, wait packet and B8G8R8A8 target are recorded (M2, M3). GPU colour clear is **not**: a runner probe first, comparing a clear drawn by a driver-built pipeline with splitting the submission for a CPU clear. | PC: the tutorial's triangle program through the loader and directly, validation clean; its presented frames equal the golden `m2-solid` frame (wait, state, draw, flip) apart from documented differences. Console: the triangle presented over several frames, each read back exactly from the flipped buffer, flips confirmed by VideoOut markers, and the clear colour exact outside the triangle. |
| C2 | Vertex, staging and index buffers. `vkCmdBindVertexBuffers`, `vkCmdBindIndexBuffer` (16-bit), `vkCmdDrawIndexed`, `vkCmdCopyBuffer`. Copies run on the CPU at submission split points: the driver ends the stream, waits for completion, copies, then continues, which keeps Vulkan's command order on shared memory. Base vertex and instancing. | Vertex buffers, 16-bit indexed draws and the vertex-buffer table are recorded (M3). `firstVertex`, `firstInstance` and instance counts are **not**: a runner probe first. | PC: the tutorial's vertex-buffer and index-buffer programs; streams equal golden `m3-vertex` (vertex-buffer table, index size, index base and count, `DRAW_INDEX_2`); split submissions keep order (a test writes a buffer in one command buffer and copies it in the next). Console: both programs read back exactly, including a staging-buffer upload. |
| C3 | Descriptor layouts, pools and sets. `vkUpdateDescriptorSets`, uniform buffers, push constants. | Uniform-buffer descriptor tables (16-byte entries, address high word 2) are recorded (M3). Push constants: first establish how the shader compiler lowers them (it has no push-constant option); a runner probe if that needs new user data. | PC: the tutorial's uniform-buffer program; descriptor tables and pixel-stage user data equal golden `m3-uniform`; push-constant data reaches the state the compiler expects without overwriting other user data. Console: the uniform colour and a transformed quad read back exactly across frames with changing uniforms. |
| C4 | Textures. Image upload through staging buffers, image views, `VkSampler`, combined image samplers. Barriers carry no state, except the colour barrier before sampling a target rendered earlier in the stream (M4 render to texture). | Nearest and bilinear sampling of a row-layout RGBA8 texture, combined image-sampler entries and render to texture are recorded (M3, M4). Other address modes need their own probes, and the LOD bias is **not** supported. Anisotropy **is** accepted where the device's own limit makes it a no-op: `anisotropyEnable` with `maxAnisotropy` inside `[1, maxSamplerAnisotropy]`, which this device reports as 1.0, so the only legal value is 1.0 and the flag cannot change a fetch (`v0-sampler-anisotropy`: two frames, 0 mismatched texels over the target). A value past the reported maximum is refused by name. The `samplerAnisotropy` *feature* stays FALSE -- nothing is filtered -- and the flag is accepted because at one sample it cannot matter. | PC: the tutorial's texture program; descriptors equal golden `m3-texture`; lavapipe reference image for bilinear filtering. Console: nearest sampling exact, bilinear within the recorded tolerance, and render to texture exact. |
| C5 | Depth. D32 attachments, depth test and write state, depth clears. | Tiled D32 (32 MiB at 4K) and depth test and write registers are recorded (M4). The depth clear follows C1's clear design. | PC: the tutorial's depth program; depth target and test registers equal golden `m4-depth`. Console: overlapping geometry resolves by depth exactly, and cleared depth reads back as the clear value. |
| C6 | Loading models. Large buffers and 32-bit index buffers. Model loading itself is application code. | **32-bit indices: a runner probe** (index size, index packets, readback). | PC: after the probe, streams equal its golden frames. Console: a model drawn with 32-bit indices reads back as lavapipe's reference, within tolerance where sampling is involved. |
| C7 | Mipmaps. Mip levels in descriptors and sampling. `vkCmdBlitImage` runs as draws sampling the source (Mesa's `vk_meta` or driver shaders), or as CPU copies at split points: nothing recorded offers a GPU blit engine. | **Mip-level storage and sampling: a runner probe**, plus the chosen blit path. | PC: descriptors and blit streams equal the probe's golden frames; lavapipe reference per level. Console: each level reads back against its reference within tolerance, and a mipmapped texture samples the expected level. |
| C8 | Multisampling. 4x colour and depth targets and resolve. 1x and 4x are what Vulkan requires and what the driver reports; 2x and 8x only if a later step needs them. | **Done.** Colour 4x and its resolve are the console's own measurement (64x64-texel tiles of four 0x4000-byte sample planes, the sample count in `CB_COLOR0_ATTRIB` *and* the `PA_SC_*` MSAA registers, and the resolve as a CPU average over that map, docs/M5_PHASE_C.md). Depth 4x renders: the depth description carries the image's sample count, `DB_Z_INFO`'s `NUM_SAMPLES` follows it, and a tested write keeps four words a texel (near 8,294,400, far 2,592,000 words). A four-sample depth texel is sixteen bytes holding its four samples, the tiles are 64x64 texels of 64 KiB stepping back sixty tiles a row, and `vkCmdClearDepthStencilImage` on one is the storage fill the measured layout allows -- proved on the console by `c8-depth-clear` (docs/HARDWARE_FINDINGS.md). A four-sample depth image *copies* into another: `VK_FORMAT_D32_SFLOAT` carries `TRANSFER_SRC` and `TRANSFER_DST` as well as the attachment bit, one `vkCmdCopyImage` moves the region's texels as they stand (a sixteen-byte texel, four samples a word), and the console's `c8-depth4-copy` proves it -- the copy writes the image's eight tiles and the alignment past them keeps the destination's own clear (Klog_Logs/format-items-run5.log, docs/HARDWARE_FINDINGS.md). A depth readback to a buffer and an upload from one still refuse by name. | PC: streams equal the probe's golden frames; console: the resolved frame equals the one-sample reference word for word (pid 119), the depth counts are the geometry's own (pid 227), the whole four-sample target clears (Klog_Logs/c8-depth4x-run2.log), and a four-sample depth copy moves the image's tiles (Klog_Logs/format-items-run5.log). |
| C9 | vkcube as a cross-check. `vkcube --display` built for the PS5 against the driver: vertex buffers, a texture, uniform buffers, depth and the swapchain. | Confirmation run; no new behaviour expected. | vkcube builds with the PS5 toolchain unmodified apart from its platform layer. On the console it renders steadily for minutes with FIFO flips and no tearing, and a captured frame matches lavapipe's vkcube frame within tolerance. |

### Phase D: breadth and benchmarks

| Step | What | New hardware behaviour |
|---|---|---|
| D1 | Sascha Willems' "Basics": dynamic uniform buffers, offscreen, instancing, texture arrays, cubemaps | yes: array and cube layouts |
| D2 | Compute: dispatch, storage buffers and images | yes: compute on AGC is untested |
| D3 | Port the Vulkan-Samples framework: PS5 platform layer, packaged assets | PC, then console |
| D4 | Performance samples as benchmarks: frame time, pipeline cache, submission cost | console |
| D5 | More formats: sRGB, float, D24S8 | yes: each format |

### Phase E: conformance and real applications

| Step | What | Where |
|---|---|---|
| E1 | Vulkan conformance subsets on the console, through the loader, pinned to a CTS tag. Recipe, the staged semantic-validation plan that now follows the closed rung 1.0, pitfalls and acceptance policy: [CTS.md](CTS.md) | console |
| E2 | RetroArch's Vulkan driver and slang shaders | console |
| E2a | The delivery a title can actually use: the driver linked into the title's own executable. `build/driver/ps5/libvulkan.so.1` is still built and signed — one file holding the driver, Mesa's runtime and the shader compiler, whose only imports are the console's own modules (`libSceLibcInternal.sprx`, `libkernel.sprx`, `libSceVideoOut.sprx`, `libSceAgc.prx`, `libSceAgcDriver.prx`, `libScePosixForWebKit.sprx`) — but **the console's loader refuses to load it into a title**: `sceKernelLoadStartModule` answers `ENOEXEC` (0x80020008) for every path under `/app0` and `ENOENT` (0x80020002) for the bare name, `dlopen` answers NULL for every candidate including the modules the title already holds, and `sceKernelDlsym` answers `ESRCH` (0x80020003) for every name on the two modules that do load (docs/HARDWARE_FINDINGS.md, 2026-09-18). A driver-enabled title therefore links `build/driver/ps5/libps5vk.ps5.a` (or the frontend links it itself) and calls the entry points as ordinary symbols; no ICD manifest and no `VK_DRIVER_FILES` are involved either way, since the frontend resolves `vkGetInstanceProcAddr` itself (`tools/build-driver.sh`; the PC loader and its manifest under `build/driver/host/` are the tests' own) | console |
| E3 | Emulators: PPSSPP, DuckStation, ParaLLEl-N64, Flycast, Dolphin, including JIT memory handling | console |
| E4 | Optional: Zink (OpenGL on this Vulkan) for OpenGL-only programs | console |

Order: accelerator step 4 first, then Phase A, whose gate decides the rest.
Step 5 grows alongside Phase B. The riskiest unknowns are A3 (compilation on
the console), B4 (completion without a flip) and D2 (compute), each probed as
soon as its phase starts.

### Vulkan version ladder

Adopted on 2026-09-15. The goal is a Vulkan 1.4 device. The version the
physical device reports (`PS5VK_DEVICE_API_VERSION`, now 1.0) is a promise to
applications that every requirement of that version works. The driver reports
the highest version whose requirements are all met, and it climbs one version
at a time, because each version requires everything the previous one does.
The instance already reports 1.3: that is Mesa's instance-level
implementation and promises nothing about the device.

**Sources.** Vulkan-Docs v1.4.354, in the ignored `.deps/native/vulkan-docs`:
- `versions-v1.4.354.adoc` (sha256 `998beee5…2435`): what each version added;
- `features-v1.4.354.adoc` (`685d567a…aeb3`): Feature Requirements;
- `vk-v1.4.354.xml` (`80e7394d…08ee`): the registry, whose `<feature>`
  requirements generate that list and whose `promotedto` attributes name each
  version's promoted extensions;
- `limits-v1.4.354.adoc`: the Required Limits table;
- `formats-v1.4.354.adoc`: Required Format Support.

**Rules.**
1. Raising the reported version is its own commit. B2's test then asserts
   every requirement of the new version: features, promoted extensions'
   entry points and properties, limits and required formats. The console
   runs a Vulkan CTS subset for that version (Phase E1) before the commit.
2. A requirement counts as met when a Phase C/D step has proven it on the
   console, not when an entry point exists.
3. "Mesa" below means Mesa 26.2.0's common runtime already provides the entry
   points (`vk_common_*`). The driver still owns every part that reaches the
   GPU.

**Extension policy.** The driver exposes an extension only after a step has
proven it on the console, and it exposes the smallest set the tutorial ladder
and the conformance subsets need. This matters because a version promotes
extensions: exposing one at the version that promotes it makes that version's
required feature set mandatory, so exposure is a version decision rather than
a convenience. Today the instance exposes `VK_KHR_get_physical_device_properties2`,
`VK_KHR_surface`, `VK_KHR_display`, `VK_EXT_debug_report` and
`VK_EXT_debug_utils`, and the device exposes `VK_KHR_swapchain` alone, which is
what keeps 1.1's and 1.2's conditional requirements out of scope for now.

#### 1.0: make the current claim true

Vulkan 1.0 requires one feature, `robustBufferAccess`, which the driver reports
**on** (proved by V0-robust, console run pid 140). It also requires the whole 1.0
API, the Required Format Support tables, and the Required Limits. The table below
is the audit of that claim, one requirement class per row, each with the console
evidence it rests on; `docs/V0_FORMATS_AUDIT.md` records the format rows.
The API surface itself is audited command by command: `tools/command_audit.py`
classifies all 137 commands the registry's 1.0 feature names -- 90 implemented by
the driver, 47 supplied by Mesa's runtime, **none refused by name and 0
unaccounted** --
and the commands of the six extensions the device exposes (0 unaccounted there
too, with `VK_KHR_swapchain`'s four device-group commands conditional on Vulkan
1.1); `--check` is a gate.

| Requirement | Today | Delivered by |
|---|---|---|
| `robustBufferAccess` (out-of-bounds buffer access stays safe) | **on** since V0-robust (console run pid 140, `golden/v0-robust`) | the driver clamps an indexed draw's count to the binding's range (`ps5vk_draw.c`); the AGC compiler has no robustness option and refuses uniform blocks larger than 16 bytes, so the shader-side path is not expressible and the fixed-function one is what a runner check proves |
| Drawing API: vertex and index buffers, descriptors, textures, depth, blending, clears (`loadOp` and `vkCmdClearAttachments`), scissors, dynamic viewport | partial: one attachment and full-target draws; static and dynamic (D1, pid 144) uniform buffers; indexed (pid 121), instanced (pid 124) and base-vertex (pid 142) draws; a staged copy path (pid 122); depth (pid 158) and blending (M4); secondary command buffers through `vkCmdExecuteCommands`, whose submission is packet-identical to a direct frame (`b8_secondary`, both PC arms 6 of 6, and `b8-secondary` in the `jobs/c8-controls` battery, `Klog_Logs/c8-controls-run5.log`); `loadOp`, `vkCmdClearAttachments` and `vkCmdClearColorImage`; **indirect draws (`c2-indirect`) and the CPU-side transfers (`c2-transfers`) proved in that same battery, 13 of 13** | C1-C5, D1 |
| Compute pipelines and dispatch (a graphics queue family must also offer compute) | **implemented and proven** (D2, pid 125): a compute pipeline from the application's SPIR-V, per-set descriptor tables shared with graphics (R7, pid 162: sampled image to storage image, 0/256 mismatched texels), the packets V0-compute proved, and the single-storage-buffer shader's word in the readback; the PC submission is identical to the console's (`golden/d2-compute`). `vkCmdDispatchIndirect` is recorded too (pid 128) | `V0-compute` (the AGC compute path), D2 |
| Blits, resolves, copies; 4x multisampling (sample counts the driver reports) | copies, readback, one-to-one blits and a 2x **scaled** blit (NEAREST and LINEAR *magnification*) of `R8G8B8A8_UNORM` (C7: `golden/c7-copy` and the per-format blit frames, both pid 163, and the tiled mip chain, pid 134). A **mirrored** region is not among the recorded ones and a `LINEAR` *minification* is not either: the driver supports both -- the application's BLIT demo ran `src (3840,0)-(0,2160) -> dst (0,0)-(3840,2160)` NEAREST on the console and its screenshot shows the mirror landing on the opposite side -- but no probe has captured a mirror, so the earlier claim of "scaled or mirrored" was corrected here rather than left standing (docs/M5_PHASE_C.md, 2026-09-21); **4x MSAA renders into four sample planes and `vkCmdResolveImage` reproduces the one-sample frame word for word** (C8, pid 119: `golden/c8-msaa` and `golden/c8-resolve`, both PC submissions identical to the console's) | C7, C8 |
| Queries (occlusion, timestamps where reported) | occlusion queries over the z-pass counter, proved at the AGC level (jobs/v0-query) and through the driver (`golden/v0-query-driver`, pid 132, with vkCmdCopyQueryPoolResults writing the same result into a buffer (pid 135); whose three regions came back 0, all and half); the driver reports `occlusionQueryPrecise` false because one count is 16 samples; and timestamps are implemented on the clock V0-query measured (pid 143: a RELEASE_MEM BOTTOM_OF_PIPE_TS write at 99.79 MHz, a period of about 10 ns), so the pool type exists, the command records that packet and the two API paths agree (pid 144, `golden/v0-timestamp-driver`) | C2's query path; `V0-query` for timestamps |
| Synchronization objects and caches: fences, semaphores, events, pipeline caches | fences and semaphores since B5; **events and pipeline caches since the B5/B6 round**: an event is the binary flag with Vulkan's set, reset and wait rules, and a cache is the empty cache an implementation is allowed to have (`driver/tests/vk_b5_events_test.c` 11 of 11 checks in both PC arms, `driver/tests/vk_b6_pipeline_cache_test.c` 6 of 6) | B5, B6 |
| Required format tables (the formats and feature bits every 1.0 device must support) | **closed**: every required row is reported, each after the console probe that proved the family's fetch, attachment, blend, blit or transfer path -- the four-byte colour families, the sampled families V0-formats fetched and blitted exactly, the packed families (pid 263), the integer and half-float colour targets, the vertex formats including the three-component integer positions V0-formats drew with (pid 146), D32's depth attachment and transfers, the descriptor-typed families (uniform and storage texel buffers, storage images and their atomics) and the sRGB pair. `tools/format_audit.py --check` exits 0 on 179 required rows, 58 reported, 0 missing a required feature and 55 conditional, `docs/V0_FORMATS_AUDIT.md` records each row, its split and its closing path, and `tests/test_tools.py` requires the two to agree -- so no count is repeated here to drift | `V0-formats`, C7 and D2, plus the compiler patch scripts `tooling/psbc/patch-descriptor-types.py` and `patch-vertex-formats.py` for the rows the pinned compiler could not express; D5 adds the formats 1.4 requires |
| Limits already reported (4 descriptor sets, 16 vertex bindings, 4 colour attachments, 4096 images) | audited: `tools/limits_audit.py --check` compares 97 of the 106 members the specification requires against its table, with footnotes 2 and 8 applied, and 0 miss -- the nine it does not compare are the rows whose type is a recommendation, implementation-dependent or a duration, or whose core column requires nothing | C2-C8, D1 |

#### 1.1

- **Required features.** `multiview` (rendering several views or layers in one
  pass; needs layered targets and a view index in shaders: probe).
  `shaderDrawParameters` only if `VK_KHR_shader_draw_parameters` is exposed;
  `storageBuffer16BitAccess` only if uniform-and-storage 16-bit access is.
- **Also required.** SPIR-V 1.1-1.3, and subgroup operations in compute
  (at least the basic ones).
- **Promoted extensions (23).** Mostly API plumbing:
  - Mesa supplies properties2 (already exposed), memory requirements2, device
    groups of one device, descriptor update template objects, and external
    fence and semaphore capability queries.
  - The driver adds bind_memory2, dedicated allocation, maintenance1-3,
    relaxed and storage-buffer block layouts, variable pointers (compiler)
    and YCbCr conversion objects. Sampling through YCbCr conversion becomes
    required only at 1.4.
- **Delivered by.** Phase C, D1 (layered targets), D2 (subgroups),
  `V1-multiview` (layered targets and a view index in shaders, a runner probe
  first), and `V1-promoted` for the promoted group: Mesa supplies most of it,
  the driver adds bind_memory2, dedicated allocation and maintenance1-3, and
  libpsbc adds variable pointers.

#### 1.2

- **Required features.** `timelineSemaphore`, `imagelessFramebuffer`,
  `uniformBufferStandardLayout`, `shaderSubgroupExtendedTypes`,
  `separateDepthStencilLayouts`, `hostQueryReset`,
  `subgroupBroadcastDynamicId`.
  - Descriptor indexing, 8-bit storage, 64-bit atomics, indirect draw count,
    minmax sampling and viewport/layer output are conditional: each is
    required only if the driver exposes the matching extension or feature.
- **Also required.** SPIR-V 1.4 and 1.5. A device claiming 1.2 must accept
  them; the probe sets themselves compile for Vulkan 1.0, the version this
  device reports (`docs/M5_PHASE_C.md`).
- **Promoted extensions (24).**
  - Mesa supplies create_renderpass2, imageless framebuffers (through its
    render passes), driver properties, and timeline semaphore calls over
    `vk_sync` (the CPU sync type needs a timeline variant, e.g.
    `vk_sync_timeline`).
  - The driver adds depth/stencil resolve (C8), separate stencil usage,
    image format lists, sampler mirror-clamp, standard and scalar block
    layouts (compiler), and shader float controls.
- **Delivered by.** C5 and C8 (depth/stencil layouts and resolve), C3
  (layouts), D2 (subgroups), `V2-timeline` (a timeline variant of the CPU sync
  type over Mesa's `vk_sync`), `V2-promoted` for the promoted group, and
  `V0-query` for host query reset.

#### 1.3

- **Required features.** `dynamicRendering` (the driver already implements
  rendering this way under Mesa's render passes), `synchronization2` (Mesa
  supplies `vkQueueSubmit2`), `maintenance4`, `privateData` and
  `pipelineCreationCacheControl` (Mesa), `inlineUniformBlock`,
  `bufferDeviceAddress`, `vulkanMemoryModel` and
  `vulkanMemoryModelDeviceScope`, `robustImageAccess`,
  `shaderTerminateInvocation`, `shaderDemoteToHelperInvocation`,
  `shaderZeroInitializeWorkgroupMemory`, `shaderIntegerDotProduct`,
  `subgroupSizeControl` and `computeFullSubgroups`.
- **Also required.** SPIR-V 1.6.
- **Promoted extensions (23).** Include extended dynamic state 1 and 2 (Mesa
  supplies the setters; the driver must honour the state), copy_commands2
  (Mesa), format_feature_flags2 and texel buffer alignment.
- **Hardware unknowns.**
  - `bufferDeviceAddress`: shaders dereference buffer addresses. The compiler
    fixes the upper address word to 2, so every buffer must stay in the
    4 GiB window. Probe.
  - `robustImageAccess`: compiler or lowering support.
  - Subgroup size control: part of compute, D2.
- **Delivered by.** C3 (inline uniform blocks), C1 (dynamic rendering, which
  the driver already implements under Mesa's render passes), D2,
  `V3-bda` (buffer device addresses inside the 4 GiB window: a runner probe
  first), `V3-promoted` for the promoted group, and `Vshader` for the
  shader features the pinned compiler has to express.

#### 1.4

- **Required features (42).** Every Vulkan 1.0 feature below becomes
  mandatory:
  - `fullDrawIndexUint32` (C6), `imageCubeArray` (D1), `independentBlend`,
    `sampleRateShading` (C8), `drawIndirectFirstInstance`, `depthClamp`,
    `depthBiasClamp` (C5), `samplerAnisotropy` (C4 probe);
  - `fragmentStoresAndAtomics`, `shaderStorageImageExtendedFormats` (D2),
    dynamic indexing of uniform-buffer, sampled-image, storage-buffer and
    storage-image arrays, `shaderImageGatherExtended`, `shaderInt16`,
    `largePoints`.
  
  So does every 1.1-1.2 feature below:
  - `samplerYcbcrConversion`, 16-bit and 8-bit storage, `variablePointers`,
    `samplerMirrorClampToEdge`, `scalarBlockLayout`, texel-buffer dynamic
    indexing, `shaderInt8`.
  
  And the promoted 1.4 features:
  - `pushDescriptor`, `dynamicRenderingLocalRead`, `maintenance5`,
    `maintenance6`, `indexTypeUint8`, `bresenhamLines`,
    `vertexAttributeInstanceRateDivisor`, `globalPriorityQuery`,
    `shaderSubgroupRotate` (and clustered), `shaderFloatControls2`,
    `shaderExpectAssume`, `pipelineRobustness`.
  - `pipelineProtectedAccess` only with protected memory.
- **Also required.**
  - Every graphics or compute queue must also advertise transfer; the driver
    already does.
  - A device whose queue families include no transfer-only family must
    support `hostImageCopy`. The driver has one family, so either it
    implements host image copies or it adds a transfer-only family.
  - `load_store_op_none` and `map_memory2` are promoted; Mesa supplies
    `MapMemory` over `MapMemory2`.
- **Raised limits.**

  | Limit | Driver today | 1.4 requires |
  |---|---|---|
  | Image dimension 1D, 2D and cube | 4096 | 8192 |
  | Image dimension 3D | 256 | 512 |
  | Image array layers | 256 | 2048 |
  | Uniform buffer range | 16384 | 65536 |
  | Push constants | 128 | 256 |
  | Bound descriptor sets | 4 | 7 |
  | Per-stage uniform buffers | 12 | 15 |
  | Per-stage resources | 44 | 200 |
  | Set uniform buffers | 36 | 90 |
  | Set storage buffers | 12 | 96 |
  | Set storage images | 12 | 144 |
  | Fragment combined output resources | 4 | 16 |
  | Compute invocations | 128 | 256 |
  | Compute work group size | (128,128,64) | (256,256,64) |
  | Sub-texel and mipmap precision bits | 4 and 4 | 8 and 6 |
  | Sampler LOD bias | 2 | 14 |
  | Viewport dimensions | 4096 | 7680 |
  | Viewport bounds range | (-8192, 8191) | (-15360, 15359) |
  | Framebuffer width and height | 4096 | 7680 |
  | Colour attachments | 4 | 8 |
  | Point size range and granularity | wide points off | up to 256, granularity 0.125 |
  | Line width granularity | wide lines off | 0.5 |
  | Push descriptors | none | 32 |
  | Buffer and image granularity | reported | at most 4096 |
  | `timestampComputeAndGraphics` | off | on |
  | `standardSampleLocations` | unproven | on |
  | Float16 and float32 signed zero, inf and NaN preservation | unspecified | on |
- **Hardware unknowns.**
  - 8192 and 7680 targets: tile layouts and the 4 GiB window.
  - Eight colour attachments and independent blending: AGC's CB_COLOR1-7
    registers.
  - Timestamp queries, wide points and Bresenham lines, and YCbCr sampling.
  - Each is a runner probe before its step exposes it.
- **Delivered by.** This is the version the ladder exists for, and every group
  in it now has an owner:
  - `V4-window` (the shader-address window, the largest target that works, and
    two large resources coexisting — probed before the limits are advertised);
  - `V4-targets` (8192 and 7680 tile layouts, and the image, viewport and
    framebuffer limits those raise, on top of `V4-window`'s proven size);
  - `V4-attachments` (eight colour attachments and independent blending);
  - `V4-queries` (timestamp queries, `timestampComputeAndGraphics` and
    `standardSampleLocations`);
  - `V4-lines` (wide points, Bresenham lines, line and point granularity);
  - `V4-ycbcr` (sampling through a sampler YCbCr conversion);
  - `V4-copy` (host image copy, or a transfer-only queue family);
  - `Vshader` (the shader-side features: subgroup rotate, float controls
    2, expect-assume, pipeline robustness, 8- and 16-bit storage, variable
    pointers, int8/int16, and the remaining subgroup operations);
  - `V4-limits` (the raised descriptor, push-constant and resource limits).
  - C6, C7, C8, D1 and D5 carry the parts that are ordinary API rather than
    new hardware.

#### Version track

Each row is one claim: the version the device reports, the work that must be
proven on the console before the claim is true, who owns each part, and what
closes it. Steps named `V<n>-*` are the version-driven work the ladder implies;
the `C`, `D` and `E` steps keep their existing meaning.

| Claim | Must be proven first | Owner | Steps | Gate |
|---|---|---|---|---|
| 1.0 | **Closed** (rung 1.0, M5 D). `robustBufferAccess`; the drawing API; blits, resolves and copies; 4x multisampling; compute; queries; the reported limits; and every required format row -- 179 required, 58 reported, **0 missing a required feature**, 55 conditional, the blockers split 0 / 0 / 0 / 0 / 0 -- each proved on the console. The acceptance test was that `docs/V0_FORMATS_AUDIT.md` had no row left that was both unproved and probe-reachable; the descriptor-typed and vertex-format rows the pinned compiler could not express were closed by `tooling/psbc/patch-descriptor-types.py` and `patch-vertex-formats.py` instead of being parked | driver, console probes | C1-C5, C7, C8, D1, D2, `V0-compute`, `V0-query`, `V0-formats`, `V0-unknowns`, `V0-robust` | B2 asserts every requirement the device claims, then the CTS 1.0 subset (E1) |
| 1.0 (full) | **Closed with the row above**: the 38 descriptor rows and the 29 vertex-format rows the pinned compiler could not express are reported and proved, so no part of the required table is parked on a compiler gap | libpsbc patches, then driver and probes | `V0-formats`' remaining rows, D2's texel buffers | B2 asserts the whole required format table, then the CTS 1.0 subset |
| 1.1 | multiview; subgroups in compute; SPIR-V 1.1-1.3; the 23 promoted extensions | driver, Mesa, libpsbc | `V1-multiview`, `V1-promoted`, D1, D2 | B2 asserts the 1.1 requirements, then the CTS 1.1 subset |
| 1.2 | timeline semaphores; imageless framebuffers; standard uniform-buffer layout; subgroup extended types; separate depth/stencil layouts; host query reset; SPIR-V 1.4-1.5; the 24 promoted extensions | driver, Mesa | `V2-timeline`, `V2-promoted`, `V0-query`, C5, C8 | B2 asserts the 1.2 requirements, then the CTS 1.2 subset |
| 1.3 | dynamic rendering; synchronization2; maintenance4; inline uniform blocks; buffer device addresses; the shader feature set; SPIR-V 1.6; the 23 promoted extensions | driver, Mesa, libpsbc | `V3-bda`, `V3-promoted`, `Vshader`, C1, C3, D2 | B2 asserts the 1.3 requirements, then the CTS 1.3 subset |
| 1.4, the goal | the 42 required features, the raised limits, and every hardware unknown above | driver, libpsbc, console probes | `V4-window`, `V4-targets`, `V4-attachments`, `V4-queries`, `V4-lines`, `V4-ycbcr`, `V4-copy`, `Vshader`, `V4-limits`, C6-C8, D1, D5 | B2 asserts the 1.4 requirements, then the CTS 1.4 subset |

#### Version-driven steps

Each step below is a probe or a piece of plumbing the versions require. The
probes follow the Phase C workflow: a runner test written for the behaviour,
run with `capture`, whose golden frames then let the driver be checked on the
PC. Steps with a `V0-` prefix gate the *first* claim, so they belong on the
critical path rather than in Phase D.

- **`V0-compute`** — Does the console expose a compute path at all? Nothing has
  run compute on AGC, the modelled helper surface has no dispatch entry, and
  neither this project nor ps5-opengl names one. A parallel PS5 project
  (`mpereiraesaa/ps5-vulkan`, GPL-3.0-or-later, validated on firmware 12.02)
  met the same absence and hand-encoded the packets, so the shape below is a
  working recipe rather than a guess. Two stages:
  1. *Import existence.* One run has already closed the cheap part of this
     question: on 2026-09-16 the console would not `dlopen` a system module,
     and a linked-import build does not expose its imports to `dlsym`, so an
     AGC export inventory cannot be enumerated from a title
     (`HARDWARE_FINDINGS.md`). The only oracle is the loader resolving an
     import, which costs one build and one launch per batch and reports at
     most the first unresolved name. Since no dispatch entry appears in the
     recorded surface and the reference project reached the same conclusion,
     the probe goes straight to stage 2 rather than spending launches on an
     enumeration.
  2. *Dispatch probe.* One workgroup writes a known pattern into a buffer, in
     capture mode. Every packet is PM4 type 3, and the direct SH writes are
     `0xc0007600 | count << 16` followed by the register index
     `(reg - 0xb000) / 4`:
     - `0xb810` grid origin, `0xb81c` workgroup size,
       `0xb830` shader address (`>> 8`, `>> 40`),
       `0xb848` `COMPUTE_PGM_RSRC1/2` (VGPR count, float mode, user SGPR
       count, TGID enables), `0xb854` `COMPUTE_RESOURCE_LIMITS` (zero: the
       default limits), `0xb8a0` `COMPUTE_PGM_RSRC3`, `0xb900` user SGPRs —
       descriptor tables and push-constant pointer — plus the reference
       profile's `0xb858`/`0xb864` `COMPUTE_DESTINATION_EN_SE0..3`
       (`0xffffffff`, `0xffffffff`, `0`, `0`) and its zeroed `0xb890`
       `COMPUTE_USER_ACCUM_0..3`, which make the shader destinations and the
       accumulators explicit;
     - `0xc0031500` with the three grid dimensions and `0x8041`
       (`DISPATCH_DIRECT`; order mode, CS enable, wave32);
     - `0xc0004600` with `0x00000407` (`EVENT_WRITE`, `CS_PARTIAL_FLUSH`) to
       wait for the waves before the readback;
     - a completion write into a readback region, either `0xc0044000`
       (`COPY_DATA`, ME register to L2) or `0xc0064900` (`RELEASE_MEM`).
     Every address in one dispatch — shader, descriptor tables, push constants,
     readback, completion — must share the compiler's high word and stay inside
     that 4 GiB window, which is why `V4-window` answers for this step too.
  3. *Compute resource metadata.* A dispatch programs `COMPUTE_PGM_RSRC1/2`
     itself, and the pinned compiler kept those words, the shader's VGPR and
     SGPR counts and its LDS size inside ACO's `ac_shader_config` without
     exporting them. The archive this repository builds exports them for
     compute stages (`tooling/psbc/patch-compute-metadata.py`, applied by
     `tools/build-psbc-ps5.sh`), so the probe dispatches
     with the compiler's values rather than guessed ones — the failure mode
     being the unmapped SGPR that faulted B8's indirect buffer. The reference
     project derives VGPRs from `rsrc1`'s granule field and bounds SGPRs
     conservatively for the same reason.
     The fields are appended to `PsbcShaderMetadata`, which every consumer
     fills by value, so the header a title compiles against has to match the
     archive it links — `driver/Makefile` names the header as a prerequisite
     for that reason. The metadata version stays 8 and the patch defines
     `PSBC_SHADER_METADATA_COMPUTE_RSRC` instead: ps5-opengl's package writer
     compares the version against its own, unpatched copy of the header and
     would refuse every package a bumped one produced. The host runner builds
     against the SDK's own header and archive and refuses the dispatch; the
     console runner builds against the installed patched header and runs it.
     The probe payload is `probes/c0/dispatch.spv` from
     `tools/build-compute-probe.sh`: one workgroup writes one known word into
     one storage buffer, so a pass proves the ISA, the packet, the descriptor
     table, the CS partial flush and the readback together.
     Compiling that payload with the patched compiler reports, for
     `PSBC_STAGE_COMPUTE`: `rsrc1` `0x602c0001`, `rsrc2` `0x00000006`, `rsrc3`
     `0`, 16 VGPRs, 108 SGPRs (informational: GFX10's `COMPUTE_PGM_RSRC1` has
     no SGPR field for this ABI, which is why the reference project bounds
     SGPRs instead of programming them), LDS 0, `user_sgpr_count` 3, the
     descriptor table in user-data dword 2, and 56 bytes of machine code. The
     dispatch programs those words verbatim. The binding has to be declared:
     compiled undeclared, ACO stores through a null descriptor and the write
     disappears without a fault. Declared as a storage buffer with stride 16 —
     the descriptor's own size in the table; 0, 4, 8, 24 and 32 all fail with
     compiler result 6 — it emits `s_mov_b32 s3`, the compiled
     `address32_hi`, `s_load_dwordx4 s[0:3], s[2:3], <table byte offset>` and
     the store through the descriptor the probe writes at the offset the
     compiler reports for the binding.
     The PC runner builds the same stream from the same payload: it compiles
     the SPIR-V with ps5-opengl's own libpsbc, takes the four resource words
     the SDK archive cannot report from `probes/c0/resources.txt`
     (`tools/build-compute-probe.sh` writes them with the patched compiler),
     and programs everything else itself. The packets it writes by hand are
     recorded as one trace span named `raw_packets`, which `tools/golden.py`
     compares word for word instead of asking a model for them. Its readback is
     NOT_REQUIRED: the host layer completes a submission's marker but runs no
     workgroup, so the console's readback is the record and the PC's job is the
     stream. `golden/c0` is the frame that ties the two together.
  - *Acceptance.* The console readback equals the pattern exactly; the stream
    is captured as golden frames; `golden.py rebuild` reproduces them on the
    PC; `agc_rules.py` passes on them; and `HARDWARE_FINDINGS.md` records
    which entry point or raw packet form works. D2's Vulkan compute then has a
    proven base rather than an assumption.
- **`V0-query`** — The query path, which 1.0 requires and nothing has
  recorded. The only GPU-to-CPU signal proven so far is a `RELEASE_MEM`
  completion marker, which is a write rather than a counter.
  - *Acceptance.* A runner probe reads an occlusion query back after a known
    drawn region and a known masked region, exactly; the query path is
    captured as golden frames and modelled on the PC. Timestamps are probed in
    the same step: the probe asks whether the console exposes a readable GPU
    timestamp, which `timestampComputeAndGraphics` and `V4-queries` need, and
    records the answer — it does (pid 143: a 100 MHz clock), so the driver's
    `vkCmdWriteTimestamp` records it (pid 144), so the feature is reported on.
- **`V0-formats`** — The required format tables. An audit of
  `formats-v1.4.354.adoc` against what the driver reports and can store,
  kept in `docs/V0_FORMATS_AUDIT.md` and reproduced by
  `python3 tools/format_audit.py` (179 formats required, 22 reported, 53
  missing a required feature after V0-formats' and C7's format work, the last
  two closed by V0-formats' integer vertex probe):
  every required format must either carry the correct feature bits or be
  recorded as a deliberate gap with its tile-layout reason. A sampled format's
  descriptor is the pair of the FORMAT field's `GFX10_FORMAT_*` word and word
  3's DST_SEL channel selectors, which carry Vulkan's fill-in rule for the
  channels a format does not have (`docs/HARDWARE_FINDINGS.md`). A view's
  component mapping composes with those selectors on top of that rule, and no
  probe has recorded the composition: the driver refuses a view whose mapping is
  not the identity where a combined image sampler reads it, by name
  (`driver/ps5vk_draw.c`, docs/M5_PHASE_C.md), and the step's remaining work is
  the probe frame that measures a mapped view's selectors the way the format
  probe measured each format's. Inputs from the
  reference project: 41 hardware-tested texel-buffer formats, 44 sampled
  formats with readback, `conformance_inventory/physical_format_validation.json`
  and `PHYSICAL_DEVICE_REPORTING.md` as a model for the reporting audit below.
- **`V0-unknowns`** — The remaining hardware unknowns, measured in **batched
  probe batteries**: one queue file, one deploy and one console run carrying many
  questions, each question its own case with its own logged verdict, so the
  discovery latency of a step stops being round-shaped. A battery claims no
  capability: its output is a `docs/FINDINGS.md` entry per answer with its
  boundary, and the driver work the answers unblock follows in its own commits.
  **Battery 1** (`jobs/unknowns/queue.txt`) asks the six questions the rung
  still owes:

  | Case to write | The question | Probe shape | Answer means |
  | --- | --- | --- | --- |
  | `unknowns-array-field` | **confirms** the array field the register database names: `DEPTH = layers - 1` at word 4 bits 0-12, `BASE_ARRAY` at bits 16-28, `TYPE` 13 (docs/HARDWARE_FINDINGS.md) | a two-layer tiled image, two colours, sampled by a `sampler2DArray` shader that fetches layer 1 | D1's arrays: the reference is the hardware's, or the descriptor needs the 2D kind after all |
  | `unknowns-cube-field` | **confirms** `TYPE` 11 with the same six-slice storage a `samplerCube` shader samples along one axis | a six-layer tiled image | D1's cubemaps |
  | `unknowns-depth4x-map` | a four-sample depth target's storage layout, as C8 measured the colour one | render the m4-depth canary into a 4x depth attachment and walk the storage the way `agc_resolve_map` did | C8's depth 4x: the tile/plane map, or the candidate list it is not |
  | `unknowns-depth4x-rects` | the four-sample depth map itself: which element a texel's sixteen bytes sit in | nested strips at one origin (1x2 up to 1x64, 2x1 up to 64x1, each drawn with a depth above the strip inside it), a 1x8 and an 8x1 strip per low bit, and 64x64-texel tile probes, all with their element sets in the log | C8's depth 4x: sixteen-byte texels, the 64-texel tile grid, and the element masks `ps5vk_image.c` refuses to guess (docs/HARDWARE_FINDINGS.md) |
  | `unknowns-packed-fetch` | **confirms** each packed format's `GFX10_FORMAT` word (`A1R5G5B5` 134, `R5G6B5` 133, `B4G4R4A4` 136, `E5B9G9R9` 132, `B10G11R11` 36, `A2B10G10R10` 50/54) and measures the DST_SEL fill-in its decode needs | the registered formats' own probe shape: a solid texture of the format, a hand-written descriptor, the decoded colour read back | each format's sampled fetch, which is what its `SAMPLED_IMAGE` row waits for |
  | `unknowns-packed-blit-dst` | what the resampler must write into those formats as a blit destination | blit each format's solid texture into a tiled destination of the same format, read the bytes back | the write path per format, which is what the `BLIT_DST` rows wait for |
  | `unknowns-depth-words` | the `DB_Z_INFO` format words for `D16_UNORM` and the stencil formats, and the stencil registers | the C5 probe's shape with each format's candidate word and the stencil write/read registers | the `DEPTH_STENCIL_ATTACHMENT` rows |

  Risk and cost: every case above is one draw and one readback, so the battery is
  one console session however many questions it carries. The cases must be in the
  runner's table before the queue names them
  (`tests/test_tools.py`, `test_every_queue_case_is_a_runner_case`), which is why
  `jobs/unknowns/queue.txt` keeps them as comments until they are written.
- **`V1-multiview`** — Layered targets and a view index in shaders, probed
  before 1.1 exposes multiview.
- **`V1-promoted`** — The 23 extensions 1.1 promotes: Mesa's half arrives with
  the runtime, the driver adds bind memory 2, dedicated allocation and
  maintenance1-3, libpsbc adds variable pointers. Acceptance: B2's 1.1
  assertions pass and the CTS 1.1 subset is clean.
- **`V2-timeline`** — A timeline variant of the CPU sync type over Mesa's
  `vk_sync`, which 1.2 requires. Acceptance: timeline semaphore waits resolve
  in order on the console and the PC.
- **`V2-promoted`** — The 24 extensions 1.2 promotes, including depth/stencil
  resolve (C8), separate stencil usage, image format lists and mirror-clamp.
- **`V3-bda`** — Buffer device addresses: shaders dereference buffer
  addresses, and the compiler fixes the upper address word, so every buffer
  must stay in the window `V4-window` characterises. Probe before 1.3 claims
  it.
- **`V3-promoted`** — The 23 extensions 1.3 promotes, including the extended
  dynamic state setters, which the driver must honour, and the copy commands 2.
- **`Vshader`** — The shader-side features the ladder needs, and the only step
  whose owner is the pinned compiler rather than the driver: robustness
  lowering, 8- and 16-bit storage, int8/int16, subgroup operations and rotate,
  float controls 2, expect-assume, terminate/demote/zero-initialize workgroup
  memory, integer dot product, full subgroups. Acceptance per feature: a
  shader that uses it compiles through libpsbc, and B6's byte-identity check
  still holds for the packages the probe sets already produce.
- **`V4-window`** — The shader-address window and large allocations, probed
  before 1.4's limits are advertised. Three questions, in this order:
  1. *Is the window global or per resource?* Read the compiler's address-high
     handling and the descriptor forms. If the high word is fixed for the
     shader, every shader-visible resource must fit one 4 GiB window, which
     caps several 1.4 limits.
  2. *Does a 1.4-sized target work?* The allocator half is answered: the pool
     is 12 GiB and served every request up to 1,061,683,200 bytes on
     2026-09-16 (`HARDWARE_FINDINGS.md`). What remains is rendering: allocate,
     render and read back an 8192-wide RGBA8 attachment exactly. If the tile
     layout or the window refuses it, record the largest width that works and
     why.
  3. *Do two large resources coexist?* One draw that reads an 8K attachment
     and writes another, both inside the window.
  - *Acceptance.* The probed size reads back exactly; the two-resource draw
    reads back exactly; the largest supported size, the allocation limits and
    the global-versus-per-resource answer are recorded in
    `HARDWARE_FINDINGS.md`; and the 1.4 limits table's image, framebuffer,
    array-layer and attachment rows carry the proven values, with every
    remaining row marked capped and given the reason.
- **`V4-targets`** — Tile layouts at 8192 and 7680 and the limits they raise,
  using what `V4-window` proved about size and the window.
- **`V4-attachments`** — Eight colour attachments and independent blending:
  AGC's `CB_COLOR1`-`CB_COLOR7` registers, probed before 1.4 exposes them.
- **`V4-queries`** — `timestampComputeAndGraphics` and
  `standardSampleLocations`, on top of `V0-query`'s timestamp finding, which
  found a readable 100 MHz clock (pid 143): the row is not capped.
- **`V4-lines`** — Wide points, Bresenham lines, and their granularities.
- **`V4-ycbcr`** — Sampling through a sampler YCbCr conversion.
- **`V4-copy`** — Host image copy, or a transfer-only queue family, whichever
  the driver can prove; 1.4 requires one of them.
- **`V4-limits`** — The raised descriptor, push-constant and resource limits
  that do not depend on the window or on new hardware.

#### Implementation probes

Two pieces of plumbing are worth probing now, because both replace machinery
the ladder's later steps would otherwise build on.

- **`P-agc-shaderlink`** — AGC's own shader creation and linking,
  `sceAgcCreateShader` and `sceAgcLinkShaders`, as an alternative to the
  current path (libpsbc emits, ps5-opengl's writer packages, the driver
  hand-builds the CX/UC state, and a driver copy renames five runtime stubs).
  Both entry points are known to resolve on hardware: the reference project
  calls them for every pipeline it builds.
  - *Acceptance.* A probe set renders identically through the AGC link path;
    the resulting CX/UC state is compared with the hand-built state on the PC;
    B6's package byte-identity gate still holds, or the difference is
    documented with its reason and its effect on the recorded digests; and the
    linker's interpolation and export state is never silently replaced — the
    reference project copies `spi_ps_input_cntl` back after linking for exactly
    that reason.
- **`P-flip-event`** — Replace the present path's polling loop
  (`sceVideoOutGetFlipStatus` for up to 200 vblanks) with a flip event queue:
  `sceVideoOutAddFlipEvent` plus `sceKernelCreateEqueue` and
  `sceKernelWaitEqueue`, which is how the reference project waits for its
  flips, and the primitive a FIFO swapchain should use.
  - *Acceptance.* The C1 triangle still presents 4 of 4 frames; the emitted
    flip packets and `golden/c1` stay byte-identical; an out-of-date swapchain
    still returns `VK_ERROR_OUT_OF_DATE_KHR`; and teardown drains pending flip
    events rather than counting poll iterations.

**Ownership.** Three owners recur, and a claim is blocked by the slowest of
them:

- **Mesa's common runtime** supplies entry points and plumbing: properties2,
  memory requirements2, device groups of one device, descriptor update
  templates, external fence and semaphore capability queries, createRenderPass2,
  imageless framebuffers, timeline semaphore calls over `vk_sync`,
  `vkQueueSubmit2`, maintenance4-6 plumbing, private data, pipeline creation
  cache control, inline uniform block storage, the extended dynamic state
  setters, the copy commands 2, and dynamic rendering, which the driver
  already renders through.
- **The driver** owns every part that reaches the GPU: bind memory 2,
  dedicated allocation, maintenance1-3 semantics, block layouts, YCbCr
  conversion objects, depth/stencil resolve, separate stencil usage, image
  format lists, mirror-clamp, queries and query reset, buffer device
  addresses, driver-side robustness, the sample counts it reports, and the
  limits it advertises.
- **libpsbc**, the pinned shader compiler, owns the shader-side features:
  bounds and image robustness lowering, variable pointers, 8- and 16-bit
  storage, int8/int16, subgroup operations and their extended types, subgroup
  rotate, float controls 2, expect-assume, terminate/demote/zero-initialize
  workgroup memory, integer dot product and full subgroups. If the pinned
  compiler cannot express one of these, the claim is blocked on SDK work
  rather than on the driver, and that is a different schedule.

**Advertised values.** Every value the physical device publishes is a promise
to applications and to the CTS. Each row below names the step that proves it;
a value with no step is lowered until one exists. The reference project's
`PHYSICAL_DEVICE_REPORTING.md` is the model: it reports only what its frontend,
allocator and encoders can execute, and says so.

| Advertised value | Step that proves it |
|---|---|
| `maxImageDimension2D`, `maxFramebufferWidth/Height` 4096 | C2-C5 (the 4K targets). Raising to 8192 is `V4-targets`, after `V4-window` |
| `maxImageArrayLayers` 256, `maxFramebufferLayers` 256 | D1 (arrays). 2048 is a 1.4 limit |
| `maxColorAttachments` 4 | C1-C5. Eight is `V4-attachments` |
| `maxComputeWorkGroupInvocations` 128, count 65535, size 128x128x64, shared memory 16 KiB | `V0-compute` — nothing has run compute yet, so these are currently unproven |
| `maxPushConstantsSize` 128 | C3. 1.4 raises it to 256, so `V4-limits` |
| `maxBoundDescriptorSets` 4, `maxPerStageResources` 44, per-stage UBO 12 and the rest | C3 and D1. 1.4 raises sets to 7 and per-stage resources to 200 (`V4-limits`) |
| Sample counts 1 and 4 for colour, depth, stencil and sampled images | C8 |
| `timestampComputeAndGraphics` | on since V0-query recorded the clock and the driver implemented `vkCmdWriteTimestamp` (pids 143 and 144); the limits report it true with a 10 ns period |
| `standardSampleLocations` off | C8; 1.4 requires it on, so `V4-queries` |
| `pointSizeRange` and `lineWidthRange` 1 to 1 | `V4-lines` raises them for 1.4 |
| `maxSamplerLodBias` 2.0, `maxSamplerAnisotropy` 1.0 | C4 for the LOD bias. Anisotropy is accepted at the reported 1.0 and is a no-op there; above it the sampler is refused by name, and `samplerAnisotropy` stays FALSE (`v0-sampler-anisotropy`) |
| `maxViewports` 1, `viewportBoundsRange` -8192 to 8191 | C2 and C5. 1.4 raises the bounds range to 15360 |
| `nonCoherentAtomSize` 256, buffer/image granularity 131072 | C1-C5, D1 |

**Order.** The ladder follows the Phase C/D steps rather than replacing them:
- 1.0 is closed: it was reported before it was earned, so it was a debt rather
  than a rise, and C1-C5, C7, C8, D2 and the four `V0-` probes are all proven;
- 1.1 and 1.2 follow with little new hardware: multiview, timeline sync and
  the depth/stencil work;
- 1.3 and 1.4 carry the larger unknowns: buffer device addresses, the
  compiler's robustness and shader features, 8K targets, eight attachments and
  timestamps.

Each rise is logged in `docs/M5_PHASE_*.md` with its CTS subset result, and the
current position in the track is in `docs/VULKAN_PROBE_ACTIVE.md`.

### Accelerator details

A development accelerator, golden command streams, is built before and
alongside Phase A so most mistakes surface on the PC:
1. A klog parser and PM4 decoder that names registers from Mesa's
   `amdgfxregs.h` (context register `n` is `0x28000 + 4n`). Built:
   `tools/ps5vk_log.py` and `tools/pm4_decode.py`.
2. A runner capture mode that logs whole command streams, register-table
   contents, user data, descriptors and every AGC helper call. One runner
   launch yields the golden files. Built: the queue keyword `capture`, which
   logs each frame's helper calls, whole stream and non-zero workspace
   chunks with the workspace FNV-1a 64. `tools/golden.py extract` checks
   every capture and writes `golden/runner/<test>-<n>.json`.
3. The frame encoder split from device glue so it builds on the PC. Host
   models reproduce the AGC helpers whose output is known, and captures
   replay the opaque ones (shader link, wait-until-safe, register defaults).
   Streams are compared with addresses expressed as region plus offset.
   Started:
   - `src/agc_abi.hpp` declares the AGC types and command helpers for both
     builds.
   - `host/agc/agc_host.cpp` models all twelve helpers the frames call. The
     wait-until-safe packet proved modellable.
   - `bash tools/build-agc-host.sh` builds the models in seconds, and
     `python3 tools/golden.py check-helpers golden/runner` replays every
     recorded call through them.

   Whole-frame comparison is built, without splitting the encoder. The
   console runner's own source builds for the PC:
   - `bash tools/build-host-runner.sh` compiles `src/diagnostics.cpp` and
     `src/probe_pack.cpp` with the runner's definitions against
     `host/ps5/ps5_host.cpp`, the PS5 system calls on Linux, and the helper
     models.
   - `python3 tools/golden.py rebuild golden/runner` runs each captured test
     through that PC runner in capture mode, using the tests' real scene
     setup. Memory is mapped at the captured addresses, and the inputs the PC
     cannot compute are replayed from the golden file. Every frame is then
     compared with its golden file.
4. Checks that encode each hardware finding: the wait packet names the flipped
   buffer, COMP_SWAP matches the target, pixel exports match the target
   format, register tables lie inside the workspace, descriptor addresses use
   the compiled high word and a zero low byte, a barrier precedes sampling a
   target drawn in the same frame, and depth targets carry all 16 registers.
   Each check is proven on a deliberately broken stream. Built:
   - `python3 tools/agc_rules.py PATH...` checks golden files, console klogs
     and PC runner klogs against eight rules. `--list` shows each rule with
     the finding behind it:
     - `wait-buffer`
     - `flip-target`
     - `scanout-swap`
     - `blend-export`
     - `tables-in-workspace`
     - `index-buffers`
     - `barrier-before-sampling`
     - `depth-target`
   - The checks replay each frame's register state packet by packet. Tables
     come from the captured workspace and direct SH writes from the stream.
     Rules that need tables or descriptors are skipped for streams without a
     workspace image.
   - `python3 tools/test_agc_rules.py` requires every golden frame to pass
     every rule. It also requires each rule to fire, alone, on a golden frame
     broken as the console once failed: a wait naming the other buffer,
     drawing into the unflipped buffer, SWAP_STD scanout, a 32_ABGR blend
     export, a table below the workspace, indices outside mapped memory, the
     render-to-texture barrier turned into a NOP, and a missing depth-target
     register.

   The descriptor address rule (compiled high word, zero low byte) cannot be
   read from a stream: the descriptor drops the low byte. The runner enforces
   it when it places resources (the `agc_*_addresses` checks).
5. From Phase B, Vulkan programs run on the PC through the driver and the host
   AGC model and are compared with the golden streams, with reference images
   from lavapipe.
