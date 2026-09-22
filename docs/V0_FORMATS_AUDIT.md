# V0-formats: the required format tables

Phase V0-formats (`docs/M5_REFERENCE.md`). A Vulkan 1.0 device must support a
set of formats with a set of features whether or not any extension is enabled
and whether or not the application asks for anything: `formats-v1.4.354.adoc`,
"Required Format Support". This file is that audit -- what the specification
requires, what this driver reports, and a reason and a closing path for every
gap. It is the record the step's acceptance names: "every required format must
either carry the correct feature bits or be recorded as a deliberate gap with
its tile-layout reason".

Reproduce it with:

```bash
python3 tools/format_audit.py          # the per-format list
python3 tools/format_audit.py --check   # exit 1 while a feature or a must: clause is unmet
make test                               # the counts and the row list below are gated there
```

The script reads the vendored specification
(`.deps/native/vulkan-docs/formats-v1.4.354.adoc`), reads the table
`driver/ps5vk_image.c` reports through `vkGetPhysicalDeviceFormatProperties`,
and prints every required feature no entry carries. `{sym1}` cells are required
outright; `{sym2}` and `{sym3}` are required on at least some of the named
formats or with caveats, and are listed as conditional so a conditional row is
never mistaken for a closed one.

## What the audit says today

| | Count |
| --- | --- |
| Formats the specification requires | 179 |
| Formats the driver reports | 58 |
| Required formats missing a required feature | 0 |
| Required formats with conditional requirements only | 55 |
| `must:` clauses no reported format satisfies | 0 |
| Formats a set-level `must:` clause names | 0 |

The specification states two kinds of requirement and the table counts them
separately. A cell's own `{sym1}` is one format's: what it misses is listed per
format. A table's **footnote** can instead require a feature of a *set* of rows --
"`DEPTH_STENCIL_ATTACHMENT_BIT` feature must: be supported for at least one of
`D24_UNORM_S8_UINT` and `D32_SFLOAT_S8_UINT`" -- and that is satisfied when *any*
named row reports the feature, so no single row's cell says whether it holds.
The audit reads those clauses and evaluates them against the driver's whole
table. R1's report (a PS5 vkQuake port's request, `PS5_VULKAN_REQUESTS.md`) is
what found the blind spot: the script filed every `{sym2}` cell as conditional
without consulting the driver at all, so this clause was invisible to `--check`
while the driver violated it (docs/BLOCKERS.md). **Round 12 closed it**: the
runner's `v0-stencil` case rendered through an image of `D32_SFLOAT_S8_UINT` with
both planes in use -- a pass that stores the stencil reference and a
`gl_FragDepth`, and a pass that compares the plane and the depth -- so one of the
two named formats reports the feature and the clause is satisfied by a console
probe (console run pid 192, `Klog_Logs/v0-stencil-run5.log`; golden
`golden/v0-stencil`, host gate `driver/tests/vk_c5_stencil_test.c`).

The driver reports exactly what the probes have proved on hardware:

- `VK_FORMAT_R8G8B8A8_UNORM` -- sampled, linear filtering, transfer, colour
  attachment (M2, M3 step 3, M4)
- the six round 6 added -- `R8_SNORM`, `R8G8_SNORM` (new entries, descriptor
  words 2 and 15), `B8G8R8A8_UNORM` (`8_8_8_8_UNORM` with the ZYXW selectors)
  and `A8B8G8R8_UINT_PACK32` (`8_8_8_8_UINT`, RGBA), which the sampled and blit
  cases now cover whole (console run pid 296, `Klog_Logs/v0-snorm-run4.log`)
- the fifteen other sampled formats of V0-formats -- `A8B8G8R8_UNORM_PACK32`,
  the `R8G8B8A8_SRGB`, `A8B8G8R8_SNORM_PACK32` and `R8G8B8A8_SNORM` forms,
  `R8_UNORM`, `R8G8_UNORM`, `R16_UNORM`, `R16G16_UNORM`,
  `R16G16B16A16_UNORM`, `R16_SFLOAT`, `R16G16_SFLOAT`, `R16G16B16A16_SFLOAT`,
  `R32_SFLOAT`, `R32G32_SFLOAT`, `R32G32B32A32_SFLOAT` -- sampled, linear
  filtering, the maintenance1 transfer bits, `BLIT_SRC`, and (round 5, console
  run pid 290, `Klog_Logs/c7-blit-dst-run3.log`) `BLIT_DST`, each with the
  colour its texel decodes to read back on the console (console run pid 163, `golden/v0-formats-sampled` and `golden/c7-blit-formats`); the
  descriptor's format word is the register database's `GFX10_FORMAT_*` value
  and its DST_SEL word the fill-in rule for the channels the format does not
  have, or the reversed `W, Z, Y, X` order for the `A8B8G8R8_*` forms
  (`docs/HARDWARE_FINDINGS.md`)
- `VK_FORMAT_B8G8R8A8_UNORM` -- colour attachment (the VideoOut framebuffers
  and the swapchain, M2, C1)
