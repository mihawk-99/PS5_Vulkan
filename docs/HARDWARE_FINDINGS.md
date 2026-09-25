# Hardware findings

What console runs established, each with the build, run id or commit that
established it. Every item here constrains GPU-facing code.

_Append-only: add new findings at the end; never rewrite an existing one._

Recorded from PS5 klog runs of the canary titles. Each item names the build
that established it.

- The GPU dereferences indirect register tables when the command buffer runs,
  so they must live in GPU-mapped direct memory. A table on the stack caused
  `GPU_FAULT_PAGE_FAULT_ASYNC` and a Shell UI restart (fixed in `9bfe637`).
- The `sceAgcDcb*` helpers return the start of the packet they wrote;
  `command.up` is the end of the stream (`9bfe637`).
- AGC helpers emit standard AMD PM4 type-3 packets: indexed CX/UC/SH table
  loads (`0x9f`, `0x64`, `0x63`), `SET_SH_REG` (`0x76`), `DRAW_INDEX_AUTO`
  (`0x2d`) and `WRITE_DATA` (`0x37`) carrying the flip marker (`9bfe637`,
  `5f1e40f`).
- `sceVideoOutUnregisterBuffers` returns `0x80290009` (busy) at shutdown and
  close still succeeds; ProsperoLight and ps5-opengl observe the same
  (`5f1e40f`).
- Gate 2 passed: one ProsperoLight-order frame (wait packet, render target,
  linked shader state, descriptors, draw and flip) was submitted and presented
  with no GPU fault (`c1fe9aa`). The launch splash hides it until
  `sceSystemServiceHideSplashScreen` is called (`c430374`).
- Colour render targets are not stored row-major. They use ps5-opengl's
  hardware-validated tiled RGBA8 layout (`tiled_rgba8_offset`): 128x128 blocks
  in row-major order, `0x10000` bytes each, with a fixed XOR bit mapping of
  the block-local x and y. A Z-order tile hypothesis (`c052925`) failed the
  full-frame check in `9339831` (1.41 red levels per neighbour step, no
  bottom-half rows recognised). Re-placing that run's samples through the
  ps5-opengl mapping fitted the coordinate card with a mean error of 0.34 red
  levels, and `79b5ace` confirmed it on hardware: 0.32 red levels per
  neighbour step (row-major: 10.09) and zero card-invariant violations across
  4,096,212 compared pixel pairs. CPU access to GPU images must convert this
  layout.
- The ProsperoLight quad maps the 1920x1080 NV12 source 1:1 onto the
  1920x1080 target: fitted slopes of 0.1327 per pixel and 0.4662 per row
  against 0.1329 and 0.4661 predicted, with the half boundary at row 540.
  Measured coverage is 1920x1079 (`79b5ace`): every sampled column ends at
  y=1078 and exactly 2,071,680 pixels are drawn, so the quad's bottom edge
  leaves row 1079 undrawn.
- The ELF converter must take the RELRO file offset from its lowest section.
  With `.data.rel.ro` present it emitted a misaligned segment and the PS5
  loader refused the executable (`0x80aa001a`); fixed in `b2dcec1` with a
  host-side PT_LOAD alignment guard.
- Shaders compiled from GLSL in this repository, with unresolved
  program-checksum registers, create, link (primitive type 4) and draw
  (`945f28f`). The full-target triangle covered all 8,294,400 pixels of the
  3840x2160 target exactly (2160 full rows) and wrote nothing into tiled
  padding. The 8-bit values arrived unchanged (0x20, 0xa0, 0xff), so the
  UNORM target applies no sRGB encoding.
- With COMP_SWAP = SWAP_STD (`CB_COLOR0_INFO` `0x8028`, the recipe of
  ProsperoLight and ps5-opengl) shader output channels 0-3 land in bytes 0-3,
  while VideoOut format `0x8000000000000000` scans bytes out as B,G,R,A.
  R=0x20 G=0xA0 B=0xFF read back as word `0xffffa020` and showed orange
  (`945f28f`). Setting SWAP_ALT (`CB_COLOR0_INFO` `0x8828`) makes framebuffer
  0 a B8G8R8A8 target and fixes it: `5fa3b84` passed M2 on hardware with all
  8,294,400 pixels equal to `0xff20a0ff`, exact coverage, empty padding and
  the TV showing the intended light blue.
- AGC defaults set the screen (`0x00d`), window (`0x082`), generic (`0x091`)
  and viewport (`0x095`) scissor bottom-right corners to `0x40004000`
  (16384x16384) with clip rectangles disabled (`0x08e` = 0), so no default
  clips a 1080p or 4K target (`945f28f`). M1's undrawn row 1079 comes from
  its quad geometry, not from scissor defaults.
- A pixel shader reads a CPU-filled uniform buffer through descriptor set 0
  (`d887138`). With `--address32-hi 2`, a descriptor-set table whose low 32
  bits sit in pixel user-data dword 2 (SET_SH_REG at SH `0x0c`) and a
  stride-form buffer descriptor (`0x28100`, `0x100002`, `1`, `0x4dfac`), frame
  0 read back all 8,294,400 pixels as the buffer's pink, `0xffff4080`.
- The wait-until-safe packet must name the buffer the frame renders into
  (`d887138`). Frame 1 drew into buffer 1 but its packet named buffer 0,
  which was on screen; the GPU waited for buffer 0 to leave scanout, which
  only the flip later in the same stream could do. Submission and the suspend
  point passed, yet the flip status stayed at frame 0's marker for 200
  vblanks. ps5-opengl's two-slot frame test passes the target buffer index,
  and so does `e27f6b8`, which passed M3 step 1 on hardware: pink in buffer
  0 (`0xffff4080`), then green in buffer 1 (`0xff30d060`) after rewriting the
  same uniform buffer, both flips at their markers, 8,294,400 exact pixels
  per frame with empty padding, and a clean VideoOut and memory release.
- Vertex and index buffers work (`e5fb5ba`, M3 step 2). A CPU-filled
  24-byte-record vertex buffer behind a stride-form descriptor (`0x28100`,
  `0x180002`, `4`, `0x5204`) and a 16-bit index list `0,1,2, 2,3,0` drawn with
  `sceAgcDcbDrawIndex` produced exactly the 2,073,600 pixels of the square
  (960-2879 x 540-1619) and nothing outside it. Every pixel matched its
  expected colour with zero error: red 0x80, alpha 0xff, and green and blue
  equal to the pixel-centre value `((2i+1)*255 + span) / (2*span)` on all
  2,073,600 pixels, so the ±1 tolerance was never used. Vertex user data was
  table pointer, base vertex 0 and NGG LDS layout `0x200`.
- Sampled textures work (`b482d6a`, M3 step 3). A 64x36 untiled RGBA8 texture
  uploaded row-major as R,G,B,A bytes at a 256-byte-aligned address, behind a
  combined image-sampler descriptor built from ps5-opengl's gallium formulas
  (`0x2000290`, `0xc3800000`, `0x8008c00f`, `0x90000fac`, 0, `0x400000`, 0, 0,
  `0x92`, `0xfff000`, sampler, 0; resource-level bit 31 set), was sampled
  through pixel user-data dword 2:
  - Nearest (`0x08000000`): all 2,073,600 square pixels equal their texel
    exactly (maximum error 0), with exact extent and empty padding.
  - Bilinear (`0x09500000`), after rewriting only the sampler word: all pixels
    within one level of the clamp-to-edge float blend at the pixel centre.
    1,864,860 were exact and 208,740 off by one; none reached the two-level
    tolerance, consistent with fixed-point filter weights.
- Clear and depth testing work (`9faf8b2`, M4 step 1). The setup:
  - a tiled D32F depth buffer at a 2 MiB-aligned address, bound with
    ps5-opengl's 16 depth-target registers (`DB_Z_INFO` `0x80000183`)
  - five indexed draws in one frame, with indirect `0x200` depth-control
    tables before draws 1 and 2: a clear triangle with ALWAYS (`0x76`), then
    four rectangles with LESS (`0x16`)

  Results:
  - The clear overwrote a colour sentinel in all 8,294,400 pixels and depth
    0.0 in all samples.
  - Every pixel shows the nearest shape's exact colour, in both draw orders:
    near-then-far on the left, far-then-near on the right.
  - Every depth sample, read through ps5-opengl's tiled depth offsets, holds
    that shape's exact float depth (`0x3f800000`, `0x3e800000`, `0x3f400000`,
    `0x3f000000`, `0x3ec00000`), so stored depth equals clip z.
  - Visible pixel counts match the geometry: clear 4,276,800; rectangles
    1,036,800 / 907,200 / 777,600 / 1,296,000.
  - Both allocations hold exactly 8,294,400 non-zero words, so neither
    wrote outside its tiled pixels.
- Blending with only `CB_BLEND0_CONTROL` programmed is incomplete (`5acd73c`,
  two runs of M4 step 2):
  - Opaque halves and the additive row (ONE, ONE) were exact on all their
    pixels, including red and blue saturation.
  - The over row kept exact red, but green read 255 and blue was wrong.
  - The premultiplied row kept exact red and green, but blue was wrong.
  - The wrong blue levels differed between the two runs (`0x77` in one,
    `0x0a`/`0x0b` in the other), which points at uninitialised
    blend-optimisation state rather than a formula error.
  - 5,767,200 of 8,294,400 pixels were exact and the rest far off.

  ps5-opengl's runtime writes `CB_COLOR_CONTROL` (`0x202`) with every draw,
  and its gallium layer sets bit 0 to keep RB+ off. The follow-up build writes
  `0x00cc0011` with each blend word and logs the register defaults.
- Writing `CB_COLOR_CONTROL` did not change the blend result (`eb236d1`): the
  sample and pixel counts were identical to `5acd73c`. The logged AGC
  defaults:
  - `0x1e0` = `0x20010001`, `0x202` = `0x00cc0010`, blend colour `0x105`-`0x108`
    = 0
  - the SX registers `0x1d5`-`0x1d8` are absent from the defaults block

  The remaining difference from ps5-opengl is the pixel shader's colour
  export format. Our packages compile for the CLI default, 32_ABGR
  (`SPI_SHADER_COL_FORMAT` 9). ps5-opengl's gallium runtime passes the format
  Mesa's `ac_choose_spi_color_formats` picks for the target: FP16_ABGR (4)
  for 8-bit UNORM, in every variant including blending. The option also
  shapes ACO's output code. The working hypothesis: opaque and ONE/ONE output
  survives the 32-bit export, but source-alpha blending needs FP16_ABGR. The
  follow-up build tests it by compiling the blend canary's pixel shader with
  `--color-format 0x99999994` through `tools/build-psbc-cli.sh`.
- Blending works once the pixel shader exports the target's blend format
  (`d050664`, M4 step 2). With the same blend words, `CB_COLOR_CONTROL` and
  geometry, only the pixel stage changed: FP16_ABGR exports
  (`SPI_SHADER_COL_FORMAT` 4, 76 bytes of code instead of 68). All 8,294,400
  pixels then equal the exact integer blend, with maximum error 0:
  - over: (233, 173, 10) and (163, 193, 90)
  - additive with saturation: (255, 100, 225) and (125, 150, 255)
  - premultiplied: (120, 132, 117) and (15, 162, 237)

  Rule: compile every pixel shader for the colour export format Mesa's
  `ac_choose_spi_color_formats` chooses for its render target, as
  ps5-opengl's gallium runtime does. The legacy 32_ABGR default drew opaque
  and ONE/ONE output correctly in M2-M4.1 but corrupts source-alpha blending.
  The earlier probe sets still use it, and the Vulkan frontend must key
  pixel-shader variants by attachment format.
- Render to texture works (`388d18e`, M4 step 3). One 175-word command
  stream does both passes with no GPU fault. The flip reached its marker with
  no waits. The setup:
  - an offscreen tiled RGBA8 target at `0x204200000` (32 MiB, compiled high
    word, 256-byte aligned), bound with COMP_SWAP 0 (`CB_COLOR0_INFO`
    `0x8028`)
  - `sceAgcCbReleaseMem(cmd, 45, 12, 1, 0, NULL, 0, 0, 0, 1, 0, 0)` as the
    barrier, then a 31-record indirect CX table that switches the target,
    viewport and scissor back to the framebuffer mid-stream
  - the offscreen image sampled through a combined image-sampler descriptor
    with the M3 formulas: `0x2042000`, `0xc3800000`, `0x821bc3bf`,
    `0x91b00fac`, 0, `0x400000`, 0, 0, `0x92`, `0xfff000`, `0x08000000`, 0.
    Resource-level bit 31 is set, which ps5-opengl's native test omitted.

  Results:
  - All 8,294,400 offscreen pixels hold their texel exactly, in R,G,B,A byte
    order (centre `0xff207e80`).
  - All 8,294,400 framebuffer pixels hold the offscreen pixel mirrored across
    the vertical centre line exactly, in B,G,R,A byte order (centre
    `0xff807e20` at x=1919).
  - Both allocations hold exactly 8,294,400 non-zero words.

  Sampling a tiled render target with word 3 `0x91b00fac` addresses it in
  texel coordinates: the GPU resolves the tiled layout that CPU readback has
  to convert. ps5-opengl's barrier is sufficient for sampling a target in the
  frame that rendered it; whether it is required was not tested.
- One process runs every own-shader test in sequence (`3889654`, runner
  `PPSA99988`, pid 256). With the queue `hold 120` / `all`, all seven tests
  passed (`runner_summary`: 7 of 7), and all eight expected screens were seen
  on the TV. Across the run:
  - VideoOut was opened, given two 3840x2160 buffers, closed and reopened for
    each test. That is seven opens, with handles rising by `0x100` from
    `0x4e100100`. Every register and close passed; unregister returned the
    known busy `0x80290009` each time.
  - Each test's stage workspace (direct memory offset `0x200000`) and
    framebuffers (`0x200200000`) were released and allocated again at the
    same addresses.
  - Readbacks matched the per-test titles:
    - M2: 8,294,400 pixels of `0xff20a0ff`
    - uniform: pink and green frames exact
    - vertex, texture (nearest and bilinear), depth and blend: passed
    - render to texture: 8,294,400 offscreen and 8,294,400 screen pixels
      exact
  The runner is now the regression suite for M2-M4.
- Decoding that run's nine logged streams with `tools/pm4_decode.py` shows the
  packets the AGC helpers emit, named by Mesa where it defines them. Every
  decoded register-table load matched the logged `agc_gpu_pointer_indirect`
  records.
  - Wait-until-safe (32 words): `SET_UCONFIG_REG` `0x30d08` = `0xcb000000`,
    a 9-word opcode `0x93` packet Mesa does not name, `SET_UCONFIG_REG`
    `0x30d08` = `0xcb000020`, then a 16-word `NOP`. Mesa names `0x30d08`
    `SQ_THREAD_TRACE_USERDATA_2`.
  - Register tables: `LOAD_CONTEXT_REG_INDEX` (`0x9f`) and
    `LOAD_SH_REG_INDEX` (`0x63`), plus `0x64` for the uconfig table, which
    `sid.h` does not name.
  - User data: `SET_SH_REG` at `0xb230` (`SPI_SHADER_USER_DATA_GS_*`, the
    NGG vertex stage) and `0xb030` (`SPI_SHADER_USER_DATA_PS_*`), carrying
    low 32-bit workspace addresses.
  - Indexed draws: `SET_UCONFIG_REG_INDEX` `VGT_INDEX_TYPE` = `0x400` once,
    then `INDEX_BASE`, `INDEX_BUFFER_SIZE` and `DRAW_INDEX_2` for each draw.
  - Barrier: `RELEASE_MEM` (`0x49`) with first payload word `0x0000c52d`
    (event 45).
  - Flip: `SET_UCONFIG_REG` `0x30d08`/`0x30d0c` = `0xc7010101`/`0`,
    `WRITE_DATA` carrying the marker `0x5053564b`, `RELEASE_MEM`, then a
    46-word `NOP` that ends the stream.
- Golden captures are complete and deterministic (runner pid 260, queue
  `capture` / `hold 60` / `all` / `m2-solid`, 4,861 klog lines). All eight
  tests passed. All ten streams arrived whole: each reassembled workspace
  matched its FNV-1a 64 and contained the logged stream, and the helper calls
  covered every stream contiguously. The captures map each AGC helper to the
  words it writes:
  - `sceAgcDriverWaitUntilSafeForRendering`: 32 words, the four packets
    above.
  - `sceAgcDcbSet{Cx,Uc,Sh}RegistersIndirect`: one 5-word table-load packet
    each.
  - `sceAgcCbSetShRegisterRangeDirect`: one `SET_SH_REG` of 2 + count words.
  - `sceAgcDcbSetIndexSize`: `SET_UCONFIG_REG_INDEX` `VGT_INDEX_TYPE`
    (3 words).
  - `sceAgcDcbSetIndexBuffer`: `INDEX_BASE` (3 words).
  - `sceAgcDcbSetIndexCount`: `INDEX_BUFFER_SIZE` (2 words).
  - `sceAgcDcbDrawIndex`: `DRAW_INDEX_2` (6 words).
  - `sceAgcDcbDrawIndexAuto`: `DRAW_INDEX_AUTO` (3 words).
  - `sceAgcCbReleaseMem`: `RELEASE_MEM` (8 words).
  - `sceAgcDcbSetFlip`: 64 words. Its `RELEASE_MEM` ends with a word whose
    low byte counts the process's flips: `0x08000101` for the first frame,
    `0x08000109` for the ninth.

  Direct memory is handed back from the previous test without being cleared,
  and AGC never writes the body of the flip's trailing `NOP`. The first
  capture (pid 258) had an earlier test's packets in that body, so streams
  depended on queue order. The runner now clears each stage workspace when
  it is mapped and each command buffer before a frame. Afterwards every
  flip's `NOP` body was zero, and the workspace images shrank to their real
  contents, for example depth from 59 to 23 chunks and blend from 59 to 22.
  `m2-solid` was captured first and again last. The two captures differ only
  in the flip counter word (stream word 67: `0x08000101` against
  `0x0800010a`) and in the VideoOut handle arguments.
- The AGC command helpers are simple encoders determined by their arguments.
  PC models (`host/agc/agc_host.cpp`) reproduce all 129 helper calls
  recorded in `golden/runner`, and all 19 in `golden/b4` (including the
  completion marker), word for word:
  - Register-table loads (context `0x9f`, uconfig `0x64`, SH `0x63`): table
    address low, address high, `0x80000000`, record count.
  - `SET_SH_REG`: SH offset, then the values.
  - `INDEX_BASE`: index address low and high.
  - `INDEX_BUFFER_SIZE`: the count.
  - `DRAW_INDEX_2`: count, address low, address high, count, modifier.
  - `DRAW_INDEX_AUTO`: count, modifier.
  - Wait and flip encode no VideoOut handle. The target buffer sets bit 0 of
    the wait's `0xcb000000` and `0xcb000020` markers, and bit 3 of the flip's
    `0xc7010101` and of `0x800040a0` in both. The flip also encodes the
    marker and the process's flip count.

  The captures fix the models' limits: 16-bit indices, the one
  render-to-texture barrier, flip mode 1, buffers 0 and 1, and the first 255
  flips. The models refuse any other arguments rather than guess.
- The captured package headers hold the fields the frame code reads from the
  shader objects, already relocated. At `+24` and `+32` are absolute table
  addresses: for the M2 vertex header, `0x20001c090` and `0x20001c060`. Byte
  90 is the stage, and bytes 91 and 92 are the CX and SH table counts (11
  and 6). This is consistent with `sceAgcCreateShader` relocating the header
  in place and returning it, but the returned pointer was not logged.
- The console runner's code, built for the PC, reproduces all ten golden
  frames exactly. Every command word, every helper call apart from CPU-stack
  pointers, every allocation address and every workspace byte matched. The
  ten frames are M2 twice, both uniform frames, the vertex frame, both
  texture frames, depth, blend and render to texture. Replaying the runner's
  adjusted target registers as AGC defaults reproduces them unchanged,
  because every adjustment `append_target_registers` makes is idempotent. A
  PC build with one blend state value changed (`CB_COLOR_CONTROL`
  `0x00cc0011` to `0x00cc0010`) is reported as different at exactly the four
  blend-state records, stage `+0xc50c`, `+0xc51c`, `+0xc52c` and `+0xc53c`.
- The console compiles SPIR-V into AGC packages byte-identical to the PC's
  (Phase A3, runner queue keyword `compile`, pid 290). The runner linked
  opengnm-psbc built for the PS5 and ps5-opengl's C writer. All 14 probe
  packages matched, and all 7 tests passed their exact readbacks drawing with
  the compiled packages. Compiling took about 1.1 ms per vertex shader and
  0.3-0.5 ms per pixel shader, as measured by the title's own monotonic
  clock. The title loaded with all 185 imports, and the system allocator was
  enough. Details: `docs/M5_PHASE_A.md`.
- A submission completes without a flip (Phase B4, runner pid 297,
  `golden/b4`). ps5-opengl's frames that do not present end with the colour
  barrier and `sceAgcCbReleaseMem(40, 0x30c, 0, 0, address, 1, value, 0, 0,
  0, 0)`, and the GPU writes value to the address. This worked in two tests:
  - `b4-marker`: `m2-solid` with its flip replaced.
  - `b4-headless`: no VideoOut, no wait packet, a direct-memory target.

  Both read back exactly. The marker packet is `c0064900 0030c528 20000000
  <address low> 00000002 <value> 0 0`. The marker already held its value 20
  µs after `sceAgcSuspendPoint` returned for a one-draw frame. `m2-solid`
  afterwards flipped as the process's first flip. Details:
  `docs/M5_PHASE_B.md`.
- Streams without draws complete, and completion does not need the barrier
  (`b5-empty`, runner pid 107): the barrier and marker (16 words) and the
  marker alone (8 words) both completed.
- `sceAgcSuspendPoint` takes 123–131 µs for every stream, from a lone marker
  to 256 full-target 3840x2160 draws (runner pids 107 and 108). Every
  completion marker so far was found on the first poll after it.
- The completion marker follows rendering (`b5-order`, runner pid 108):
  - A frame of 256 heavy full-target draws in one colour and a last
    full-target draw in another was sampled at its centre pixel.
  - When submission returned, the pixel held the heavy colour: the frame was
    still rendering.
  - When the marker was seen, the pixel held the last draw's colour.
  - With 16 heavy draws both had arrived before submission returned.
  - Marker-based fences are therefore consistent with rendering being
    complete. Pid 107's reading from memory bandwidth, that the marker came
    first, was wrong.

- A stream can carry two pipelines, and a colour clear can be a drawn
  rectangle (`c1-clear`, runner pid 117, `golden/c1-clear`):
  - A viewport and scissor larger than the target clip and shift nothing.
    Mesa's `vk_meta` rounds a clear rectangle's viewport up to a power of two,
    so clearing the whole 3840x2160 target used a 4096x4096 viewport with the
    corners scaled by `2/4096`; they landed on 0, 3840, 0 and 2160 and every
    pixel of the target held the clear colour.
  - A second pipeline's linked context, uniform and SH tables mid-stream
    replace the first's, and both draws render where they should. Each
    pipeline's link outputs live in their own 64 KiB workspace.
  - `DRAW_INDEX_AUTO` fetches from the vertex-buffer table: a non-indexed draw
    reads a vertex buffer, which every recorded vertex-buffer draw (M3, M4)
    had reached through an index buffer.
  - **The register tables a shader object carries hold no user-data
    registers.** A draw's vertex user data at SH `0x8c` therefore survives
    into the next draw, whatever pipeline it uses. Frames before C1 wrote no
    user data at all and rendered correctly only because those SGPRs were
    zero; a draw after a clear must write its own base vertex and NGG LDS
    layout, or it takes the clear's vertex-buffer table address as its base
    vertex. Every draw now writes its pipeline's vertex user data.
  - Clearing that framebuffer on the CPU costs 17,530 µs, more than a 60 Hz
    frame, and splits the submission; the same clear on the GPU added 41 words
    and nothing measurable (suspend point 29 µs with the clear, 27 µs
    without), so clears are drawn.

## 2026-09-16: what the allocator serves, and how AGC is reachable

Recorded from runner pids 122 and 123 and diagnostics-app pid 124, with the
capability probes the version track asked for.

- **The direct-memory pool is 12 GiB and serves 1 GiB requests.**
  `sceKernelGetDirectMemorySize` returned 12,884,901,888 bytes, and every
  request in the ladder succeeded, each released immediately, at 2 MiB
  alignment: a 4K target (33,177,600 bytes), a 7680x4320 RGBA8 attachment
  (132,710,400), the 8192 square image dimension (268,435,456), that
  attachment at 4x MSAA (530,841,600), and eight such attachments at once
  (1,061,683,200). The allocator therefore does not limit a 1.4-sized target.
  `V4-window` still has to answer whether the shader-address window covers
  them and whether tile layouts exist at that size.
- **AGC is reachable only through linked imports, not through `dlopen`.** The
  diagnostics app's dynamic path reports `agc_module UNSUPPORTED` (pid 124):
  `dlopen("libSceAgc.sprx")` fails, so no module handle exists to `dlsym`
  against. A linked-import build does not expose its imports either: asking
  the process scope for a 32-name inventory (pid 123) reported every name
  unexported, including `sceAgcInit`, which that same run called successfully.
  An AGC export inventory therefore cannot be enumerated from a title on this
  console. A candidate name can only be tested by importing it and letting
  the loader resolve it, one build and one launch per batch, and a failure
  names at most the first unresolved symbol. The recorded 13-name helper
  surface carries no dispatch, query or timestamp entry, which is the
  practical evidence `V0-compute` and `V0-query` have to work from.

## 2026-09-16: the first compute dispatch, and what its packet shape needs

Recorded from runner pid 107 (`docs/M5_PHASE_C.md`): one 1x1x1 workgroup of
`probes/c0/dispatch.spv`, submitted through `sceAgcDriverSubmitDcb` as raw PM4
packets, read back exactly.

