# Linked canary shader packages

The isolated `PPSA99998` canary looks for two PS5 shader packages:

```text
/download0/probes/shaders/vertex.bin
/download0/probes/shaders/pixel.bin
/download0/probes/shaders/checksums.txt
```

For packaged test assets, the same three files may instead be placed under
`/app0/probes/shaders/` in the canary application image.

These must be PS5 shader ELF packages produced by the PS5 OpenGL/PSBC/ACO
toolchain. The canary validates the ELF section table, `.shader_header` and
`.shader_text` sections, PS5 shader-header magic/version, declared sizes,
stage (`2` vertex or `1` pixel), and the FNV-1a-64 file checksum against
`checksums.txt`. The manifest format is one entry per line, for example:

```text
vertex 0123456789abcdef
pixel  fedcba9876543210
```

The canary distribution includes a known-good pair derived from the local
ProsperoLight split assets. A package is passed to `sceAgcCreateShader` only
when both packages and their checksums validate, the stage-specific AGC
program-checksum register is present and nonzero, and linked
AGC initialization succeeds. Shader creation is the only new AGC operation in
this gate; shader linking, command encoding, queue submission, and presentation
remain disabled.

Use `tools/prepare_canary_shaders.py` to make the deployable directory after
the OpenGL toolchain produces packages:

```text
python tools/prepare_canary_shaders.py vertex.agc.sb pixel.agc.sb -o probes/shaders
```

To reproduce the bundled split-asset packages from a ProsperoLight checkout:

```text
python tools/package_split_agc.py \
  --vertex-header ../ProsperoLight/assets/private/geometry.header.bin \
  --vertex-text ../ProsperoLight/assets/private/geometry.text.bin \
  --pixel-header ../ProsperoLight/assets/private/pixel.header.bin \
  --pixel-text ../ProsperoLight/assets/private/pixel.text.linear-buffer.bin \
  --resources ../ProsperoLight/assets/private/netflix-video-resources.bin \
  -o probes/shaders
```

The wrapper preserves each header and machine-code section byte-for-byte and
records the resolved register values, source file names and the optional
`resources.bin` descriptor tables in `PROVENANCE.txt`. The bundled pixel
package uses ProsperoLight's SDR linear-buffer code; ProsperoLight shares one
pixel header between its SDR and P010 code, and both are 2304 bytes.

After package validation and shader creation, `PPSA99998` performs one
canary-only `sceAgcLinkShaders` call using primitive type `6`. It supplies
zeroed link context/uniform work areas at offsets `0x5000` and `0x6000` in a
64 KiB direct-memory workspace. Command encoding, queue submission, and
presentation remain disabled until link behavior is confirmed.

`PPSA99998` builds a bounded command stream in the same 64 KiB workspace but
does not submit it; its overall result is the command-encoding result. With
the complete-state canary enabled, it copies bounded CX and SH register
tables from the created shaders and uses the link-generated UC area before
encoding the draw packet. Live
submission is isolated in `PPSA99996`, built with
`AGC_LINKED_CANARY=1 AGC_LIVE_SUBMISSION_CANARY=1`.

The submission canary builds one complete frame in ProsperoLight's SDR
presentation order (`run_live_frame`), using the same 64 KiB workspace:

1. Load `resources.bin` (checked against the `resources` entry in
   `checksums.txt`) at `0xc000` and relocate its descriptor tables. Open
   VideoOut, allocate and register two 1920x1080 SDR framebuffers, and fill a
   1920x1088 NV12 coordinate card with neutral chroma: a horizontal luma ramp
   (Y = 16 + 219x/1919) in the top half and a vertical ramp in the bottom
   half, so each grey output level encodes a source x or y.
2. Emit the `sceAgcDriverWaitUntilSafeForRendering` packet.
3. Take the 16 render-target registers from `sceAgcGetRegisterDefaults`, aim
   them at framebuffer 0, and add 15 viewport/scissor registers.
4. Append the linked CX table (`0x5000`), both shaders' CX tables, the UC area
   (`0x6000`) and the combined SH table (`0x6800`).
5. Write the vertex descriptor to SH `0x8c + slot` (geometry constants at
   `0x7800`, resource tables) and the 30-word pixel descriptor to SH `0x0c`
   (NV12 planes, bilinear samplers, pixel constants at `0x7900`).
