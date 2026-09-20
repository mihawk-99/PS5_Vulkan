# Probe milestones M1-M4

What each canary title proves, how it is built, and its pass criteria, plus the
console test runner and its tooling. Top-level plan:
[VULKAN_PROBE_PLAN.md](VULKAN_PROBE_PLAN.md). Current state and next actions:
[VULKAN_PROBE_ACTIVE.md](VULKAN_PROBE_ACTIVE.md).

_Reference file: read it when working on these canaries or the runner._

## Milestone 1

The application serves the in-memory JSON Lines log at `http://<PS5-IP>:<port>/log`.
It tries port `8080` first and automatically falls back through `8081`–`8099`
if the port is already in use; the selected port is displayed in the banner.
Every JSON line is also written to the kernel log through
`sceKernelDebugOutText` with a `[PS5VK] ` prefix, so records written before a
process or GPU fault remain visible in klog next to the crash report.

At startup the stable host loads packaged probe definitions from
`/app0/probes/`, then applies optional overrides from `/download0/probes/`, and
otherwise uses built-in safe defaults. `/data/homebrew/PPSA99999` is the loader
staging location and is not a title runtime data mount. `modules.json` selects
up to three module candidates;
`memory-tests.json` selects bounded allocate/map/write/unmap/release tests.
Invalid definitions are skipped and never enable GPU submission.
`/health` is a simple connectivity check. The listener is read-only and does
not require filesystem access.
It covers AGC and AGC-driver module loading, symbol resolution, direct-memory
allocation/mapping/write/unmap/release, register-default and wait-packet
discovery, and private AGC command encoding. Shader execution and GPU
submission are explicitly recorded as `SKIPPED`.

## Milestone 2: own shaders at native 4K

M1 (`PPSA99996`) drew with ProsperoLight's prebuilt shader machine code. M2
(`PPSA99995`, "PS5 Own Shader 4K Canary") draws with shaders compiled from
source in this repository, the path a Vulkan frontend will use for SPIR-V:

1. `shaders/m2/fullscreen.vert` emits one triangle covering the whole target
   from `gl_VertexIndex`, and `shaders/m2/solid.frag` writes one colour
   (R=0x20, G=0xA0, B=0xFF, A=0xFF).
2. `tools/build-probe-shaders.sh m2` runs GLSL -> SPIR-V (glslang, Vulkan 1.2) ->
   NIR/ACO (opengnm-psbc) -> AGC shader ELF (ps5-opengl's package writer) and
   writes `probes/m2` with checksums and a provenance receipt.
3. The title creates and links the packages (primitive type 4), then submits
   one frame in ps5-opengl's triangle order: wait packet, CX (target,
   viewport/scissor, link and both shaders), UC, SH, `DRAW_INDEX_AUTO` with 3
   vertices, flip. It targets two 3840x2160 SDR framebuffers of `0x2000000`
   bytes each (`AGC_OUTPUT_4K`). Framebuffer 0 is a `B8G8R8A8_UNORM` target
   (`CB_COLOR0_INFO` COMP_SWAP = SWAP_ALT), the byte order VideoOut scans out.

Pass criteria, all measured from the CPU readback of framebuffer 0: all
8,294,400 target pixels read back as word `0xff20a0ff` (`agc_solid_coverage`,
`agc_solid_colour`), and the tiled padding outside the target stays zero
(`agc_solid_padding`: the non-zero word count over the whole allocation equals
the drawn count). Anything short of exact coverage fails; the first undrawn
and first mismatching pixel are logged. The build also logs the AGC defaults
of the screen, window, generic and viewport scissor registers
(`agc_clip_default_*`) to explain M1's undrawn row 1079 if it recurs.

## Milestone 3: GPU resources

M3 feeds shaders data the CPU prepares, one resource kind per build, each
with an exact readback. Step 1, a uniform buffer, is `PPSA99994`
("PS5 Uniform Buffer 4K Canary", `AGC_UNIFORM_BUFFER_CANARY`):

1. `shaders/m3/uniform_colour.frag` returns the `vec4` in a uniform buffer at
   set 0, binding 0; the vertex stage reuses M2's full-target triangle.
2. `tools/build-probe-shaders.sh m3-uniform` compiles both stages with `--address32-hi 2`
   (shaders combine 32-bit pointers with that high word) and declares the
   binding as `0:0:uniform_buffer:1:0:16`. It checks the compiler metadata and
   writes `probes/m3/bindings.txt` (address high word, pixel user-data count,
   descriptor-set dword, binding offset and stride), so the title takes the
   layout from the build instead of hard-coding it.