- **A compute dispatch works with SET_SH_REG plus `DISPATCH_DIRECT`, with no
  AGC helper.** The recorded 13-name helper surface has no dispatch entry, so
  the probe writes the compute registers directly — `0xb810`, `0xb81c`,
  `0xb830`, `0xb848`, `0xb854`, `0xb8a0` and the user data at `0xb900` — then
  submits `DISPATCH_DIRECT` (`0xc0031500`) with the grid and the initiator, an
  `EVENT_WRITE` `CS_PARTIAL_FLUSH` (`0xc0004600`, `0x407`) and its completion
  marker. The readback held `0xa5a5a5a5`.
- **The wave size the dispatch asks for has to match `COMPUTE_PGM_RSRC1`'s VGPR
  granule.** That field is 1 and the compiler reported 16 VGPRs, which is
  `(16 - 1) / 8`: the shader was compiled for wave32, and the initiator carries
  `CS_W32_EN`. A wave64 build of the same code would report 3, so the field
  decides which bit the dispatch needs.
- **A compute shader's descriptor table pointer is a 32-bit user-data
  address.** The shader loads through `s[2:3]`, but only user-data dword 2 is
  programmed: `s_load_dwordx4 s[0:3], s[2:3], 0` follows `s_mov_b32 s3,
  <address32_hi>`, so the table's high half is the constant the shader was
  compiled with rather than a second user-data dword. Every address in one
  dispatch — shader, table, storage buffer, completion marker — has to share
  that high word, which is why the probe refuses to submit when one does not.
- **A declared storage buffer occupies a 16-byte table entry.** Declaring
  binding 0 as `PSBC_DESCRIPTOR_STORAGE_BUFFER` with stride 16 is what makes
  ACO emit the descriptor load; strides 0, 4, 8, 24 and 32 all fail with
  compiler result 6. Undeclared, ACO emits a store through a null descriptor
  that the console drops without a fault, so a dispatch whose binding is not
  declared fails by readback instead of by error.

## 2026-09-16: the driver's vk_meta clear renders

Recorded from runner pid 109 (`docs/M5_PHASE_C.md`): four frames presented
through the driver, each cleared by Mesa's vk_meta and then drawn over by the
corner triangle, a second pipeline, in the same stream.

- **A clear vk_meta draws through the driver covers the attachment.** The half
  inside the triangle's diagonal held the triangle's colour and the half
  outside it the clear's -- 4,139,524 pixels each, no pixel in any other colour,
  and every word of the frame written. A clear that only covered where the
  triangle draws, or that left the image untouched, would show as pixels in
  neither colour or as zeros.
- **A display image reads back in B8G8R8A8 order.** The clear's value is red
  0x40, green 0x80, blue 0xff opaque, and the frame holds it as `0xff4080ff`:
  `A<<24, R<<16, G<<8, B`. The offscreen `R8G8B8A8` images the b7 and b8 frames
  are drawn into pack the other way (`A<<24, B<<16, G<<8, R`), which is why the
  runner has two helpers for the same colour and why comparing a display image
  with the offscreen packing fails with every pixel in neither colour.
- **Two pipelines in one stream work through the driver, not only in raw AGC.**
  The clear's pipeline and the triangle's are separate compilations with their
  own linked context and SH tables, and the application's pipeline, vertex
  buffers, push constants and dynamic state survive the meta draw that replaced
  them while it ran. The earlier `c1-clear` probe recorded the same shape with
  hand-built packets.

## 2026-09-17: the flip helper needs an open VideoOut, and mapped memory

Recorded from runner pid 117 (`Klog_Logs/flip-probe.log`) and pid 115
(`Klog_Logs/flip-probe-2.log`), both queued as `c1-flip`
(`docs/M5_PHASE_C.md`).

- **`sceAgcDcbSetFlip` writes nothing unless the calling process holds an open
  VideoOut.** Three passes into the stage workspace -- a stack buffer filled
  with `0xa5a5a5a5`, the workspace zeroed, and the workspace filled with the
  pattern -- returned `returned_delta 0x0`, `up_delta 0` and `words_written 0`,
  with every word still holding what the pass had put there. The test opened no
  VideoOut, and the helper left the buffer alone rather than refusing the call:
  it returned its input pointer and encoded nothing.
- **The buffer's location is not the reason.** A stack buffer was pid 117's
  first pass, and pid 115 repeated the call in direct memory mapped into the
  process, where a real flip's command buffer lives; the result was identical.
  A command buffer outside mapped memory is not accepted -- nothing was written
  to the stack -- but moving to mapped memory did not make the helper write
  either.
- **A flip's own words are 17 of the 64 it reserves.** Run pid 113's captured
  flips are 64 words, of which the helper wrote words 1-17 (`0x79`, `0x37` and
  the `0x49` marker); words 18-63 held whatever the buffer already had. So the
  probe asks whether the pattern pass was written to at all, not whether the
  write count equals the helper's return.

## 2026-09-17: the flip helper with a VideoOut open, from runner pid 109

Recorded from runner pid 109 (`Klog_Logs/flip-probe-3.log`), the same
`c1-triangle` queue as the two runs above, with the probe moved to where the
driver holds VideoOut open (`docs/M5_PHASE_C.md`).

- **The gate is VideoOut, not the buffer, and a stack command buffer is
  accepted.** The same call that wrote nothing with no VideoOut open wrote 19
  words into a stack buffer and 19 into the stage workspace once the driver's
  VideoOut was open. Two runs above are corrected here: the buffer never had
  to be in mapped memory, and "a command buffer outside mapped memory is not
  accepted" is wrong.
- **`sceAgcDcbSetFlip` writes 19 words and reserves 64.** Words written is 19,
  `up_delta` is 64, and the 45 words after the helper's own are the caller's:
  they still held the pattern in the probe and held the frame's tail in the
  flips of run pid 113. Those 19 are the three packets of the captured flip
  (`0x79`, `0x37`, `0x49`, 18 words with their alignment words) plus the
  header of the `0x2c` packet that follows, whose 1 + 44 words fill the
  reservation.
- **The helper's packets carry the caller's arguments and VideoOut's state.**
  Against the captured frame 0 flip the probe's words are identical except for
  four: the field that tracks which VideoOut buffer is flipped (`c7010101` for
  buffer 0, `c7010109` for buffer 1), the caller's marker in the `0x37` packet,
  that buffer's address (`800040a0` against `800040a8`), and VideoOut's own
  flip counter in the `0x49` packet, which walks one per call -- 1 to 4 for the
  four presented frames, 5, 6 and 7 for the probe's three calls.

## 2026-09-17: what the reserved words hold, and how the driver makes them deterministic

Recorded from runner pid 109 and from the capture of run pid 110
(`Klog_Logs/flip-probe-4.log`); the comparison these facts make possible is in
`docs/M5_PHASE_C.md`.

- **The wait helper reserves too.** `sceAgcDriverWaitUntilSafeForRendering`
  writes 16 words and advances the caller's pointer by 32; the 16 after its own
  are the caller's, exactly as the flip's 45 are. A frame's wait packet
  therefore carries the previous submission's words at that offset, and the
  first frame of a process carries whatever the allocation last held, because
  direct memory is recycled without being cleared.
- **Zeroing the queue's submission buffer once makes a run reproducible.**
  `ps5vk_queue_init` clears its 2 MiB buffer when it maps it, so the reserved
  words of a stream are a function of this queue's own submissions: zeros in
  the first, the previous submission's words in every later one. With that, a
  replay and a console capture agree word for word; without it the PC model and
  the console can only agree by accident.
- **The driver's register tables are an allocation like any other.** A command
  buffer's table chunk (0x40000) lands wherever the process's allocator puts
  it, and the addresses in its user data follow: the console's chunk was at
  0x200030000 and a PC run without the captured region put it at 0x200000000,
  a 0x30000 difference in every user-data word that points into it. Listing the
  captured region in the replay gives the allocation the console's address.

## 2026-09-17: the colour flush is not a wait (render to texture, C4)

Measured from the driver's render-to-texture runs (console pids 143-148;
`docs/M5_PHASE_C.md` has each run and its numbers).

- **`RELEASE_MEM` event 45 flushes the colour buffers; it does not order the
  texture fetches behind it.** A draw that samples a colour image an earlier
  draw in the same command buffer rendered into, with the packet in the words
  between them, reads part of that image as it was *before* the render: the
  frame's readback showed 40661, then 38325, then 36006 of its 2073600 pixels
  as zero, every non-zero pixel exact. The zeros formed a band the frame's
  *first* scanlines fetched, which is the image's *last* rows -- the fill pass's
  most recently written data -- and the band's depth moved between runs, so the
  fetches were overtaking the flush rather than reading a layout the sample got
  wrong. A CPU readback of the image after the same submission held every one of
  its 8294400 texels exactly, so nothing was lost: the writes were simply not
  visible yet when the fetches ran.
- **A submission boundary is the wait.** Splitting the submission where the
  sampling draw starts, with no copy at the split, makes the queue submit the
  words before it, wait for that step's completion marker -- which its own
  colour flush precedes -- and only then run the draw. The same run then read
  back 2073600 of 2073600 pixels exact, three times in a row (pids 146-148).
  Rule: a draw that samples a target an earlier draw in the same command buffer
  rendered into carries the event-45 packet *and* a submission split; the packet
  alone is a flush, and the hardware needs the waited-for boundary to make the
  sample observe it.
- **A step's end packets land in the step after it.** Each step ends with the
  colour flush and the completion marker (16 words), written at the first words
  of the next step; the queue restores those words before running each step, so
  the stream buffer after the last step holds only that step's own words whole.
  A capture that means to report every step has to keep its own copy of each as
  it was submitted (`ps5vk_queue.c`), which is what the runner's capture now
  reads.

## 2026-09-17: depth through the driver (C5)

Measured from the console's c5-depth runs (pids 149-158; `docs/M5_PHASE_C.md`
has each run and its numbers). The runner's M4 step 1 had already proved the
registers; these runs prove the driver reaches them and what the readback needs.

- **The depth target registers and control words carry over from the canary
  unchanged.** A D32_SFLOAT attachment of the target's size, with `DB_Z_INFO`
  0x80000183, the Z read and write bases from the image's address, the depth
  view 0, `DB_DEPTH_SIZE_XY` from the target, and `DB_DEPTH_CONTROL` built from
  the pipeline's state -- Z_ENABLE bit 1, Z_WRITE_ENABLE bit 2 and ZFUNC in bits
  4-6, whose values are Vulkan's compare-op order (LESS 1, ALWAYS 7) -- reads
  back 8,294,400 of 8,294,400 samples exact: each rectangle's own z where it
  draws and the clear's 1.0 outside. Nothing about the depth path needed a new
  hardware rule, only the driver's own plumbing.
- **A depth clear is a vk_meta draw, and the clear value reaches it through
  `VkRenderPassBeginInfo`.** The driver records the clear as Mesa's rectangle
  draw through the same meta path as C1b's colour clear: `DB_DEPTH_CONTROL` 0x76
  (test and write with ALWAYS) and the colour masks zeroed in its table, with the
  rect's z carrying the clear value into the position. Vulkan 1.0 requires one
  clear value per CLEAR attachment, so a pass that clears colour *and* depth
  needs both, in attachment order: the harness passed only the colour's and the
  attachment kept whatever the stack held (pid 153's readback: 0 outside the
  geometry). The hardware and the driver were right; the API-level usage was not.
  Rule: the depth attachment is clear value index 1 in a pass whose colour
  attachment is index 0.
- **The task row flip is a property of the geometry, not the depth.** A probe
  that generates its vertices with the runner's `ndc_y` (target row 0 at the
  geometry's -y edge) and draws them through the driver's y-down viewport lands
  them turned over, exactly as the C2 checkers' `rows_bottom_up` records. The
  C5 checker lacked the flip and read a correct frame as a partial one
  (pid 152's colour map: the far rectangle drawn mirrored), which is the second
  time this rule has cost a run.

## Mip levels: a linear texture's sample reads the descriptor's own address

Measured on 2026-09-17, Phase C7 (console runs pid 117-134,
`Klog_Logs/c7-mip-run*.log`). A 256x256 RGBA8 image with five levels -- 256,
128, 64, 32 and 16 texels, each level one solid grey (10, 100, 200, 40, 60) --
uploads correctly through one `vkCmdCopyBufferToImage` region per level: the
image's storage holds 0xa0a0a at offset 0, then 0x646464 at 0x40000, 0xc8c8c8
at 0x50000, 0x282828 at 0x54000 and 0x3c3c3c at 0x56000, with every level's
bytes fully written (the per-level layout ps5-opengl's
`ps5_linear_mip_storage_extent` sizes, ps5vk_image.c's rule).

Sampling that chain reads **level 0 every time**, whichever way the level is
asked for:

| How the sample named a level | What the frame read |
| --- | --- |
| ps5-opengl's descriptor fields: word 3 bits 12-15 first level and 16-19 last level, word 5 bits 4+ the last level, word 9 the LOD range, word 10 the mip filter | level 0 (pid 117-124) |
| the same, with the LOD range pinned per frame (minLod = maxLod = k) | level 0 (pid 132) |
| an explicit `textureLod(..., k)` in the shader | level 0 (pid 124-127) |
| **a view of level k alone** (baseMipLevel = k, levelCount = 1, which is the single-level descriptor the console proved) | level 0 (pid 134) |

The last row is the finding: for a **linear (untiled) texture** this console's
sampler reads the descriptor's own address and has no level arithmetic at all,
so the descriptor's first-level, last-level and LOD-range fields are recorded
but not acted on. ps5-opengl's own mip path agrees that this is unvalidated:
its `PS5_ENABLE_TEXTURE_MIPMAP_CANDIDATE` gate is off by default and its
descriptor writer carries exactly those fields.

What this does not say: whether a **tiled** chain (a colour attachment's tile
layout, where each level's tiles could carry the level) selects levels. That is
the next probe, and the reason the driver refuses a view of more than one level
or a base level above 0 rather than answering every sample with level 0
(ps5vk_draw.c, ps5vk_sampled_image).

## An instance count is read by the draw immediately after it

`sceAgcDcbSetNumInstances(count)` emits one PM4 packet, opcode `0x2f`, header
`0xc0002f00`, with the count as its only body word, and the draw that follows it
runs that many instances. The place matters: a count programmed earlier is not
what the draw reads, because the reset the *previous* draw left behind is the
packet nearest to it. Console run 17 is the evidence -- three instances asked
for, one drawn, with the driver's stream holding `0x2f 3` before the register
tables and `0x2f 1` before the `DRAW_INDEX_2` -- and run 19 is the fix, with the
count immediately before the draw and one immediately after:

```
three instances: tables, user data, 0xc0002f00 0x3, INDEX_BASE, INDEX_COUNT,
                 DRAW_INDEX_2, 0xc0002f00 0x1
```

`gl_InstanceIndex` reaches the shader, and instance *i* of one indexed draw of
the m3-vertex square lands where the shader's own maths puts it: 129,600 px (a
480x270 square) per instance, at columns 528..1007, 1680..2159 and 2832..3311 for
`(i-1)*0.6` of NDC space at a quarter scale, each with the colour its index
computes (`0x8000ff`, `0x8040bf`, `0x808080`). This is the same packet and the
same order ps5-opengl's runtime uses (`ps5_agc_set_instances`) and the one
radeonsi and RADV emit.

## A driver frame is read back through the tiled layout, never row-major

The colour target a driver test draws into is stored in 128x128-pixel blocks of
`0x10000` bytes, row-major between blocks and XOR-mapped inside one
(`tiled_rgba8_offset`, `src/diagnostics.cpp`). A tiling is a permutation of the
pixels, so a row-major read of the target returns the *right colours in the
right proportions and at the wrong coordinates*: C2's instancing check counted
exactly 129,600 pixels of each instance colour -- the square's true area -- while
every one of its six positional boxes failed (`Klog_Logs/c2-instancing-run18.log`).
Any new readback goes through `FramebufferView` with `kTiledRgba8Layout`, and a
pixel no draw touched reads as the zero word, not as a clear colour.

## A linear mip chain has no level selection: the descriptor is not what stops it

Measured on 2026-09-17, Phase C7's bisection (console runs pid 125-127,
`Klog_Logs/c7-mip-run25.log` to `run27.log`). The chain is the five-level 256x256
RGBA8 image above (greys 10, 100, 200, 40, 60 at levels 0 to 4), and the frame
draws five bands of 432 rows whose fragment rows name the LOD
(`floor(5 * y / 2160)`, `OpImageSampleExplicitLod` in the SPIR-V). The console
read **level 0's grey in every band**, with the descriptor's words taken from
the capture:

| Descriptor | Value | What it says |
| --- | --- | --- |
| word 2 | `0x803fc03f` | 256x256, `RESOURCE_LEVEL` set (the field's own comment in AMD's register database is "must be 1") |
| word 3 | `0x90040fac` | type 2D, **BASE_LEVEL 0, LAST_LEVEL 4** |
| word 5 | `0x00400040` | PERF_MOD 4, **MAX_MIP 4** (the image's last level, ps5-opengl's rule) |
| word 9 | `0x00400000` / `0x00400400` | LOD range 0..4 for the band frame, 4..4 for the frame pinned to level 4 (1/256 units) |
| word 10 | `0x04000000` | mip filter nearest (bits 26-27), no LOD bias |

So the sample ignores the level fields, the LOD the shader computes *and* the
sampler's own LOD clamp: with minLod = maxLod = k the frame still read the texel
at the descriptor's base address, for every k from 0 to 4, with both mip
filters, in five frames of two tests over three runs. A view of one level alone
does the same (pid 134). The per-level storage is right -- each level's first
texel is its own grey at the ps5-opengl per-level offset -- so this is not an
upload or layout failure but the absence of level arithmetic for **linear**
(SW_MODE 0) storage.

What this does not say: whether a **tiled** chain selects levels. That is the
next probe, and the reason the driver keeps refusing nothing here but answering
every sample with level 0 (ps5vk_draw.c, ps5vk_sampled_image): a frame that
samples a chain reads level 0 rather than the level it asked for, which is what
the probe's log says.

## A tiled mip chain does select levels, and its layout is the hardware's own

Measured on 2026-09-17, Phase C7 (console runs pid 128-131, `Klog_Logs/c7-mip-run31.log`).
A five-level 256x256 RGBA8 chain stored the way the driver stores an attachment
-- 64 KiB tiles of 128x128 texels, one level after another -- and described by
the same descriptor as the linear chain (view of all five levels, word 3 first
level 0 last level 4, word 5 MAX_MIP 4, word 9 the sampler's LOD range, word 2
`RESOURCE_LEVEL`) behaves differently from every linear encoding tried: **the
LOD moves the address the sampler reads.**

With every 4 KiB page of the chain's storage holding its own index, the two low
bytes of the frame name the page:

| Pinned LOD | 0 | 1 | 2 | 3 | 4 |
| --- | --- | --- | --- | --- | --- |
| Page | 0x50 | 0x1c | 0x08 | 0x06 | 0x01 |
| Byte offset | 0x50000 | 0x1c000 | 0x08000 | 0x06000 | 0x01000 |

and the frame whose LOD comes from the fragment's row reads exactly those pages
in that order. So a level *is* selected for tiled storage, where no linear
descriptor field moved it off level 0.

The offsets are not the driver's layout (level 0 at 0, then 0x40000, 0x50000,
0x60000, 0x70000): the hardware reads the small levels first, each level's
region rounded to a power of two -- 0x4000, 0x4000, 0x8000, 0x10000 and 0x40000
bytes from 0, the whole chain in 0x60000 bytes -- with the 8x8 page grids
showing the intra-level swizzle as well. That is AddrLib's mip chain rule for a
64 KiB swizzle mode (`addrlib/src/gfx9/gfx9addrlib.cpp`,
`Gfx9Lib::ComputeMipChainInfo`, `GetMipTailDim`, `MipTailOffset256B`), whose
source is in `.deps/native/mesa/mesa-26.2.0/src/amd/addrlib/`. Until the driver
stores a tiled chain that way, a mipmapped attachment samples the right level of
the wrong texels -- which is what the probe's five solid levels show as one
level's grey everywhere.

## The tiled mip chain layout, measured to the byte

Measured on 2026-09-17, Phase C7 (console runs pid 131-134,
`Klog_Logs/c7-mip-run33.log` and `run34.log`). Every 4-byte word of a five-level
256x256 RGBA8 chain's storage was written with its own byte offset in its low 24
bits, so the colour each frame read named the address the sampler fetched. Two
things came out of it.

**Inside a level the map is the driver's own.** For the 16x16 and the 32x32
levels every one of the 1024 sampled texels landed exactly where
`tiled_rgba8_offset` (the map the target readbacks decode, 128x128-texel blocks
of 0x10000 bytes with an XOR pixel map) puts it, and the larger levels agree to
within the sampling grid's texel rounding. So the swizzle the driver already
uses is the one this console reads, level for level.

**Between levels the layout is not the driver's.** The driver sizes a tiled
chain with each level's tiles after the last -- level 0 at 0, then 0x40000,
0x50000, 0x60000, 0x70000 for the five levels. The hardware reads:

| Level | 0 | 1 | 2 | 3 | 4 |
| --- | --- | --- | --- | --- | --- |
| Base | 0x20000 | 0x10000 | 0x8000 | 0x4800 | 0x800 |

that is, the small levels first, and the whole chain inside 0x60000 bytes where
the driver's layout needs 0x80000. This is AddrLib's mip chain rule for a 64 KiB
swizzle mode (`Gfx9Lib::ComputeMipChainInfo` with `GetMipTailDim` and
`MipTailOffset256B`; the source is in `.deps/native/mesa/mesa-26.2.0/src/amd/addrlib/`),
whose measured table entries this chain follows for the levels that fit its tail.

With the chain stored that way the console reads every level correctly: one
frame whose bands take their LOD from the fragment's row, and five frames whose
sampler LOD range is pinned to one level each, all read the five levels' own
greys (console run pid 134, `golden/c7-mip-tiled`). The linear chain of the same
five levels reads level 0 in every one of those frames, which is why level
selection needs tiled storage on this console.

## The sampler's DST_SEL supplies a format's missing channels, not its FORMAT field

Measured on 2026-09-17, Phase V0-formats (console runs pid 148 and later,
`Klog_Logs/v0-formats-sampled-run4.log`, then the captured run). A combined
image sampler descriptor's FORMAT field (word 1 bits 20-28, the GFX10
`IMG_DATA_FORMAT`) says how a texel's bytes decode; it does **not** say what the
shader sees in the channels the format does not have. Word 3's low twelve bits
are four three-bit selectors -- X at bits 0-2, Y at 3-5, Z at 6-8, W at 9-11,
with 0 the constant zero, 1 the constant one and 4 to 7 the fetched X, Y, Z and
W -- and a format's missing components **alias** instead of filling. With the
identity selectors (R, G, B, A, the twelve bits `0xfac` the M3 canary and C4
ran), a solid texel read back as:

| Format | Texel bytes | Fetched as |
| --- | --- | --- |
| `R8_UNORM` | `80` | 0.5, 0.5, 0.5, 0.5 -- one component, replicated into all four |
| `R8G8_UNORM` | `80 40` | 0.5, 0.25, 0.5, 0.25 -- two components, cyclically (R, G, R, G) |
| `R16_UNORM`, `R16_SFLOAT`, `R32_SFLOAT` | one channel | the same replication, alpha included |
| `R16G16_UNORM`, `R16G16_SFLOAT`, `R32G32_SFLOAT` | two channels | the same cycle |

Vulkan's fetch rule requires (R, 0, 0, 1) for a one-channel format, (R, G, 0, 1)
for two, (R, G, B, 1) for three and the identity for four, so the selectors are
what carry the fill: `0x204`, `0x22c`, `0x3ac` and `0xfac`
(driver/ps5vk_private.h, `PS5VK_FORMAT_SWIZZLE_R001`), chosen per format entry.
With them all seventeen formats fetched the colour their texel decodes to,
channel for channel, in the first run of the fixed driver (pid 148, 17 of 17),
and the console's own submission capture is `golden/v0-formats-sampled`. RADV
does exactly this: `radv_compose_swizzle` maps a format description's swizzle --
`PIPE_FORMAT_R8_UNORM`'s is X, 0, 0, 1 -- into these selectors
(`src/amd/vulkan/radv_image_view.c`, `src/amd/common/ac_descriptors.c`).

Two consequences worth recording. The descriptor's format word and its selectors
are one pair: a format table entry that carries only the `IMG_DATA_FORMAT` gets
the identity rule, which is right for four-channel formats and wrong for every
other one. And a probe that fills a texture with one value cannot tell
replication from a four-channel re-read -- both produce the same colour -- so
the fill has to be a value the decode can name, which is what the seventeen
one-value textures do.

## A tiled row keeps four texels contiguous

Measured on 2026-09-17, Phase C7's copies (console run pid 155,
`Klog_Logs/c7-copy-run3.log`). The tiled RGBA8 map's XOR places a row's texels
in runs: for a fixed y the byte offsets of x and x+1 differ by four bytes inside
a run of four texels and jump outside it, so a 128-texel row is 32 runs of 16
bytes and no longer one anywhere (checked against the fitted map for every row
and every 4-texel offset). That is what a CPU copy into or out of tiled storage
has to walk, and the driver records exactly one memory copy per run
(driver/ps5vk_image.c, ps5vk_cmd_buffer_copy_image_region). The console's own
run confirms the placement: every one of the 64x36 destination texels landed at
the address the map names (`agc_c7_copy tiled_texels` 2304 of 2304 for the copy
frame and 2304 of 2304 for the one-to-one blit frame), and the frames' readbacks
were the canary's pattern pixel for pixel.

## An sRGB fetch linearises the first three fetched components, not the channels

Measured on 2026-09-17, Phase C7's per-format blits (console run pid 160,
`Klog_Logs/c7-blit-formats-run4.log`). A texel of
`VK_FORMAT_A8B8G8R8_SRGB_PACK32` written as A, B, G, R = 0xff, 0xe1, 0xbc, 0x89
-- that is, red 0x89, green 0xbc, blue 0xe1, all of which linearise to 0x40,
0x80 and 0xc0 -- read back as **0x89, 0x80, 0xc0**: the green and blue channels
were linearised, the red was not.