- `VK_FORMAT_D32_SFLOAT` -- depth/stencil attachment (M4 step 1, C5) and the
  transfer source and destination bits, which a depth image needs before it
  can be created for a copy: `c8-depth4-copy` clears two four-sample depth
  images, copies one into the other with `vkCmdCopyImage` and finds the
  source's word across the image's eight tiles (console run pid 244,
  `Klog_Logs/format-items-run5.log`, docs/HARDWARE_FINDINGS.md); the buffer
  forms and a shape with no measured map still refuse by name
- `VK_FORMAT_R32G32_SFLOAT`, `VK_FORMAT_R32G32B32_SFLOAT`,
  `VK_FORMAT_R32G32B32A32_SFLOAT`, `VK_FORMAT_R32G32B32A32_UINT`,
  `VK_FORMAT_R32G32B32_SINT` and `VK_FORMAT_R32G32B32_UINT` -- vertex buffers
  (M3, M4, Mesa's meta rectangles in C1b, and the two three-component integer
  positions of V0-formats: the runner's `v0-vertex-sint` and `v0-vertex-uint`
  drew the m3-vertex square with them, all three components exact, the third
  read back as the fragment's alpha, console run pid 146)
- the ten signed integer formats of V0-formats -- `R8_SINT`, `R8G8_SINT`,
  `R8G8B8A8_SINT`, `A8B8G8R8_SINT_PACK32`, `R16_SINT`, `R16G16_SINT`,
  `R16G16B16A16_SINT`, `R32_SINT`, `R32G32_SINT` and `R32G32B32A32_SINT` --
  sampled through an `isampler`, each with the value its texel holds read back
  on the console (console run pid 145, `golden/v0-formats-sampled-sint`); the
  descriptor's word is the register database's SINT form of the same field
  widths and the same selectors the unsigned entries use, and the two-channel
  pair's word was fitted a candidate a battery (`8_8_SINT` 19 with `RG01`; the
  SNORM word 15 was refuted) (`docs/HARDWARE_FINDINGS.md`)
- the integer and half-float colour targets of V0-formats (console run pid 205,
  `golden/v0-targets`): the unsigned five -- `R8G8B8A8_UINT`,
  `A8B8G8R8_UINT_PACK32`, `A2B10G10R10_UINT_PACK32`, `R16G16_UINT` and
  `R32_UINT` -- drawn with a constant `uvec4` and read back, each holding the
  integer its own storage order and width carries, with the CB words Mesa's
  `ac_get_cb_format`/`ac_get_cb_number_type`/`ac_translate_colorswap` pick and
  the exports its `ac_choose_spi_color_formats` picks (`UINT16_ABGR`, or `32_R`
  for the single-channel 32-bit one); and `R16G16_SFLOAT`, whose half-float
  target renders and blends with `FP16_ABGR`. The signed four wait on a compiler
  crash (`docs/HARDWARE_FINDINGS.md`)