6. Encode a four-vertex quad draw and a flip with marker `0x5053564b`, then
   `clflush` the workspace.

Every step is logged (`agc_live_*`), with every address the GPU will read
(`agc_gpu_pointer_*`) and the full command stream. The AGC `Dcb` helpers
return the start of the packet they wrote, so the stream length is taken from
`command.up`. The stream is then decoded and every indirect register-load
packet (opcodes `0x63`, `0x64`, `0x9f`) is checked against the direct-memory
workspace, a port of ps5-opengl's `validate_indirect_register_tables`.

`PPSA99996` submits only when the imports resolve, validation passes, every
step above reports ready (`agc_pre_submit_state`), and the title was built with
`AGC_LIVE_SUBMIT_ARMED=1`. Without that define it logs `REFUSED` with the
reason "not armed". When armed, it waits at `sceAgcSuspendPoint` and for the
VideoOut flip status to reach the marker, reads framebuffer 0 back on the CPU
(`agc_framebuffer_layout` decodes the target through ps5-opengl's tiled RGBA8
layout and requires a smoothness score of at most 0.5 red levels per step,
with row-major logged for contrast; `agc_framebuffer_pattern` checks the
coordinate card's monotonic and constant directions on every drawn pixel;
`agc_framebuffer_extent` measures coverage; `agc_framebuffer_readback` logs
decoded samples), then holds the GPU-drawn frame on screen for 300
vblanks (`agc_live_frame_hold`) so it can be inspected. The live frame hides
the Shell splash screen first (`agc_live_hide_splash`); otherwise the splash
covers the frame until the CPU demo hides it after the canary returns. If
completion is not confirmed it retains VideoOut and all GPU buffers instead of
releasing them. Otherwise it drains pending flips (up to 120 vblanks), then
unregisters and closes VideoOut and releases the framebuffers and source
before the CPU demo scene opens its own VideoOut handle. Unregister result
`0x80290009` (busy) is logged as `WARN`: ProsperoLight tolerates it and
ps5-opengl records it on every shutdown before a successful close.

`PPSA99988` ("PS5 Vulkan Test Runner") runs every own-shader test from M2 to
M4. It replaced the per-test titles `PPSA99995`-`PPSA99989`: it is installed
once, and which tests run is data. Build and deploy it in place with:

```text
PARAM_PATH=sce_sys/param-runner.json \
APP_DEFINITIONS="AGC_LINKED_CANARY=1 AGC_TEST_RUNNER=1 AGC_OUTPUT_4K=1 AGC_LIVE_SUBMIT_ARMED=1 AGC_SHADER_COMPILER=1" \
make deploy
```

`AGC_SHADER_COMPILER=1` links the PS5 shader compiler for the `compile`
keyword below. It needs `bash tools/build-psbc-ps5.sh` once, and adds about
15 MB to the executable.

The title packages every set under `/app0/probes/`, and each test links its
own:

| Test | Package set | What it checks |
| --- | --- | --- |
| `m2-solid` | `m2` | Solid full-target triangle |
| `m3-uniform` | `m3` | Uniform buffer rewritten between two frames |
| `m3-vertex` | `m3-vertex` | Vertex and index buffers |
| `m3-texture` | `m3-texture` | Nearest and bilinear texture sampling |
| `m4-depth` | `m4-depth` | Clear and depth testing |
| `m4-blend` | `m4-blend` | Over, additive and premultiplied blending |
| `m4-rtt` | `m3-texture` | Render to texture |
| `b4-marker` | `m2` | Completion without a flip: `m2-solid` ending in a completion marker (shows nothing) |
| `b4-headless` | `m2` | Headless completion: no VideoOut, a direct-memory target (shows nothing) |
| `b4-timing` | `m2` | Whether the suspend point waits for the GPU: 1, 16 and 128 repeated draws (shows nothing) |

At launch the runner reads its queue from `/app0/jobs/queue.txt`. Uploading
one of the repository's queues replaces it without a rebuild:

```text
python3 tools/ps5_console.py push-jobs --replace PPSA99988 jobs/regression/queue.txt
python3 tools/ps5_console.py push-jobs --replace PPSA99988 jobs/capture/queue.txt
python3 tools/ps5_console.py push-jobs --replace PPSA99988 jobs/compile/queue.txt
python3 tools/ps5_console.py push-jobs --replace PPSA99988 jobs/b4/queue.txt
python3 tools/ps5_console.py push-jobs --replace PPSA99988 jobs/b4-timing/queue.txt
```

