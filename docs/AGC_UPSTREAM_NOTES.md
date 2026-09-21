# Upstream AGC sources: mkwii-ps5 and its dependencies, checked against this driver

Reader's note: this file is the record of one investigation (2026-09-21). It is stable,
not a log; the run evidence it cites lives in `docs/M5_PHASE_C.md` and the checks it
added live in `tools/`.

## What the repository is

[Phi1ow/mkwii-ps5](https://github.com/Phi1ow/mkwii-ps5) is a **static recompilation of
Mario Kart Wii PAL** (via [WiiCompiled](https://github.com/patchzyy/Wiicompiled), a
PPC-to-x86-64 static recompiler) that renders the game's **GX** command stream through
**AGC** directly. It is not an emulator and it does not use Vulkan: its 90-odd
`ps5/gpu/*.cpp` files implement GameCube/Wii graphics on the same console layer this
driver sits on, which is the whole reason it is worth reading. It credits
[ps5link-sdk](https://github.com/Rufidj/ps5link-sdk) (a linker that produces real PS5
titles, plus `examples/gpu_cube` and shader containers) and
[SharpProspero](https://github.com/SvenGDK/SharpProspero) (the C# original of the same,
and the register/tiling tables).

Two things about it are worth stating before anything else, because they bound how much
weight it can carry: **no console evidence is in the repository** (the README points at
runtime logs under `/data/PPSA99611/UserData/Logs/`, which are not committed), and
**"a clean build has not been verified"** in its own words. Its 60 FPS figure is
"reported on PS5 Pro after kstuff is paused". So it is a source of *mechanisms and
tables* to check here, not a source of measurements to import. Its code is
GPL-3.0-only, which is why the one file below that carries text from it is marked that
way (the rest of this repository is GPL-3.0-or-later).

## Verified here: the tile swizzle equations

`tools/check-tile-equations.py`, run by `tools/check-mip-layout.sh`, transcribes the
address equations from SharpProspero's `AgcTilingTables.cs` -- 45 equations, selected by
dimension (2D/3D), fragment count (1/2/4/8 samples) and element size (1/2/4/8/16 bytes)
-- and holds them against the maps this driver measured **on the console**, read out of
`driver/ps5vk_image.c` rather than restated:

| driver row | tile | equation | result |
| --- | --- | --- | --- |
| `ps5vk_tiled_1b_terms` | 256x256 | 0 | agrees |
| `ps5vk_tiled_2b_terms` | 256x128 | 1 | agrees |
| `ps5vk_tiled_4b_terms` | 128x128 | 2 | agrees |
| `ps5vk_tiled_8b_terms` | 128x64 | 3 | agrees |
| `ps5vk_tiled_16b_terms` | 64x64 | 4 | agrees |
| `ps5vk_tiled_depth2_terms` | 256x128 | 26 | agrees |
| `ps5vk_tiled_depth_offset` | 128x128 | 27 | agrees |
| `ps5vk_tiled_depth4_terms` | 64x64 | 34 | agrees |

Every measured row is the published equation, over every texel of its tile. That is the
third independent statement of the same maps: the console measurement
(`docs/HARDWARE_FINDINGS.md`, the C7 address map), the pinned **AddrLib** the oracle
builds (`tools/mip-layout-oracle.cpp`) and this table.

Two consequences. First, mkwii-ps5's own depth transcription
(`ps5/gpu/gx_depth_offset.h`, "Tiled32_4, tile mode 24, equation 27") **agrees with
equation 27 over all 16384 texels of a tile** -- it is written in a different term form,
and the check is what says the two forms are the same map. Second, the table names the
equations for the modes `driver/ps5vk_image.c` refuses because nothing here has walked
them: **two-sample colour is equation 7, four-sample colour 12, two-sample depth 30,
four-sample depth 34**. Those are candidates for a console probe, not claims: this
repository's rule is that a map the driver *uses* has been measured, and the table's
role is to say which probe is worth running and to give the GPU-shader form of the map
(which AddrLib cannot provide inside a shader).

## Corroborated: constants and disciplines this driver already has

Each of these is an independent implementation agreeing with ours, which is worth
something for a platform with no documentation:

| their code | ours | |
| --- | --- | --- |
| `desc_constant`: `0xfac \| (77 << 12)`, record 16 | `PS5VK_UNIFORM_BUFFER_FLAGS` (`ps5vk_draw.c`) | identical |
| `desc_structured`: `0x204 \| (5 << 12)` | the regular-buffer descriptor word | identical |
| `kRenderTargetOffsets[16]` starts `0x318, 0x31B, 0x31C, ...` | `ps5vk_draw.c`'s colour-target block starts `0x318`, `0x31b` | same registers |
| direct memory is granted in 16 KiB pages; a smaller alignment just fails | `ps5vk_AllocateMemory` raises the request to `PS5VK_DIRECT_PAGE_BYTES` and to `PS5VK_LARGE_ALIGNMENT` for large blocks | already done, independently |
| `sceVideoOutSetBufferAttribute2(..., tiling 0, ..., dcc 0, clear 0)` | the same call with the same zeros (`ps5vk_wsi.c`) | identical |
| `clflush` per 64 bytes + `__atomic_thread_fence(SEQ_CST)` before reading GPU-written memory | the same discipline in the readback paths | identical |
| a suspend point after every submitted operation (with two build ids and the failures each sequencing caused) | `sceAgcSuspendPoint` per submission (`ps5vk_queue.c`) | same sequencing |

## New capability the driver does not use: AGC's own resource-slot table

`AgcShader.cs` (SharpProspero) and `examples/gpu_cube/main.c` (ps5link-sdk) document the
shader handle AGC returns from `sceAgcCreateShader`:

* `handle + 8` is a pointer to the user-data layout; `+46` is `sharpResourceCount[4]`,
  one count per resource kind (read-only, read-write, sampler, constant buffer);
* `+8 + kind*8` is a pointer to that kind's `sharpResourceOffset` array, whose entries are
  `offsetInDwords:15 | small:1` -- the dword a descriptor goes at, and whether the shader
  expects a four-dword descriptor;
* `handle + 24` / `handle + 32` are the context- and shader-register arrays with their
  counts at `+91` / `+92`.

This is exactly the information this driver reconstructs from `PsbcShaderMetadata`
(`descriptor_set0_user_data_dword`, `push_constant_user_data_dword`,
`vertex_buffer_table_user_data_dword`, and the per-set array R7 added). AGC's copy is the
*consumer's* view of the same fact, and it is available after the same
`sceAgcCreateShader` call this driver already makes in `ps5vk_pipeline.c`.

That matters because of the failure mode R9 was: when the compiler and the driver
disagree about which user-data dword a resource lands in, the shader reads a register
nothing wrote and returns a **silent zero**. A cross-check at shader creation would turn
that class into a named refusal, and the `small` flag would replace this driver's
inference of descriptor width (4 dwords for buffers, 8 for textures) with what the shader
actually declared.

## Ranked work this opens (each with its gate)

1. **Read the AGC shader handle and check it against the metadata.** Console probe first:
   log the handle's per-kind counts, offsets and the `small` flags beside
   `PsbcShaderMetadata` for the probe shaders, and compare. If they agree, the driver
   gains a by-name refusal for disagreement (and the metadata keeps its place as the
   compile-time record). This is the item with the largest payoff per unit of risk: it
   closes the silent-zero class rather than one instance of it. Gate: the probe plus a
   host test over recorded handle bytes.
2. **Two-sample and four-sample placements from the table.** Vulkan 1.0 allows
   `VK_SAMPLE_COUNT_2_BIT`, and this driver refuses the paths whose placement no probe has
   walked. Equations 7 / 12 / 30 / 34 are the candidates; each still needs its own
   console run, because the rule here is measured, not transcribed.
3. **The slice and z terms (coordinates 2 and 3) in the same table.** These are the
   equations' only terms this driver has no counterpart for, and the candidate explanation
   for the "array layers are D1" refusals in `ps5vk_image.c` (copy, resolve, readback and
   clear all refuse more than one layer from a non-zero layer). A probe would decide
   whether an array slice is a block-grid offset (what the driver assumes) or an XOR term
   inside the tile (what the table's 3D rows suggest).
4. **`sceAgcAcbDmaData` for in-command-buffer copies.** Both repositories resolve it at
   runtime (`libSceAgc.sprx`; `ps5/gpu/agc_blit.cpp` uses it for GPU-side data moves),
   and this driver does not use it: its copies are CPU work at a submission split, which
   is correct and costs a split. Deferred until a case is slow because of the splits;
   named here so it is not rediscovered.
5. **Driver-internal GPU shaders.** ps5link-sdk's `shaders/` turns AMD GPU assembly into
   AGC shader containers (`build.sh`, `tools/agcpack.py`), which is how a *driver* can
   ship its own blit/copy/resolve programs rather than borrowing Mesa's `vk_meta`
   pipelines. That is the route to operations the CPU path cannot do at all (a scaled or
   filtered blit at speed). Large, and not needed for correctness today.

## What does not transfer

* **The GX semantics.** Their engine never meets array-layer subsets, `vkCmdBlitImage`
  scaling rules, or a hardware MSAA resolve -- GX has none of them -- so its solutions
  are for problems this driver does not have, and its gaps are not evidence about ours.
* **Its placement modes.** The modes it uses are the ones this driver already has; the
  value was the *table*, not the modes.
* **Its numbers.** No console logs, no captures, an unverified clean build, and a frame
  rate reported for a paused-overlay configuration. Nothing here was imported as
  evidence.
* **Its code.** GPL-3.0-only, and mostly GX-shaped. What this repository took is the
  equation table, with attribution, in one clearly marked file
  (`tools/check-tile-equations.py`); everything else above is a mechanism to re-derive
  here and prove on the console.