The descriptor's DST_SEL selectors can move a fetched component to another
output channel, which is how `A8B8G8R8_*`'s reversed memory order is handled at
all: with the identity selectors a `A8B8G8R8_UNORM` texel's alpha would come out
as its red. The selectors are `W, Z, Y, X` for those formats (the twelve bits
`0x977`, `PS5VK_FORMAT_SWIZZLE_WZYX`), and the console's own fetch of the
UNORM and SNORM forms confirms it. The **sRGB** curve, though, is bound to the
*fetch* order: the hardware linearises the first three fetched components and
then selects, so a byte-reversed sRGB texel has the curve applied to the wrong
three bytes and no selector can move it. The driver therefore does not claim
`VK_FORMAT_A8B8G8R8_SRGB_PACK32` at all -- it needs a shader-side swizzle, which
is a gap the audit records. The other sixteen sampled formats are unaffected,
and the same run's `v0-formats-sampled` pass is what shows it: the sRGB packed
format failed there while `c7-blit-formats`' CPU decode of it was correct, which
is how the two halves of the format's path were told apart.

## The mip chain rule checks out for the levels that are not in the tail

Checked on 2026-09-17, offline, against the measured five-level 256x256 chain
(`kMipLevelBases`, measured in Phase C7 and recorded above). Transcribing
AddrLib's rule for a 64 KiB swizzle mode
(`Gfx10Lib::GetMipTailDim`, `GetMaxNumMipsInTail`, and the chain-sizing block of
`gfx10addrlib.cpp`; the sources are in `.deps/native/mesa`) predicts exactly what
the console measured for everything outside the mip tail:

- `GetMipTailDim` for a thin 2D resource halves the **width**: the tail's largest
  dimension is 64x128 texels for the driver's 128x128-texel tile, so the first
  level that fits the tail is level 2 (64x64), and `firstMipInTail` is 2 --
  which is what the measured bases imply (levels 0 and 1 sit at whole-tile
  offsets, levels 2 and up inside one);
- the tail block is reserved first (`blockSize` = 0x10000 bytes), the levels
  outside it follow **from the tail upwards**: level 1 at 0x10000 and level 0 at
  0x10000 + 0x10000 = 0x20000, exactly the measured bases;
- the chain ends at 0x20000 + 256x256x4 = 0x60000, exactly the measured storage.