Each line holds one of:
- a test name, or `all` for every test
- `hold <vblanks>`: 1-3600 vblanks for every checked frame of the queue
  (default 300)
- `capture`: every frame logs a golden capture before submission. That is
  its whole command stream, each AGC helper call with its arguments and the
  words it wrote, the non-zero 64-word chunks of its stage workspace, and the
  workspace's FNV-1a 64. The command stream itself is unchanged.
- `compile`: every test compiles its package set on the console before
  linking (Milestone 5 Phase A3, `docs/M5_PHASE_A.md`). For each stage the
  runner reads `<stage>.spv` and the stage's line of `compile.txt`, parses it
  like the opengnm-psbc CLI, compiles it with `psbc_compile_shader` and
  packages the result with ps5-opengl's C writer. The package must be
  byte-identical to `<stage>.bin` (`agc_console_compile_<stage>`, which logs
  the compile time, and on a mismatch the first differing byte). The compiled
  package then replaces the loaded one, so validation, `checksums.txt`,
  linking and drawing all use the console's output. Needs a runner built
  with `AGC_SHADER_COMPILER=1`; otherwise the queue is rejected.

Blank lines and `#` comments are ignored. An unknown or malformed line rejects the
whole queue (`runner_queue` `FAIL`); without the file every test runs once.

The runner hides the splash screen once, then for each queued test:
1. It logs `runner_test_start` with the test name.
2. It loads, validates and links the test's package set into a fresh 64 KiB
   stage workspace (`link_shader_packages`).
3. It runs the test, which opens, uses and releases its own VideoOut target
   and buffers.
4. It releases the workspace and logs `runner_test`, `PASS` only when the
   test's own readback checks passed.

A test whose submission may still be in flight keeps its memory and stops the
queue (`runner_abort`). `runner_summary` passes when every queued test passed.
`python3 tools/ps5_console.py klog` reads the run straight from the console's
klog port, prints each test's result and saves the whole log under
`Klog_Logs/`.

The `m2-solid` test loads `vertex.bin`, `pixel.bin` and `checksums.txt` from
`/download0/probes/m2/` or `/app0/probes/m2/`. Every set also holds the
SPIR-V (`vertex.spv`, `pixel.spv`) and `compile.txt`, the exact compiler
options in CLI syntax, for the `compile` keyword. `SHA256SUMS` covers all of
them. These packages are compiled from `shaders/m2` by:

```text
bash tools/build-probe-shaders.sh m2
```

The script needs Linux, the ps5-opengl SDK checkout next to this
repository (override with `PS5_OPENGL_SDK`) and glslang. It takes glslang from
`GLSLANG`, then `PATH`; CachyOS provides it with
`sudo pacman -S --needed glslang`.

It also needs the SDK's own `opengnm-psbc` compiler, which the SDK checkout
ships as source. Build it once, in place:

```text
make -C ../ps5-opengl-sdk-0.2.0/third_party/opengnm-psbc
```

Run the SDK's `toolchain/build-opengnm-psbc.sh` instead when the pinned source
tree needs re-verifying. The compiler binary must keep its executable bit; a
checkout copied from a Windows filesystem loses it and the script then reports
a missing compiler.

Every set in this directory was rebuilt on CachyOS against glslang 16.4.0 with
`--target-env vulkan1.0`, the environment the driver's own device version
matches. The packages, `bindings.txt` and the compiler option files come out
byte for byte as they were, so the golden `/golden/c1-triangle` capture still
holds the packages these scripts write; the SPIR-V is 1.0 now, and the
`SHA256SUMS` and `PROVENANCE.txt` receipts moved with it and with the glslang
release and the SHA-256 of the locally linked probe compiler. The vertex
package uses ESGS ring item size 1, as ps5-opengl's
runtime packager does for vertex-source NGG shaders. Like those runtime
packages, both leave their program-checksum registers unresolved, so the
runner logs them as `NOT_REQUIRED` and applies ps5-opengl's header checks
instead. It links with primitive type `4` and runs `run_own_shader_frame`,
which shares the VideoOut target, wait packet, render-target registers, linked
shader state, indirect-table validation, submission, flip-marker wait, hold
and teardown with `run_live_frame`. It draws three auto-generated vertices with
no descriptors or source image. The readback (`agc_solid_*`) requires every
3840x2160 pixel to equal `0xff20a0ff` and the tiled padding to stay zero.
Framebuffer 0 is programmed as a B8G8R8A8 target (`CB_COLOR0_INFO` COMP_SWAP
= SWAP_ALT, logged as `agc_live_render_target` `color_info`) so the shader's
RGBA output matches VideoOut's B,G,R,A scanout; `agc_solid_channel_order`
reports `SWAPPED` if red and blue come back exchanged.