3. The title places a descriptor-set table at workspace `0xc000` and the
   16-byte buffer at `0xc100`, and refuses to draw unless both addresses have
   the compiled high word. The binding-0 descriptor uses ps5-opengl's
   hardware-run stride form: buffer address low, address high with element
   stride 16 in bits 16-31, element count 1, flags `0xfac | 77 << 12`. The
   table's low 32 bits go into the pixel user-data dword named by
   `bindings.txt`, written with `SET_SH_REG` at SH `0x0c` after the SH table.
4. Frame 0 writes pink (R=0xFF G=0x40 B=0x80) into the buffer and draws into
   buffer 0 with flip marker `0x5053564b`; frame 1 rewrites the same buffer
   with green (R=0x30 G=0xD0 B=0x60) and draws into buffer 1 with marker
   `0x5053564c`. Descriptor and user data stay unchanged between frames.

Pass criteria: each frame's 8,294,400 pixels equal its colour word
(`0xffff4080`, then `0xff30d060`) with exact coverage and empty padding
(`agc_solid_*` with `frame` 0 and 1), and `agc_uniform_buffer` reports 2 of 2.
A constant or stale colour cannot pass both frames, so the pair proves the
shader read the buffer's current contents through the descriptor.

Step 2, vertex and index buffers, is `PPSA99993` ("PS5 Vertex Buffer 4K
Canary", `AGC_VERTEX_BUFFER_CANARY`):

1. `shaders/m3/vertex_colour.vert` reads a `vec2` position and a `vec4`
   colour from 24-byte vertex records, and `vertex_colour.frag` writes the
   interpolated colour. `tools/build-probe-shaders.sh m3-vertex` declares the
   attributes (`r32g32_float` at offset 0, `r32g32b32a32_float` at offset 8,
   stride 24) and records the vertex user-data layout in `bindings.txt`: the
   vertex-buffer table, base-vertex and NGG LDS layout dwords, and the LDS
   layout value. The LDS layout tells the pixel stage where the vertex outputs
   are; M2's pixel shader read none, so leaving it zero went unnoticed there.
2. The title writes four vertices of a square covering the middle quarter of
   the target (NDC ±0.5: pixels 960-2879 x 540-1619 at 4K) and the index list
   `0,1,2, 2,3,0`. A vertex-buffer table at workspace `0xc000` holds one
   stride-form descriptor, as ps5-opengl's `ps5_vertex_buffer_descriptor`
   builds it: address low, address high | stride 24 << 16, vertex count 4,
   flags `0x5204`. Its low 32 bits go into the vertex-buffer table dword,
   beside base vertex 0 and the LDS layout, all written with SET_SH_REG at SH
   `0x8c` as ps5-opengl's gallium draw fills them.
3. The draw follows ps5-opengl's indexed quad: 16-bit index size, index
   buffer, index count 6, then `sceAgcDcbDrawIndex`.

Every corner has red 0x80 and alpha 1. Green is 0 on the left and 1 on the
right, blue 0 at the top and 1 at the bottom, so both triangles share one
plane per channel. Pass criteria:
- exactly the 2,073,600 square pixels are drawn and nothing outside them
  (`agc_vertex_coverage`);
- every square pixel has red 0x80, alpha 0xff, and green and blue within one
  level of the value at its pixel centre (`agc_vertex_colour`,
  `agc_vertex_gradient`);
- the tiled padding stays empty (`agc_vertex_padding`).

A missing index buffer, a wrong stride or swapped attributes breaks the edges
or the gradients.

Step 3, sampled textures, is `PPSA99992` ("PS5 Texture 4K Canary",
`AGC_TEXTURE_CANARY`):

1. `shaders/m3/texture.vert` passes a texture coordinate from 16-byte vertex
   records, and `texture.frag` samples a `sampler2D` at set 0, binding 0.
   `tools/build-probe-shaders.sh m3-texture` declares the attributes and a
   48-byte combined image sampler (`0:0:combined_image_sampler:1:0:48`), and
   records both stages' user-data layouts in `bindings.txt`.
2. The title reuses the indexed square's vertex input and uploads a 64x36
   RGBA8 texture at workspace `0xd000`. That is a 256-byte boundary, which
   the descriptor needs because it stores the address shifted right by 8.
   Every texel is distinct: red is the column x 4, green the row x 7, and
   blue a 0x20/0xe0 checker. Each texel covers exactly 30x30 target pixels,
   and 64-texel rows need none of the 256-byte row padding that linear 2D
   storage uses.
3. The combined image-sampler descriptor follows ps5-opengl's gallium
   runtime for a single-level, untiled 2D `R8G8B8A8_UNORM` texture:
   - word 0: address >> 8
   - word 1: format `0x03800000` | ((width-1) & 3) << 30 | address >> 40
   - word 2: (width-1) >> 2 | (height-1) << 14 | resource-level bit 31
   - word 3: 2D identity swizzle `0x90000fac`
   - word 5: single level, `0x00400000`
   - word 8: clamp to edge, `0x92`
   - word 9: LOD range `0x00fff000`
   - word 10: the sampler word

   The table's low 32 bits go into the pixel user-data dword (SH `0x0c`).
4. Frame 0 samples nearest (`0x08000000`) into buffer 0. Frame 1 rewrites
   only the sampler word to bilinear (`0x09500000`) and draws into buffer 1.

Pass criteria:
- In both frames, the square's exact extent and empty padding.
- Nearest reproduces every texel exactly.
- Bilinear matches the clamp-to-edge float blend at each pixel centre
  within two levels per channel. Hardware filter weights are fixed-point,
  so the error histogram is logged.
- `agc_texture` reports 2 of 2.

## Milestone 4: render state

M4 adds the state a real scene needs beyond one shape, one build per step:
clear and depth testing, then blending, then offscreen rendering.

Step 1, clear and depth testing, is `PPSA99991` ("PS5 Depth 4K Canary",
`AGC_DEPTH_CANARY`):

1. `shaders/m4/depth_colour.vert` reads a `vec3` position and a `vec4` colour
   from 28-byte records. The M3 `vertex_colour.frag` is reused, and
   `tools/build-probe-shaders.sh m4-depth` records the vertex user-data
   layout.
2. A tiled D32F depth buffer (`0x2000000` bytes, the colour target's size) is
   bound with ps5-opengl's 16 depth-target registers:
   - `DB_Z_INFO` `0x80000183`
   - stencil disabled
   - Z read and write base: address >> 8, plus high byte >> 40
   - size `(W-1) | (H-1) << 16`
3. Before the frame every colour pixel holds the sentinel `0xff804080` and
   every depth sample 0.0, so nothing could pass without a working clear.
4. The frame draws, from one vertex and one index buffer, with a `0x200`
   depth-control table before the first and second draws:
   - a full-target clear triangle at z = 1 with ALWAYS (`0x76`)
   - then LESS (`0x16`) for four constant-colour rectangles at depths 0.25,
     0.75, 0.5 and 0.375
   - the left pair draws near first, so the far rectangle must be rejected in
     the overlap
   - the right pair draws far first, so the near rectangle must overwrite it
5. Every edge is on a pixel boundary, and every depth is an exact binary
   fraction in [0, 1].

Pass criteria:
- No sentinel survives (`agc_depth_clear`).
- Every pixel shows its nearest shape's exact colour (`agc_depth_colour`).
- Every depth sample holds that shape's exact float depth, read through
  ps5-opengl's tiled depth offsets (`agc_depth_values`).
- Neither buffer holds data outside its tiled pixels (`agc_depth_padding`).

Step 2, blending, is `PPSA99990` ("PS5 Blend 4K Canary", `AGC_BLEND_CANARY`).
It uses the M4 rectangle geometry and the `m4-blend` shader set. That set is
the `m4-depth` shaders with the pixel stage compiled for FP16_ABGR colour
exports (`--color-format 0x99999994`, the export Mesa chooses for an 8-bit
UNORM target, as ps5-opengl's gallium runtime passes it).

`CB_BLEND0_CONTROL` (`0x1e0`) is encoded as ps5-opengl's gallium runtime does:
- enable bit 30
- colour source factor | function << 5 | colour destination factor << 8
- when alpha differs: bit 29, alpha source << 16, alpha destination << 24

The factors used are ONE = 1, SRC_ALPHA = 4 and INV_SRC_ALPHA = 5; the
function is 0 (add). ps5-opengl's hardware-run words are reproduced exactly:
`0x40000504` for transparency and `0x40000101` for additive.

Each draw also writes `CB_COLOR_CONTROL` (`0x202`) = `0x00cc0011`, as
ps5-opengl's gallium runtime does for every draw: normal mode, copy ROP3, and
bit 0, which it sets to keep RB+ blend optimisation off. The title also logs
the AGC defaults of `0x1e0`, `0x202`, `0x105`-`0x108` and `0x1d5`-`0x1d8`
(`agc_blend_default_*`).

The frame draws five rectangles, each with its own blend word:
1. Left half, opaque (blend `0`): (200, 50, 25).
2. Right half, opaque: (25, 100, 225).
3. Row 1, over (`0x65010504`): colour SRC_ALPHA / INV_SRC_ALPHA, alpha ONE /
   INV_SRC_ALPHA. Yellow (255, 255, 0) at alpha 153 gives (233, 173, 10) on
   the left and (163, 193, 90) on the right.
4. Row 2, additive (`0x40000101`): (100, 50, 200) gives (255, 100, 225)
   with red saturated, and (125, 150, 255) with blue saturated.
5. Row 3, premultiplied (`0x40000501`): (0, 102, 102) at alpha 102 gives
   (120, 132, 117) and (15, 162, 237).

Colours are multiples of 5 and alphas multiples of 51, so every result is an
exact 8-bit level, and target alpha stays 255.

Pass criteria:
- Every pixel is within one level per channel of the exact blend, computed
  per pixel in draw order (`agc_blend_colour`, with an exact-count histogram).
- No data lies outside the tiled pixels (`agc_blend_padding`).

Step 3, offscreen rendering, is `PPSA99989` ("PS5 Render To Texture 4K
Canary", `AGC_RENDER_TO_TEXTURE_CANARY`). It follows ps5-opengl's
hardware-run render-to-texture test and reuses the `m3-texture` shaders for
both passes.

1. The first pass draws into an offscreen tiled RGBA8 target the size of the
   framebuffer, in standard byte order (COMP_SWAP 0, like a Vulkan
   `R8G8B8A8_UNORM` attachment):
   - a full-target quad whose texture coordinates all name the centre of
     texel (0, 0), as background
   - the M3 64x36 texture across the middle quarter, sampled nearest
2. `sceAgcCbReleaseMem(cmd, 45, 12, 1, 0, NULL, 0, 0, 0, 1, 0, 0)` flushes
   and invalidates colour-buffer data (event 45).
3. An indirect CX table then re-aims the target, viewport and scissor
   registers at the framebuffer (COMP_SWAP 1). New pixel user data points the
   descriptor-set dword at a descriptor for the offscreen image: word 3
   `0x91b00fac` for a tiled render target, size 3840x2160.
4. The second pass draws a full-target quad that samples the offscreen image
   mirrored left to right, nearest, one texel per pixel.

Pass criteria:
- Every offscreen pixel holds its texel in R,G,B,A byte order
  (`agc_rtt_offscreen`).
- Every framebuffer pixel holds the offscreen pixel mirrored across the
  vertical centre line, in B,G,R,A byte order (`agc_rtt_screen`).
- Both allocations hold exactly 8,294,400 non-zero words (`agc_rtt_padding`).

A missing barrier, a wrong target switch or a wrong descriptor would break
the mirrored copy or the byte order.

## Test runner and console tools

Each hardware iteration used to need its own title: a new title ID to
install, then klog copied by hand. The M2-M4 tests now run in one installed
title, `PPSA99988` ("PS5 Vulkan Test Runner", `AGC_TEST_RUNNER`), and the PC
reads results straight from the console. The milestone steps above name the
per-test titles that first passed on hardware (`PPSA99995`-`PPSA99989`).
Those titles are retired; their tests run in the runner as `m2-solid`,
`m3-uniform`, `m3-vertex`, `m3-texture`, `m4-depth`, `m4-blend` and `m4-rtt`
(see `probes/SHADER_PACKAGES.md`).

The development loop:
1. When console-side code changes, deploy the runner in place (`make deploy`
   with its `PARAM_PATH` and `APP_DEFINITIONS`). ShadowMountPlus keeps serving
   the same `/data/homebrew/PPSA99988` folder, so nothing is reinstalled.
2. To change which tests run, upload a queue without rebuilding:
   `python3 tools/ps5_console.py push-jobs --replace PPSA99988
   jobs/regression/queue.txt`, `jobs/capture/queue.txt` for golden
   captures, `jobs/compile/queue.txt` to compile every test's shaders on the
   console first (Phase A3, `docs/M5_PHASE_A.md`), or `jobs/b4/queue.txt`
   for the flip-free completion tests (Phase B4, `docs/M5_PHASE_B.md`).
3. Start `python3 tools/ps5_console.py klog`. It skips the klog backlog,
   prints `ready: launch the title`, follows the run, summarises it at
   `run_end` and saves the full klog under `Klog_Logs/`.
4. Launch the runner on the console.

Build in the repository checkout. The checkout sits on the native filesystem,
so `build/` and `dist/` stay incremental in place and there is no separate
sync step.

```text
bash tools/build-all-titles.sh
PARAM_PATH=sce_sys/param-runner.json \
APP_DEFINITIONS="AGC_LINKED_CANARY=1 AGC_TEST_RUNNER=1 AGC_OUTPUT_4K=1 AGC_LIVE_SUBMIT_ARMED=1" \
make deploy
```

`tools/build-all-titles.sh` builds every title, then fails on any error,
compiler warning or misaligned PT_LOAD segment. Its titles compile with
different definitions, so each one is a full compile.

Summaries (`ps5_console.py klog` and `summary`) list only the statuses that
need attention. Identical records, such as one per test, collapse into one
line with a count. Informational results log as `INFO` or `NOT_REQUIRED`:
register defaults the AGC block does not contain, and checksum-register
inspections where shader creation does not require the register.

`python3 tools/pm4_decode.py stream <klog>` decodes every logged command
stream into AMD PM4 packets. It names packets and registers from Mesa's
`sid.h` and gfx103 `amdgfxregs.h`, and labels addresses by region, such as
`stage+0xc200`. `--fields` adds register fields and `--test` selects one test.
It also checks each decoded register-table load against the
`agc_gpu_pointer_indirect` records of the same stream.
`python3 tools/pm4_decode.py register cx 0x31c 0x8828` decodes one value by
its AGC table offset.

Golden files come from one runner launch with `jobs/capture/queue.txt`:
`python3 tools/golden.py extract <klog> golden/runner`. Each file holds the
following for one frame:
- the frame's test and target buffer, and how many flips the process made
  before it (`flips_before`; frames that end in a completion marker do not
  flip)
- its command words
- the AGC helper calls that wrote them, with their arguments and word spans
- the stage, framebuffer and other allocation addresses
- the non-zero 64-word chunks of the stage workspace, with its FNV-1a 64

Extraction writes nothing unless every capture is complete:
- the reassembled workspace matches its FNV-1a 64
- the logged stream equals the stream inside the workspace
- every chunk and helper call arrived
- the helper calls cover the stream contiguously

`pm4_decode.py` reads register tables out of a captured workspace, and
`--calls` shows each helper call before the packets it wrote.

Checking a change on the PC instead of the console:

```text
bash tools/build-host-runner.sh
python3 tools/golden.py rebuild golden/runner
python3 tools/golden.py rebuild golden/b4
```

`golden/runner` holds the M2-M4 frames of runner pid 260, `golden/b4` the
Phase B4 frames of pid 297 (`b4-marker`, `b4-headless`, then `m2-solid`), and
`golden/b5` the frames of pid 107 (`b5-empty`'s two draw-free streams,
`b4-headless` and `m2-solid`), and `golden/b5c` the frames of pid 108
(`b5-order`'s two frames, `b4-headless` and `m2-solid`; the `b5-order` frames
are checked but not rebuilt, because their test needs rendered pixels). On
the PC, submission completes every recorded
completion marker, as the console had by the time the suspend point returned.

The first command builds the console runner's source for Linux in about 7
seconds. The second reruns every captured test and compares every frame with
its golden file in about 10 seconds:
- command words
- helper calls
- allocation addresses
- the whole stage workspace

Helper arguments that point into CPU memory are compared as `cpu`; the data
they point to is covered by the command words.

`host/ps5/ps5_host.cpp` stands in for the PS5 system calls:
- `/app0` maps to the repository and `/download0` to `build/host/rebuild`.
- Direct memory is mapped at the captured addresses.
- Klog goes to stdout.
- Submission and flips succeed without a GPU.

Inputs the PC cannot compute are replayed from each test's first golden
file:
- the AGC register defaults
- the relocated shader headers
- the link outputs at stage `0x5000`-`0x6800`
- the VideoOut handle
- the frame number the flip counter continues from

`golden.py check-helpers` checks the helper models on their own.

Pass criteria: `runner_summary` passes only when every queued test's own
readback checks passed. Each test links a fresh stage workspace and opens and
releases its own VideoOut target, as its per-test title did. Until
data-driven scenes exist, the data is the choice and order of built-in tests;
a new kind of test still needs a runner build.