One piece does **not** match, and it is the one the next measurement is for: the
rule's offsets for the levels *inside* the tail (`mipOffset = (m > 6) ? (16 << m)
: (m << 8)`, with `m` counted down from `GetMaxNumMipsInTail` = 12) give
0x8000, 0x4000 and 0x2000 for levels 2, 3 and 4, where the console read 0x8000,
**0x4800** and **0x800**. Two of the three differ, so either the tail's offsets
are relative to something the code does not name `offset`, or `m` does not start
where the transcription assumes. The six-level chain's page map
(`c7-mip-pages-six`) has a four-level tail, which is what tells the two apart:
the rule predicts 0x8000, 0x4000, 0x2000, 0x1000 for its levels 2 to 5.

## A mip tail has no per-level base: its levels are interleaved into one block

Follow-up to the rule check above, read out of AddrLib's addressing code
(`Gfx10Lib::HwlComputeSurfaceAddrFromCoord` in `gfx10/gfx10addrlib.cpp`,
`.deps/native/mesa`). For a level **outside** the tail the address is
`sliceSize * slice + mipInfo[level].offset + block + bank`, so each such level
really does sit at its own base -- which is why the transcribed rule reproduced
0x10000 and 0x20000 exactly. For a level **inside** the tail the same code uses
`mipInfo[level].macroBlockOffset` (which the chain sizing sets to **zero** for
every tail level) plus the level's **coordinates shifted by `mipTailCoordX/Y/Z`**:
the tail's levels are interleaved into the one block the chain reserved, not laid
out at per-level bases at all.

That is why the simple `mipOffset` transcription disagreed with the measured
0x8000, 0x4800 and 0x800: those three are the *addresses* the console's sampler
read for the levels' texels, which is what the address map measures, but they are
not offsets a base table can reproduce -- a CPU-side layout has to reproduce the
tail's interleaving (AddrLib's `pMipInfo[level].pitch/height` and `mipTailCoord*`)
or be measured per chain shape. It also says what the driver's tiled upload needs
for a mipmapped image: the tail block's reserved space and the per-level
coordinate offsets, not a table of level bases.

## The mip tail's addressing rule, checked against the measured bases

Follow-up to the interleaving finding above: with AddrLib's tail coordinate rule
(`gfx10addrlib.cpp`, the `mipX`/`mipY` extraction from `mipOffset` and
`mipTailCoordX = mipX * Block256_2d[index].w`, where `Block256_2d[2]` is `{8, 8}`
for four-byte texels) the measured bases come out of the swizzle equation the
driver already uses (`tiled_rgba8_offset`), not out of a base table:

| Level | `mipOffset` (rule) | (mipX, mipY) | Swizzled address | Measured |
| --- | --- | --- | --- | --- |
| 3 | 0x4000 | (0, 8) | 0x4800 | **0x4800** |
| 4 | 0x2000 | (4, 0) | 0x800 | **0x800** |
| 2 | 0x8000 | (8, 0) -> 0x8400 | 0x8000 is the swizzle of **(8, 4)** | 0x8000 |

Two of the three tail levels match the rule exactly, which confirms both the
coordinate extraction and the swizzle: a tail level's texels are read at the
swizzled address of its `(mipX, mipY)` coordinate inside the one reserved block.
The first tail level (level 2) is one 4-unit step off in `mipY` -- the swizzle of
(8, 4), not of (8, 0), is what the console read -- so either its coordinate
carries a term the other levels do not, or the count `m` starts one lower for it.
The six-level chain's page map (`c7-mip-pages-six`, four tail levels) is what
tells those apart.

## Level 2's base is 0x8400: the tail rule holds for all three tail levels

Checked on 2026-09-18, offline, against run 33's own raw addresses and against
AddrLib built as a host tool. It corrects one entry of the five-level table
above: **level 2's base is 0x8400, not 0x8000**, and with that entry fixed the
whole chain is AddrLib's rule and the console's measurement at once.

The finding above ("The mip tail's addressing rule, checked against the measured
bases") read 0x8000 for level 2 out of the address map and recorded it as one
4-unit step off the rule. The map's own numbers say otherwise. Run 33's
`agc_mip_addresses` records are the addresses the five pinned-LOD frames
fetched, level by level
(`Klog_Logs/c7-mip-run33.log` and `run34.log`, `"field":"pinned_address"`):

| Pinned LOD | 0 | 1 | 2 | 3 | 4 |
| --- | --- | --- | --- | --- | --- |
| Fetched | 0x2f0fc | 0x13cfc | 0xb4fc | 0x58fc | 0x8fc |

A pinned frame samples its level's centre, texel `(side / 2 - 1, side / 2 - 1)`
-- the map's fitted texel for each of the five addresses, independently of any
base, is that texel for all five (`tiled_rgba8_offset`, the map the target
readbacks decode). So each address is a level's base plus the map of one known
texel, and the bases fall out:

| Level | Side | Texel | Map of that texel | Base + map | Base |
| --- | --- | --- | --- | --- | --- |
| 0 | 256 | (127, 127) | 0x0f0fc | 0x2f0fc | 0x20000 |
| 1 | 128 | (63, 63) | 0x03cfc | 0x13cfc | 0x10000 |
| 2 | 64 | (31, 31) | 0x030fc | 0xb4fc | **0x8400** |
| 3 | 32 | (15, 15) | 0x010fc | 0x58fc | 0x4800 |
| 4 | 16 | (7, 7) | 0x000fc | 0x8fc | 0x800 |

0x8000 for level 2 cannot produce 0xb4fc: it would need a texel of a 64x64 level
whose map is 0x34fc, and 0x34fc has bit 13 set with bit 9 clear, which no texel
of that level reaches (bit 13 comes from x bit 4, which also sets bit 9). 0x8000
is the *lowest* address level 2's texels reach -- the map of its texel (0, 4) --
and 0x8400 is the level's start, the map of its texel (0, 0), which is what the
tail coordinate says it is.

That is AddrLib's answer as well. `tools/check-mip-layout.sh` builds the pinned
AddrLib (`.deps/native/mesa`, the same sources `docs/M5_REFERENCE.md` names) for
the host and asks `Addr2ComputeSurfaceInfo` for the chain -- the call radv makes
(`src/amd/common/ac_surface.c`) -- and it prints, for a five-level 256x256
64 KiB-swizzled four-byte chain:

| Level | `macroBlockOffset` | `mipTailOffset` | tail coordinate | Base |
| --- | --- | --- | --- | --- |
| 0 | 0x20000 | 0 | -- | 0x20000 |
| 1 | 0x10000 | 0 | -- | 0x10000 |
| 2 | 0 | 0x8000 | (64, 0) | 0x8400 |
| 3 | 0 | 0x4000 | (0, 64) | 0x4800 |
| 4 | 0 | 0x2000 | (32, 0) | 0x800 |

`surfSize` 0x60000, `firstMipIdInTail` 2: the chain's measured length and the
measured start of the tail, exactly. A level outside the tail sits at its
`macroBlockOffset`; a level inside it has **no base of its own** -- its
`macroBlockOffset` is zero and its texels are read at the swizzle of its own
coordinates shifted by the tail coordinate, so the level's start is the swizzle
of that coordinate and its texels' lowest address is generally not its start.
`mipTailOffset` (0x8000 for level 2) is not an address at all; reading it as one
is what produced the wrong entry.

Two consequences. The probe's `kMipLevelBases` and the C7 test's
`kLevelBases` now hold 0x8400 for level 2, and the driver test pins the table to
run 33's five fetched addresses rather than to a reading of them, so a base that
is one of a level's addresses instead of its start fails the test. And the
driver's own tiled sizing still lays each level's tiles after the last
(`ps5vk_image_storage`: 0x80000 for this chain against the measured 0x60000):
that sizing is larger than the hardware's, so nothing overflows today, but a
tiled upload has to place texels by the rule above, tail interleaving included,
when that step lands (docs/M5_PHASE_C.md).

## A tile is 64 KiB of elements, whatever the element size

Checked on 2026-09-18, offline, against AddrLib and the SDK.
`tools/check-mip-layout.sh` prints AddrLib's block for a 128x128 image of each
element size: 1 byte gives 256x256, 2 gives 256x128, 4 gives 128x128, 8 gives
128x64 and 16 gives 64x64, each exactly 0x10000 bytes. That is the driver's own
table (`driver/ps5vk_image.c`, `ps5vk_tile_extent`) and the SDK's
`ps5_tiled_color_tile`, and the check script now holds AddrLib's rows against
it.

AddrLib refuses a multi-sample shape -- `ADDR_INVALIDPARAMS` for 2, 4 and 8
samples, in every swizzle mode and for both the texture and the colour flag --
so the four-sample row rests on the SDK's `ps5_tiled_color_msaa4_tile` alone:
1 byte gives 128x128, 2 gives 128x64, 4 gives 64x64, 8 gives 64x32 and 16 gives
32x32, again exactly 0x10000 bytes of four-sample elements each, which is what
the driver's halving of the one-sample extents produces.

Worth separating what that does and does not settle. The four-sample **extent**
is unmeasured on the console: C8's run read its four-sample target's storage
linearly and found the colour in 8,294,400 of 8,388,608 words (four a texel),
which a wrong extent would not have shown, and the resolve's CPU pass walks that
map texel by texel, so a tile that is not 64x64 would put every resolved texel
somewhere else. The invariant both tables do hold is that a tile is 0x10000
bytes of elements; the next C8 run should measure the four-sample map itself
(a known pattern read back through it), which is a step of the resolve
(docs/M5_PHASE_C.md).

## A four-sample colour target is four 16 KiB sample planes, turned by the block

Checked on 2026-09-18 against the console, pid 110 to pid 119.
`vkCmdResolveImage` needs to read a texel's four samples, so the four-sample
target's storage had to be mapped. It is not AddrLib's to give: AddrLib refuses
a multi-sample shape (`ADDR_INVALIDPARAMS`; the only 64 KiB mode it accepts for
four-sample color is `ADDR_SW_64KB_R_X`, and its coordinate function disagrees
with the console's own measured one-sample map by four, so it is a size oracle
here and not an address one). The map below is the console's own measurement,
from a frame that covers the whole target with a green/blue gradient: a word's
value names the pixel it came from, and its address is where the map put it
(`agc_resolve_map` in the runner, `src/diagnostics.cpp`).

**One tile is 64x64 texels in four 0x4000-byte sample planes**, in sample
order, each plane one four-byte word a texel. The four planes of a tile hold
*identical* words for a frame whose fragment shader runs once a pixel
(8,294,400 drawn words became 33,177,600 when the sample count reached the
hardware, and plane 0, 1, 2 and 3 compared equal to the word). Inside a plane
the texel's place is the same measured swizzle the one-sample map uses
(`ps5vk_image.c`, `ps5vk_tiled_texel_offset`), at 64-texel granularity.

**The block's own place in the grid turns the plane**: bit 5 of the
block-local y is XORed with bit 0 of the block's column, and bit 5 of the
block-local x with bit 0 of the block's row. Measured on thirteen blocks --
(0,0) unturned, (1,0) (0,32), (2,0) unturned, (3,0) (0,32), (4,0) unturned,
(5,0) (0,32), (8,0), (16,0) and (32,0) unturned, (59,0) (0,32), (0,1) (32,0),
(1,1) (32,32), (0,2) unturned -- each fitting all 2048 words of its plane
exactly. The one-sample 128-texel tile has no such turn in its own measured
map, so the two maps differ beyond the tile size.

**`CB_COLOR0_ATTRIB.NUM_SAMPLES` alone does not make the hardware render four
samples.** Two things were missing, and both were found by measuring rather
than by reading: the driver cleared the field again right after setting it (a
leftover `records[3].value &= ~(0x7000u | 0x18000u)`), and the rasterizer needs
its own state -- `PA_SC_AA_CONFIG` (MSAA_NUM_SAMPLES and MSAA_EXPOSED_SAMPLES
are the sample count's log2, MAX_SAMPLE_DIST 6), `PA_SC_MODE_CNTL_0.MSAA_ENABLE`,
the four `PA_SC_AA_SAMPLE_LOCS_PIXEL_*` registers with the standard 4x pattern
`0x622ae6ae`, the centroid priorities `0x32103210`, both `PA_SC_AA_MASK_*` full
and `DB_EQAA`'s sample counts. Every earlier four-sample run wrote **one** word a
texel (pids 110-116: the field was cleared, so the target was programmed as a
one-sample one, whatever the rasterizer was told); with the clear removed and
the rasterizer registers in the table, the same frame wrote **four**
(pid 118, 33,177,600 words). The driver programs the registers in
`ps5vk_multisample_registers` (`driver/ps5vk_draw.c`) and the sample planes in
`ps5vk_image_copy_address` (`driver/ps5vk_image.c`).

## A readable GPU clock, and the ZPASS_DONE write's wider footprint

The console writes a readable 64-bit GPU clock: a RELEASE_MEM (PKT3 opcode 0x49)
with the BOTTOM_OF_PIPE_TS event (40) and an EOP selector naming the timestamp
(`EOP_DST_SEL(MEM) | EOP_INT_SEL(SEND_DATA_AFTER_WR_CONFIRM) |
EOP_DATA_SEL(TIMESTAMP)` = 0x63000000; `EOP_INT_SEL(NONE)` = 0x60000000 writes it
too) puts the clock at the address the packet names. Measured against
CLOCK_MONOTONIC over a 1.0877 s sleep, the counter advanced 108,536,495 ticks:
**99.79 MHz, a period of about 10.02 ns**, which is the 100 MHz reference clock a
GPU timestamp is expected to be; the interval carries the submit and marker
latency, which is the ~0.2% below nominal. The magnitude (1.036e12 ticks, 2.9
hours) is the console's uptime, so the clock is not per-title. There is **no**
valid-bit in it, unlike the z-pass counter, whose bit 63 the same hardware sets
(V0-query). Console run pid 143, `Klog_Logs/v0-timestamp-run5.log`.

Two facts about the write's *footprint* came out of the same probe. A
ZPASS_DONE EVENT_WRITE leaves the counter value at the address it names **and**
counter words across a wider range: the console's second-frame capture showed
`0x800000000007e900` and `0x8000000000000000` from stage+0x130 to stage+0x1fc
after a frame whose two samples went to stage+0x120 and stage+0x128. A capture
holds the workspace as it stands when the frame is built, so a probe that reads
its samples between frames has to clear the whole 0x200-byte scratch block, not
just its own slots, or the leftover words are a workspace difference no PC
rebuild can replay (tools/golden.py rebuild). The other fact is framing: a
RELEASE_MEM packet is **eight** words (header `0xc0064900`, op `0x0030c528`,
selector, address lo, address hi, two immediate words, one trailing word), and
the runner's `validate_indirect_register_tables` counts a type-3 packet as its
count field plus two, so a seven-word version is refused before submission.

## The array and cube layout: a slice is a chain, and a cube is six slices

D1's texture arrays had one number nobody had: where layer *i* of a tiled image
lives. AddrLib answers it, and `tools/mip-layout-oracle.cpp` now prints the
answer (`tools/check-mip-layout.sh`, which holds it against the oracle shape by
shape): **a slice is a whole chain of its own, and the layers are consecutive
chains**. A 256x256 five-level two-layer image is `sliceSize` 0x60000 -- exactly
the one-layer chain's measured 0x60000 -- with the layers at 0x0 and 0x60000;
a 64x64 six-slice image is six 0x10000 slices at 0x0..0x50000.

The second half of the same measurement: **AddrLib's ADDR2 API has no cube flag
at all** (`ADDR2_SURFACE_FLAGS` carries 22 fields and `cube` is not one of
them; the older `ADDR_SURFACE_FLAGS` has one, which is why the first version of
the oracle tried to set it and failed to compile). A cube is a six-slice array
to the layout rule, and the oracle's two rows are byte-identical -- so a cubemap
costs the driver no new storage rule, only the descriptor's kind word.

**Boundary.** This is the *storage* answer, not the sampler's. Which descriptor
field makes a view read layer *i* -- and which kind word makes six layers a cube
-- is still unmeasured, and it is Battery 1's first two questions
(`docs/M5_REFERENCE.md`, `V0-unknowns`). The driver's own array sizing already
agrees with the oracle by construction: `ps5vk_image_storage` multiplies the
measured chain by the layer count for the shapes its table covers
(`driver/ps5vk_image.c`), which is what "layers are consecutive chains" means.
What the driver still refuses is every copy and view that names a layer but the
first (`array layers are D1`), which is the work the answers unblock.

## The image resource descriptor, field by field, and the format words

Every sampled-image descriptor the driver writes can now be checked against the
pinned register database instead of against a probe:
`.deps/native/mesa/src/amd/registers/gfx10-rsrc.json` carries the
`SQ_IMG_RSRC_WORD*` field layouts (`register_types`) and the `GFX10_FORMAT`
values, and `gfx9.json` carries the resource type enum (`SQ_RSRC_IMG_TYPE`).
What the driver already wrote matches it exactly -- `FORMAT` at word 1 bits
20-28 (`ps5vk_draw.c`'s `entry->image_format << 20`), `WIDTH_HI`/`HEIGHT` at word
2, the four `DST_SEL`s at word 3 bits 0-11 with `BASE_LEVEL` 12-15 and
`LAST_LEVEL` 16-19, `SW_MODE` 20-24, `MAX_MIP` at word 5 bits 4-7 -- and two
fields the driver has never written are in the same table:

| Field | Register | Bits | What it holds |
| --- | --- | --- | --- |
| `DEPTH` | `SQ_IMG_RSRC_WORD4` | [0, 12] | the slice count minus one: an array's or cube's layer count |
| `BASE_ARRAY` | `SQ_IMG_RSRC_WORD4` | [16, 28] | the view's first layer |
| `TYPE` | `SQ_IMG_RSRC_WORD3` | [28, 31] | `SQ_RSRC_IMG_2D` 9 (the driver's `0x9...` kind), `SQ_RSRC_IMG_CUBE` 11, `SQ_RSRC_IMG_2D_ARRAY` 13, `SQ_RSRC_IMG_2D_MSAA` 14, `SQ_RSRC_IMG_2D_MSAA_ARRAY` 15 |

So D1's arrays are `DEPTH = layers - 1` and `BASE_ARRAY = view->base_layer` in
word 4 with `TYPE` 13, and a cubemap is the same six slices with `TYPE` 11 --
which is the *storage* answer above ("a cube is a six-slice array") meeting the
descriptor answer. A third confirmation of the layer model comes from radv
itself (`src/amd/vulkan/radv_image.c`, `GetImageSubresourceLayout`):
`arrayPitch = surf_slice_size` and a layer's offset is `slice_size * layer`, the
same "a slice is a chain, layers consecutive" the oracle prints.

**The format words** the audit's packed rows wait for are in the same file
(`GFX10_FORMAT`, the value the descriptor's `FORMAT` field takes):
`5_9_9_9_FLOAT` 132 for `E5B9G9R9_UFLOAT_PACK32`, `5_6_5_UNORM` 133 for
`R5G6B5_UNORM_PACK16`, `1_5_5_5_UNORM` 134 for `A1R5G5B5_UNORM_PACK16`,
`4_4_4_4_UNORM` 136 for `B4G4R4A4_UNORM_PACK16`, `10_11_11_FLOAT` 36 for
`B10G11R11_UFLOAT_PACK32`, `2_10_10_10_UNORM` 50 and `_UINT` 54 for the
`A2B10G10R10_*` forms, `8_8_8_8_SRGB` 130 for `R8G8B8A8_SRGB`, and the 16- and
32-bit families at 7/8/13, 23/24/29, 65/66/71 and 72-77.

**Boundary.** None of this is a console measurement: it is the reference the
probe cases are written from, and the reference is only worth what its
confirmation is. Battery 1's array and cube cases confirm the two word-4 fields
and the two `TYPE` values on the hardware, and its packed cases confirm each
`FORMAT` word's decode, before any row is reported or any bit is claimed.

## A four-sample depth target: four words a texel, and what the write needs

C8's depth half began with three things the console had never done: create a
four-sample depth attachment, have a draw write depth into it, and read its
storage back. The measurement is the runner's battery `jobs/unknowns-depth4x`
(`unknowns-depth4x-map`, `-less` and `-ramp`), whose cases claim nothing; these
are its answers so far.

**The attachment works, and so does the clear.** The depth image carries
`samples = 4`, the render pass's depth description has to say the same (the
runtime asserts when it does not: `vk_common_CmdBeginRenderPass2: Assertion
'image_view->image->samples == pass_att->samples' failed`, which killed the first
console attempts and reproduced on the PC), and `DB_Z_INFO`'s `NUM_SAMPLES`
(bits 2-3, the count's log2 -- `0x8000018b` for four) is set from it. A 4K
four-sample depth target is 128 MiB: 2025 64x64-texel tiles of 64 KiB. The
pass's CLEAR wrote its 1.0 to **33,177,600 words = 8,294,400 texels x 4 samples**,
the whole target, so every texel really has four words of storage.

**The write needs the test on.** With the depth test disabled and the write on
(the M4 canary's write path, ALWAYS), a four-sample target keeps **no** drawn
depth at all: clear 33,177,600, near 0, far 0 (run pid 226, `-map`). With LESS
enabled the same geometry writes depth, and the counts are the geometry's own: a
near rectangle 960x1080 texels and a far one 1440x810 that overlaps it by
960x540. The near one wins the overlap, so the far one keeps (1440x810) -
(960x540) = 648,000 texels, and the console read back **near 8,294,400 words and
far 2,592,000** -- exactly four words a texel for each, with the depth test
deciding the overlap (run pid 227/228, `-less`). At one sample the untested write
path works (C5); at four it does not, which is the battery's second answer.

**Four words, one sample each.** The last question was how a texel's four words
sit: four 0x4000-byte sample planes (the colour target's map) or four consecutive
words (16-byte texels). A ramp frame -- one quad whose depth runs 0.25 to 0.75
across the target, so a word's depth decodes to the column it came from -- shows
**columns advancing word by word** (`0000 0001 0000 0001 0001 0002 ...`), not
four words of one column, so the samples are separate words rather than a packed
16-byte texel. The exact within-plane order for a 64x64-texel block is the one
thing still open: the swizzle masks the driver holds are the 128-texel *one
sample* map's, and fitting them to this sequence is the next measurement (a probe
that draws single-texel quads at known columns, whose offsets name the map
directly).

**Boundary.** These are measurements from three cases that claim no capability:
`ps5vk_image_copy_address` walks a four-sample depth side as four 0x4000 planes
today, which the four-words-a-texel answer supports, but until the within-plane
order is fitted the driver's map is a hypothesis, and no probe result is reported
as a C8 closure yet.

## Four-sample depth: sixteen-byte texels, and a clear that needs no map

The entry above read the ramp's word sequence as one word a column and left the
within-plane order open. Decoding the same ramp against the four-sample pattern
says the opposite, and three more probes agree: **a four-sample depth texel is
sixteen bytes and its four words are that texel's four samples.** The ramp's
first eight words are pixel 0's four samples at x = 0.375, 0.875, 0.125 and
0.625 -- the MSAA pattern's own x offsets, in sample order -- then pixel 1's
four, then pixels 2 and 3 at words 16..23, which is what "columns advancing word
by word" was: the samples of one pixel, not four texels of one column. The
strongest count is a 1x2-texel strip (`unknowns-depth4x-rects`, the nested
family at the origin): its eight words sit in **two** elements, so an element is
four words and a texel is one element. Four 0x4000-byte planes of four-byte
texels would have put those eight words in eight elements.

**The tile grid.** Eight 64x64-texel probes (the same case) put a 64x64-texel
block's words in 64 KiB tiles whose index steps **+1 per 64 texels of x and -60
per 64 texels of y** -- rows of sixty tiles, walked backwards as y grows, with
the image's first tile 1,920 tiles into the allocation. That is the one-sample
map's *shape* at the measured four-sample block size, and not its masks.

**The element masks.** A strip per low bit at a known origin reads the
within-tile element swizzle off the storage one bit at a time, in element units:
**x bits 0..5 are 1, 4, 2048, 16, 560, 144** and **y bits 0..4 are 2, 8, 1024,
272, 96** (all inside a 4,096-element tile, so all read within one page), while
**y bit 5 moves the storage by sixty tiles** -- the largest of the measured
jumps, and the reason the swizzle's period is wider than one tile rather than a
mask inside it. These are the fitted masks, not a driver constant: the driver
refuses to place a four-sample depth sample itself and says so.

**A one-texel quad does not rasterize at four samples.** The single-texel probe
(15 quads at bit positions of x and y, each with its own depth and tint) found no
depth word and no colour word anywhere in the target, three runs in a row
(`unknowns-depth4x-single`, pids 153/168/227 in their klogs). Two-texel strips
draw, and every measurement above uses them. The failed case claims nothing.

**The clear.** The target is 128 MiB -- the driver sizes a 4K four-sample depth
image as the whole allocation -- and the storage is exactly four words a texel:
the hardware's own clear wrote 33,177,600 words = 8,294,400 texels x 4. So
`vkCmdClearDepthStencilImage` on such an image needs no sample map at all: every
word of the image takes the clear value, which is one 32-bit fill of the
storage, and that is what the driver records. The console proved it
(`c8-depth-clear`, Klog_Logs/c8-depth4x-run2.log, run 2): the whole target --
33,554,432 words, the 128 MiB allocation and not just the image's 33,177,600 --
holds 0.5 exactly, so a clear that wrote one sample in four or walked the
one-sample map's four-byte texels fails the check.

**Boundary.** The element masks are measured for the low six bits of each axis
within a tile; y bit 5 and above move whole tiles, and the exact per-texel map of
those high bits is not fitted. The driver therefore refuses a CPU copy of a
four-sample depth image **by name** (`ps5vk_image_copy_side`) instead of placing
samples with a hypothesis, and clears one by filling its storage. A partial-range
clear of such an image is refused by name too. What remains open is the copy
path, not the clear or the attachment.

## The unsigned integer fetches work with the UNORM selectors; the signed ones do not

Measured on the console while trying to close V0-formats' signed integer
families the way the unsigned ones were closed (Klog_Logs/v0-formats-sampled-sint-run1.log,
run pid 119). The unsigned set is proved: `R8_UINT`, `R8G8_UINT` and
`R8G8B8A8_UINT` fetch the integer their texel holds through a `usampler`, with
the register database's `8_UINT` 5, `8_8_UINT` 18 and `8_8_8_8_UINT` 60 and the
same DST_SEL selectors their UNORM twins use (`R001`, `RG01`, `RGBA`).

The signed twins are not the same shape. With `8_SINT` 6, `8_8_SINT` 19 and
`8_8_8_8_SINT` 61 and those same selectors, `R8_SINT`'s frame passed while
`R8G8_SINT`'s read back **red 0x40, green 0x00, blue 0x00** where its texel was
`0x40, 0x80`, and `R8G8B8A8_SINT`'s failed too. So a signed integer fetch does
not put its second component where the unsigned one puts it: the selectors for
the signed words are their own, and the next attempt has to fit them (a probe
that samples one channel at a time, the way the depth map's masks were fitted,
is the cheap way). The change was reverted rather than shipped: nothing claims
the signed rows yet, and the audit still reads 31 formats reported.

## A wider integer texel has to hold a value a byte can carry

Measured while extending the unsigned integer probe from the 8-bit formats to
`R16_UINT`, `R16G16_UINT`, `R32_UINT`, `R32G32_UINT` and `R32G32B32A32_UINT`
(Klog_Logs/v0-formats-sampled-uint-run2.log, run pid 119). The probe's shader
writes the fetched integer over 255 into an R8G8B8A8_UNORM target, so the byte
the frame keeps is the integer itself only while the integer fits a byte: the
first table used 64 x 255 and 128 x 255 for the 16- and 32-bit texels and the
console read back **255, 255, 0** -- the target clamped 64.0 and 128.0 to 1.0.
The register words themselves are not in question (`16_UINT` 11, `16_16_UINT`
27, `32_UINT` 20, `32_32_UINT` 62, `32_32_32_32_UINT` 75), and the corrected
table -- texels of 64, 128 and 192 at the wider widths -- was written but not
run to a pass before the change was reverted: nothing claims those rows, and
the audit still reads 31 formats reported. The eight-bit three
(`R8_UINT`/`R8G8_UINT`/`R8G8B8A8_UINT`, run 1 of the same battery) stay proved.

### What the signed integer fetch did, channel by channel

The signed run's own log (Klog_Logs/v0-formats-sampled-sint-run1.log) has the
per-format readback, so the failure is narrower than "the selectors are wrong".
`R8_SINT` (one channel, texel 0x40) came back red 64, green 0, blue 0, alpha 1
-- correct for a one-channel format (alpha fills in as one, and the shader
writes it as the integer 1) -- and `R8G8_SINT` (texel 0x40, 0x80) came back red
64, green 0, blue 0, alpha 127. So the first component arrives where the first
component should and the **second component does not arrive in green at all**:
with the `RG01` selectors the hardware put the second signed byte somewhere the
green channel did not read. 127 in alpha is the other signed byte read as a
positive integer (0x7f), which says the data reached the fetch and the *select*
is what is misplaced. The leading candidate is therefore a swap of the first two
selectors for the signed words (`GR01` rather than `RG01`), which one battery
with `R8G8_SINT` alone can settle; the 4-byte format's frame never recorded, so
it has no reading of its own yet.

### The signed selectors: two candidates, both refuted

Fitting the signed words' selectors one candidate at a time
(Klog_Logs/v0-formats-sampled-sint-run2.log, run pid 120):

* `RG01` on `8_8_SINT` 19 for `R8G8_SINT` with a texel of `0x40, 0x80` read back
  **red 64, green 0, blue 0, alpha 127**;
* `YX01` (the first two selectors swapped: R out of Y, G out of X) read back
  **red 0** and the case scored 0 of 1.

So the first byte does map to X -- swapping put red to zero -- and the second
byte does not reach green under either order. Alpha 127 in the first reading is
the interesting number: it is `0x7f`, the second byte read as a *positive*
integer, which says the fetch produced a value the selectors then placed
somewhere other than green, not that the data was missing. The next candidates
worth a battery, in order: `RGBA` (leave the selectors alone and let the fill-in
rule place the tail), then the two-channel words' own pair from the register
database (`8_8_SNORM` 20 and `8_8_USCALED`, to see which the hardware treats as
the signed pair), and only then a wider search. Two data points are recorded;
neither is a fit, and nothing claims the signed rows.

### Three selector candidates, and what they say to fit next

The signed pair `R8G8_SINT` (texel `0x40, 0x80`) against the register database's
`8_8_SINT` 19, one candidate a battery:

| Selectors | Fetched (R, G, B, A) | run |
| --- | --- | --- |
| `RG01` | 64, 0, 0, 127 | Klog_Logs/v0-formats-sampled-sint-run1.log |
| `YX01` | 0, .. | run2 |
| `RGBA` | 64, 0, 0, 0 | run3 |

The first byte lands in red whatever the selectors (except the swap, which moved
it away), and **the second byte never lands in green**: with `RG01` it appears as
127 in alpha -- `0x7f`, the second byte read as a positive integer -- and with
`RGBA` the alpha is the fill-in zero. So this is not a selector problem: the
*word* `8_8_SINT` 19 does not present the pair the way the unsigned `8_8_UINT` 18
does. The next thing to fit is the data format itself, not the selectors: the
register database's other two-channel eight-bit forms (`8_8_SNORM` 20,
`8_8_USCALED`, `8_8_SSCALED`) sampled with the same identity selectors, and only
if none of them presents the pair, the signed eight-bit rows are a hardware
limit to record rather than a gap to close. Nothing claims the signed rows; the
audit still reads 36 formats reported.

### A corrected word table, and one reading that has to be thrown away

The candidate list above was written from memory of the register database's
numbering instead of from the database, and one entry was wrong. Read out of
the pinned `gfx10-rsrc.json`, the eight-bit family is:

| GFX10_FORMAT | value | | GFX10_FORMAT | value |
| --- | --- | --- | --- | --- |
| `8_UNORM` | 1 | | `8_8_UNORM` | 14 |
| `8_SNORM` | 2 | | `8_8_SNORM` | 15 |
| `8_UINT` | 5 | | `8_8_UINT` | 18 |
| `8_SINT` | 6 | | `8_8_SINT` | 19 |
| | | | `8_8_8_8_UNORM` | 56 |
| | | | `8_8_8_8_SNORM` | 57 |
| | | | `8_8_8_8_UINT` | 60 |
| | | | `8_8_8_8_SINT` | 61 |

So the "SNORM word" run (Klog_Logs/v0-formats-sampled-sint-run4.log) sampled
**`32_UINT` 20**, not `8_8_SNORM`, and its reading (red 64, green 64, blue 64,
alpha 64 for a two-byte texel) says nothing about the signed pair: it is a
32-bit unsigned format read through a two-byte upload. It is recorded here only
so the next attempt does not repeat the mistake. The signed pair's real
candidates are `8_8_SNORM` 15 and, if that does not present the pair, the
USCALED/SSCALED forms -- whose numbers have to be read out of the database
rather than recalled. `8_8_UINT` 18 with `RG01` remains the unsigned proof
(36 formats reported); nothing claims the signed rows.

## The signed pair's word is 8_8_SINT 19, and the earlier refutations were reading a frame that could not pass

Fitting the two-channel signed word for `R8G8_SINT` turned up a flaw in the
probe before any word could be compared. The expectation the earlier candidates
ran with was `0x40, 0x80` -> `64, 128, 0`, but `0x80` is **-128** read as
signed, and no signed two-channel decode can write 128 in green. Runs 1-3's
readings -- red 64 and green 0, whatever the selectors did -- are exactly what a
*correct* signed fetch of `64, -128` writes into an RGBA8 target: 64 in red and
-128 clamped to 0 in green. Re-read against that expectation:

| Selectors | Read back | run |
| --- | --- | --- |
| `RG01` | 64, 0, 0, 127 | Klog_Logs/v0-formats-sampled-sint-run1.log |
| `YX01` | 0, 64, 0, 0 | run2 (the swap moved the first byte out of red) |
| `RGBA` | 64, 0, 64, 0 | run3 (red 64 and green 0 again; the identity selector also reads Z) |

So none of the three refuted the word: they had asked it for a value a signed
decode cannot produce. The probe now carries two frames a signed fetch can meet
-- a positive pair (`0x20, 0x40` -> `32, 64, 0`) and the negative second byte
(`0x40, 0x80` -> `64, 0, 0`) -- and the case passes only when both hold.

The word search that followed, one candidate a battery:

| Candidate | Selectors | Frame | Read back | run |
| --- | --- | --- | --- | --- |
| `8_8_SNORM` 15 | `RGBA` | `0x20, 0x40` -> `32, 64, 0` | 0 of 38,400; red 255 in every pixel, middle `255, 255, 255, 255` | `Klog_Logs/v0-formats-sampled-sint-run6.log`, pid 141 |
| `8_8_SNORM` 15 | `RGBA` | `0x40, 0x80` -> `64, 0, 0` | 0 of 38,400; red 255 in every pixel, middle `255, 0, 255, 0` | run6 |
| `8_8_SINT` 19 | `RG01` | both frames | **2 of 2** | `Klog_Logs/v0-formats-sampled-sint-run7.log`, pid 142 |

The SNORM word is refuted, not merely unfitted: its frames come back saturated,
which is what the SNORM decode does to an integer probe whose shader divides the
fetch by 255 -- the word hands the fetch the decoded float, not the two bytes.
`8_8_SINT` 19 with `RG01` is the fit, the same selectors the unsigned pair is
proved with (`8_8_UINT` 18), and it is the *signed* word, so the row's claim is
the fetch's own semantics.

The rest of the signed family followed through the same isampler, one positive
frame each, with the register database's SINT word of the same field widths and
the same selectors the unsigned entries use (`Klog_Logs/v0-formats-sampled-sint-run8.log`,
run pid 145):

| Format | Word | Selectors | Texel -> read back |
| --- | --- | --- | --- |
| `R8_SINT` | `8_SINT` 6 | `R001` | `0x20` -> 32, 0, 0 |
| `R8G8_SINT` | `8_8_SINT` 19 | `RG01` | `0x20, 0x40` -> 32, 64, 0 and `0x40, 0x80` -> 64, 0, 0 |
| `R8G8B8A8_SINT` | `8_8_8_8_SINT` 61 | `RGBA` | `0x20, 0x40, 0x60, 0x7f` -> 32, 64, 96 |
| `A8B8G8R8_SINT_PACK32` | `8_8_8_8_SINT` 61 | `WZYX` | `0x7f, 0x60, 0x40, 0x20` -> 32, 64, 96 |
| `R16_SINT` | `16_SINT` 12 | `R001` | `0x20, 0x00` -> 32, 0, 0 |
| `R16G16_SINT` | `16_16_SINT` 28 | `RG01` | 32, 64 -> 32, 64, 0 |
| `R16G16B16A16_SINT` | `16_16_16_16_SINT` 70 | `RGBA` | 32, 64, 96, 127 -> 32, 64, 96 |
| `R32_SINT` | `32_SINT` 21 | `R001` | 32 -> 32, 0, 0 |
| `R32G32_SINT` | `32_32_SINT` 63 | `RG01` | 32, 64 -> 32, 64, 0 |
| `R32G32B32A32_SINT` | `32_32_32_32_SINT` 76 | `RGBA` | 32, 64, 96, 127 -> 32, 64, 96 |

**11 of 11 frames of the signed case passed** -- the two pair frames and the
nine family frames -- so all ten signed rows are reported with `SAMPLED_IMAGE`
and the maintenance1 transfer bit, and every texel value is one a byte carries.
The driver reports **46 formats**. The audit's row list moves with them: each
row loses `SAMPLED_IMAGE` and gains `TRANSFER_SRC`, which the maintenance1 rule
requires of a format the driver reports as sampled, so the probe-reachable count
stays 173 while the features open move: `SAMPLED_IMAGE` on 7 rows instead of 17,
`TRANSFER_SRC` on 25 instead of 15.

## The colour targets' CB_COLOR0_INFO words, and the frames that hold them

A colour target's format is one word: `CB_COLOR0_INFO`'s FORMAT (bits 2-6),
NUMBER_TYPE (bits 8-10) and COMP_SWAP (bits 11-12), with SIMPLE_FLOAT (bit 15)
set the way Mesa sets it for every colour target. ps5-opengl's
`sceGnmCreateRenderTarget` fills the three from a `GnmDataFormat` -- the data
format from the channel sizes, the number type from the channel type, the
component order from the channel layout (`sceGnmDfGetRtChannelType`/`Order`) --
and Mesa's `ac_get_cb_format`, `ac_get_cb_number_type` and
`ac_translate_colorswap` pick the same three values for these formats, which is
the cross-check docs/V0_FORMATS_AUDIT.md asks for. The driver now has that table
(`ps5vk_find_colour_format`, driver/ps5vk_image.c) in place of the hardcoded
RGBA8/BGRA8 pair, and the console's own frames read each format's texel back
(`Klog_Logs/v0-targets-run2.log`, run pid 172; run 1, pid 169, read the same
five):

| Format | FORMAT | NUMBER_TYPE | COMP_SWAP | Texel the frame held |
| --- | --- | --- | --- | --- |
| `R8G8B8A8_UNORM` | 10 `8_8_8_8` | 0 UNORM | 0 `SWAP_STD` | `0xffc08040` (bytes R, G, B, A) |
| `B8G8R8A8_UNORM` | 10 | 0 | 1 `SWAP_ALT` | `0xff4080c0` (bytes B, G, R, A) |
| `A8B8G8R8_UNORM_PACK32` | 10 | 0 | 2 `SWAP_STD_REV` | `0x4080c0ff` (bytes A, B, G, R) |
| `R8G8B8A8_SRGB` | 10 | 6 SRGB | 0 | `0xffe1bc89` (the sRGB encodings of 0x40, 0x80 and 0xc0) |
| `A2B10G10R10_UNORM_PACK32` | 9 `2_10_10_10` | 0 | 0 | `0xf0080100` (R 256, G 512, B 768 of 1023, A 3 of 3) |

Each frame drew the whole 3840x2160 target through the driver -- one device and
one pipeline a format, the m4-blend geometry and a colour the format quantises
exactly -- and **5 of 5 held exactly the word their format's CB_COLOR0_INFO word
writes**. The tiled map is the one the colour probes have used since M2,
word for word, which is what lets one map serve every four-byte texel here: a
format whose texel is not four bytes needs its own map, so the audit's colour
work is grouped by texel width and this first family is the four-byte one.

Two things this does not claim. Blending is still off for all of them
(`COLOR_ATTACHMENT_BLEND`): the driver programs no blend registers
(`CB_BLEND0_CONTROL`, `CB_COLOR_CONTROL`) and refuses a pipeline that enables
blending by name (driver/ps5vk_pipeline.c), so nothing is silently ignored; M4's rule -- the pixel shader must
export the target's blend format, FP16_ABGR for these 8-bit targets -- is what
that step has to carry through the driver's own pipeline path. And the
`sRGB` number type is proved in the *write* direction only: 0x40, 0x80 and 0xc0
of 255 came back as 0x89, 0xbc and 0xe1, the same pairs V0-formats' sampling
probe measured from the other side.

## Blending through the driver: CB_BLEND0_CONTROL, the FP16_ABGR export, and what the three families hold

The driver blends now, and the console's own frames say so. A pipeline's
colour-blend state becomes `CB_BLEND0_CONTROL` (0x1e0): the colour source and
destination factors in bits 0-4 and 8-12, the colour equation in bits 5-7, and
the alpha half at 16, 24 and 21 with `SEPARATE_ALPHA_BLEND` (bit 29) only when it
differs from the colour half -- the fields AMD's register headers name
(`S_028780_*`) and the word the M4 blend canary proved. VkBlendFactor is **not**
that register's numbering: Vulkan's 4 and 5 are `DST_COLOR` and
`ONE_MINUS_DST_COLOR` where AMD's are `SRC_ALPHA` and `ONE_MINUS_SRC_ALPHA`, so
the driver maps the factors through a table and refuses the constant and
second-source factors by name (they need the blend-constant registers or a
second colour source, which it programs neither of). A blending draw writes the
word and `CB_COLOR_CONTROL` (0x202) `0x00cc0011` beside its colour target's
registers; a draw whose pipeline does not blend writes neither, so every earlier
draw's words are unchanged. The pixel stage exports FP16_ABGR when the
attachment blends -- what Mesa's `ac_choose_spi_color_formats` picks for the
8-bit and ten-bit targets -- which is the M4 step 2 rule.

`v0-targets` reads both things back for the five four-byte colour targets
(`Klog_Logs/v0-targets-run3.log`, run pid 186; `golden/v0-targets` is that run's
ten submissions): a solid frame, then a frame that clears the target to a
destination colour and draws a source over it with the additive state (`ONE`,
`ONE`, `ADD`). Both colours are exact in each format's own units, so the sums
are exact integers:

| Format | Solid word | Destination + source | Blended word |
| --- | --- | --- | --- |
| `R8G8B8A8_UNORM` | `0xffc08040` | bytes 10+40, 20+50, 30+60 | `0xff5a4632` |
| `B8G8R8A8_UNORM` | `0xff4080c0` | the same in B, G, R order | `0xff32465a` |
| `A8B8G8R8_UNORM_PACK32` | `0x4080c0ff` | the same in A, B, G, R order | `0x32465aff` |
| `R8G8B8A8_SRGB` | `0xffe1bc89` | linear 0x40+0x40, 0x80+0x00, 0x00+0xc0 of 255 | `0xffe1bcbc` |
| `A2B10G10R10_UNORM_PACK32` | `0xf0080100` | 128+256, 256+512, 512+128 of 1023 | `0xe80c0180` |

**10 of 10 frames held exactly the word their format's word writes.** The sRGB
row is the interesting one: the blend happened in *linear* space and the CB
re-encoded, so 0x89 + 0x89 came back as 0xbc -- the sum of the two linear values
(0x40 + 0x40 of 255 = 0x80) re-encoded, which is the next pair V0-formats'
sampling probe measured from the other side. A hardware that blended the encoded
bytes would have saturated to 0xff, and one that ignored the blend state would
have left 0x89.

Five formats report `COLOR_ATTACHMENT_BLEND` from this run. The rest of the
audit's blend rows still wait: `R5G6B5`, `A1R5G5B5` and `R8_UNORM`/`R8G8_UNORM`
have texels that are not four bytes (their own tiled maps), the `R16_*` float
family needs its own export and word check, and the integer families need the
UINT16_ABGR/SINT16_ABGR exports Mesa picks for them rather than FP16_ABGR.

## The integer and half-float colour targets, and the compiler crash that stops the signed half

The colour-target table grew the four-byte integer and half-float families, and
the console's own frames read the unsigned five back
(`Klog_Logs/v0-targets-run5.log`, run pid 205; `golden/v0-targets` is that run's
seventeen submissions -- twelve from the UNORM/sRGB/ten-bit case and five from
the integer one):

| Format | CB FORMAT | NUMBER_TYPE | COMP_SWAP | Export | Texel held |
| --- | --- | --- | --- | --- | --- |
| `R8G8B8A8_UINT` | 10 `8_8_8_8` | 4 UINT | 0 `SWAP_STD` | 7 `UINT16_ABGR` | `0xffc08040` |
| `A8B8G8R8_UINT_PACK32` | 10 | 4 | 2 `SWAP_STD_REV` | 7 | `0x4080c0ff` |
| `A2B10G10R10_UINT_PACK32` | 9 `2_10_10_10` | 4 | 0 | 7 | `0xcc020040` |
| `R16G16_UINT` | 5 `16_16` | 4 | 0 | 7 | `0x00800040` |
| `R32_UINT` | 4 `32` | 4 | 0 | 1 `32_R` | `0x00000040` |

**5 of 5 held exactly the integer their texel's own word carries** -- one frame
a format, the m2 fullscreen triangle drawing a constant `uvec4(0x40, 0x80,
0xc0, 0xff)` and the target's storage read back through the tiled map. The
integer targets' CB word also carries the number-type-dependent bits Mesa's
`ac_build_cb_state` sets: `BLEND_CLAMP` (bit 15) clear and `BLEND_BYPASS`
(bit 16) set for the integer types ("set blend bypass according to docs if
SINT/UINT"), `ROUND_MODE` (bit 18) set for every type that is not normalized.
The driver refuses a blending pipeline on an integer attachment, which Vulkan
forbids and the CB bypasses. `SIMPLE_FLOAT` (bit 17) stays whatever AGC's
default word carries, as it does for every format proved before this.

The half-float pair at four bytes joined the UNORM case: `R16G16_SFLOAT` renders
and blends with the `FP16_ABGR` export Mesa picks for the 16-bit float class.
10.0 and 20.0 are exact halves (`0x4900`, `0x4d00`), so the solid frame's word
is `0x4d004900`, and the blended frame's 1.0 + 3.0 and 2.0 + 4.0 are 4.0 and
6.0, so its word is `0x46004400` -- both read back exactly, **12 of 12 frames**
of that case.

**One reading is a compiler crash, not a hardware limit.** The signed half of
the family did not get a reading. `v0-targets-sint`'s shader -- a constant
`ivec4` with `SPI_SHADER_SINT16_ABGR` -- compiles once: its first frame,
`R8G8B8A8_SINT`, passed on the console. The *second* compile of that same
shader in one process then aborts the runner inside ACO:

```
# reason:  abort is called(system)
# backtrace:
# aco::register_allocation(aco::Program*, aco::ra_test_policy)
# aco::lower_to_hw_instr(aco::Program*)
# aco::(anonymous namespace)::add_entry(...)
```

It is deterministic: four runs stopped at the same frame, three of them with
that same backtrace (`Klog_Logs/v0-targets-run4.log`, `-run5.log`,
`Klog_Logs/v0-targets-sint-run2.log`, `-run3.log`), and the crash is in the
compiler's register allocator rather than in the driver's GPU path. The signed
colour entries exist in `ps5vk_colour_formats` and carry no feature bit; the
probe that would prove them, and the compiler fault behind it, are the next
step's problem (docs/M5_PHASE_C.md, the active file's open findings).

## 2026-09-18: a title loads no module of its own at run time

The E2 delivery ships the driver as `libvulkan.so.1` beside `eboot.bin`, under
the name RetroArch's Vulkan driver asks for, and the console answers that the
file is not a module it will load. `jobs/e2-module` asks the loader itself,
through `sceKernelLoadStartModule`, and the answers separate the two failures
(`Klog_Logs/e2-module-run7.log`, pid 250):

| candidate | answer |
| --- | --- |
| `/app0/sce_module/libc.prx`, this repository's own runtime module | handle 20 |
| `libSceVideoOut.sprx`, a module the title imports by name | handle 42 |
| `/app0/libvulkan.so.1`, the signed driver | `0x80020008` (ENOEXEC) |
| `/app0/libvulkan-abs.so`, whose soname is that absolute path | `0x80020008` |
| `/app0/libps5vk-control.so`, `-signed.so`, `-abs.so` | `0x80020008` |
| `libvulkan.so.1`, bare | `0x80020002` (ENOENT) |

The loader works from a title, then, and it finds a module by bare name when the
title's own import list names it; what it refuses is every shared object this
project builds -- signed into the FSELF container or not, with the soname the
payload SDK's `hello_so` recipe uses or not -- as a module image. The
differentiator is neither the container nor the soname but the PS5 module shape:
the SCE module parameters and export table `tools/rebuild-libc.sh` writes into
`runtime/libc.prx`, and which a linker-produced shared object does not carry.
Three controls were staged for this (`tools/build.sh`): the same trivial library
bare, signed, and with the absolute path in its soname, and all three read
`0x80020008`; `dlopen` of all three, of the driver, of the runtime module and of
`eboot.bin` itself answered NULL.

Resolving an entry point is a second wall. `sceKernelDlsym` answered `ESRCH`
(`0x80020003`) for every name on every module that *did* load, system modules
included: `/app0/sce_module/libc.prx`'s `malloc` and its NID `gQX+4GDQjpM`, and
`libSceVideoOut.sprx`'s `sceVideoOutOpen` and its NID `Up36PTk687E` -- a symbol
this same process calls successfully through its own linked import. The loader's
own `dlsym` answered NULL for the same handles. `dlopen` is no route either:
twelve candidates in `RTLD_NOW` and `RTLD_LAZY`, and `RTLD_NOLOAD` for the
modules the process already holds (`libkernel.sprx`, `libSceLibcInternal.sprx`,
`libSceVideoOut.sprx`, `libSceAgc.sprx`), all came back NULL with `dlerror()`
NULL as well, in the process whose `sceKernelLoadStartModule` returned handles 20
and 42. The converted title does import the console's `dlopen` (NID
`UteVS6B1ZrU#D#E` in `build/eboot.elf`), so the entry point exists and does
nothing here.