The `m3-uniform` test loads `vertex.bin`, `pixel.bin`,
`checksums.txt` and `bindings.txt` from `/download0/probes/m3/` or
`/app0/probes/m3/`, all produced by:

```text
bash tools/build-probe-shaders.sh m3-uniform
```

The script has the same requirements as the M2 script. It compiles with
`--address32-hi 2`, declares the pixel binding `0:0:uniform_buffer:1:0:16`,
fails unless the metadata shows exactly that binding and a descriptor-set
user-data dword inside the pixel user-data range, and records the layout in
`bindings.txt`:

```text
address32_hi 2
pixel_user_sgpr_count 3
pixel_descriptor_set0_dword 2
pixel_set0_binding0_offset 0
pixel_set0_binding0_stride 16
```

`run_uniform_buffer_frames` loads and validates `bindings.txt`
(`agc_uniform_bindings`), requires the descriptor-set table and buffer to lie
at the compiled address high word (`agc_uniform_addresses`), and logs the
descriptor and user-data words. It then draws two frames through
`build_own_shader_frame`, which M2 shares: the pixel user data is written with
`SET_SH_REG` at SH `0x0c` after the SH table. Frame 0 draws pink into buffer 0
and frame 1 green into buffer 1 after rewriting the buffer. Each frame is
submitted, its own flip marker awaited, read back with `agc_solid_*` (tagged
with its frame number) and held on screen; `agc_uniform_buffer` summarises
both.

The `m3-vertex` test's packages, `checksums.txt` and
`bindings.txt` (`address32_hi`, `vertex_user_sgpr_count`,
`vertex_buffer_table_dword`, `vertex_base_vertex_dword`,
`vertex_ngg_lds_dword`, `vertex_ngg_lds_layout`) load from
`/download0/probes/m3-vertex/` or
`/app0/probes/m3-vertex/`, built by:

```text
bash tools/build-probe-shaders.sh m3-vertex
```

`run_vertex_buffer_frame` validates the bindings (`agc_vertex_bindings`) and
requires the vertex-buffer table and vertex data to lie at the compiled
address high word (`agc_vertex_addresses`). It writes the vertex records, the
16-bit index list and the stride-form descriptor, then draws one indexed frame
through `build_own_shader_frame`: vertex user data at SH `0x8c` (the
vertex-buffer table, base vertex 0 and the NGG LDS layout, as ps5-opengl's
gallium draw fills them), then
`sceAgcDcbSetIndexSize`, `sceAgcDcbSetIndexBuffer`, `sceAgcDcbSetIndexCount`
and `sceAgcDcbDrawIndex`.
The readback (`agc_vertex_*`) checks the square's exact extent, red and alpha,
and the green and blue gradients against their pixel-centre values.

The `m3-texture` test draws with packages from `probes/m3-texture`
(`bash tools/build-probe-shaders.sh m3-texture`). Its `bindings.txt` holds the
vertex keys of `m3-vertex` and the pixel keys of `m3-uniform`, with a 48-byte
binding.

`run_texture_frames` works as follows:
- It shares `prepare_vertex_input` with the vertex-buffer test and
  `load_pixel_bindings` with the uniform test.
- It requires the descriptor-set table (`0xc300`) and the texels (`0xd000`)
  to lie at the compiled address high word, with the texels on a 256-byte
  boundary (`agc_texture_addresses`), then uploads the texels.
- It draws a nearest frame into buffer 0, then a bilinear frame into buffer
  1, by rewriting only the sampler word of the combined image-sampler
  descriptor.

`agc_texture_*` checks each frame's extent, padding and colours: exact for
nearest, and within two levels of the float blend for bilinear, with an error
histogram.