- the colour targets of V0-formats (console run pid 186, `golden/v0-targets`):
  `R8G8B8A8_UNORM` and `B8G8R8A8_UNORM` re-proved by rendering, and
  `A8B8G8R8_UNORM_PACK32`, `R8G8B8A8_SRGB` and `A2B10G10R10_UNORM_PACK32` --
  which report `COLOR_ATTACHMENT` from this run -- each drawn over the whole
  3840x2160 target and read back through the tiled word map. Two frames each:
  a solid one, and one that clears the target to a destination colour and adds
  a source over it with `CB_BLEND0_CONTROL`'s additive state (`ONE`, `ONE`,
  `ADD`) and the pixel shader exported as FP16_ABGR, which is what Mesa's
  `ac_choose_spi_color_formats` picks for these formats -- so all five report
  `COLOR_ATTACHMENT_BLEND` too. The `CB_COLOR0_INFO` word is the data format,
  number type and component order ps5-opengl's `sceGnmCreateRenderTarget` writes
  and Mesa's `ac_get_cb_format` / `ac_get_cb_number_type` /
  `ac_translate_colorswap` pick: `8_8_8_8` with `SWAP_STD`, `SWAP_ALT`
  (`B8G8R8A8`'s byte order) or `SWAP_STD_REV` (`A8B8G8R8`), the sRGB number type
  for `R8G8B8A8_SRGB`, and `2_10_10_10` for `A2B10G10R10_UNORM_PACK32`
  (`docs/HARDWARE_FINDINGS.md`)

`VK_FORMAT_A8B8G8R8_SRGB_PACK32` **is** reported now, and how it became possible
is the round-17 mechanism: the console linearises the first three *fetched*
components, so a texel stored the way Vulkan describes this format -- A, B, G, R
-- could never have its red linearised by any selector. The driver therefore
stores the format's texels in the R, G, B, A order its `R8G8B8A8_SRGB` twin has
and swaps the four bytes at every boundary where an application's bytes meet the
image's (`ps5vk_format.storage_reversed`): uploads, readbacks and both blit
directions. The image's layout is the driver's own -- every image is created with
`VK_IMAGE_TILING_OPTIMAL` -- so no application observes the order, and the fetch
is the twin's own path: the `8_8_8_8_SRGB` word with the straight RGBA selectors
(`docs/HARDWARE_FINDINGS.md`).

`BLIT_DST` is no longer what keeps the sampled formats in the list: round 5's
`v0-blit-dst` blits the one source colour into a linear image of **39 formats**,
reads each destination back raw, and finds the bytes the format's own decode
names read backwards -- `ps5vk_rgba8_to_texel` is `ps5vk_texel_to_rgba8`'s
inverse, so a blit into a format and the sampler's fetch of it agree (console
run pid 290, `Klog_Logs/c7-blit-dst-run3.log`). The 34 rows that were missing
only `BLIT_DST` report it now, and `VK_FORMAT_R8G8B8A8_SRGB` left the list
entirely. Round 6 closed the fetch rows that were left: the one- and two-channel SNORM
forms got entries (descriptor words 2 and 15 from the register database), and
the two rows the table had never claimed -- `B8G8R8A8_UNORM`, whose sampler word
is `8_8_8_8_UNORM` with the ZYXW selectors Mesa's own `radv_compose_swizzle`
picks for that byte order, and `A8B8G8R8_UINT_PACK32`, `8_8_8_8_UINT` with the
straight RGBA ones -- report their sampled and linear-filtered fetch, the blit
case reads them back (console run pid 296, `Klog_Logs/v0-snorm-run4.log`: 25 of
25 sampled frames, 52 of 52 blit frames, 53 of 53 formats as the audit records
them). Round 7 gave `R16G16B16A16_UINT` the entry it never had -- the register
database's `GFX10_FORMAT_16_16_16_16_UINT` (69) with the RGBA selectors, so its
typed fetch, its blit source and its blit destination are proved by the same
three cases as every other row (console runs pid 320, `Klog_Logs/v0-u16-run3.log`,
and pid 327, `Klog_Logs/v0-u16-blit-run1.log`: 53 of 53 blit frames, 54 of 54
formats as the audit records them). Round 8 proved the **eight-byte tiled colour map** and
`R16G16B16A16_UINT`'s attachment with it: the driver has AddrLib's own row for
eight-byte elements (`ps5vk_tiled_8b_terms`, which
`tools/check-mip-layout.sh` now compares with the oracle), and the integer
target case draws that format and reads its two words at every one of the
target's 8294400 texels (console run pid 338,
`Klog_Logs/v0-wide-target-run2.log`). Round 15 closed the depth attachment:
`D16_UNORM`'s `DEPTH_STENCIL_ATTACHMENT`. The M4 canary's own frame -- the same
geometry, shaders and clear of 1.0 -- renders through a D16 depth attachment,
whose `DB_Z_INFO` word is `Z_16` (1) where D32F's measured word carries 3, and
the depth reads back through the two-byte element's own map: all 8294400 samples
hold the depth the geometry and the clear wrote (console run pid 129,
`Klog_Logs/v0-d16-run2.log`), with the console's own conversion rounding 0.75 to
0xbfff (0.75 x 65535 is 49151.25), which the first run of the round read as a
mismatch. Round 16 went after the sixteen-byte pair and met a **first-compile**
fault instead of the mismatch: a purpose-built wide probe's case, holding
`R32G32B32A32_UINT` alone, aborted inside ACO at its first pipeline compile (pid
135, `Klog_Logs/v0-wide16-run3.log`), and moving the rows to the **front** of the
two family tables aborted the same way before a frame was logged (pid 139,
`Klog_Logs/v0-wide16-run4.log`; the backtrace resolves to `aco::lower_branches` ->
`aco::lower_to_hw_instr` -> `aco::reindex_ssa`). Round 9's measurement is the
counter-reading that keeps the rows **reachable**: drawn **last** in the same two
cases, they rendered -- 8263680 of 8294400 texels holding their words, 99.63 per
cent, the first mismatch at (64, 2128) holding 0x0 (pid 109,
`Klog_Logs/v0-wide2-run1.log`) -- so the rows keep round 9's place at the end of
`kTargetFormats`/`kUnsignedTargets`, where the next battery draws them mid-case.
**Round 17 found what the aborts behind the last rows actually were, and it was
not the compiler.** `ps5vk_CreateImage` asserted on an unsupported combination,
and an assert's abort backtrace walks whatever ACO frames the stack still held,
so a case whose format the driver's table did not carry read as a compiler fault.
The proof is the call in flight: every one of those runs' last probe is
`b7_create_device`, and the next call a frame makes is `b7_create_image` --
round 4's signed case, round 13's depth cases (whose first row, `D16_UNORM`, had
no entry in the driver's table at all) and round 16's sixteen-byte runs alike
(`Klog_Logs/v0-targets-sint-run2.log`, `-run3.log`, `v0-depth-run1..3.log`,
`v0-wide16-run2..4.log`). The driver now **refuses by name**
(`VK_ERROR_FORMAT_NOT_SUPPORTED`) instead of asserting
(docs/HARDWARE_FINDINGS.md, "An unsupported image is refused, not asserted"), and
the split has no compiler-fault class left: what earlier rounds quoted against it
is reachable work that a claim, a console run and a keep-or-revert decides.
**Every reachable row left is one the case tables must claim before their
targets can be created**: the ten signed attachment rows, the two sixteen-byte
rows, and `D16_UNORM`'s and `D32_SFLOAT`'s `SAMPLED_IMAGE` and `BLIT_SRC`.

Round 18 claimed those words and ran the batteries. **Nine of the ten signed
rows passed**: `v0-targets-sint` drew `R8_SINT`, `R8G8_SINT`, `R8G8B8A8_SINT`,
`A8B8G8R8_SINT_PACK32`, `R16_SINT`, `R16G16_SINT`, `R16G16B16A16_SINT`,
`R32_SINT` and `R32G32_SINT` and read every one of their 8294400 texels back as
the `ivec4(0x20, 0x40, 0x60, 0x7f)` word their storage carries -- `9 of 10
integer colour targets` with the tenth the sixteen-byte row (console run pid
110, `Klog_Logs/v0-sint-run1.log`) -- so those nine rows' `COLOR_ATTACHMENT` is
proved and their rows left the reachable list. **The three sixteen-byte rows did
not**: `R32G32B32A32_UINT`, `_SINT` and `_SFLOAT` each read back 8263680 of
8294400 texels (99.63 per cent) with the *same* mismatch box -- x 64..3839,
y 2128..2143, the sixteen rows that begin sixteen texels into the final 64-texel
tile row -- so the sixteen-byte tiled map is one bug and not three (pids 110 and
111, `Klog_Logs/v0-sint-run1.log`, `Klog_Logs/v0-wide16-run5.log`). Their claims
are out again, they stay reachable work, and the driver's new refusal names them
in the log -- `format 108: type 1, tiling 0, usage 0x10 ... is not a supported
image` -- where the old assert killed the title
(`Klog_Logs/v0-sint-run2.log`). Round 12 added the **packed 16-bit** targets and
`R16_UINT`: `R5G6B5_UNORM_PACK16`
and `A1R5G5B5_UNORM_PACK16` render and blend (their five- and six-bit channels
exact in the storage) and `R16_UINT` renders, every one of each target's 8294400
texels holding its texel's word (console run pid 118,
`Klog_Logs/v0-packed-run1.log`: 10 of 10 integer targets, 26 of 26 colour
targets, 54 of 54 formats). **The blend family is closed with them**: the only
rows still missing `COLOR_ATTACHMENT_BLEND` are the two the hardware's sRGB
fetch order parks. Round 11 added the **narrow** targets: `R8_UNORM`, `R8G8_UNORM` and
`R16_SFLOAT` render with blending, and `R8_UINT` and `R8G8_UINT` without, in all
8294400 texels of their one- and two-byte targets (console run pid 117,
`Klog_Logs/v0-narrow-run5.log`: 9 of 9 integer targets, 22 of 22 colour targets,
54 of 54 formats as the audit records them). The one-byte element's tiled map is
AddrLib's own row, which the driver had been getting wrong by using the
four-byte one. Round 10 added the eight-byte **two-channel** targets: `R32G32_UINT` and
`R32G32_SFLOAT` render their two 32-bit channels through the 32_32 data format
(a four-slot export, because 32_AR left the second channel untouched), and all
8294400 texels of each hold the pair (console run pid 112,
`Klog_Logs/v0-wide3-run3.log`: 7 of 7 integer targets, 16 of 16 colour targets,
54 of 54 formats as the audit records them). Round 9 added the eight-byte
**half-float** target: its
`CB_COLOR0_INFO` word (16_16_16_16 with the FLOAT number type and Mesa's
FP16_ABGR export) renders the exact quarters 0.25, 0.5, 0.75 and 1.0 into all
8294400 texels of an eight-byte target, and its blend to 2.0 in alpha (console
run pid 109, `Klog_Logs/v0-wide2-run2.log`: 14 of 14 colour targets, 54 of 54
formats as the audit records them). What keeps the rows below open is narrower
still: a colour-attachment word (and, for a texel that is not four or eight
bytes, a map for it -- the sixteen-byte one is AddrLib's next row and its target
matched only 99.63% of its texels when a probe drew it, round 9's measured
piece), a texel buffer, the depth sampler for `D16_UNORM`/`D32_SFLOAT`. The formats V0-formats and C7 added moved the
audit from 7 reported formats to 20, and the two integer vertex formats of the
vertex-fetch probe are
**closed rows**: they are reported with `VERTEX_BUFFER` alone and no longer
appear below.