**The consequence for a frontend is that a title's graphics library arrives by
linking, not by loading.** A title's own imports are bound by the loader at
process start, which is how the runner's driver, VideoOut and AGC calls all work
(`AGC is reachable only through linked imports`, above), and it is what the
sibling native projects do: ps5-opengl's title runtime binds AGC through
`load_apis(NULL, NULL, NULL, &agc, &video)` under `PS5_NATIVE_TITLE_RUNTIME` and
keeps its `dlopen` path for payloads only. A driver a title must use therefore
has to be linked into its executable, with its entry points resolved as ordinary
symbols. The module-shaped delivery would need the converter's application
exports, which `tooling/native/sce_module_writer.cpp` refuses by name, and even
then `sceKernelDlsym` would have to answer for the module's own symbols, which no
module on this console has done.

Two namespace readings from the same run are worth keeping. `/app0` and
`/app0/sce_module` exist inside a title and hold the staged files at their staged
sizes -- 16,695,760 bytes for the driver, 1,284,674 for `libc.prx` -- while
`/data/homebrew/PPSA99988` and `/temp0` do not exist there at all (`stat`, errno
2). And an FTP read of the same paths returns *decrypted* payloads (16,706,040
and 1,335,962 bytes, the sizes `ftpsrv`'s `SELF` toggle reports and serves), so a
file read back over FTP is not the container the loader is handed; nothing
re-signs a file on write (`docs/DEPLOYMENT.md`).

## 2026-09-19 — Fragment inputs alias without RADV's location assignment

RetroArch showed a correctly shaped green RGUI menu with blue flicker and dark
triangles despite zero driver refusals and successful recording/submit/present.
Synchronized readback found a correct menu texture and white vertex colours,
but corrupt completed swapchain pixels. The standalone compiler's postprocessed
NIR gave both `vTexCoord` (location 0) and `vColor` (location 1) `base=0`.
ACO consequently sampled the texture with attr0.xy and also used attr0.xyzw
as colour: red/green became texture coordinates and blue/alpha read undefined
components. Input-semantic construction rejected the collision internally,
leaving an empty list and unresolved AGC linkage while compilation returned OK.

The pinned RADV path calls `ac_nir_assign_fs_input_locations` immediately before
`radv_nir_shader_info_pass`; the standalone wrapper omitted it. PS5_Vulkan now
patches its compiler work copy to run that pass at the same point. SDK source is
unchanged. `tools/check-fragment-inputs.sh` fails before the fix and passes dense
and sparse input cases afterward, and is included in `tools/check-driver.sh`.

The corrected statically linked RetroArch ran the full 30-second watch and the
script closed it. Trace refusals and observed end/submit/present errors are zero.
All 909 opaque-white menu samples read white (previously 0/909), and the two
consecutive completed frames are byte-identical. Owner: "Colours correct; no
flicker or triangles". Artifact: `evidence/fragment-inputs/capture.json`.
This fixes a compiler integration defect; it adds no version/format claims.

A preliminary equivalent ONE/ZERO opaque-pass test did not remove either defect.
The driver's conditional blend-register programming remains a separate review
question, not the cause established by this run.

## 2026-09-19 — The tile maps are AddrLib's sixteen-pipe rows, and a two-byte element has its own

Two questions had to be settled before a tiled image whose element is not four
bytes could be walked on the CPU: which of the address library's rows the
console's own storage is, and whether the row for a two-byte element is a scaling
of the four-byte one. `tools/mip-layout-oracle` gained a `swizzle` command that
derives a row from AddrLib itself -- one coordinate bit at a time, then required
to reproduce every texel and every sample of a tile through
`Addr2ComputeSurfaceAddrFromCoord` -- and `tools/check-mip-layout.sh` holds the
driver's tables against it.

**The console is the sixteen-pipe, non-RbPlus configuration, and that is itself a
measurement.** AddrLib selects a swizzle pattern by the pipe count and by the
ASIC revision (`gfx10addrlib.cpp`, `GetSwizzlePatternInfo`: the pattern index is
`m_colorBaseIndex + elemLog2`, where `m_colorBaseIndex` carries `m_pipesLog2`),
so a library built the default way answers with a *different map*. Built for
sixteen pipes, a 256-byte pipe interleave and a Navi10 revision, the oracle's
four-byte one-sample render-target row reproduces the console's measured map
exactly -- every one of a tile's 16384 texels, which the check prints as
`measured match` -- and the same configuration's depth row reproduces
`ps5vk_tiled_depth_offset`. With one pipe, 16128 of those 16384 texels differ. The
rows are `GFX10_SW_64K_R_X_1xaa_PATINFO[22]` (`{3, 2, 74, 31, 0}`) and
`GFX10_SW_64K_Z_X_1xaa_PATINFO[22]` (`{3, 10, 74, 31, 0}`) of the sixteen-pipe
group (`gfx10SwizzlePattern.h`).

**A two-byte element's row is not a scaling of the four-byte one.** The tempting
rule -- halving the element halves the x coordinate, so `map2(2x, y) = map4(x, y)`
and `map2(2x + 1, y) = map4(x, y) + 2` -- holds in AddrLib's one-pipe rows (0
violations over a tile) and fails in the console's sixteen-pipe rows (15872 of
16384), and it fails in the depth rows at every pipe count. The console's
two-byte row is its own table entry, `GFX10_SW_64K_R_X_1xaa_PATINFO[21]` (`{3, 1,
74, 30, 0}`, a 256x128-texel tile), whose y terms differ from the four-byte row's
(bit 12 comes from y bit 4 rather than y bit 3), so the difference is not a shift
of the whole map. `driver/ps5vk_image.c` holds both rows as
`ps5vk_tiled_4b_terms` and `ps5vk_tiled_2b_terms`, and the check requires each to
be the row the oracle derives. A first attempt derived the two-byte row by that
scaling rule and passed a check built on a default (one-pipe) oracle; the
sixteen-pipe library is what exposed it, which is why the oracle's configuration
is pinned in the tool.

**A four-sample texel's placement depends on the mode.** The render-target row
puts its samples at address bits 14 and 15 -- four 0x4000-byte planes at one
swizzled position, which is the model C8 measured for colour and the driver's
`PS5VK_SAMPLE_PLANE_BYTES` -- while the depth row puts them at bits 2 and 3, so a
four-sample depth texel is one sixteen-byte texel whose four words are its four
samples. That is the structure C8's ramp measured from the other side, and it is
what a CPU copy of a four-sample depth image needs: all four samples sit inside
the sixteen bytes, so one texel moves in one piece and no per-sample walk is
needed. The driver now walks it (`ps5vk_tiled_depth4_offset`, and the copy path's
refusal is gone), and the same row also turns the tile's own place in the grid
into bits 10 to 15 of the texel's position -- the column's low bit into bit 10,
the row's two low bits into 11, 14 and 15 -- which only a derivation over more
than one tile shows. A first transcription of that turn had a fifth line, the
row's second bit into bit 12, which the oracle's `coords` dump for a 4x4-tile
region rejected at (0, 128): AddrLib places bit 12 from the texel's own y bit 3,
not from the tile's row index.

**Constant blend factors are now programmable.** `CB_BLEND0_CONTROL`'s four
constant factors (AMD's field values 13, 14, 19 and 20) read
`CB_BLEND_RED/GREEN/BLUE/ALPHA` (0x105 to 0x108, the offsets AMD's register
database and the sibling SharpProspero SDK both name), which the driver records
from the pipeline's `blendConstants` when the state asks for one; the SRC1 factors
stay refused because they need `dualSrcBlend`, which this device does not
advertise (`maxFragmentDualSrcAttachments` 0). A pipeline that blends with
anything else records exactly the words it recorded before, so no earlier stream
changed.

Both readings still owe the console: one probe that renders into a two-byte tiled
attachment and reads it back through the map is what moves the audit's two-byte
rows, and one constant-factor blend frame is what claims the four factors. The
arithmetic above is checked offline, not on hardware.

## 2026-09-19 — The three format items on the console: a two-byte element cannot be tiled, the constant factors blend exactly, and a four-sample depth image copies

The three cases of `jobs/format-items` ran on the console (pid 244,
`Klog_Logs/format-items-run5.log`) with all four queued tests passing, and the
run answered a question the offline work had assumed away.

**A two-byte element has no tiled path, because no two-byte format can be an
attachment.** The driver's storage is tiled for an image whose usage carries
`COLOR_ATTACHMENT` or `DEPTH_STENCIL_ATTACHMENT` and is the row layout
otherwise (`ps5vk_image_storage`), and no two-byte format in the table carries
either bit: the required-format table asks for neither on a two-byte format, and
the one 16-bit target the first draft of this case asked for aborted the process
on `ps5vk_CreateImage`'s `assert(ps5vk_image_supported(...))` (raised in
`ps5vk_image.c:651`, backtrace `__assert` <- `ps5vk_CreateImage` <-
`run_vulkan_depth4_copy_frames`). So `ps5vk_tiled_2b_terms` -- the row
`tools/check-mip-layout.sh` derives from AddrLib and compares with the driver --
has **no reachable path**: every two-byte image this driver creates is the row
layout, and the row's own proof is the transfer round trip the new `v0-transfer-16`
case runs: a 500x128 `R16_UNORM` image whose 1000-byte source rows the driver
stores as 1024-byte rows (its memory requirement is 131072 bytes where the tight
texels are 128000, which the case logs), filled through `vkCmdCopyBufferToImage`
and read back through `vkCmdCopyImageToBuffer`, all 64000 words equal. The
two-byte row stays a derivation with an oracle witness and no hardware one.

**The constant blend factors blend exactly.** `v0-blend-constant` draws the
m4-blend rectangle over the harness's offscreen clear (red 0x40, green 0x80,
blue 0xff, opaque) with `CONSTANT_COLOR`/`ONE_MINUS_CONSTANT_COLOR` and
`CONSTANT_ALPHA`/`ONE_MINUS_CONSTANT_ALPHA` and the constants {0.25, 0.5, 0.75,
1.0} one a channel, and every one of the target's 8294400 pixels holds
`0xff88586c` -- the exact blend, channel for channel, with a maximum channel
error of 0 against the arithmetic: red 0.25 of 0xf0 plus 0.75 of 0x40, green
half of 0x30 and half of 0x80, blue 0.75 of 0x60 plus 0.25 of 0xff, alpha the
source's 0xff. The run also settles which shader a blending 8-bit target needs:
the first attempt drew with the `b8-corner` shaders, whose pixel stage exports
the legacy 32_ABGR, and every pixel came back `0xff88586c`-shaped but wrong in
two channels (`0xff88586c` was measured with the m4-blend set; the b8-corner run
measured a word `colour_error` 143 away from the expectation), because the
pipeline's SPI format for a blending 8-bit target is FP16_ABGR and the hardware
reads the shader's export by that format. A blending case has to draw with the
set whose exports match the SPI format (`probes/m4-blend`).

**A four-sample depth image copies, and the copy's extent is the image's
tiles.** `c8-depth4-copy` clears two four-sample D32 images (256x128, eight
64x64-texel tiles) to 0.5 and 0.25 and copies the first into the second: the
driver's clear fills the whole 2 MiB allocation (524288 words), the copy writes
the eight tiles (131072 words) with the source's word, and the first word the
destination's own clear survives in is exactly the first word past them
(131072), because the driver's storage is the tiles rounded up to its 2 MiB
alignment and the alignment is no texel's. Two things had to change for the copy
to run at all: `VK_FORMAT_D32_SFLOAT`'s table entry carried the depth attachment
bit alone, so an image created for a transfer aborted the process on the same
`ps5vk_CreateImage` assert, and `ps5vk_image_transfer_check` refused every depth
image by name. The entry now carries `TRANSFER_SRC` and `TRANSFER_DST` as well,
and a depth image copies into a depth image of its own format; the buffer forms
and the shapes with no measured map still refuse by name.

**And the copy no longer records one entry a run.** The first console run of the
copy case failed with `no memory to record an image copy
(VK_ERROR_OUT_OF_HOST_MEMORY)`: `ps5vk_cmd_buffer_copy_image_region` expanded a
region into one `struct ps5vk_memory_copy` a run, a sixteen-byte texel is one
run each, and even the shrunken 256x128 case's 32768 records did not fit the
application's allocator -- a 4K four-sample depth copy would have needed 8.3
million of them, and a 4K readback 2 million. The region is now **one** record
(`struct ps5vk_memory_copy.image_copy`) that carries the two sides, the
rectangle and the texel size, and the queue walks its runs at the split point
(`ps5vk_image_copy_execute`), which is the walk the recording used to do itself.
The console's green run is that fix's proof.

## 2026-09-19 — The transfer rows close, and the row layout's padding is part of the proof

Rung 1.0's transfer family was the cheapest large block left in the audit: 25
formats -- the 8-, 16- and 32-bit signed and unsigned integer families, the
packed 16-bit forms `R5G6B5`, `B4G4R4A4` and `A1R5G5B5`, the packed 32-bit
float forms `B10G11R11` and `E5B9G9R9`, and `A2B10G10R10` in both number types
-- each missing `TRANSFER_SRC` alone, because they had been added for their
*fetch* (an upload destination and a sampled read) and nothing had ever read one
*back*. The driver now reports the bit for all 25, and one case proves the path
the bit names: `v0-transfer-formats`, console run pid 266
(`Klog_Logs/v0-transfer-formats-run1.log`), 25 of 25 rows PASS, then `m2-solid`.

What the case establishes, per format: a 64x16 image created with sampled and
transfer usage and **no attachment usage** is the row layout, so a staging
buffer's bytes go in through `vkCmdCopyBufferToImage` and come back through
`vkCmdCopyImageToBuffer`, and every one of the tight bytes has to come back
exactly -- for the smallest format that is 1024 bytes, for the largest 16384.
A source row of 64 texels is deliberately not a multiple of the 256-byte row
alignment, so the padded pitch is part of the proof, and the image's memory
requirement has to be the rows the driver's rule makes of the tight ones:
`R8_UINT`'s 1024 tight bytes are stored as 4096 (64 bytes a row padded to 256,
sixteen rows), `R5G6B5`'s 2048 as 4096, while `R32_UINT`'s 4096 and
`R32G32B32A32_UINT`'s 16384 fill their rows and store exactly that. Every row
that mismatched would have been caught at the byte, and none did.

Two rows left the audit's table entirely with this round:
`VK_FORMAT_B4G4R4A4_UNORM_PACK16` and `VK_FORMAT_E5B9G9R9_UFLOAT_PACK32` had
nothing else missing. The audit now reads 51 missing rows and 133
probe-reachable features on 41 rows, down from 53 and 158.

The case also records the shape of the remaining work for this family: the
transfer *source* path is the row layout for every non-attachment image, so a
format's transfer bit is a claim the driver can make as soon as its element size
is known -- no tile map, no attachment word, no shader export. That is why this
block closed in one round while `BLIT_DST` (which needs a per-format write path)
and `COLOR_ATTACHMENT` (which needs a per-format export) are the expensive ones.

## 2026-09-19 — A vertex attribute's absent components fetch as (0, 0, 0, 1), measured

`v0-vertex-formats` draws one frame per vertex format the compiler's
`PsbcVertexFormat` can express, with the attribute bound as the row's own format
(console run pid 283, `Klog_Logs/v0-vertex-formats-run3.log`, nine of nine rows).
The rows whose format has fewer components than the shader's `uvec4`/`ivec4`
declare read the **fill rule** Vulkan names for absent components: a one- or
two-component attribute fetches (x, 0, 0, 1) and (x, y, 0, 1). The console says
so twice over: the unsigned probe writes each component back as `value / 255`,
so its frame holds the byte 1 in alpha (1 / 255 written back out), while the
signed probe adds 128 first and holds 129 there. Both are the same w = 1.

The same run closes the nine `VERTEX_BUFFER` rows the audit listed as reachable
-- `R32_UINT`, `R32_SINT`, `R32G32_UINT`, `R32G32_SINT`, `R32G32B32A32_SINT`,
`R8G8B8A8_UNORM`, `A8B8G8R8_UNORM_PACK32`, `B8G8R8A8_UNORM` and
`A2B10G10R10_UNORM_PACK32` -- with the frame's middle pixel naming each fetch:
(64, 0, 0, 1), (64, 128, 128, 129) and so on for the integer classes, the
format's own colour for the 8888 UNORM layouts, and (1, 32, 128, 255) for the
ten-bit row, whose 4, 128, 512 and 3 therefore decode and re-encode with
rounding rather than truncation.

## 2026-09-19: a blit destination's bytes are the format's fetch read backwards

The resampler's write path (`ps5vk_rgba8_to_texel`) exists now, and the console
confirms on 39 formats what the audit needs: a scaled blit of one
`R8G8B8A8_UNORM` colour into a linear image of a format, read back raw with
`vkCmdCopyImageToBuffer`, holds exactly the bytes `ps5vk_texel_to_rgba8`'s
decode of that format reads (console run pid 290,
`Klog_Logs/c7-blit-dst-run3.log`, 39 rows, 39 distinct formats, all matching;
the round's first successful battery was run 2, pid 288, whose log reads the
same).

Two byte orders came out of it that are worth recording separately, because they
are *not* the same rule and the Vulkan names invite the mistake:

| format | one texel of `(0x40, 0x80, 0xc0, 0xff)` | which byte is red |
| --- | --- | --- |
| `R8G8B8A8_UNORM` | `40 80 c0 ff` | first |
| `A8B8G8R8_UNORM_PACK32` | `ff c0 80 40` | **last** |
| `B8G8R8A8_UNORM` | `c0 80 40 ff` | third |
| `A8B8G8R8_UINT_PACK32` | `40 80 c0 ff` | first |
| `A8B8G8R8_SINT_PACK32` | `20 40 60 7f` | first |

So the *UNORM* `A8B8G8R8_*` family stores its channels byte-reversed from
Vulkan's `A8B8G8R8_UNORM_PACK32` definition (which puts R in bits 7:0, i.e.
first in little-endian memory), while the *integer* `A8B8G8R8_*` family stores
them in Vulkan's order. Both are what the console's fetch of that AGC format
word does -- the driver's decode was measured, not assumed
(`docs/HARDWARE_FINDINGS.md`, the V0-formats entries) -- and the encode is now
its exact inverse, which is why the audit can claim `BLIT_DST` on these rows at
all. The two sRGB forms remain outside this: their fetch linearises the first
three *fetched* components, so no write path can make a blit into them mean what
Vulkan's name says (`docs/V0_FORMATS_AUDIT.md`).

Three of the round's faults were mine, and each is a rule for the next family:

- **An archive older than its sources is not rebuilt by the app build.** The
  first console run of the round deployed a driver without the new encode and
  without the new bits, because `tools/build.sh` links prebuilt archives and
  rebuilds only the application's own objects. It now refuses to link
  `build/driver/ps5/libps5vk.ps5.a` or `libpsbc_driver.ps5.a` when any
  `driver/*.c`/`.h` is newer. The run's abort read like an ACO fault (its
  backtrace walked stale ACO frames on the aborting stack) and was really
  `vkCreateImage`'s assert firing on a format whose entry lacked the transfer
  bits the case asked for.
- **`BLIT_SRC`/`BLIT_DST` imply the matching `TRANSFER_*` bit.** Vulkan does not
  let an image be created with a usage the format's features do not allow, so a
  format that reports `BLIT_DST` without `TRANSFER_DST` cannot be a blit
  destination in practice. `B8G8R8A8_UNORM` and `A8B8G8R8_UINT_PACK32` carried
  exactly that pair of impossible bits for one commit.
- **A component's width is not its texel's block size.** `vk_format_get_blocksize`
  says an `R8G8_UINT` texel is two bytes; that is two *eight*-bit components, not
  one sixteen-bit one, and an encode derived from the block size wrote
  `40 40` where the fetch reads `40 80`. The encode now names (bits, channels,
  signedness) per format, exactly as the decode does. The same round found
  `ps5vk_scale8` overflowing a 32-bit channel: `value * UINT32_MAX` wrapped in
  32 bits and every 32-bit integer encode came out zero, so the host gate's
  first run read fifteen failed rows instead of the console's one.

## 2026-09-19: the sampler words the last fetch rows needed

Round 6 closed the audit's remaining fetch rows, and two of the three words came
from the register database and Mesa rather than from a new measurement (console
run pid 296, `Klog_Logs/v0-snorm-run4.log`: 25 of 25 sampled frames, 52 of 52
blit frames, 53 of 53 formats as the audit records them).