The `m4-depth` test draws with packages from `probes/m4-depth`
(`bash tools/build-probe-shaders.sh m4-depth`). Its `bindings.txt` holds the
vertex keys only.

`run_depth_frame` sets up the frame:
- It places 19 records and 27 indices through `prepare_vertex_input`.
- It maps a depth buffer (`agc_live_depth`), fills colour with a sentinel and
  depth with 0.0, and writes two `0x200` depth-control records at `0xc500`.

It then submits five indexed draws through `build_own_shader_frame`, which
emits the depth-target registers with the colour target:
- the clear with ALWAYS
- four rectangles with LESS

`check_depth_frame` compares every pixel's colour word and every depth sample
with the nearest shape. It logs samples at the clear and at each pair's
overlap, near-only and far-only pixels.

The `m4-blend` test loads `probes/m4-blend`: the `m4-depth`
shaders with the pixel stage compiled for FP16_ABGR colour exports, drawn with
no depth target bound. Build the packages with:

```text
bash tools/build-psbc-cli.sh
bash tools/build-probe-shaders.sh m4-blend
```

`tools/build-psbc-cli.sh` builds `build/host/opengnm-psbc-probe`, following
these rules:
- It copies the SDK's opengnm-psbc CLI (MIT) with one added option,
  `--color-format`, which sets the per-MRT `SPI_SHADER_COL_FORMAT` nibbles
  that libpsbc already accepts.
- It links against the SDK's prebuilt `libpsbc.a` with the SDK host flags.
- Without `--color-format` it produces the SDK binary's output byte for byte.
- The `m4-blend` set fails unless the compiled pixel `SPI_SHADER_COL_FORMAT`
  is FP16_ABGR (4).

`run_blend_frame` works as follows:
- It writes 20 records and 30 indices through `prepare_vertex_input`, using
  the `put_rect` geometry it shares with the depth test.
- It places one `0x1e0` blend-control record per mode at `0xc500`.
- It submits five indexed draws, each applying its own record: two opaque
  halves, then over, additive and premultiplied rows.

`check_blend_frame` compares every pixel with the exact per-channel blend of
the covering rectangles in draw order, within one level.

The `m4-rtt` test loads the `m3-texture` packages and uses them for both
passes.

`run_render_to_texture_frame` sets up the frame:
- It places three textured quads (12 records, 18 indices) and uploads the M3
  texture.
- It maps an offscreen target (`agc_live_offscreen`) the size of the
  framebuffer.
- It writes two combined image-sampler descriptors with the shared
  `write_image_descriptor`: the texture at `0xc300`, and the tiled offscreen
  target at `0xc340`.
- It stores the framebuffer's 31 target registers as an indirect CX table at
  `0xc400`.

`build_own_shader_frame` then emits the frame in order:
1. The first two draws, aimed at the offscreen target (`FrameResources`
   `colour_target`, COMP_SWAP 0).
2. The third draw step: `sceAgcCbReleaseMem` (event 45, control 12), the
   framebuffer CX table, and pixel user data for the target descriptor.
3. The mirrored full-target quad.

`check_rtt_frame` compares
every offscreen and every framebuffer pixel with its exact expected word.

`tools/build-probe-shaders.sh` builds all six sets and replaced the earlier
per-milestone `build-m2-shaders.sh` and `build-m3-shaders.sh`; it reproduces
their packages, checksums and bindings byte for byte.

`PPSA99997` is a separate linked submission-import canary. Build it with
`APP_DEFINITIONS=AGC_DRIVER_CANARY=1` and `param-driver-canary.json`. It
matches ProsperoLight's library split: `sceAgcDriverSubmitDcb` is linked from
`libSceAgcDriver`, while `sceAgcSuspendPoint` is linked from `libSceAgc`.
It queries both linked addresses and never calls either function. A launch
failure in this isolated title indicates that this import profile is
unavailable; it does not affect `PPSA99998` or `PPSA99999`.

The packager rejects missing or zero stage-specific program-checksum registers,
so unresolved creation-test packages cannot accidentally be uploaded as
`vertex.bin` or `pixel.bin`.

When packages are present but checksum resolution is not yet available, the
canary emits `agc_shader_checksum_forensics_*` records. These contain the
package checksum-register value and read-only FNV-1a/CRC32 candidates computed
from the machine-code section. The probe does not modify the package; it is
intended for comparison with a package later accepted by the OpenGL runtime.