## The gaps, and why each one is one

Every gap is a claim that is not true yet, so the driver reports the feature
off rather than guessing. The families, with what closes each:

| Missing features | Formats | Why the bit is off | What closes it |
| --- | --- | --- | --- |
| `BLIT_DST`, `BLIT_SRC`, `SAMPLED_IMAGE`, `SAMPLED_IMAGE_FILTER_LINEAR`, `COLOR_ATTACHMENT`, `COLOR_ATTACHMENT_BLEND` | `A8B8G8R8_SRGB_PACK32` | **closed** by round 17: the console applies the sRGB curve to the first three *fetched* components, and this format's Vulkan layout puts red last, where the curve cannot reach it. The driver stores its texels in the R, G, B, A order its `R8G8B8A8_SRGB` twin has (`ps5vk_format.storage_reversed`) and swaps the four bytes at every boundary where an application's bytes meet the image's -- the uploads, the readbacks and the two blit directions -- so the fetch, the blits, the transfers and the attachment pair are that twin's own paths | nothing: all six features are proved, one console frame each (round 17, pid 235) |
| `COLOR_ATTACHMENT`, `COLOR_ATTACHMENT_BLEND` | the `R16_*`/`R16G16_*`/`R16G16B16A16_*` families, `R32_SFLOAT`, `R32G32_SFLOAT`, `R32G32B32A32_SFLOAT`, the packed 16-bit forms and the integer families | a colour target needs the AGC `CB_COLOR0_INFO` format word **and** the pixel shader compiled for that export format (M4 step 2's rule); the four-byte UNORM, sRGB and ten-bit forms are recorded now, with blending, and the single-channel 32-bit float target R32_SFLOAT is recorded too -- the CTS requires its colour-attachment bit (`dEQP-VK.api.info.format_properties.r32_sfloat`) and the console proved the 32_R export encodes 0.75 as 0x3F400000 (runs/v0-target-float, docs/M5_PHASE_C.md round 7); a format whose texel is not four bytes needs its own tiled map | the format word from ps5-opengl, the export format from Mesa's `ac_choose_spi_color_formats`, then the M4 blend canary's readback per format |
| `DEPTH_STENCIL_ATTACHMENT` | `D24_UNORM_S8_UINT` and `D32_SFLOAT_S8_UINT`, named by the table's second `must:` clause, which requires the feature of **at least one** of them | the clause needs a stencil path: every depth draw programs `DB_STENCIL_INFO` disabled (`0x20000180`), no stencil state and no stencil clear is programmed, and neither stencil format has a `DB_Z_INFO`/`DB_STENCIL_INFO` word recorded | a runner probe of the DB stencil registers and the two formats' words -- ps5-opengl 0.3.0's hardware-run `append_depth_target_state` is the offset reference -- then the C5-style readback per format; `D16_UNORM`, the rest of this row, is closed (round 15) |
| `UNIFORM_TEXEL_BUFFER` | the thirty-eight rows that wanted it | **closed** by blocker round 5: `PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER` gives the compiler the binding, and the driver writes the V# a `texelFetch` on a `samplerBuffer` reads (docs/BLOCKERS.md, the descriptor types) | nothing: every row that wanted the bit reports it, one console frame a format |
| `STORAGE_TEXEL_BUFFER` | the twenty rows that wanted it | **closed** by blocker round 6: `PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER` gives the compiler the binding and the driver writes the same V# the uniform fetch reads, which an `imageStore` on an `imageBuffer` writes through (docs/BLOCKERS.md, the descriptor types) | nothing: every row that wanted the bit reports it, one console frame a format, its texels read back out of the buffer's own memory |
| `STORAGE_IMAGE` | the sixteen rows that wanted it | **closed** by blocker round 7: the compiler names the type, the driver writes its 32-byte entry as the sampled image descriptor without the sampler, and an `imageStore` on a `writeonly image2D` writes through it (docs/BLOCKERS.md, the descriptor types) | nothing: every row that wanted the bit reports it, one console frame a format, the image's own memory read back |
| `STORAGE_IMAGE_ATOMIC` | `R32_SINT`, `R32_UINT` | **closed** by blocker round 9: the compiler lowers `imageAtomicAdd` to `image_atomic_add ... storage:image` against the 32-byte entry round 7 proved, so the probe needed nothing but the claim | nothing: both rows report the bit, one console frame a format, the counts read out of the image's own memory |

## The audit's list, row by row

The table above groups the gaps by what closes them; this is the audit's own
list, one row per format, so the step's acceptance -- "every required format must
either carry the correct feature bits or be recorded as a deliberate gap with its
tile-layout reason" -- is a check rather than a reading. `tools/format_audit.py`
prints it and `tests/test_tools.py` requires the two to agree, so a driver change
that closes or opens a gap moves this table with it.

| Format | Features no entry carries |
| (none: every required format carries every feature the specification requires of it) | -- |

The audit prints the depth/stencil table's second `must:` clause as its own
section while it is unmet, and it is the only requirement whose subject is a
**set** rather than a row: it is unmet while *neither* of the two formats reports
the feature, and met as soon as one does. Round 12 met it with
`VK_FORMAT_D32_SFLOAT_S8_UINT`, so the section is empty now -- and the two rows
below moved from it into the reported set, each with
`VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT` and nothing else (`v0-stencil`;
`D24_UNORM_S8_UINT` is reported too, because its entry is the same path with
`DB_Z_INFO`'s `Z_24` where D32F's is `Z_32_FLOAT`). The probe is the runner's
`v0-stencil` case, four frames and one signal each, whose console run is
`Klog_Logs/v0-stencil-run5.log` (pid 192, `golden/v0-stencil`):

| Format | The clause's feature | Where the row stands |
| `VK_FORMAT_D32_SFLOAT_S8_UINT` | `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT` | reported: the console probe below proved it, and it is what satisfies the clause |
| `VK_FORMAT_D24_UNORM_S8_UINT` | `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT` | reported the same way: its `DB_Z_INFO` word is `Z_24` where D32F's is `Z_32_FLOAT`, which is one field of the same probe |

## What is parked, and on what

Two of the inputs this step needs are not the driver's to add.
`.deps/native/psbc/include/psbc_compile.h` lists its descriptor types and its
vertex formats in full:

```
typedef enum {
    PSBC_DESCRIPTOR_NONE = 0,
    PSBC_DESCRIPTOR_UNIFORM_BUFFER,
    PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,
    PSBC_DESCRIPTOR_STORAGE_BUFFER,
} PsbcDescriptorType;

typedef enum {
    PSBC_VERTEX_FORMAT_NONE = 0,
    PSBC_VERTEX_FORMAT_R32_FLOAT,
    PSBC_VERTEX_FORMAT_R32G32_FLOAT,
    PSBC_VERTEX_FORMAT_R32G32B32_FLOAT,
    PSBC_VERTEX_FORMAT_R32G32B32A32_FLOAT,
    PSBC_VERTEX_FORMAT_B8G8R8A8_UNORM,
    PSBC_VERTEX_FORMAT_R10G10B10A2_UNORM,
    PSBC_VERTEX_FORMAT_B10G10R10A2_UNORM,
    PSBC_VERTEX_FORMAT_R10G10B10A2_SNORM,
    PSBC_VERTEX_FORMAT_B10G10R10A2_SNORM,
    PSBC_VERTEX_FORMAT_R10G10B10A2_USCALED,
    PSBC_VERTEX_FORMAT_B10G10R10A2_USCALED,
    PSBC_VERTEX_FORMAT_R10G10B10A2_SSCALED,
    PSBC_VERTEX_FORMAT_B10G10R10A2_SSCALED,
    PSBC_VERTEX_FORMAT_R32_SINT,
    PSBC_VERTEX_FORMAT_R32G32_SINT,
    PSBC_VERTEX_FORMAT_R32G32B32_SINT,
    PSBC_VERTEX_FORMAT_R32G32B32A32_SINT,
    PSBC_VERTEX_FORMAT_R32_UINT,
    PSBC_VERTEX_FORMAT_R32G32_UINT,
    PSBC_VERTEX_FORMAT_R32G32B32_UINT,
    PSBC_VERTEX_FORMAT_R32G32B32A32_UINT,
    PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM,
} PsbcVertexFormat;
```

A third category is the hardware's own: an sRGB fetch linearises the **first three
fetched components**, before any `DST_SEL` selector, so a byte-reversed sRGB
format has the curve applied to the wrong three bytes and no selector can move
it. `VK_FORMAT_A8B8G8R8_SRGB_PACK32` and `VK_FORMAT_B8G8R8A8_SRGB` are that
case, measured on the console (docs/HARDWARE_FINDINGS.md, "An sRGB fetch
linearises the first three fetched components, not the channels", console run
pid 160): their twelve missing features are not reachable by any probe or
driver word, only by a shader-side swizzle the application would have to write.

Blocker round 4 gave libpsbc the **texel-buffer descriptor types** and the
driver their 16-byte entry and hardware word; round 5 proved the uniform half of
it -- a `samplerBuffer` fetch reads the buffer view's texels through the V#
`driver/ps5vk_draw.c` writes -- and round 6 the storage half, an `imageStore` on
an `imageBuffer` writing through the same V#, one console frame a format each, so
every row that wanted `VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT` or
`VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT` reports it now (docs/BLOCKERS.md). No
descriptor path names a **storage image** yet, and the vertex
enum's twenty-two original values are all **32-bit-component or packed**
formats:
`R32` float, signed and unsigned with one to four components, the 8888
`B8G8R8A8`/`R8G8B8A8` UNORM layouts, and the eight 10-10-10-2 forms
(`R10G10B10A2_UNORM`/`_SNORM`/`_USCALED`/`_SSCALED` and their
`B10G10R10A2` twins). ps5-opengl's own `ps5_vertex_format`
(`src/gallium/ps5/ps5_screen.c`) maps exactly those and nothing else, which is
the independent reading of the same limit.

So a vertex attribute of **any** 8-bit or 16-bit component -- unorm, snorm,
float or integer -- and the byte-reversed 8888 SNORM and integer layouts had no
format word to build at all, however much a probe might show about their texels;
blocker rounds 1 to 3 gave the compiler's enum those layouts and the hardware
fetched every one. The nine vertex rows that *are* expressible --
`R32_UINT`, `R32_SINT`, `R32G32_UINT`, `R32G32_SINT`, `R32G32B32A32_SINT`,
`R8G8B8A8_UNORM`, `A8B8G8R8_UNORM_PACK32`, `B8G8R8A8_UNORM` and
`A2B10G10R10_UNORM_PACK32` -- were reachable work and the descriptor word their
own enum value builds is now proved for them too.

The 25 `TRANSFER_SRC` features these rows carried are **closed**: every format that
was missing only that bit now reports it, and `v0-transfer-formats` round-trips
each one through a linear image of its own -- a staging buffer's bytes in
through `vkCmdCopyBufferToImage`, out through `vkCmdCopyImageToBuffer`, every
byte equal, the padded rows the driver's rule makes of them, and the bit itself
asserted (console run pid 266, `Klog_Logs/v0-transfer-formats-run1.log`). Two
rows left the table entirely with it: `VK_FORMAT_B4G4R4A4_UNORM_PACK16` and
`VK_FORMAT_E5B9G9R9_UFLOAT_PACK32` had nothing else missing.

The 34 `BLIT_DST` features these rows carried are **closed**: the resampler's
write path is `ps5vk_rgba8_to_texel`, one encode a format, and `v0-blit-dst`
blits a single `R8G8B8A8_UNORM` source into a linear image of 39 formats, reads
each destination back with `vkCmdCopyImageToBuffer` and compares the bytes
against the encode's own rule -- 39 rows, 39 distinct formats, every one
matching (console run pid 290, `Klog_Logs/c7-blit-dst-run3.log`). One row left
the table entirely with it: `VK_FORMAT_R8G8B8A8_SRGB` had nothing else missing.
The three rows that still want the bit are the hardware-blocked sRGB pair and
`R16G16B16A16_UINT`, which has no entry to write a texel shape from.

The 38 `UNIFORM_TEXEL_BUFFER` features these rows carried are **closed**:

R32_SFLOAT is the row CTS round 8 added: the CTS requires the bit for it
(`dEQP-VK.api.info.format_properties.r32_sfloat`) and the console confirmed the
fetch -- 17 of 17 uniform texel buffers in `v0-formats-texel-buffer`, pid 109,
title digest `c1f75ff6...`
blocker round 5 gave the compiler the descriptor type
(`tooling/psbc/patch-descriptor-types.py`, `PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER`
with its 16-byte entry) and the driver the V# a `texelFetch` on a `samplerBuffer`
reads, and `v0-formats-texel-buffer` (with its `-uint` and `-sint` twins) fetches
four texels, one per quarter of the target, out of a buffer view of each format:
every quarter of every row must hold its own texel's level, so a fetch that
ignores the index, reads the wrong stride or runs off the view's end shows as a
failed quarter. The V# is the one RADV builds for GFX10 -- the format entry's
`DST_SEL` and format word, `RESOURCE_LEVEL` and no `ADD_TID_ENABLE`, which is what
round 4's groundwork had wrong -- and the draw's own table accounting had to count
the texel-buffer entry before any of its type checks could skip it
(docs/M5_PHASE_C.md, blocker round 5). Console run pid 130,
`Klog_Logs/v0-texel-buffer-run3.log`: 16 of 16, 11 of 11 and 10 of 10 rows, every
one 38400 of 38400 checked pixels, `v0-formats` 54 of 54 after the mirror moved
and `m2-solid`, 1411 PASS records and no FAIL. Eighteen rows left the table
entirely with it.

The 20 `STORAGE_TEXEL_BUFFER` features these rows carried are **closed**:

R32_SFLOAT is the same round's storage half: 8 of 8 storage texel buffers in
`v0-formats-texel-buffer-store`, same run
blocker round 6 gave the compiler the storage descriptor type
(`PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER`, the same 16-byte entry) and the driver
the same V# the uniform fetch reads, which an `imageStore` on an `imageBuffer`
writes through. `v0-formats-texel-buffer-store` (with its `-uint` and `-sint`
twins) stores one level per quarter of the target into the texel that quarter
names and then reads the buffer's own memory back: every texel must hold its
level in the format's own encoding -- the same bytes the fetch probe's rows
upload -- so a store that ignores the index, writes the wrong stride or runs off
the view's end leaves a texel at the zero it started from. Four rows left the
table entirely with it. Console run pid 135,
`Klog_Logs/v0-texel-buffer-store-run3.log`: 7 of 7, 6 of 6 and 6 of 6 rows, every
one four of four texels byte-exact, `v0-formats` 54 of 54 after the mirror moved
and `m2-solid`, 818 PASS records and no FAIL. Two rows of the first two runs
measured the float classes' rounding: a decimal literal one digit short of
level/255 lands one ULP lower in a 32-bit float texel, and a division done in the
shader is the compiler's to turn into a reciprocal multiply, which lands one ULP
higher for 96 and 127 -- the shader carries the correctly rounded literals now
(docs/M5_PHASE_C.md, blocker round 6).

The 16 `STORAGE_IMAGE` features these rows carried are **closed**: blocker
round 7 gave the compiler the descriptor type
(`PSBC_DESCRIPTOR_STORAGE_IMAGE`, a 32-byte entry) and the driver its writer --
the sampled image descriptor's first eight words, the sampler's three left out,
which is the shape ps5-opengl 0.3.0's console-validated
`ps5_storage_image_view_descriptor` builds. `v0-formats-storage-image` (with its
`-uint` and `-sint` twins) stores one level per quarter of the target into texel
(quarter, 0) and the next quarter's into texel (quarter, 1) of a 64x2 row-layout
image, then reads the image's own memory back: every texel must hold its level in
the format's own encoding, so a descriptor whose row pitch, format word or
`DST_SEL` selectors are wrong puts a store in the wrong texel, row or channel.
Fourteen rows left the table entirely with it, and the audit's missing list is
down to the two sRGB rows the hardware blocks and the two atomics rows. Console
run pid 161, `Klog_Logs/v0-storage-image-run2.log`: 6 of 6, 5 of 5 and 5 of 5 rows,
every one eight of eight texels byte-exact over the image's two rows, `v0-formats`
54 of 54 after the mirror moved and `m2-solid`, 713 PASS records and no FAIL. The
first run measured two expectation bugs of the case's own, both fixed: its
readback used a fixed 256-byte row for formats whose padded row is wider (512 for
an eight-byte texel, 1024 for a sixteen-byte one), and the float shader computed
`level/255` with a division, which the compiler turns into a reciprocal multiply
that lands one ULP high for 96 and 127 (docs/M5_PHASE_C.md, blocker round 7).

The 2 `STORAGE_IMAGE_ATOMIC` features these rows carried are **closed**: blocker
round 9 found that the compiler already lowers `imageAtomicAdd` to
`image_atomic_add ... storage:image` against the 32-byte storage image entry
round 7 proved, so the round needed no compiler or driver change -- only the
probe and the claim. `v0-formats-storage-image-atomic` (with its `-sint` twin)
zeroes the image's own two rows of texels, has every fragment of the frame's
quarter add its texel through the atomic (one to row 0, two to row 1) and reads
the counts back: the quarter's 960x2160 pixels each, order-independent, so a lost
update leaves a texel short -- the shape a read-modify-write instruction would
produce under millions of concurrent writers. Console run pid 186,
`Klog_Logs/v0-storage-image-atomic-run2.log`: 1 of 1 and 1 of 1 rows, every one of
the eight texels holding 2073600 (row 0) or 4150720 (row 1) -- the quarter's pixel
count times the row's addend -- `v0-formats` 54 of 54 after the mirror moved and
`m2-solid`, 208 PASS records and no FAIL. The first run passed 0 of 1: its shader
exported a constant colour where every storage image probe's frame check expects
the quarter's level in red, which the fix made the same export as theirs.

**0 features on 0 rows are parked on `PsbcDescriptorType`, 0 features on 0 rows on `PsbcVertexFormat`, 0 features on 0 rows are blocked by the hardware's fixed fetch order, 0 features on 0 rows are blocked by the compiler fault, and 0 features on 0 rows are probe-reachable work.** Every row the specification requires is either reported with every feature it requires or listed above with the fetch order that stands in its way, and the depth/stencil table's second `must:` clause -- which R1's report found invisible to `--check` while the driver violated it -- is satisfied now that `D32_SFLOAT_S8_UINT` reports the feature a console probe proved (round 12: the machinery in `58bfc70`, the claim with the probe in this round). Rounds 1 to 3 added every vertex layout the `PsbcVertexFormat` enum could not name, rounds 5 to 9 proved the descriptor types in turn -- the uniform fetch, the storage store, the storage image and its atomics -- and round 12 the stencil path, so nothing is parked on either enum and no clause is unmet any more (docs/BLOCKERS.md). A second PS5 Vulkan implementation makes the clause-misreading mistake with a fuller inventory -- `mpereiraesaa/ps5-vulkan` states the clause in structured form and still verdicts the table satisfied by `D32_SFLOAT` alone (docs/HARDWARE_FINDINGS.md) -- which is why the clause is read from the specification rather than from a cell's marker.

`tests/test_tools.py` recounts this split from the row table above and from these two enum lists, so a driver change that closes or opens a reachable feature has to move the summary line with it.

## The reference inputs these closures use

`docs/M5_REFERENCE.md` names them for this step: ps5-opengl's 41
hardware-tested texel-buffer formats, its 44 sampled formats with readback, and
`conformance_inventory/physical_format_validation.json` with
`PHYSICAL_DEVICE_REPORTING.md` as the model for the reporting audit. None of them
is adopted yet: each entry above is a claim waiting on the console probe that
proves it, exactly as the 8-bit entries were.

## What this step does not claim

The reported claim is still false for the formats in the tables above: a
Vulkan 1.0 device must support them and this one does not. Nothing here changes
that -- it makes the size and the shape of the remaining work a list instead of
an unknown, and it keeps the driver from reporting a bit no probe has proved.
`B2` asserts the requirements the device does claim; the rows above are what it
cannot assert yet. The sampled families are the largest piece closed: every
reported row's fetch, linear filter and blit source are proved, and what the
audit still counts against them is the colour-attachment word -- or, for three
rows, the depth sampler or a missing entry. The rows closed since are the two
integer vertex formats, the blit destinations, the transfer sources, and the
fetch rows round 6 added (`R8_SNORM`, `R8G8_SNORM` and the two the table had
never claimed, `B8G8R8A8_UNORM` and `A8B8G8R8_UINT_PACK32`).