| format | descriptor word | selectors | why |
| --- | --- | --- | --- |
| `R8_SNORM` | `GFX10_FORMAT_8_SNORM` = 2 | `R001` | the 8-bit SNORM sibling of `8_UNORM` (1); `gfx10-rsrc.json` lists the whole enum, and 8/16/32-bit SNORM sit one above their UNORM twins |
| `R8G8_SNORM` | `GFX10_FORMAT_8_8_SNORM` = 15 | `RG01` | the two-channel twin of 14 |
| `B8G8R8A8_UNORM` | `GFX10_FORMAT_8_8_8_8_UNORM` = 56 | `ZYXW` (0x0f2e) | VideoOut's byte order: the memory channels are B, G, R, A, so the fetch's X is the memory's Z, Y its Y, Z its X and W its W -- exactly what `radv_compose_swizzle` composes for the format's description in Mesa, and the selector form of the `COMP_SWAP` ALT the colour target already used |
| `A8B8G8R8_UINT_PACK32` | `GFX10_FORMAT_8_8_8_8_UINT` = 60 | `RGBA` | the **integer** packed family keeps Vulkan's own byte order: the console's fetch of it reads R first (round 5's blit destination run, `Klog_Logs/c7-blit-dst-run3.log`), unlike the UNORM packed family, whose fetch reads R last |

The lesson worth keeping is that a format's *decode* and its *descriptor
selectors* are two readings of one measured fact: `PS5VK_FORMAT_SWIZZLE_WZYX`
exists because the A8B8G8R8 UNORM fetch is reversed, and B8G8R8A8 needs the
same reversal applied one channel further along (`ZYXW`), which is why the two
constants differ by one rotation.

**A third ACO fault, this time a signal rather than an abort.** The round's
first battery died with `SIGFPE` (integer divide fault) whose backtrace ends in
`aco::schedule_program`/`aco::lower_to_hw_instr`, right after the *second* case
in the process had compiled its pipeline; the first case had failed before its
own teardown, so its device and pipeline stayed alive and the next case's
compile was the second in that process. With the failing case fixed, no later
battery hit it (runs 3 and 4, pids 293 and 296, both clean). It joins the
second-compile abort of the signed pixel shader (`register_allocation` ->
`lower_to_hw_instr` -> `add_entry`) as a compiler fault rather than a hardware
limit, and both are quoted as such in `docs/V0_FORMATS_AUDIT.md`.

Two case-machinery faults were found the same way and are worth recording
because they hid behind each other:

- **A texture's texel size lives in three tables.** The runner's own helper
  (`driver/tests/ps5vk_triangle.c`, `texture_texel_bytes`) decides whether a
  format can be uploaded at all; a driver entry with a sampler word is not
  enough, and the refusal reads `VK_ERROR_FORMAT_NOT_SUPPORTED` from a path that
  never reaches the driver's image creation.
- **A table shared by three cases must hold rows all three can pass.** The
  sampled table grew from 22 to 52 rows while only the *blit* case kept being
  run whole: the main sampled case iterates the same table, and an integer row's
  texel fetched through a *float* sampler returns its raw value (0x40 fetches as
  64.0), so those rows can only pass the two typed cases. The main case skips
  them now, and the table's texels are the *blit decode's* for the rows the blit
  case checks: a signed 32-bit channel divides by 0x7fffffff, so a 0x20 colour
  needs 0x10000000, not the unsigned twin's 0x40000000-style value.

## 2026-09-19: the eight-byte unsigned fetch, and a compiler that compiles once

`R16G16B16A16_UINT` had no entry in the driver's format table. It takes the
register database's `GFX10_FORMAT_16_16_16_16_UINT` (69) with the straight RGBA
selectors -- its signed twin's word 70 with the same selectors -- and the console
fetches it as the typed value its texel holds: 64, 128 and 192 of 16 bits read
back as 0x40, 0x80 and 0xc0 in all 38400 pixels of the frame (console run pid
320, `Klog_Logs/v0-u16-run3.log`). Its blit halves are the resampler's decode and
encode, which the per-format blit case and `v0-blit-dst` prove (pid 327,
`Klog_Logs/v0-u16-blit-run1.log`: 53 of 53 and 40 destinations).

**Compiling the unsigned texture case's shader costs the process its next
compile.** Two queues died with the round-6 `SIGFPE` in `aco::schedule_program`
(pids 308, 317) after that case's frames had all passed, when the *next* case's
pipeline was compiled; running the case last in its own queue still faults after
its final frame, in the teardown or the runner's post-queue probe. The frames
themselves are logged before the fault, so the evidence is unaffected, but no
battery can put another case after this one. With the round-6 finding (the
`SIGFPE` when an earlier case's failed device is still alive), that is two
process-state faults around the same case, on top of the signed pixel shader's
second-compile abort: these are compiler faults, and the audit quotes them as
such rather than as hardware limits.

**The eight-byte tiled colour map is the row's remaining piece**, and it is a
derivation away: `build/host/mip-layout-oracle swizzle 8 1 64kb_r_x` answers
AddrLib's row for it (tile 128x64 texels, ten terms), exactly as it answers the
four-byte, two-byte and four-sample-depth rows `tools/check-mip-layout.sh`
already compares the driver against. `ps5vk_tile_extent` has the geometry
(128x64 for eight bytes, 64x64 for sixteen) and nothing applies it to a colour
target yet.

## 2026-09-19: the eight-byte colour map is AddrLib's row, and the console agrees

An eight-byte texel's tiled colour target needed its own map, and it is the
oracle's row rather than a new measurement: AddrLib's `64kb_r_x` row for
eight-byte elements is a 128x64-texel tile with ten terms, which
`build/host/mip-layout-oracle swizzle 8 1 64kb_r_x` prints and the driver now
carries (`ps5vk_tiled_8b_terms`). The console proves the pair: the integer
target case draws `R16G16B16A16_UINT` over the whole target and reads **both
words of all 8294400 texels** -- 0x00800040 and 0x00ff00c0, the four 16-bit
channels 0x0040, 0x0080, 0x00c0, 0x00ff little endian (console run pid 337,
`Klog_Logs/v0-wide-target-run1.log`, 6 of 6 integer targets; the commit's own
build read the same, pid 338).

Three register facts went with it, each from the register database and Mesa's
`ac_choose_spi_color_formats` rather than from a probe, and each now load-bearing
for a row the audit still lists as open:

| wide format | `CB_COLOR0_INFO` data format | number type | SPI export |
| --- | --- | --- | --- |
| `R16G16B16A16_UINT` | `COLOR_16_16_16_16` = 12 | UINT = 4 | `UINT16_ABGR` = 7 |
| `R16G16B16A16_SFLOAT` | 12 | FLOAT = 7 | `FP16_ABGR` = 4 |
| `R32G32_UINT` / `_SFLOAT` | `COLOR_32_32` = 11 | 4 / 7 | `32_AR` = 3 |
| `R32G32B32A32_UINT` / `_SFLOAT` | `COLOR_32_32_32_32` = 14 | 4 / 7 | `32_ABGR` = 9 |

A 16-bit-wide target's export is not the legacy 32_ABGR default the driver uses
for a four-byte target: Mesa names FP16_ABGR (float and UNORM) or
UINT16_ABGR/SINT16_ABGR (integer), and 32_AR for two 32-bit channels. The
sixteen-byte rows' map is the oracle's too (64x64-texel tiles, eight terms) and
is the next row to apply.

## 2026-09-19: a half-float target's words, and the pitch a sixteen-byte target still wants

An eight-byte `R16G16B16A16_SFLOAT` colour target stores the shader's colour as
four 16-bit halves, and round 9's target proves the pair with values that are
exact in that storage: 0.25, 0.5, 0.75 and 1.0 are 0x3400, 0x3800, 0x3a00 and
0x3c00, so the target's two words are 0x38003400 and 0x3c003a00 in every one of
its 8294400 texels, and the blended frame -- cleared to 0.125, 0.25, 0.5 and 1.0
and added to 0.125, 0.25, 0.25 and 1.0 -- holds 0x38003400 and 0x40003a00
(console run pid 109, `Klog_Logs/v0-wide2-run2.log`, 14 of 14 colour targets).
With round 8's 16_16_16_16 UINT row, that is both of the eight-byte Cb formats
proved; the 32_32 and 32_32_32_32 rows' words are in the table and carry no
feature bit yet.

**A sixteen-byte colour target is 99.63 per cent right, and the missing 0.37 per
cent is a boundary.** Drawing `R32G32B32A32_UINT` over the whole target and
reading it back through AddrLib's own sixteen-byte row (64x64-texel tiles,
`ps5vk_tiled_16b_terms`, which the mip-layout gate compares with the oracle)
finds 8263680 of 8294400 texels holding the expected words, the first mismatch
at (x 64, y 2128): a tile-column boundary, eight rows above the bottom. The map
itself is the oracle's, so the difference is in what the *hardware* was told
about the target rather than in how the driver reads it -- and the only register
that carries layout information the driver does not set for a wide target is the
**CB pitch** (`records[13]` keeps AGC's default, masked to `0xffffff00`), whose
default was established for a four-byte target. A tile row is 128 texels of
four-byte elements but 64 of sixteen-byte ones, so a pitch left at the four-byte
value over-strides every second tile row, which is the shape of the mismatch.
That is the next measurement: what the pitch register must hold for a sixteen-
byte target, and whether the eight-byte one (which passes) needs its own value
too -- the eight-byte row passed with the same default, so a tile row of 128
eight-byte texels is evidently what the default names.

## 2026-09-19: a two-channel 32-bit target wants the four-slot export

Two corrections stand together, and the first corrects the round-9 entry above.

**There is no CB pitch register in the target block.** The driver's
`ps5vk_target_offsets` is CB_COLOR0_BASE (0x318), VIEW (0x31b), INFO (0x31c),
ATTRIB (0x31d), DCC_CONTROL (0x31e), CMASK (0x31f), FMASK (0x321),
CLEAR_WORD0/1 (0x323/0x324), DCC_BASE (0x325), the four 0x390-series EXT bases
and ATTRIB2/3 (0x3b0/0x3b8). The register the round-9 entry called "the CB
pitch" is CB_COLOR0_DCC_BASE_EXT, and writing it would have been a mistake: a
tiled GFX10 colour target's layout is named by ATTRIB's COLOR_SW_MODE (AGC's
default, which the driver keeps) plus ATTRIB2's width and height, and the
element size picks the PATINFO row inside that mode -- which is exactly the row
the driver's maps mirror. So the 16-byte rows' 99.63 per cent is not a pitch
problem; the failing probe now logs the mismatch's bounding box, and that is
what the next attempt at those rows will read.

**A 32_32 target needs an export that carries two 32-bit channels, and 32_AR is
not it on this hardware.** With `SPI_SHADER_32_AR` (3) the console wrote the
first channel of every texel and left the second at zero -- all 8294400 texels
matched on word 0 and none on word 1, for both the UINT and the FLOAT row. With
`SPI_SHADER_32_ABGR` (9) both channels land: `R32G32_UINT` holds 0x00000040 and
0x00000080, `R32G32_SFLOAT` the words 0x3e800000 and 0x3f000000, in every texel
of their targets (console run pid 112, `Klog_Logs/v0-wide3-run3.log`). Mesa's
`ac_choose_spi_color_formats` picks 32_AR for a 32_32 FLOAT target on the
assumption that the shader's two channels reach the R and A export slots; a
compiler that exports the colour's own four slots needs the four-slot format
instead. The driver's colour table carries 9 for both rows and the blended frame
uses the same export (the entry names one, so blending does not fall back to
FP16_ABGR).

## 2026-09-19: the one-byte element's row, and what a narrow target needs

An 8-bit colour target was never reachable before because the driver read the
one-byte element's texels through the **four-byte** map: `ps5vk_tiled_texel_offset`
chose its row by element size only for two, eight and sixteen bytes and let
everything else fall through to the four-byte table. AddrLib's own row for a
one-byte element is a 256x256-texel tile with **thirteen** terms -- a different
shape, not a scaling -- and it is `ps5vk_tiled_1b_terms` now, with
`tools/check-mip-layout.sh` comparing it with the oracle beside the two-,
four-, eight- and sixteen-byte rows.

With it, the one- and two-byte targets render and read back exactly: 0x40 in
every texel of an `R8_UNORM`, `R8_UINT` or `R8G8_UNORM`-style one-byte channel,
0x8040 in every texel of an `R8G8` target, 0x3400 in an `R16_SFLOAT` one, and
the blended frames hold the same values after the clear and the additive draw
(console run pid 117, `Klog_Logs/v0-narrow-run5.log`; 22 of 22 colour targets and
9 of 9 integer targets). The CB words are COLOR_8 (1), COLOR_8_8 (3) and
COLOR_16 (2) with the number type the format names, and the exports are
FP16_ABGR for the normalized and half-float targets, UINT16_ABGR for the integer
pair -- Mesa's `ac_choose_spi_color_formats` values, which the console confirms
stored the right bytes in the right order.

One case-side fact belongs with them: a target's readback must be gated on **its
own** size. The target cases had required `target_bytes >= kFramebufferBytes`
(3840x2160 four-byte texels); a one-byte target is a quarter of that and a
sixteen-byte one four times it, so the gate silently skipped the narrow rows'
checks and reported them as failures with no readback record. The floor is the
target's own extent times its element size now.

## 2026-09-19: the packed 16-bit targets, and the blend family closed

The 5_6_5 and 1_5_5_5 colour-buffer formats render and blend: with the register
database's COLOR_5_6_5 (16) and COLOR_1_5_5_5 (17) and Mesa's FP16_ABGR export,
a four-channel float colour lands in their packed channels exactly when the
colour is exact in them -- 8/31, 16/63 and 8/31 give the solid words 0x4208
(R5G6B5) and 0xA108 (A1R5G5B5, alpha's one bit set by the colour's 1.0), and the
blended frames' additive draws give 0x8410 and 0xC210. All 8294400 texels of each
target hold them (console run pid 118, `Klog_Logs/v0-packed-run1.log`; 26 of 26
colour targets and 10 of 10 integer targets, `R16_UINT`'s 0x0040 among them).

With that, `COLOR_ATTACHMENT_BLEND` is proved for every row the audit lists as
reachable: the only formats still missing the blend bit are
`A8B8G8R8_SRGB_PACK32` and `B8G8R8A8_SRGB`, whose fetch the hardware linearises
before any selector can reorder the channels -- the same fixed fetch order that
keeps their sampled and blit features off. The blend family therefore leaves the
audit's reachable list, and the packed pair leaves the missing list entirely
(48 required formats are short of a required feature now, where 50 were before
the round).

## 2026-09-19: a depth image in a shader's descriptors is a compiler fault

Three console batteries of round 13 say the same thing in three shapes: **a
shader whose descriptors include a depth image cannot be compiled in this
driver** -- the process aborts inside ACO with
`radv_nir_lower_descriptors` -> `aco::lower_branches` -> `lower_to_hw_instr`.

| run | what ran | where it aborted |
| --- | --- | --- |
| pid 119, `Klog_Logs/v0-depth-run1.log` | the whole sampled table with `D16_UNORM` and `D32_SFLOAT` in it | the depth row's pipeline, after ~15 colour frames |
| pid 120, `Klog_Logs/v0-depth-run2.log` | the per-format blit case (depth image as the blit's *source*, RGBA8 destination sampled by the frame) | its sixteenth frame, which is the depth row |
| pid 121, `Klog_Logs/v0-depth-run3.log` | a two-row depth blit case of its own | the first frame |

The per-format blit case's sixteenth frame is a second reading of the same
round: the same fault ends a run after roughly fifteen pixel stages, whichever
stages they are, so a fifty-five-row case cannot be proved in one process any
more. Both readings point the same way -- the fault is the compiler's, and it is
the third face of the family this project has recorded (the signed pixel stage's
second compile, the process-state SIGFPE in `aco::schedule_program`, and now a
depth descriptor in the shader).

For the audit that is a *quote*, not a gap: `D16_UNORM`'s and `D32_SFLOAT`'s
`SAMPLED_IMAGE` and `BLIT_SRC` leave the driver's feature table and are counted
against the compiler fault, with these three runs as the evidence. What remains
provable for the depth formats is `D16_UNORM`'s depth attachment: its
`DB_Z_INFO` word is `Z_16` (the register database's value 1, where D32F's
measured word carries 3), and a depth *target* is not a depth *descriptor*, so
no shader compile is involved.

## 2026-09-19: the depth words a D16 target needs

`D16_UNORM`'s depth attachment is the one reachable feature rung 1.0 has left
that is not behind the compiler fault, and its two register facts are
derivations rather than new measurements:

- **`DB_Z_INFO`'s FORMAT field** is the register database's `ZFormat`: `Z_16` =
  1, `Z_24` = 2, `Z_32_FLOAT` = 3. The console measured D32F's word as
  0x80000183 (M4 step 1, C5), whose low nibble is that 3, so a D16 target's word
  is **0x80000181** and the rest of the word -- the tile mode, the compression
  bits and the sample count C8's four-sample half sets -- is shared.
- **The two-byte depth map** is AddrLib's own `64kb_z_x` row for two-byte
  elements: a 256x128-texel tile with **thirteen** terms, not the four-byte
  row's 128x128 with eleven. `build/host/mip-layout-oracle swizzle 2 1 64kb_z_x`
  prints it, `ps5vk_tiled_depth2_terms` carries it, and
  `tools/check-mip-layout.sh` compares the two.

The driver refuses a depth attachment that is not D32_SFLOAT by name today
(`ps5vk_draw.c`), so a D16 depth target cannot yet be created for a draw: that
refusal and the case's two-byte readback are the probe that turns this
groundwork into the feature. The four-byte path is unaffected -- `c5-depth` and
`c5-depth-clear` pass on the console with the map now taking its element size
and the word now taking its format (pid 125,
`Klog_Logs/v0-depth2-run1.log`).

## 2026-09-19: D16's depth attachment, and how the console rounds a depth to sixteen bits

A D16_UNORM depth attachment renders like the D32F one the M4 canary proved: the
same frame, the same shaders, the same clear of 1.0, with `DB_Z_INFO`'s FORMAT
field at `Z_16` (the register database's value 1, so the word is 0x80000181
where D32F's measured word is 0x80000183) and the depth read back through
AddrLib's 2-byte Z_X row. Every one of the target's 8294400 depth samples holds
the value the geometry and the clear wrote, and the colour is the depth-tested
frame's own (console run pid 129, `Klog_Logs/v0-d16-run2.log`).

**The conversion is round-to-nearest, and it is worth a table.** The round's
first run expected 0xc000 for the far rectangle's 0.75 and the console stored
**0xbfff**:

| depth | x 65535 | stored |
| --- | --- | --- |
| 0.25 (near rectangle) | 16383.75 | 0x4000 |
| 0.75 (far rectangle) | 49151.25 | 0xbfff |
| 1.0 (the clear) | 65535 | 0xffff |

So 0.25 rounds *up* and 0.75 rounds *down*: the value is the nearest integer, not
the value plus a half truncated. A probe that writes a depth and reads it back
has to do the same arithmetic, which is what the round's second run did.

The driver refuses a depth attachment that is not D32_SFLOAT or D16_UNORM by
name, and a D16 image's fetch word (`16_UNORM` = 7) is still inert: the sampler
and the blit source of both depth formats remain behind the compiler fault of
the previous entry, which is why neither is claimed.

## 2026-09-20: a sixteen-byte colour target's pixel stage aborts ACO as a process's *first* compile

The sixteen-byte colour rows (`R32G32B32A32_UINT`, `R32G32B32A32_SFLOAT`) have two
console readings that disagree about *where* their blocker is, and the difference
is the row's place in the case's table.

Round 9 drew them **last** in the two family cases and read them back through the
sixteen-byte map (`ps5vk_tiled_16b_terms`, AddrLib's own row): 8263680 of 8294400
texels held the expected words, 99.63 per cent, the first mismatch at (64, 2128)
holding 0x0 (`Klog_Logs/v0-wide2-run1.log`, run pid 109; the rows were then taken
out of the case tables, docs/M5_PHASE_C.md round 9). So the row's pixel stage
*can* compile and its target *does* render.

Round 16 gave the same rows their own probe and then moved them to the **front**
of the family tables, and in both shapes the process aborted inside ACO at its
first pipeline compile:

| run | shape | result |
| --- | --- | --- |
| pid 133, `Klog_Logs/v0-wide16-run2.log` | the unsigned family with the sixteen-byte row in it and its own cases after | eight frames passed (`R8G8_UINT` last) and the process aborted while the next frame's pipeline was compiled; 308 PASS records |
| pid 135, `Klog_Logs/v0-wide16-run3.log` | the case `v0-target-wide16-uint`, the row alone | died at the case's first compile, right after `b7_create_device`; 45 PASS records |
| pid 139, `Klog_Logs/v0-wide16-run4.log` | the family cases with the rows moved to the front | died before its first frame was logged, the same 45 PASS boundary; the battery relaunched the title three times and every process died the same way |

All three backtraces walk ACO's lowering, not the driver's GPU path:
`aco::lower_branches` -> `aco::lower_to_hw_instr` -> `aco::reindex_ssa` (pid 139's
frames 0x449383, 0x439549, 0x438598, 0x4157df, 0x4045e3, 0x435f26 and 0x40016b
resolved by nearest symbol over the deployed build's `build/llvm-pie.elf`; the
same chain appears in the other two runs, at addresses 0x50 lower because the
builds differ by the case tables).

**Reading.** The two readings differ in the row's *position*, not in the row's
words: round 9's compile was its process's seventh or sixteenth, round 16's was
its first. What makes a process's first compile different -- the compiler's arena,
its caches, or its register-allocation state -- is not measured. What is measured
is that the same target's pixel stage lowered fine mid-case and aborts as a first
compile, so the rows stay **probe-reachable** work quoted against nothing: their
case keeps the place the working measurement came from (last in the table), and a
battery that draws them mid-case is what says whether round 9's 0.37 per cent
mismatch is still there or the first-compile fault is the only thing between them
and a claim. Quoting them against the compiler fault would have hidden a row a
console run has already rendered.

## 2026-09-20: an unsupported image is refused, not asserted -- three rounds of "compiler faults" were one assert

The rung's last blockers had been read as compiler faults: the signed colour
targets' "second compile aborts ACO's register allocator" (round 4), the depth
rows' `SAMPLED_IMAGE`/`BLIT_SRC` "aborts in `radv_nir_lower_descriptors` ->
`aco::lower_branches`" (round 13), and the sixteen-byte targets' "first compile
aborts ACO" (round 16). They are one thing, and it is not the compiler:
`ps5vk_CreateImage` asserted on an unsupported combination, and **an assert's
abort backtrace walks whatever ACO frames the stack still held** -- the compiler
had just run for the case's previous frame, whose frames were still the newest
complete chain on that stack.

The proof needs no unwinder. Every one of those runs' last [PS5VK] record before
`abort is called(system)` is `b7_create_device`, and the next call a frame makes
is `b7_create_image`; image creation cannot reach the shader compiler at all.

| run | last probe before the abort | what the next call was |
| --- | --- | --- |
| pids 133, 135, 139, `Klog_Logs/v0-wide16-run2.log`, `-run3.log`, `-run4.log` | `b7_create_device` | `b7_create_image` of `R32G32B32A32_UINT`, whose entry carries no `COLOR_ATTACHMENT` bit |
| pids 119, 120, 121, `Klog_Logs/v0-depth-run1.log`, `-run2.log`, `-run3.log` | `b7_create_device` (run 1, after the texture buffer's `b7_map_buffer_memory`), `b7_create_copied_view` (runs 2 and 3) | the depth image the case samples: `D16_UNORM` had **no entry in the driver's format table at all** then, and neither depth row carried `SAMPLED_IMAGE` |
| round 4's signed runs, `Klog_Logs/v0-targets-sint-run2.log`, `-run3.log`, `v0-targets-run4.log`, `-run5.log` | `b7_create_device` | `b7_create_image` of the case's second signed format, whose entry the attempt had not claimed |

The per-format facts are in the driver's own table: `ps5vk_image_supported`
requires the requested usage to be inside the format entry's features, and the
entries for the signed, sixteen-byte and sampled-depth rows did not carry them
(rounds 4, 9 and 13 removed those claims when their attempts failed -- for a
reason that was this assert, not the measurement).

**What changed.** `ps5vk_CreateImage` returns `VK_ERROR_FORMAT_NOT_SUPPORTED`
with the format, tiling and usage named instead of asserting, so a case whose
row the table does not carry yet fails loudly in the log rather than killing the
title; `driver/tests/vk_b3_image_test.c` checks that refusal for
`R32G32B32A32_UINT` and a colour attachment. The assert that remains at that call
site is the extent/mip/layer limit one, which no probe can reach by accident.

**What follows.** The compiler-fault class in the audit is empty. The rows quoted
against it -- the ten signed attachments, the two sixteen-byte attachments and
`D16_UNORM`'s and `D32_SFLOAT`'s `SAMPLED_IMAGE`/`BLIT_SRC` -- are reachable work:
their claims go into the driver's table in the same round as the battery that
proves them, and a run that fails reverts the claim. One caution for the next
signed battery: round 4's *first* signed frame, `R8G8B8A8_SINT`, **passed** --
image, pipeline, draw and every texel -- before the second format's unclaimed
entry aborted the process, so the signed words and the ivec4 shader are already
measured, not speculative.

The genuine compiler faults the project has recorded stand as recorded: the
SIGFPE in `aco::schedule_program` after the unsigned texture case's compile
(`Klog_Logs/v0-u16-run1.log`, `v0-snorm-run1.log`), whose top frame is the
faulting instruction and not an abort.

## 2026-09-20: the sixteen-byte colour map is one bug, and it sits in a sixteen-row band of the last tile row

The three sixteen-byte colour rows (`R32G32B32A32_UINT`, `_SINT`, `_SFLOAT`) were
claimed and drawn: the signed one inside `v0-targets-sint` (console run pid 110,
`Klog_Logs/v0-sint-run1.log`) and the other two in the family cases with the rows
last (pid 111, `Klog_Logs/v0-wide16-run5.log`). All three fail **identically**:

| row (frame) | matching | of | first mismatch | box |
| --- | --- | --- | --- | --- |
| `R32G32B32A32_SINT` (frame 9) | 8263680 | 8294400 | (64, 2128) | x 64..3839, y 2128..2143 |
| `R32G32B32A32_UINT` (frame 10) | 8263680 | 8294400 | (64, 2128) | x 64..3839, y 2128..2143 |
| `R32G32B32A32_SFLOAT` (frames 26 and 27) | 8263680 | 8294400 | (64, 2128) | x 64..3839, y 2128..2143 |

The arithmetic of that box is the finding. 30720 mismatches over a 16-row band
is **1920 a row -- exactly half of the target's 3840 texels**, and the band
begins at x 64, so the odd 64-texel tile columns are the ones that differ. The
band is y 2128..2143: the last tile row starts at 2112 (33 x 64), and the
mismatch covers its tile-local rows **16 to 31** -- the middle sixteen of its
forty-eight rows, with rows 0..15 and 32..47 correct. The stored word at the
first mismatch is 0x0, so the read lands on memory the GPU never wrote (a hole),
not on a neighbour's texels -- which is why a solid-colour frame reports a
mismatch at all (a shifted read of a solid target would read the same colour).

What is already excluded: the terms are AddrLib's own -- `tools/check-mip-layout.sh`
compares `ps5vk_tiled_16b_terms` (64x64-texel tiles, eight terms) against the
oracle and passes -- and the same machinery is exact for the four- and eight-byte
rows, whose targets read back 8294400 of 8294400. So the suspect is how the
sixteen-byte element's map is *applied*, or a difference between the oracle's
row and the hardware's own last-tile-row behaviour that only a sixteen-byte
element exposes.

**The next probe should read the target linearly, not through the map.** The
case already has the target's storage mapped, and the readback that failed walks
it through the same map whose band is in question -- so a check that walks the
*allocation* word by word and marks which sixteen-byte words hold the expected
colour says where the writes actually landed, and the shape of the unwritten
region names the hardware's addressing for those rows. A gradient frame (each
texel's colour a function of its x) answers the same question from the other
side, and a second colour drawn into one band would say whether a hole or a
displacement produces the zero word; the linear dump is the cheaper first
reading, and it is what this map owes.

## 2026-09-20: the sixteen-byte tile's twist is the block-level XOR four-sample images have

The sixteen-byte colour map's band (`ps5vk_tiled_16b_terms`, the three rows at
99.63 per cent with the same mismatch box) is one missing **twist**, and a ramp
frame measured it exactly.

The probe: `v0-wide16-ramp` draws the colour family's rect with a colour that
encodes the fragment's own position -- `R = x/4096`, `G = y/4096`, both exact in
a 32-bit float -- into `R32G32B32A32_SFLOAT`, and the case then walks the
mapped storage **without the map**: every word names the texel that wrote it
(`decoded` 8294400 of 8294400, `foreign` 0; console runs pids 113, 114 and 115,
`Klog_Logs/v0-wide16-ramp-run1.log` .. `-run3.log`). The interpolation is exact
because the rasteriser evaluates the ramp at the pixel centre, so
`G * 4096` is the rect-space row; the target's own row is its mirror
(`row = 2159 - G * 4096`), which the first run's "found tile" records showed by
putting the words of rows 31 and 63 where the driver's map looks for rows 2128
and 2096.

The dump is then unambiguous. For three tiles -- the image's second tile, a tile
of a full tile row, and the band's tile -- the case records the ramp value the
word at each texel's **mapped** address carries, sixteen columns and all
sixty-four tile-local rows:

| tile (row, column) | driver's in_y 0..15 | 16..31 | 32..47 | 48..63 |
| --- | --- | --- | --- | --- |
| 1 (0, 1) | holds in_y 32..47 | 48..63 | 0..15 | 16..31 |
| 1921 (32, 1) | holds in_y 32..47 | 48..63 | 0..15 | 16..31 |
| 1981 (33, 1, the last partial row) | holds in_y 32..47 | **unwritten** | 0..15 | 16..31 |

So the driver's `in_y` bit 5 is inverted for **odd tile columns**: the hardware's
row for a texel is the driver's `in_y ^ 32` when `x / 64` is odd, and the holes
are exactly the texels whose twisted row falls past the last tile row's
forty-eight rendered rows (32 x 64 = 30720 = the measured mismatch count, and the
odd columns only, because even columns have no twist). That is the same
block-level XOR the four-sample images use, and the driver already writes it for
them:

```c
in_y ^= (x / tile_width & 1u) * (tile_height / 2u);   /* tile_height / 2 = 32 */
```

**The fix is to apply it to sixteen-byte elements too** (and to the case-side copy
of the map, `tiled_wide16_offset_for`, which reads the target back). Whether the
x-axis half of that twist (`in_x ^= (y / tile_height & 1) * (tile_width / 2)`)
applies as well is not yet measured, because a solid frame cannot see a swap of
written words: the ramp frame can, and its verification pass -- decode every
probe tile's words and compare them with the texel that should have written them
-- is what the next run reads before the three rows' `COLOR_ATTACHMENT` bits are
claimed again.

## 2026-09-20: the twist confirmed and applied -- the sixteen-byte colour rows are proved

The ramp frame's candidate check settled the map in one run. It applies two
candidate block twists to the term table the repository holds -- the y half alone
and both halves -- and reads every probe tile's word at each candidate's address,
comparing it with the texel whose own colour should have written it. The y-half
candidate matched **8192 of 12288** probe texels (the two even tile rows it can
account for), and the two-half candidate matched **11264 of 11264** of the texels
that exist at all, first mismatch none (pid 116,
`Klog_Logs/v0-wide16-ramp-run4.log`); the 1024 it could not match before were
probe rows past the target's height, which the check now skips.

So a sixteen-byte element's tile carries the **whole** block XOR a four-sample
image's does, and `ps5vk_tiled_texel_offset` applies it:

```c
   } else if (element_bytes == 16u) {
      in_x ^= (y / tile_height & 1u) * (tile_width / 2u);
      in_y ^= (x / tile_width & 1u) * (tile_height / 2u);
   }
```

The case-side copy (`tiled_wide16_offset_for`, which reads a target back) got the
same two lines. With them the three rows read back **every texel**: the unsigned
family's eleven integer targets and the signed case's ten both hold their words,
the colour family's twenty-eight frames (the sixteen-byte float row's solid and
blended ones among them) hold theirs, and `v0-formats` reads 54 of 54 with the
audit mirror moved (pids 117 and 118, `Klog_Logs/v0-sint-run4.log`,
`Klog_Logs/v0-wide16-run8.log`: 409 and 1469 PASS records, no FAIL). Their
`COLOR_ATTACHMENT` bits stay in the driver's table, their rows lost the feature in
the audit, and the split now reads **4 features on 2 rows probe-reachable**: the
two-byte depth pair's `SAMPLED_IMAGE` and `BLIT_SRC`, which no attempt has reached
yet.

## 2026-09-20: the depth pair's sampler and blit source, and rung 1.0 closed

`D16_UNORM` and `D32_SFLOAT` were the last two rows no probe had reached: round
13's attempt died at `vkCreateImage` (the assert that is a named refusal now)
before any depth-descriptor shader compiled, so their `SAMPLED_IMAGE` and
`BLIT_SRC` were open with no reading at all. Two cases closed them.

**The fetch.** `v0-formats-sampled-depth` uploads a 0.5 depth into a 256x4 image
of each row's format -- `0x8000` for D16, `0x3f000000` for D32 -- samples it
through the m3-texture shader, and reads the frame back: **2 of 2 formats fetched
the colour their texel holds** (0x80 in red, nothing in green or blue), which is
the R001 selector single channel the depth entries' fetch words (16_UNORM 7,
32_FLOAT 22) name. So a depth image needs **no descriptor field of its own** in
this driver: the colour descriptor path's fetch word carries it.

**The blit source.** A blit between a depth image and a colour one is not a blit
of compatible formats, and the driver refuses one by name -- "a depth image
copies into a depth image of its own format" -- so `c7-blit-depth` uploads each
row's texels into a depth image and blits it, one to one, into a second depth
image of the same format, then checks the destination's own bytes: **1024 of
1024 texels for both** (D16 at a 512-byte row pitch, D32 at 1024), first bad
texel none (run pid 126, `Klog_Logs/v0-depth-pair-run4.log`). The blit path
needed the harness and the driver to carry a depth image's *aspect* instead of
assuming colour (the texture view, the upload and readback regions, the copied
image's usage), and `vkCmdCopyImageToBuffer` had refused a depth source by name
since C7: it now reads one back through the depth map the C5 rows measured,
which is what the maintenance1 note's `TRANSFER_SRC` asks of a format the driver
reports as sampled. `v0-transfer-formats` round-trips a D16 image through a
linear image and reports the bit (pid 128, `Klog_Logs/v0-transfer-formats-run3.log`).

**Rung 1.0 is closed.** The audit's split reads **74 features on 38 rows parked
on `PsbcDescriptorType`, 29 on 29 on `PsbcVertexFormat`, 12 on 2 blocked by the
hardware's fixed fetch order, 0 on 0 blocked by the compiler fault and 0 features
on 0 rows probe-reachable**; 51 formats are reported and **46** miss a required
feature, every one of them quoted against the enum or the fetch order that closes
it. `tests/test_tools.py` now asserts the finish line itself -- the compiler and
reachable counts must both be zero -- so a change that opens a probe-reachable
row has to close it or quote it.

## 2026-09-20: the compile-order fault reproduces, twice and differently, and not on the host

The compiler fault the project recorded as a genuine one -- a SIGFPE in
`aco::schedule_program` after the unsigned texture case's compile -- was
re-measured against the current driver, with the fault-capable probe bounded to
three cases and the runner's own cleanup. It reproduces, and the second run shows
a different fault in the same place.

**The queued shape** (`Klog_Logs/aco-state-run1.log`, pid 171; queue
`v0-formats-sampled-uint`, `m2-solid`, `v0-formats`, `m2-solid`): every row of the
unsigned texture case passed -- the last one `format 107` (`R32G32B32A32_UINT`),
`agc_format_sample PASS format 107 fetched as 40 80 c0` -- and then the process
died with **signal 8, "integer divide fault"**, `rip 0x430d54`, before the case's
own summary event and with no `runner_summary`: the fault is in the compile that
follows the case (or in its teardown), which is exactly the recorded shape. The
battery's retries each started a new title process and died with a SIGSEGV
instead (pids 172, 175 and 178: "page fault (user read instruction, page not
present)").

**The case alone** (`Klog_Logs/aco-state-unsigned-alone.log`, pid 181; queue
`v0-formats-sampled-uint`, `exit`): the same case died *earlier and differently*
-- **signal 11**, "page fault (user write data, page not present)", fault address
`0x7eeffdec2` (an unmapped stack page, and not a dword-aligned address), during
the case's setup and before any frame, after `agc_linked_shader_creation` -- and a
SIGSYS followed. So the fault is not a deterministic function of the shader
sequence: the same case, alone, faulted in a different place with a different
signal than it did with another case queued.

**The host does not show it.** `tools/check-aco-state.sh` builds
`tooling/psbc/compile-sequence.c` against the same compiler archive the driver
links (`libpsbc.pic.a` from the work copy) and compiles, in one process: the
recorded sequence (the unsigned case's shader twice, then `m3-texture`'s and
`v0-formats`' own sampler2D stages) and then every probe set's pixel stage three
times over -- **105 compiles, no fault**, `result=0` for every one. The compiler
source, the SPIR-V, the descriptor bindings and the options are the ones the
console ran; only the build of the compiler differs (the host archive is gcc's,
the console's is prospero-clang's), which is where the difference has to be.

**What this settles and what it does not.** It settles that the fault is the
console compiler build's, not the driver's state: nothing a probe can set -- a
failed image creation, a device destroyed mid-case, a descriptor the audit parks
-- is in the host sequence either, and the host is clean. It does not identify
the instruction: the console backtrace is three frames deep
(`0x4045e3`, `0x43a326`, `0x40016b`) because the faulting build has no frame
pointers there, and the recorded `aco::schedule_program` attribution comes from
the earlier runs' deeper backtrace. The next step is the 0.3.0 fork's compiler,
whose ACO carries a `num_waves == 0` guard of exactly this fault's class
(docs/BLOCKERS.md, the SDK section): running the same queue against it is what
tells whether the guard is the fix. No row is blocked by any of this: the
unsigned case's frames all complete when it is queued so its fault lands after
them, which is why it keeps the last place in its battery.

## 2026-09-20: the audit's "0 probe-reachable" was a tooling artefact, and the clause it hid is real

The HARDWARE_FINDINGS entry "Rung 1.0 is closed" records the split reading "0
features on 0 rows probe-reachable" and the suite asserting that the reachable
count must be zero. That reading was wrong, and it was wrong because the audit
never asked the question: `tools/format_audit.py` filed every non-`{sym1}` cell as
conditional without consulting the driver's table at all, so a requirement stated
in a table **footnote** -- over a set of rows rather than in one cell -- could not
appear in the missing list or fail `--check`.

The clause is real and the driver violates it. `formats-v1.4.354.adoc`'s
depth/stencil table footnote (vendored copy, lines 3739-3742) states two
requirements of `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT`:

- must: be supported for at least one of `X8_D24_UNORM_PACK32` and
  `D32_SFLOAT` -- **met**, by `D32_SFLOAT` (M4 step 1, C5);
- must: be supported for at least one of `D24_UNORM_S8_UINT` and
  `D32_SFLOAT_S8_UINT` -- **unmet**: the driver's table carries
  `DEPTH_STENCIL_ATTACHMENT_BIT` for `D32_SFLOAT` (`driver/ps5vk_image.c:442-446`)
  and `D16_UNORM` (`:428-432`) and for no other depth format, and neither stencil
  format has an entry at all.

The measurement is `required_clauses` over the vendored specification, which
returns exactly those two clauses; the second names no format that reports the
bit. What was blind was the *audit*, not the hardware: the same `{sym2}` marker on
those two cells is what made the requirement look like the BC and 4444 rows'
extension caveats. It is not one -- it is an unconditional `must:` over a set.
`tests/test_tools.py` counted the split and asserted the reachable count was zero,
so the false reading was not merely recorded, it was gated.

**What this changes.** The split reads **2 features on 2 rows probe-reachable**,
the audit's counts gain "1 `must:` clause no reported format satisfies" and "2
formats a set-level `must:` clause names", and `--check` fails on the clause. The
requirement is a disjunction -- the clause is satisfied as soon as one of the two
formats reports the bit -- so the probe work is the stencil path: a
`DB_STENCIL_INFO`-enabled render target of one of those two formats, its stencil
state and clear, and the stencil plane read back. The register reference is
ps5-opengl 0.3.0's hardware-run `append_depth_target_state`, whose 64 KiB-Z_X
template programs AGC `0x0011` `DB_STENCIL_INFO` `0x20000181` where our depth
template programs `0x20000180` (disabled), with the stencil bases at
`0x0013`/`0x0015`/`0x001b`/`0x001d` and the clear at `0x001a`. The lesson is
general: a requirement the audit cannot see is one the record may claim to have
closed, so the audit now evaluates set-level clauses and the split is counted from
the row table rather than asserted as zero.

## 2026-09-20: a second PS5 Vulkan implementation corroborates the sRGB fetch order, and reads the depth/stencil clause our audit used to miss

`mpereiraesaa/ps5-vulkan` (a native gfx1013 Vulkan-style implementation validated
on its author's console, firmware 12.02) is the repository our
`docs/M5_REFERENCE.md` already names for `conformance_inventory/` and
`PHYSICAL_DEVICE_REPORTING.md`. Read against the blockers this workstream is
holding, three of its artifacts are evidence rather than background:

**The sRGB curve goes on the fetched bytes, and its own witness shows it.**
`conformance_inventory/physical_format_validation.json` carries the console
evidence for `VK_FORMAT_A8B8G8R8_SRGB_PACK32`'s sampled-image cell: the texel
bytes are `[128, 64, 32, 255]` -- the *same* set the `R8G8B8A8_SRGB` witness
uses -- and the expected readback is "exact readback 0xff370d04, identical to the
recorded VK_FORMAT_R8G8B8A8_SRGB witness for the same bytes", with the comment
that the decode "0x80 to 0x37, 0x40 to 0x0d, 0x20 to 0x04 happens in the texture
unit". For a byte-reversed sRGB format Vulkan's channels are A=byte 0, B=byte 1,
G=byte 2, R=byte 3, so the *conformant* readback of those bytes is 0xffff0d04 --
R is the byte the format calls red (255 = 1.0), G the decoded 32 and B the decoded
64. Their expectation is the linearise-bytes-0..2 answer instead, which is exactly
this project's measurement: the console applies the curve to the first three
*fetched* components, before any selector, so no selector can move it and the two
byte-reversed sRGB rows stay unreachable by a driver word
(docs/HARDWARE_FINDINGS.md, "An sRGB fetch linearises the first three fetched
components, not the channels"; console run pid 160). It is a second
implementation's own console evidence agreeing with the limit this project
recorded, and it is worth quoting because that repository's capability matrix
still advertises the bit: the witness and the advertisement disagree, in the
direction our audit's two documented rows predict.

**Their inventory states the depth/stencil footnote as two clauses; their verdict
reads it as one.** `conformance_inventory/requirements.json` carries the
requirement as both a flat sentence and a structured one:
`structured: all of: [any of: VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32;
any of: VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT]`, and
`conformance_inventory/tools/derive_core_target.py` parses and evaluates exactly
that form (`parse_structured_requirement`, `requirement_satisfied`). Its
`reporting_matrix.json` verdict for the table is nevertheless the flat union:
`kind: format-any-of-rule`, all four formats in one `any of`,
`detail: supported by ['VK_FORMAT_D32_SFLOAT']`, **`verdict: satisfied`** in the
graphics profile -- the compute profile only reaches `blocker` because
`D32_SFLOAT` is not in its set. That is the same reading error R1's report found
in this project's `tools/format_audit.py` (round 11, docs/BLOCKERS.md): a clause
that requires the feature of one of *two* groups read as one group of four, so
the second clause never reaches the verdict. Two independent implementations
making the same mistake is why the audit now evaluates clauses (`required_clauses`)
and why `tests/test_tools.py` recounts them from the row table.

**Two descriptor cross-checks, and one open question they answer.** Their
`src/depth_layout.c` gives the D32 depth surface as a 128-texel-rounded pitch and
height, four bytes a texel, 64 KiB alignment and `swizzle_mode = 24` -- the
`SW_MODE` this project measured for the same targets (DB_Z_INFO 0x80000183,
docs/HARDWARE_FINDINGS.md) -- though it supplies the footprint only and says so
("pixel addressing DOES require pipe XOR topology and is not supplied"), which is
the map C5 measured. Their texel-buffer descriptor
(`src/descriptor_encode.c`) is the other structured mode:
`OOB_SELECT = 1` (`STRUCTURED`) with the record count in texels, where this
driver writes `OOB_SELECT = 0` (`STRUCTURED_WITH_OFFSET`). Both are valid; RADV
draws the same distinction, using `STRUCTURED_WITH_OFFSET` for a buffer *view*
(`radv_buffer_view.c`) and `STRUCTURED` only for a stride descriptor
(`radv_cmd_buffer.c`), which is what this driver's V# follows. No claim moves.

## 2026-09-20: the stencil plane of a combined depth/stencil attachment, measured

Round 12 rendered through a `VK_FORMAT_D32_SFLOAT_S8_UINT` attachment for the
first time on this project's console, and the frames measured four things the
stencil path's words rest on (console run pid 192,
`Klog_Logs/v0-stencil-run5.log`; golden `golden/v0-stencil`).

**The plane's layout is the AddrLib derivation's.** `ac_surface.c`'s
`gfx9_compute_surface` computes a second miptree for the stencil of a combined
depth/stencil surface: `flags.stencil = 1`, `bpp = 8`, the depth surface's own
swizzle mode (64 KiB Z_X), based at `align(surf_size, baseAlign)` in the same
allocation. Driving the frame's setup pass to store 0x5a everywhere and then
reading the image's mapped storage back *through the driver's own map of that
plane* -- 256x256 bytes a 64 KiB tile, the oracle's `swizzle 1 1 64kb_z_x` row --
found 0x5a in all 65536 bytes of the plane's first tile. A uniform value fills a
tile whatever its internal swizzle is, so this does not pin the plane's swizzle;
what it pins is the plane's *existence, extent and position*: the hardware wrote
the reference into the bytes the driver reserved for it beside the depth surface,
and the plane does not overlap the depth plane (whose own first tile held the
setup pass's 0.75 in all 16384 texels).

**`DB_STENCIL_INFO` is 0x20000181 for a separate plane.** FORMAT = STENCIL_8 (1),
SW_MODE = 24 -- the depth surface's measured 64 KiB Z_X -- and bit 29
(TILE_STENCIL_DISABLE) set, which is the word ps5-opengl 0.3.0's hardware-run
`append_depth_target_state` programs for the separate stencil buffer its runtime
binds. The Z half of the frame used `DB_Z_INFO` 0x80000183 unchanged from
D32_SFLOAT's measured word, and the stencil half worked with the stencil read and
write bases pointing at the plane (0x013/0x015 low, 0x01b/0x01d high) and every
stencil bit in `DB_DEPTH_CONTROL` conditional on the plane being bound.

**The compare path is the register pair RADV emits.** The frame's second pass
rejected every fragment when the plane held 0x5a and its reference was 0x00, and
drew the whole target when the reference was 0x5a -- with the same state as the
setup pass but FUNC EQUAL in `DB_DEPTH_CONTROL` (bits 8-10, back face 20-22),
STENCIL_ENABLE (bit 0) and BACKFACE_ENABLE (bit 7), the six face operations in
`DB_STENCIL_CONTROL` (0x10b, REPLACE as 3 << 4) and each face's reference,
compare mask, write mask and op value in `DB_STENCILREFMASK` (0x10c/0x10d,
0x01ffff5a for reference 0x5a and both masks 0xff). Vulkan's operation order is
not the register's; RADV's translation is the one that works.

**Vulkan's ignore rule has to be implemented, not assumed.** A rendering whose
attachment carries no stencil plane must program *no* stencil state: an early
version of the driver set only the enable bits from the pipeline's
`stencilTestEnable` and still wrote its compare functions into
`DB_DEPTH_CONTROL` (0x00200216 where the console's own recorded depth frame has
0x00000016), which the `golden/c5-depth` comparison caught before any console
run. Every stencil bit, and all three stencil registers, are conditional on the
bound plane now, and the depth-only golden is what says so.

## 2026-09-20: the byte-reversed sRGB fetch, and which of the two rows the curve actually blocks

The sRGB finding recorded earlier is a measurement of **one** of the two
byte-reversed rows: a texel of `VK_FORMAT_A8B8G8R8_SRGB_PACK32` whose memory
bytes are A, B, G, R read back with its red -- the fourth byte -- unlinearised,
because the console applies the curve to the first three *fetched* components
before any selector. Round 13 separated the other row from it and measured it
(console run pid 194, `Klog_Logs/v0-srgb-run2.log`).

`VK_FORMAT_B8G8R8A8_SRGB`'s memory bytes are B, G, R, A: the first three fetched
components are the format's *whole* colour triple, so the curve lands on the
three bytes the format calls its colour channels -- just in the reverse order --
and the `ZYXW` selectors the driver already uses for `B8G8R8A8_UNORM` are what
puts them in the right outputs. The driver's entry for it (the register
database's `8_8_8_8_SRGB` word, 130, with those selectors) fetched a texel stored
as B, G, R, A = 0xe1, 0xbc, 0x89, 0xff as **0x40, 0x80, 0xc0** -- the same
linearised colour its `R8G8B8A8_SRGB` twin fetches from 0x89, 0xbc, 0xe1 --
and the same table's rows then proved its blit source (54 of 54 formats) and its
blit destination and transfer pair. So the hardware's fixed fetch order blocks
the *packed* format, whose alpha byte takes the curve in a colour channel's
place, and not the byte-reversed 8888 one: **one row's six features became six
reported features and two left**, and the audit's hardware class is 8 features on
2 rows rather than 12. What the row still misses is its colour-attachment pair,
which is a `CB_COLOR0_INFO` word and an export format rather than a fetch order.

## 2026-09-20: the byte-reversed sRGB colour target, and the audit's last row

Round 13 proved `VK_FORMAT_B8G8R8A8_SRGB`'s fetch, blit and transfer path and
left its colour-attachment pair for round 14. That pair's mechanism is the
`CB_COLOR0_INFO` word: the same `8_8_8_8` data format and sRGB number type
`R8G8B8A8_SRGB` uses (10 and 6, the M4/C5 and round-8 words) with `SWAP_ALT`,
the component swap the swapchain's `B8G8R8A8` byte order has carried since M2.
With it the hardware encodes the linear colour a shader exports into bytes 2, 1
and 0, which is what the frames measured: the solid colour 0x40, 0x80, 0xc0 of
255 (whose sRGB encodings are 0x89, 0xbc, 0xe1) is stored as **0xff89bce1**, and
the additive blend of that destination with a 0x40, 0x00, 0xc0 source is stored
as **0xffbcbce1** -- the sum 0x80, 0x80, 0xc0 of 255, whose encodings are 0xbc,
0xbc, 0xe1 (console run pid 195, `Klog_Logs/v0-srgb-target-run1.log`). The blend
is additive in the target's own encoded space, which the `R8G8B8A8_SRGB` row
measured in round 8; the byte-reversed row is the same arithmetic with the bytes
exchanged.

**What that leaves.** The audit's missing list is a single row:
`VK_FORMAT_A8B8G8R8_SRGB_PACK32`, six features, quoted against the fetch order
this file measured (pid 160). Every other required format carries every feature
the specification requires of it, so the split reads 0 / 0 / 6 features on 1 row
/ 0 / 0 -- and the one row's blocker is a hardware behaviour, not a missing
driver word.

## 2026-09-20: the ACO divide fault's mechanism, and the one-case shape that reproduces it

The compile-order fault this project recorded as a SIGFPE in
`aco::schedule_program` had been reproduced and isolated (round 8) but not
explained. Round 15 reduced it to **one case** and identified the arithmetic in
the pinned compiler's source.

**The reduced shape.** `jobs/aco-min/queue.txt` runs
`v0-formats-sampled-uint` alone -- the unsigned texture case, whose compile
followed the first recorded fault -- and nothing else is needed: the case's rows
all pass, the last one (`format 107`, `R32G32B32A32_UINT`) fetches its colour,
the device goes idle, and the process then dies with **signal 8, SIGFPE,
"integer divide fault"** with `rax = rcx = rdx = 0` and `rip 0x4306e4`
(Klog_Logs/aco-min-run1.log, pid 211). Round 8's shape needed the case *plus* a
case queued behind it; one case alone is enough, so the fault belongs to the end
of that case's own compiles and teardown rather than to a following case's
compile.

**The mechanism, from the pinned tree.** The compiler the driver links is
`third_party/opengnm-psbc` (the SDK's frozen work copy). In
`src/amd/compiler/aco_live_var_analysis.cpp`'s `update_vgpr_sgpr_demand`, the
register-pressure path sets

    program->num_waves = 0;   /* "this won't compile, register pressure reduction necessary" */

when the new demand exceeds the limit, and the ordinary path can also reach zero
through `max_suitable_waves`. `get_addr_regs_from_waves` (the same file) then
computes

    uint16_t sgprs = std::min(program->dev.physical_sgprs / waves, 128);

-- an integer division by `waves`. A caller that reaches it with `waves == 0`
faults exactly as measured: a divide with a zero divisor and no memory access,
which is why the signal is SIGFPE rather than SIGSEGV and why the register
window shows zeroes instead of an address.

**The fix is already written, in the 0.3.0 fork.** ps5-opengl 0.3.0's
`toolchain/opengnm-psbc-ps5.patch` adds, at that very point:

    program->num_waves = max_suitable_waves(program, program->num_waves);
    +      if (!program->num_waves) {
    +         /* Fixed registers can lower occupancy below the workgroup minimum after
    +          * the initial pressure check. Let the normal spiller reduce demand. */
    +         program->max_reg_demand = temp_demand;
    +         return;
    +      }

Its comment names the cause -- fixed registers lowering occupancy after the
initial pressure check -- and the guard returns before any division by zero. The
pinned tree has no such guard (the fork's patch is the only place this project's
inputs carry it). So the fault is a **genuine ACO bug of the occupancy class**,
not driver state: it is a specific missing guard in the pinned compiler, its
arithmetic is identified, and the fork this project has already cloned carries
the fix. The host compiles the same 105 shaders cleanly, which says the host
build does not reach that state for them; it does not make the fault the
driver's.

## 2026-09-20: the packed sRGB format's storage order, and the audit's last row closed

The objective's last format row was `VK_FORMAT_A8B8G8R8_SRGB_PACK32`, and the
reason it was open is a property of the texture unit, not of a missing word: the
console applies the sRGB curve to the first three *fetched* components, before
any selector (pid 160), and this format's Vulkan layout is A, B, G, R -- so the
curve lands on the alpha byte and two colour bytes, and the red it must
linearise is the fourth byte, which no selector and no format word can move into
the curved set. The driver closes it by choosing the image's own layout, which
Vulkan leaves to the implementation: every image is created with
`VK_IMAGE_TILING_OPTIMAL`, so nothing but the driver's defined operations
observes the order.

**The mechanism.** The format's texels are stored in the R, G, B, A order its
`R8G8B8A8_SRGB` twin has (`ps5vk_format.storage_reversed`, driver/ps5vk_image.c),
and the four bytes are swapped at every boundary where an application's bytes
meet the image's: the row and tiled uploads, the readbacks, and the two blit
directions. Everything inside -- the sampler's fetch, the attachment's export,
the clear, an image-to-image copy -- works in the stored order, which is why the
descriptor is the twin's own: the register database's `8_8_8_8_SRGB` word (130)
with the straight RGBA selectors, and the colour target's word the same 8_8_8_8
data format and sRGB number type with `SWAP_STD`.

**The measurement.** The console holds it end to end (pid 235,
`Klog_Logs/v0-srgb-packed-run1.log`): `v0-formats-sampled` 27 of 27 rows fetched
the colour their texels hold -- the packed row's texel, stored as Vulkan's
A, B, G, R = 0xff, 0xe1, 0xbc, 0x89, comes back as the colour its red, green and
blue decode to, which is what its twin fetches from the same three levels;
`c7-blit-formats` 55 of 55 blitted and read back; `v0-blit-dst` and
`v0-transfer-formats` round-tripped it; `v0-targets` held both of its frames (the
solid colour's encodings read back in the format's order as `0xffe1bc89`, the
additive blend's as `0xffe1bcbc`); `v0-formats` 58 of 58 rows and `m2-solid`; 7 of
7 tests and no FAIL. The audit that day reads 179 required, 58 reported, **0
missing a required feature**, 55 conditional, 0 `must:` clauses unmet, and the
split 0 / 0 / 0 / 0 / 0 -- every format a Vulkan 1.0 device must support carries
every feature the specification requires of it.

## 2026-09-20: the SDK fork does not fix the compile-order fault

The compiler migration moved the driver to ps5-opengl 0.3.0's own fork (upstream
`opengnm-psbc` a92a1228 + the release's `toolchain/opengnm-psbc-ps5.patch`, tree
a27cbecc, metadata version 14) with this repository's compiler patches on top. The
compile-order fault was expected to close with it (rounds 15-16). It does not.

**The console measurement.** `jobs/aco-min` run pid 261 on the migrated build
(`Klog_Logs/aco-min-fixed.log`) faults exactly as before: `signal: 8 (SIGFPE)`,
`integer divide fault`, `rax = rcx = rdx = 0`, `rip = 0x430a14` -- the same
signature and the same address as pid 260 on the same build before the wave
patch, and as round 15's pid 211 on the old compiler. The dividers the fault was
attributed to are not it: `get_addr_regs_from_waves` divides by its `waves`
argument, so the round's patch put a witness on that divide (and restored
`workgroup_size`'s documented "unknown is UINT_MAX" invariant before
`calc_min_waves`), and the witness **never fired** in either run. The function
that would have printed is in the deployed `eboot.bin` (the string is present).

**The backtrace is not trustworthy.** Symbolizing against `build/llvm-pie.elf`
puts the three recorded frames in `aco::register_allocation`,
`aco::label_instruction` and `std::vector<aco::Temp>::__assign_with_size`, but the
instruction at the reported `rip` is a `movq`, and the second frame's address
lands mid-straight-line code: the console's three-frame walk names a region, not
a statement.

**The host cannot reproduce it.** `tools/psbc/compile-sequence.c` linked against
the work copy's compiler archive, in the recorded shape (the unsigned texture
case's pixel stage twice, then `m3-texture`) and in the stress shape (every probe
set's pixel stage three times, 120 compiles in one process), is clean under:
gcc and clang 22 archives instrumented with `-fsanitize=integer-divide-by-zero`;
the same two built with the PS5 build's own defines (`-DOPENGNM_PSBC_ORBIS=1`,
`-DNDEBUG`, which the host configuration does not carry and the PS5
configuration does); and stacks limited to 256 KB. No divide by zero is reported
and no fault occurs, so the fault lives in the console compiler build or the
console runtime around it, not in a source path the host walks.

## 2026-09-20: the "compile-order SIGFPE" was the runner's own divisor, not ACO

Every attribution above this entry -- `aco::schedule_program`, then
`aco::optimize` / `aco::register_allocation` / `aco::label_instruction`, then
the pinned compiler's `get_addr_regs_from_waves` divide and the fork's missing
`num_waves` guard -- came from the same mistake, and the fault was in this
repository the whole time. It is a **division by zero in the test runner's
sampled-format loop**, and it is fixed.

**The addresses had a load base.** The console's crash report prints absolute
addresses, and the same report names the image they are in:
`# /app0/eboot.bin`, `#  xotext: 0000000000400000:00000000009f0000 nsegs: 4` --
every one of the runs above carries that line (Klog_Logs/aco-min-fixed.log,
aco-min-stdout.log, aco-min-deep2.log, wedge.log). `build/llvm-pie.elf` is a
position-independent executable whose first `PT_LOAD` is vaddr 0, so the eboot
is loaded at **0x400000** and every address in the report has to be rebased
before symbolizing. Symbolized without the rebase, the three frames of the
`jobs/aco-min` crash land in ACO's optimizer; rebased, they are

    0x404599 -> run_linked_agc_canary      0x43a4a6 -> main      0x40016b -> _start

which is the runner's own call chain, with no compiler frame in it at all. The
`rip` rebases the same way: `0x430b34 - 0x400000 = 0x30b34`, the address of
`div %r13d` in `run_sampled_format_frames` (build/llvm-pie.elf of the same
build). The instruction is `mov %ecx,%eax; xor %edx,%edx; div %r13d`, and the
register dump of the fault reads `rax = rcx = rdx = 0` with
`r13 = 0` -- the dividend, the zeroed high half, and the divisor, exactly.

**The divisor is a texel size of a row that was never filled in.**
`run_sampled_format_frames` sizes each row's packed buffer by

    std::vector<std::uint8_t> texels(kSampledTextureWidth * kSampledTextureHeight
                                     * format.texel_bytes);
    for (std::size_t texel = 0; texel < texels.size() / format.texel_bytes; ++texel)

and the case passes `formats.size()`. `sampled_unsigned_formats()` is declared
`std::array<SampledFormat, 10>` and initializes **seven** rows; the remaining
three are value-initialized, so `format.texel_bytes == 0` and the loop's
condition is `0 / 0`. The seven real rows are the seven the console's klog
shows fetched, and the crash is at the eighth: the run's last records are
`agc_format_sample PASS "format 107 fetched as 40 80 c0"` and
`b7_device_wait_idle PASS`, and the case's next statement is that division
(Klog_Logs/aco-min-deep2.log, pid 283). `sampled_signed_formats()` had the same
shape -- declared 11, seven rows -- so the signed case was the same fault one
case later.

**The host was never asked the question.** What the host ran was the *compiler*
(`tools/check-aco-state.sh`, the UBSan archives of `libpsbc`), and it compiled
every shader cleanly, which said nothing about a loop in the runner. Run the
case through the driver-enabled host runner
(`build/host/runner_host_driver --cases driver`) and the host dies the same
way: `SIGFPE`, exit 136, the same divide. The fault was never the console
build's, never the compiler's, and never ACO's.

**The fix, and what proves it.** The two tables declare the seven rows they
initialize; the cases take the table's own size instead of repeating it; and
`static_assert(sampled_formats_filled(...))` after each `sampled_*` table makes
a declared-but-unfilled row -- the only direction the compiler cannot catch --
a build error, checked by both compilers. On the console (`jobs/aco-min`,
pid 299, Klog_Logs/aco-min-final.log) all seven tests pass, each
`v0-formats-sampled-uint` reports its rows as **"7 of 7 sampled formats fetched
the colour their texel holds"**, `m2-solid` and `v0-formats` regress, and the
klog contains **no `signal:` record at all**. The same case on the host no
longer takes a signal either.

**Retired by this.** `get_addr_regs_from_waves` and the fork's `num_waves`
guard are not this fault's mechanism (the witness on that divide never fired
because it was never reached); the "console-build-specific" framing, and the
stack-exhaustion reading of it, go with them. The 32 MiB compile stacks the
driver and the runner allocate stay as robustness -- they answer a real
recursion, not this fault.

**The rule this leaves.** A console `rip` and backtrace are absolute addresses
in an image the loader placed; rebase them by the base the crash report's own
`xotext:` line gives before symbolizing anything. Matching the build is not
enough when the build is a PIE: `build/llvm-pie.elf`'s vaddrs start at zero and
the console's do not.

## Depth bias: the two enables, the 2^-23 unit, and an inert clamp (R1)

Three measurements from the console (`Klog_Logs/r-depth-bias{2,3,5}.log`, pids 362,
363, 364; the committed build's run is pid 367), each read out of a D32_SFLOAT depth
plane the frame itself wrote:

- **The six `PA_SU_POLY_OFFSET_*` words (0x2de through 0x2e3) do nothing without
  `PA_SU_SC_MODE_CNTL`'s POLY_OFFSET_FRONT_ENABLE (bit 11) and POLY_OFFSET_BACK_ENABLE
  (bit 12).** With the block recorded and the bits clear, a constant factor of 4096
  units left every texel at the clear's value and kept no pixel; with the bits set the
  same frame kept all of them. ps5-opengl sets both for a fill-mode polygon offset
  (`src/gallium/ps5/ps5_screen.c:2036`), which is where the bits come from.
- **One unit of the constant factor is 2^-23 of depth for the D32F word**, not the
  fragment's own exponent: 4096 units moved 0.5 (0x3f000000) by 8192 ULP to
  0x3effe000, i.e. 4096 x 2^-23 of depth. The field's -23 is therefore the unit of the
  register and not a per-fragment scale.
- **The clamp register (0x2df) is inert on this path.** A 2e-5 clamp (0x3727c5ac) left
  a 0.00049 pull intact, and the same clamp on a ramp's slope changed no pixel, so the
  driver caps the constant factor in the register it writes (the slope half's gradient
  is the polygon's, so that half is the hardware's word plus a named gap). Whether the
  register needs a mode this path does not set is not measured; what is measured is
  that it does not clamp here.


## 2026-09-22: compute reads a sampled image and writes a storage image

`d2-compute-images`, runner pid 162 (`Klog_Logs/r7-compute-regression.log`,
`evidence/r7-compute/capture.json`), dispatched once over 64x4 RGBA8 texels,
fetching set 0's combined image sampler and writing set 1's storage image.
A Vulkan image-to-buffer copy after the dispatch read back the shader's BGRA
permutation with **0 mismatches over 256 texels**. Each set had its own table
and compiler-named user-data pointer. Both descriptor encodings came from the
shared graphics writer. The same battery read `0xa5a5a5a5` from the existing
single-storage-buffer shader under both direct and indirect dispatch.

This proves the two-set sampled/storage-image compute path for this linear
RGBA8 allocation. It does not establish additional formats, descriptor arrays,
or the vkQuake lightmap pass. Goldens: `golden/d2-compute-images/`.


## 2026-09-22: single-level padded texture pitch (R12)

PPSA99988 PID 196: a 32x36 RGBA8 sampled image has 128-byte source rows and
256-byte stored rows. Setting the non-array, single-level 2D descriptor's word 4
to 63 (64 texels of pitch minus one) reproduced the texture exactly with nearest
sampling and within the established bilinear tolerance. The 64x36 baseline
passed in the same run. Evidence: golden/r12-pitch, jobs/r12-pitch/queue.txt.
This proves the narrow padded 2D case; it does not prove padded array or mip-chain
placement, which remains guarded. Four submissions replay identically on host.

## 2026-09-22 — R16 first hardware run and mip-tail coverage correction

PID 206 tested Quake's 512x512 five-level tiled water texture: upload a 0/254
checkerboard, generate four lower levels with linear blits, then sample each
pinned LOD. CPU checks using the same additive addressing as the driver found
87,040 expected texels after each frame. Hardware instead found only 7,776,000
of 8,294,400 expected pixels for the 64x64 level; the other three levels were
exact. This candidate failed; no vkQuake launch followed. The failed capture
and measurements remain in golden/r16-mip-blit-before.

The independent whole-chain AddrLib query (the measured 64KB_R_X colour mode)
shows why the CPU check was insufficient: the packed tail origin participates
in the XOR swizzle. Adding its swizzled value can carry into another bit.
For both the 256x256/five-level and 512x512/five-level chains, addition differs
at 2,048 texels; XOR differs at zero of 87,296 and 349,184 queried texels.
The per-level origins and previously recorded centre addresses do not change.

This explicitly narrows the older C7 claim: matching level origins and sampled
centres did not establish every texel in a packed tail. The earlier narrative
about the shifted tail coordinate was correct; its implementation as addition
and the matching CPU probe were not. Earlier entries/goldens are retained.
The corrected candidate is still awaiting its hardware rerun at this entry.


## 2026-09-22 — R16 corrected mip readback accepted, PID 207

The user explicitly resumed corrected readback, then vkQuake relink/launch.
PS5 PID 207 returns 277 PASS, zero FAIL. m2-solid, c7-mip-upload and the new
r16-mip-blit pass; every pinned lower mip matches 8,294,400/8,294,400 pixels,
including the formerly failing 64x64 level. Independent shifted-coordinate
CPU checks match all 87,040 lower texels in each frame. Ten streams replay
exactly. Known benign VideoOut unregister-busy warning; title closed and
count=0 confirmed. Failed PID 206 evidence and its separate correction remain.

Explicit rebuild reproduces 14,428,778-byte archive SHA-256
8d5206d5d4fc1535c342916b71c81e57d62ae4086d14fcd993074bc4c2fc8c67.
Fifteen targeted loader/direct/link arms, cache package checks, eleven driver
gates, port five gates/scan and template relink PASS. Two deployed ELF reads
and all five PT_LOAD segments match local content. No visual settings changed.

Source is accepted; checkpoint edb8ebd retains the original parked patch.
Evidence: golden/r16-mip-blit-corrected; reproduction: jobs/r16-mip-blit/README.md.
This accepts mip transfer/sampling, not M6. vkQuake relink/launch follows.


## 2026-09-23 — R17 descriptor arrays accepted, PID 209

The user requested R17 then R18. Each descriptor array element now has its own
record; writes, copies and partial updates use binding record indices. Shared
graphics/compute validation and emission walk elements at their declared stride.
Dynamic offsets retain binding/element order. Input-attachment arrays remain
refused pending a subpass-index witness.

PS5 PID 209: 104 PASS, zero FAIL. Both scalar d2-compute-images and the new
r17-descriptor-array produce all 256 exact output texels; two streams replay
exactly. The new case writes/copies/partially updates a three-image array,
changes its source set afterwards, and requires final order [2,0,1]. Host direct
checks confirm all three emitted image addresses. Title closed; count=0 checked.
Two deployed ELF reads and all five PT_LOAD segments match.

Explicit driver archive: 14,434,234 bytes, SHA-256
aad0ebc750f06f00e130b524e4ebe055a8d190682a9abfbdba2ab105935f634a.
Full check-driver initially 169/170 PASS: capability direct expected compiler
stderr but a cache hit skipped compilation. That warning-specific test now
explicitly disables cache; its three loader/direct/link arms PASS. Eleven gates,
port five gates/scan and template relink PASS. No runtime cache change.
Evidence/reproduction: jobs/r17-descriptor-array and golden/r17-descriptor-array.
R18 exact padded image shape remains next; no vkQuake retry yet.

## 2026-09-23 — R18 first candidate rejected; row mip origins measured

PID 211 removes only the padded-chain refusal. C4's single-level 32-wide
nearest/bilinear case remains correct, but both 224x195/eight-level and
32x36/six-level chains fail their full-frame pinned-LOD checks. Status totals:
476 PASS, 33 FAIL; title closed and count=0 verified. No vkQuake retry follows.
Failed captures are retained in golden/r18-padded-mips-before.

The address-filled 224x195 chain returns level origins 0x12800, 0x6400,
0x3300, 0x1a00, 0xd00, 0x600, 0x200, 0. These exactly match the pinned
AddrLib ADDR_SW_LINEAR result: smaller levels precede larger levels; stored
width/height round up and row bytes align to 256. The driver's allocation
size is correct (275,456 bytes), but its upload/readback offsets were forward.
Nine samples per level also record horizontal/vertical steps; address-map.txt
contains the 72 values. The address collection's PASS is not pixel acceptance.

This descriptor supplies word 4's custom pitch, unlike the old C7 row-chain
runs whose word 4 was zero and whose levels did not select. That old measured
failure remains; the candidate now supplies pitch for aligned 2D mip chains as
well, with a separate 256x256/five-level full-frame regression queued. Layout
and descriptor corrections still require a fresh hardware run.

## 2026-09-23 — R18 row mip layout accepted, PID 214

The shared row-chain layout now places smaller levels before larger levels,
keeps each row aligned to 256 bytes, and sums all levels for layer size.
Non-array 2D mip descriptors supply the stored pitch even when the base width
is already aligned. Padded mip chains can now record. Arrays retain their
separate descriptor fields and guards; no port texture/visual workaround.

PS5 PID 214: 531 PASS, zero FAIL. All 19 pinned mip frames match all 8,294,400
pixels: 224x195/eight levels (the actual vkQuake image), 32x36/six levels,
and 256x256/five levels. Single-level C4 nearest/bilinear also passes.
All 21 command streams replay exactly. Two deployed ELF reads and all five
PT_LOAD segments match. Known benign VideoOut unregister-busy warning; title
closed and count=0 checked. Goldens: golden/r18-padded-mips-complete.
The verifier accepts PID 213/214 readbacks and rejects failed PID 211.

Explicit driver build: 14,434,994 bytes, SHA-256
cef1d81708d06d6fa68b2ac5df6b3f781c0fb59e3026e83e09ee469b112167fa.
Twenty-one targeted check-driver loader/direct/link arms, shader cache checks,
all eleven gates, port five gates/scan and template relink pass. The later
recorder-only change adds log_driver_stages; lint, unit and runner gates pass,
and the driver archive is unchanged. PID 213's successful pixel-only capture,
PID 211's failed candidate, and the misqueued PID 212 history are retained.
Reproduction: jobs/r18-padded-mips/README.md and the fixed queue beside it.
Next: vkQuake deployment and launch; M6 is not claimed.


## 2026-09-23 — R19 UINT32 indexed draws accepted, PID 216

vkQuake PID 215 named UINT32 indices as the next refusal. The shared draw
path now uses the bound element width for robust bounds, firstIndex byte
offsets and the native size packet. It writes index size on every indexed
draw: the first host candidate found a stale cached-size flag across separate
recordings (direct passed; indirect and secondary omitted the packet).

The new probe uses vertices 65536..65539, firstIndex 3, and a nine-element
request clamped to the six remaining elements. PS5 PPSA99988 PID 216 reports
257 PASS, zero FAIL: direct, indirect and secondary frames each match all
8,294,400 white pixels, with exact UINT32/offset/bounds packet checks. UINT16
regressions pass before and after. All five Vulkan streams replay exactly
without migration options. Two deployed ELF reads and all five PT_LOAD
segments match; title closed and count=0 verified. The known VideoOut
unregister-busy warning is unchanged.

Explicit archive: 14,434,594 bytes, SHA-256
f2666ab80aadfb5a64722f9b7f014dd29c9c595462e9a1ae65d854ddbd26d411.
All 170 check-driver arms and shader-cache checks pass; eleven gates, port
five gates/scan, template relink and final runner rebuild/lint pass. Historical
UINT16 captures remain unchanged: check-driver's explicit migration verifies
a fresh exact UINT16 size write before every indexed draw and compares every
other packet/table. Missing/reused/wrong-size writes fail the unit witness.
Default golden comparison remains strict; new R19 captures use that default.

Evidence: jobs/r19-index32/queue.txt and check.py; golden/r19-index32/run-1.json,
readback.txt, replay.txt and deployed-proof.txt. Reproduce pixel acceptance:
python3 jobs/r19-index32/check.py Klog_Logs/r19-index32.log --pixels.
Next is vkQuake relink/deployment/launch; playable-world acceptance is open.


## 2026-09-23 — R20 corrects the R10 readback diagnosis, PID 219

Correction to the earlier quarter-width claim: the old v0-subpass probe
indexed a tiled attachment as linear rows. It also never mapped the writer
attachment and substituted the expected word when no mapping existed. Its
reported 16/16 writer values were not observations. PID 218 reproduces that
old 4/16 reader result and is retained in golden/r20-subpass-before; it does
not establish a descriptor-width defect.

The probe now maps the writer, requires both mappings and decodes both using
the previously measured tiled RGBA8 layout. In each of two frames it compares
all 8,294,400 writer pixels and all 8,294,400 reader pixels against the shader's
positional band pattern. PID 219: 128 PASS, zero FAIL; every pixel matches.
The first frame's two captured submissions replay exactly. The host emits
four submissions for two frames; its full dump is retained and only the first
two are compared with the console's first-frame capture. No second-frame
command capture is claimed; both frames have complete pixel verification.

This is a test correction, with no production driver or shader change. Archive
remains f2666ab80aadfb5a64722f9b7f014dd29c9c595462e9a1ae65d854ddbd26d411.
The v0_subpass loader/direct/PS5-link arms, all eleven gates and final lint pass.
Two deployed ELF reads/all PT_LOAD segments match; title closed and count=0
verified. The known unregister-busy warning remains. The verifier accepts
PID 219 and rejects the old PID 218 capture. Reproduce with
jobs/r20-subpass/queue.txt and python3 jobs/r20-subpass/check.py
Klog_Logs/r20-subpass.log; evidence in golden/r20-subpass. This retires the
quarter-width finding, without asserting all game visuals are correct.

## 2026-09-23: vkQuake target cache work is measurable frame cost

R21 PID 222 measures an average 509.54 MiB of target CPU cache eviction per
frame, taking 7.87 ms of 15.20 ms queue wall time. Native submit/marker waiting
is 2.85 ms, flip waiting 7.79 ms; the first two components overlap queue time.
27 intervals cover 6,334 frames after the first present. These are CPU wall
timings, not GPU shader timestamps. jobs/r21-profile/baseline.txt preserves
all intervals and identity. Repeated registrations of a target are flushed
repeatedly without intervening work; eliminating those is the next measured
candidate, not yet a claimed performance improvement.

## 2026-09-23 — R22 duplicate target flush removal

Identical address/byte ranges are evicted once per flush operation; all distinct
ranges, step boundaries, markers and CPU copies remain. PID 224 completes seven
cases, 1,043 PASS / zero FAIL; 14 exact replays. The query-copy runner allocates
equal-size copy storage before the counter pool; replay metadata now reflects
that order without changing command words. PID 223 timed out during final
resolve readback and remains an incomplete capture, not a passing battery.

Archive b95beefd…; explicit build, 170 driver/cache checks, eleven gates, port
five gates/shader scan and template relink PASS. Port PID 225 runs 300 seconds,
6,557 presents, no reported game/audio error. Flush cost falls 7.872 -> 5.788 ms
and 509.54 -> 373.73 MiB/frame, but increased flip wait leaves FPS around 20–30.
No FPS gain claimed. Both trace reads and deployed ELF match; PID correlated,
closed and idle verified. See jobs/r22-target-flush and its retained goldens.

## 2026-09-23 — R23 swapchain transfer-source readback

Swapchain images now report/accept TRANSFER_SRC alongside COLOR_ATTACHMENT;
TRANSFER_DST remains refused. Existing image-copy code is used unchanged.
PID 229: 238 PASS, zero FAIL, original C1 plus four copied frames. Each copy
compares every one of 8,294,400 linear pixels against an independently checked
tiled render before presentation; both buffers are covered twice. Sixteen
captured draw/flip streams replay exactly.

The full host runner exposed an absent optional helper workspace and stale
initial flip-counter metadata. The former is guarded; replay seeds the existing
model from recorded process state. No captured command or tolerance changed.
See jobs/r23-display-readback/replay-notes.txt for original failures. Explicit
driver build, host/cache checks, eleven gates, port five gates/scan and template
relink pass. Lint/unit/runner and console build pass after the harness-only fix.
Archive e662f699…; two deployed ELF reads/all load segments match, title closes
and idle is verified. Goldens: golden/r23-display-readback.

## 2026-09-23 — depth state persists across an attachment change

R25's depth-attached then colour-only passes prove that omitting DB_DEPTH_CONTROL
from the second pass does not disable the first pass's test: 518,400 overlapping
pixels retain the wrong colour, while depth samples remain correct. Explicitly
writing zero when the depth/stencil rendering ends fixes D32 and D16 overlays
and preserves ordinary depth, stencil, bias and display readbacks. The reset uses
the existing AGC indirect context-register helper. Evidence: jobs/r25-depth-detach,
PIDs 243–245. No hardware-layout or firmware-derived data is introduced.

## 2026-09-23 — sampler LOD bias readback (R26)

PID 250 measures the sampler word-2 signed bias field with implicit derivatives
selecting level 2. Bias -2/-1/-0.5/0/0.5/1/2/0 produces exact RGBA greys
10/100/150/200/120/40/60/200 across eight entire 3840x2160 frames. The fractional
cases use linear mip filtering; all 66,355,200 pixels match without tolerance.
The encoding follows public Mesa ac_build_sampler_descriptor. This covers the
driver's advertised +/-2 range endpoints and the fractions used here, not an
exhaustive test of every fractional bit pattern. jobs/r26-lod-bias holds evidence.

## 2026-09-23 — a system call costs ~20 us; colour targets need no blanket flush

R34's cost probe (port evidence m6-r34-cost-probe), 10,000 calls each on the
console: clock_gettime 20.3 us, getpid 20.1 us, sceKernelReadTsc 11.8 ns,
sceKernelGetProcessTimeCounter 12.1 ns, an uncontended mutex pair 16.3 ns. The
TSC ticks at the reported 1,596,300,232 Hz (32,167,462 ticks over 20.150 ms).
Anything on a per-frame path that enters the kernel pays ~20 us. Whether this is
the firmware or the homebrew environment is not known.

R37 (jobs/r37-mapped-flush): with every CPU access to a colour target going
through driver paths that flush what they write and invalidate what they read,
the per-step whole-target flush is needed only for memory the application maps;
the full runner battery is identical with and without it. The completion marker
of a vkQuake step is written within ~0.15 ms of the submission (R38).

## 2026-09-24 — a bare primitive-restart write lands in the draw before it (R64)

VGT_MULTI_PRIM_IB_RESET_EN (uconfig 0x3092c) written right after an indexed
draw, with no event between them, takes effect while that draw is still
fetching indices: past about 300 indices the rest of a restart strip draw
fetched its 0xffff indices as vertices, with the same result for 1894 to 4096
indices and for quads, single triangles and six-vertex strips. An SQ_NON_EVENT
(EVENT_WRITE, event type 0) before the write orders it behind the draw: a draw of
4096 indices run 64 times is untouched. A VS_PARTIAL_FLUSH before it works too,
at the cost of a stall. This matches Mesa's public note on GFX10 and GFX10.3
(ac_gpu_info.c, has_prim_restart_sync_bug) and RADV's handling of it. Evidence:
jobs/r64-restart-strips (A/B build, then PID 449 with the fix).

## 2026-09-24 — every submission starts with primitive restart off (R65)

A command stream the queue submits after a split (the driver's CPU copies split
a command buffer's submission) starts with VGT_MULTI_PRIM_IB_RESET_EN off, even
when the words before the split left it on: restart strips drawn first after a
copy fetched their 0xffff indices as vertices (PID 501), and a draw without
restart after the same split fetched vertex 0xffff correctly with no write
before it. Whether the reset comes from the console's submission path or the
GPU's own state handling is not known; the driver treats a split like the start
of a command buffer. Evidence: jobs/r65-restart-split.
