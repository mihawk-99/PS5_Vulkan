# The blockers after rung 1.0

Stable. This is the work plan for the features `docs/V0_FORMATS_AUDIT.md` quotes
against a blocker now that the Vulkan 1.0 reachable-format rung is closed: the
libpsbc enums (`PsbcDescriptorType`, `PsbcVertexFormat`), the GPU's fixed sRGB
fetch order and the ACO stability findings. The audit stays the record of which
feature is proved or quoted; this file is the plan and the mechanism log.

## The method, one mechanism a round

1. **Determine the encoding** from a public codebase or the register database
   before writing anything: KytyPS5's `guest_gpu/gpu_defs.h` carries the GFX10
   hardware format numbering (`BufferFormat`, the same space the repository's
   fetch words use), its `guest_gpu/gpu_format.cpp` the per-format properties,
   and SharpProspero the AGC surface; ps5-opengl's `ps5_screen.c` the mapping
   ps5-opengl itself uses.
2. **Extend the compiler as a patch script**, never by editing the SDK tree:
   `tooling/psbc/patch-*.py` rewrites the work copy that
   `tools/build-psbc-ps5.sh` rebuilds, is idempotent and fails loudly when its
   anchors move.
3. **Wire the driver** (the VkFormat → enum table and the format entry's feature
   bits) and move the audit mirror with it.
4. **Extend the focused probe** with rows for the new formats; each row carries
   the vertex data and the expected readback, so a wrong fetch word shows as a
   failed frame and not as a plausible colour.
5. **Prove it on the console** with a bounded battery, then move the audit row,
   its tests and the split line in the same commit that keeps the claim.
6. **Regress** the already-proven paths: the same battery runs `v0-formats` (the
   audit mirror) and `m2-solid`.

A mechanism that fails its battery reverts its claim and is quoted in the audit
against the thing that failed, exactly as the rung's rows were.

## Mechanism log

| Round | Mechanism | State |
| --- | --- | --- |
| 1 | The eight-bit component vertex formats: `R8_UNORM`, `R8_SNORM`, `R8_UINT`, `R8_SINT`, `R8G8_UNORM`, `R8G8_SNORM`, `R8G8_UINT`, `R8G8_SINT`. `tooling/psbc/patch-vertex-formats.py` appends the enum values and their Mesa pipe formats; the descriptor word comes from Mesa's vertex-element table (GFX10 words 1, 2, 5, 6, 14, 15, 18, 19). | **Proved**: `v0-vertex-formats` 17 of 17 rows, every one 8294400 of 8294400 pixels (pid 110, `Klog_Logs/v0-vertex-formats-run4.log`); `v0-formats` 54 of 54 after the mirror moved. `PsbcVertexFormat`: 29 → 21 parked |
| 2 | The sixteen-bit component vertex formats (`R16_*`, `R16G16_*`, `R16G16B16A16_*` in the UNORM/SNORM/FLOAT/UINT/SINT classes). The same patch script; the descriptor words come from the hardware's 7, 8, 11, 12, 13, 23..29 and 65..71. The three SNORM formats had no format entry at all and got one (VERTEX_BUFFER alone: their sampled and blit bits are conditional requirements no probe has claimed). | **Proved**: `v0-vertex-formats-16` 15 of 15 rows, every one 8294400 of 8294400 pixels (pid 111, `Klog_Logs/v0-vertex-formats-16-run1.log`); `v0-formats` 54 of 54 after the mirror moved; 54 formats reported, 40 missing. `PsbcVertexFormat`: 21 → 6 parked |
| 3 | The 8888 SNORM, SINT and UINT layouts: `R8G8B8A8_SNORM`/`_SINT`/`_UINT` and their `A8B8G8R8_*_PACK32` twins, six rows closed by three enum values because Vulkan's packed form names the same memory order as its R8G8B8A8 twin (hardware words 57, 61, 60). | **Proved**: `v0-vertex-formats-8888` 6 of 6 rows, every one 8294400 of 8294400 pixels (pid 112, `Klog_Logs/v0-vertex-formats-8888-run2.log`); `v0-formats` 54 of 54. `PsbcVertexFormat`: 6 → 0 parked -- **this blocker is closed** |
| 4 | `PsbcDescriptorType`, first mechanism: the **texel-buffer** descriptor types. `tooling/psbc/patch-descriptor-types.py` adds `PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER` and `_STORAGE_TEXEL_BUFFER`, maps them onto their Vulkan descriptor types in the RADV layout the compiler builds and names them in the CLI, so a probe shader can declare a `samplerBuffer` binding; the driver gives them a 16-byte stride, records the write's `pTexelBufferView`, and writes the typed V#. | **Groundwork in place** (compiler patch, driver stride, write record and V# writer; gates green). Round 5 found that the writer alone was not enough: see round 5 |
| 5 | The **uniform texel buffer**, proved. The V# is the one RADV's own `radv_make_texel_buffer_descriptor` builds for GFX10: word 0 the view's address, word 1 the address high with the element stride, word 2 the view's range in elements, word 3 the format entry's `DST_SEL` selectors, `S_008F0C_FORMAT_GFX10` of the entry's word and `S_008F0C_RESOURCE_LEVEL` -- **not** `ADD_TID_ENABLE`, which adds the thread's id to the element index and walks a fetch off the end of a small view (the first console run's failure, and the one thing round 4's groundwork had wrong). The second failure was in the draw's table accounting: the texel-buffer arm of the validation loop `continue`d past `table_bytes = MAX2(...)`, so a stage whose only binding was a texel buffer reserved a zero-byte table and wrote its entry past what it had reserved. The probe is `samplerBuffer`/`usamplerBuffer`/`isamplerBuffer` over a four-texel buffer view, one texel per quarter of the target: red is the quarter's own element, green is element 0 fetched with a constant index (which is what tells a wrong index from a fetch that returns nothing) and blue is a constant that proves the frame drew. | **Proved**: `v0-formats-texel-buffer` 16 of 16 rows, `-uint` 11 of 11 and `-sint` 10 of 10, every row 38400 of 38400 checked pixels of its three signals (pid 130, `Klog_Logs/v0-texel-buffer-run3.log`); `v0-formats` 54 of 54 after the mirror moved and `m2-solid`; 1411 PASS records, no FAIL. 37 features on 37 rows closed: 74 → 37 parked, 38 → 22 rows, 18 rows left the missing list |
| 6 | The **storage texel buffer**, proved. No new descriptor code was needed: round 4's type, the same 16-byte entry and the same V# serve a `writeonly imageBuffer`, which compiles to `buffer_store_format_xyzw` against the entry the uniform fetch loads. The probe stores one level into the texel each quarter of the target names and reads the buffer's own memory back, every texel against its level in the format's own encoding -- the bytes the fetch probe uploads, from the same encoder. Two runs measured the float classes' rounding: a decimal literal a digit short of `level/255` lands one ULP lower in a 32-bit float texel, and a division in the shader becomes a reciprocal multiply that lands one ULP higher for 96 and 127; the shader carries correctly rounded literals now. | **Proved**: `v0-formats-texel-buffer-store` 7 of 7 rows, `-uint` 6 of 6 and `-sint` 6 of 6, every row four of four texels byte-exact (pid 135, `Klog_Logs/v0-texel-buffer-store-run3.log`); `v0-formats` 54 of 54 after the mirror moved and `m2-solid`; 818 PASS records, no FAIL. 19 features on 19 rows closed: 37 -> 18 parked, 22 -> 18 rows, four rows left the missing list |
| 7 | The **storage image**, proved. The compiler's type, its 32-byte entry and its `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` mapping land by a canonical rewrite of `tooling/psbc/patch-descriptor-types.py` (the type and strides are ps5-opengl 0.3.0's own fork's); the driver's stride, write record, pipeline mapping, validator and 32-byte writer follow ps5-opengl's console-validated `ps5_storage_image_view_descriptor` -- the sampled image descriptor's first eight words, the sampler's three left out. The probe stores one level per quarter of the target into texel (quarter, 0) and the next quarter's into (quarter, 1) of a 64x2 row-layout image and reads the image's own memory back, so a wrong pitch, format word or `DST_SEL` puts a store in the wrong texel, row or channel. | **Proved**: `v0-formats-storage-image` 6 of 6 rows, `-uint` 5 of 5 and `-sint` 5 of 5, every row eight of eight texels byte-exact over the image's two rows (pid 161, `Klog_Logs/v0-storage-image-run2.log`); `v0-formats` 54 of 54 after the mirror moved and `m2-solid`; 713 PASS records, no FAIL. 16 features on 16 rows closed: 18 -> 2 parked, 18 -> 4 rows, fourteen rows left the missing list. Two expectation bugs of the case's own were measured and fixed first (a fixed 256-byte row stride, and a shader division the compiler turns into a reciprocal multiply) |
| -- | `PsbcDescriptorType`, the **storage image's atomics** | not planned: ps5-opengl 0.3.0's runtime has no `imageAtomic` path either (its GL shader-image support stores and loads only), so nothing public shows what the hardware needs there |
| 9 | ACO stability: the `aco::schedule_program` SIGFPE and the re-test of the depth-descriptor compile | **Reproduced, isolated and explained** (rounds 8 and 15): the depth-descriptor shape is clean and the signed second compile is clean; the fault reproduces with **one case** (`v0-formats-sampled-uint` alone, its rows all passing, then SIGFPE with a zero divisor: `rax = rcx = rdx = 0`, Klog_Logs/aco-min-run1.log), and the pinned compiler's own arithmetic explains it -- `update_vgpr_sgpr_demand` can leave `program->num_waves = 0` and `get_addr_regs_from_waves` divides by it. ps5-opengl 0.3.0's patch adds the missing guard at exactly that point (`if (!program->num_waves) { ... return; }`), so the fault is a **genuine ACO bug of the occupancy class with a named fix**, and the pinned tree is where it lives (docs/HARDWARE_FINDINGS.md). Next: the SDK fork migration (metadata v14, every package re-proved) to run the same queue against the guarded compiler |
| 8 | The two sRGB rows: whether a shader-side swizzle, descriptor selector or format reinterpretation can make the byte-reversed sRGB fetches conformant | planned; ps5-opengl 0.3.0's runtime is the second reading of the same limit -- it enables sRGB for `R8G8B8A8_SRGB` alone (`PS5_ENABLE_SRGB_CANDIDATE`) and has no byte-reversed sRGB format at all |
| 10 | The **storage image atomics**, proved -- and with them the descriptor blocker closed. `imageAtomicAdd` already lowered to `image_atomic_add ... storage:image` against round 7's 32-byte entry, so the round needed no compiler or driver change: the case zeroes the image's own rows, every fragment of the frame's quarter adds its texel (one to row 0, two to row 1), and the case reads back the count the quarter's 960x2160 pixels must have accumulated -- order-independent, so a lost update leaves a texel short. | **Proved**: `v0-formats-storage-image-atomic` 1 of 1 and `-sint` 1 of 1, all eight texels exact (pid 186, `Klog_Logs/v0-storage-image-atomic-run2.log`); `v0-formats` 54 of 54 and `m2-solid`; 208 PASS records, no FAIL. 2 features on 2 rows closed: **74 -> 0 parked**, 40 -> 2 formats missing a required feature. The split's `PsbcDescriptorType` class is empty |
| 11 | The **audit's conditional cells**: a mandatory table's footnote can require a feature of a *set* of rows -- the depth/stencil table's second `must:` clause requires `DEPTH_STENCIL_ATTACHMENT_BIT` of at least one of `D24_UNORM_S8_UINT` and `D32_SFLOAT_S8_UINT` -- and `tools/format_audit.py` filed every `{sym2}`/`{sym3}` cell as conditional without consulting the driver, so a clause the driver violated reached neither the missing list nor `--check`. R1's report (a PS5 vkQuake port, `PS5_VULKAN_REQUESTS.md`) is what found it; the script now reads the footnotes whole (`required_clauses`), evaluates each clause against the driver's whole table, prints the unmet ones one row per named format, and fails `--check` on them exactly as on a missing feature. The vertex-format and descriptor bits a `{sym2}` cell *did* name were never affected: every one of those was proved and claimed one row at a time. A second implementation makes the same mistake with a better inventory: `mpereiraesaa/ps5-vulkan`'s `conformance_inventory/requirements.json` carries the clause as `structured: all of: [any of: D32_SFLOAT, X8_D24_UNORM_PACK32; any of: D24_UNORM_S8_UINT, D32_SFLOAT_S8_UINT]` and its own tooling can evaluate that form, while its `reporting_matrix.json` verdict is the flat union of all four formats -- `verdict: satisfied`, `supported by ['VK_FORMAT_D32_SFLOAT']` (docs/HARDWARE_FINDINGS.md) | **Fixed and disclosed**: one clause is unmet -- the depth/stencil one, 2 features on 2 rows -- so the split reads 2 features on 2 rows probe-reachable where it read 0 on 0 (docs/V0_FORMATS_AUDIT.md). `tests/test_tools.py` counts the clause and its rows against the document and no longer asserts the probe-reachable count is zero, which is the claim the blind spot had made unfalsifiable. The rows are the **stencil path** of round 12 |
| 17 | The **packed byte-reversed sRGB row**, the audit's last gap. Its Vulkan layout is A, B, G, R and the console's sRGB curve covers the first three *fetched* components, so red -- the fourth byte -- can never be linearised from that storage; no selector or descriptor word can move the curve. The driver stores the format's texels in the R, G, B, A order its `R8G8B8A8_SRGB` twin has (`ps5vk_format.storage_reversed`) and swaps the four bytes at every boundary where an application's bytes meet the image's: the uploads (row and tiled), the readbacks and both blit directions. The image's layout is the driver's own -- every image is created with `VK_IMAGE_TILING_OPTIMAL` -- so nothing else observes the order, and the fetch is the twin's own path (the `8_8_8_8_SRGB` word with straight RGBA selectors). | **Proved**: `v0-formats-sampled` 27 of 27 rows fetched the colour their texels hold, `c7-blit-formats` 55 of 55, `v0-blit-dst`, `v0-transfer-formats`, `v0-targets` (the row's solid and blended frames), `v0-formats` 58 of 58 and `m2-solid`, 7 of 7 tests and no FAIL (pid 235, `Klog_Logs/v0-srgb-packed-run1.log`). **The audit's missing list is empty and `--check` exits 0**: 179 required, 58 reported, 0 missing, 55 conditional, 0 clauses unmet, split 0 / 0 / 0 / 0 / 0. Host gates: `vk_v0_formats_test.c` (27 rows, its upload check knowing the storage), `vk_c7_clear_image_test.c` (the encode and the readback's swap), and `vk_c7_blit_formats_test.c`, whose table now mirrors the console case's whole set so the two cannot drift |
| 14 | The **byte-reversed sRGB target**: `B8G8R8A8_SRGB`'s colour-attachment pair. Its `CB_COLOR0_INFO` word is the same `8_8_8_8` data format and sRGB number type as `R8G8B8A8_SRGB`'s with `SWAP_ALT`, the swap the swapchain's byte order uses, so the hardware encodes the linear colour into B, G and R order; the CPU clear encode gained the blue-first branch beside the curve. | **Proved**: `v0-targets` holds the solid frame's `0xff89bce1` and the additively blended frame's `0xffbcbce1` for the row, with `v0-formats` 57 of 57 and `m2-solid`, 3 of 3 tests and no FAIL (pid 195, `Klog_Logs/v0-srgb-target-run1.log`). **The audit's missing list is down to one row**: `A8B8G8R8_SRGB_PACK32`'s six features, the packed format the curve really does block, so the split reads 0 / 0 / 6 features on 1 row / 0 / 0 |
| 13 | The **byte-reversed sRGB fetch**, separated from the packed one. pid 160's measurement is of `A8B8G8R8_SRGB_PACK32`, whose memory bytes are A, B, G, R: the curve lands on the alpha byte and two colour bytes, so no selector can fix it. `B8G8R8A8_SRGB`'s bytes are B, G, R, A -- its whole colour triple -- so the curve lands on the right three bytes and the `ZYXW` selectors `B8G8R8A8_UNORM` already uses place them. The round gave the format its entry with the `8_8_8_8_SRGB` word and those selectors, its blit decode and encode (`ps5vk_texel_to_rgba8`/`ps5vk_rgba8_to_texel`, the blue-first order composed with the curve), and taught the harness its texel size. | **Proved**: `v0-formats-sampled` (the fetch and linear filter), `c7-blit-formats` (54 of 54 blit sources), `v0-blit-dst`, `v0-transfer-formats`, `v0-formats` 57 of 57 and `m2-solid`, 6 of 6 tests and no FAIL (pid 194, `Klog_Logs/v0-srgb-run2.log`). Its fetch, blit pair and transfer pair report; only `COLOR_ATTACHMENT`/`COLOR_ATTACHMENT_BLEND` are left, and they are the CB word and export mechanism of the next round. The audit's hardware class falls from 12 features on 2 rows to **8 on 2** |
| 12 | The **stencil path** the disclosed clause needs: `D24_UNORM_S8_UINT` and `D32_SFLOAT_S8_UINT` need a `DB_Z_INFO`/`DB_STENCIL_INFO` word pair, stencil read and write bases, the stencil test's control word, and a probe that rasterises through it and reads the stencil plane back. `driver/ps5vk_draw.c`'s depth template programs `DB_STENCIL_INFO` disabled (`0x20000180`) and every stencil base and clear word zero, so the path is the same template with the stencil plane enabled and its state programmed; ps5-opengl 0.3.0's hardware-run `append_depth_target_state` is the offset-and-word reference (`0x0010` `DB_Z_INFO`, `0x0011` `DB_STENCIL_INFO` `0x20000181` where its 64 KiB-Z_X S8 template programs `0x20000180`, `0x0013`/`0x0015`/`0x001b`/`0x001d` the stencil bases, `0x001a` `DB_STENCIL_CLEAR`), and its `ps5_screen.c` is the second reading of a stencil attachment | **Proved**: `v0-stencil` 4 of 4 frames on the console -- the setup pass stores the stencil reference (0x5a) and `gl_FragDepth` 0.75, the test pass with `STENCILFUNC EQUAL` and the depth test passes and paints the whole target, a reference the plane does not hold and a test pass behind the plane's depth both leave it red, and the no-test control paints -- with `v0-formats` and `m2-solid`, 3 of 3 tests and 0 FAIL (pid 192, `Klog_Logs/v0-stencil-run5.log`; golden `golden/v0-stencil`). `D32_SFLOAT_S8_UINT` and `D24_UNORM_S8_UINT` report `DEPTH_STENCIL_ATTACHMENT_BIT` and nothing else, so the audit's second `must:` clause is satisfied and the split's probe-reachable class is empty: **0 / 0 / 12 / 0 / 0**. Host gate: `driver/tests/vk_c5_stencil_test.c` (13 checks direct, 4 loader) reads the words back out of the draw's tables -- `DB_STENCIL_INFO` 0x20000181, the plane's bases, `DB_DEPTH_CONTROL`'s stencil bits, `DB_STENCIL_CONTROL` and both `DB_STENCILREFMASK` words. The runs also measured two things worth keeping: the stencil plane's content read back through the driver's own layout is 0x5a in all 65536 bytes of its first tile (so the plane is where the derivation places it), and the depth plane's first tile holds the setup pass's 0.75 in all 16384 texels |

## What each blocker needs

**`PsbcVertexFormat`.** The compiler's switch maps a `PsbcVertexFormat` to a Mesa
`pipe_format`, and Mesa's vertex-element emission turns that into the vertex
descriptor's `DATA_FORMAT` word; so a format is expressible as soon as the enum
and the case exist. The hardware's own numbering (KytyPS5's `BufferFormat`) shows
every layout the Vulkan formats below the 32-bit classes need, so the limit was
the enum, not the hardware.

**`PsbcDescriptorType` -- closed.** Every type the parked rows needed is proved:
the uniform texel buffer (round 5), the storage texel buffer (round 6), the
storage image (round 7) and its atomics (round 9), 74 features on 38 rows down to
none. What the audit still lists is the two sRGB rows, which are the hardware's
and not this enum's, and the depth/stencil table's second `must:` clause, which
is the stencil path's (round 12). The record of how each type was determined,
extended and proved is the mechanism table above.

**`PsbcDescriptorType` (the original reading).** The compiler accepted only `UNIFORM_BUFFER` (16-byte
entries), `COMBINED_IMAGE_SAMPLER` (48-byte entries: a 32-byte image descriptor
and a 16-byte sampler) and `STORAGE_BUFFER`, and the driver had no texel-buffer
or storage-image descriptor path at all. The mechanism is the same shape as the
vertex formats -- a new enum value, its entry stride, the type bits the hardware
descriptor carries and the shader-side load -- but it also needs the driver's
`vkUpdateDescriptorSets` path to write those descriptors, which is why it came
after the vertex families. Rounds 4 and 5 did the uniform half of it; the
storage half -- an `imageBuffer` load/store and a storage image the compute path
writes -- is the same three pieces again: the type in `patch-descriptor-types.py`,
a V# (or an image descriptor) whose words a probe proves, and the claim.

**The sRGB fetch order.** The console applies the sRGB curve to the first three
fetched components *before* any selector, so a byte-reversed sRGB format
linearises the wrong three bytes. A workaround has to change which components the
fetch reads, not how they are selected: candidates are a descriptor selector the
driver has not tried, a format reinterpretation (fetch as UNORM and apply the
curve in the shader), and a shader-side swizzle the *application* would have to
write (which is not a driver fix). If no driver-side variant is exact, the two
rows stay quoted against the hardware and this file records the refutations.

**Which of the two rows the measurement actually refutes.** The measurement
(pid 160, docs/HARDWARE_FINDINGS.md) is of the *packed* format: a texel of
`A8B8G8R8_SRGB_PACK32` whose memory bytes are A, B, G, R = 0xff, 0xe1, 0xbc,
0x89 read back with red 0x89 *unlinearised* -- bytes 1 and 2 took the curve and
byte 3 did not, and byte 3 is the red the format's channel map names. For
`B8G8R8A8_SRGB` the memory bytes are B, G, R, A: the first three fetched
components are the *whole* colour triple, so the curve lands on the three bytes
the format calls its colour channels, and the ZYXW selectors the driver already
uses for `B8G8R8A8_UNORM` are what puts them in the right outputs (output
R = Z = byte 2 = the format's red). The audit's family row notes both formats as
one case; the *fetch* half of `B8G8R8A8_SRGB` is therefore worth a probe of its
own -- the sRGB format word with the ZYXW selectors -- before its six features
are quoted against the hardware. What the row also needs is its attachment and
blit half: the CB word's sRGB number type with `SWAP_ALT`, the export format
Mesa's `ac_choose_spi_color_formats` picks for it, and the driver's own encode
for a blit destination, each of which the same probe can carry. Until that run
exists, `A8B8G8R8_SRGB_PACK32` stays the measured refutation and
`B8G8R8A8_SRGB` stays a candidate.

**ACO stability.** The `aco::schedule_program` SIGFPE followed the unsigned
texture case's compile and a failed case's live device; its top frame is the
faulting instruction, so it is a real compiler fault and the question is which
driver state triggers it. The depth-descriptor compile, by contrast, was never
reached before round 17 removed the `vkCreateImage` assert, so it needs a fresh
attempt on the current driver before it can be called a compiler fault at all.

**The stencil path.** The depth/stencil table's second `must:` clause is the only
requirement the audit finds unmet that a console probe can close, and it is
narrower than "stencil in general": one of two formats has to report
`DEPTH_STENCIL_ATTACHMENT_BIT`, which needs an image of that format to be
creatable and usable as a render target. The registers are the depth template's
own sixteen (rounds 14/15, M4's canary) with the stencil half switched on --
`DB_STENCIL_INFO`'s enable word, the stencil read/write bases at the same 64 KiB
tile address as Z, `DB_STENCIL_CLEAR`, and the stencil test's control and
reference words in the draw's state -- and ps5-opengl 0.3.0's
`append_depth_target_state` (offsets `0x0010`, `0x0011`, `0x0013`, `0x0015`,
`0x001a`, `0x001b`, `0x001d`) plus its `ps5_screen.c` stencil attachment path are
the console-validated reference. The probe has to show the stencil plane itself,
not just that a draw ran: a stencil pass whose fragments depend on the reference
the plane holds, at one and four samples, with the plane read back through its
own map. The two formats' `DB_Z_INFO` words are the *next* question and can
differ: S8 with D24 packs the stencil into the depth word's spare byte, while
`D32_SFLOAT_S8_UINT` needs the second 64 KiB plane ps5-opengl's template leaves
disabled when it has no stencil, which is why the probe measures the words before
either format is claimed.

## ps5-opengl-sdk-0.3.0: what it unlocks, and what moving to it costs

The SDK this repository pinned (`ps5-opengl-sdk-0.2.0`) was replaced on
2026-09-20 by the 0.3.0 release, which is the first public OpenGL 4.6 / GLSL 4.60
release of ps5-opengl: Mesa **26.2.0**, the same Mesa base the pinned compiler
vendored, and the opengnm-psbc fork applied as
`toolchain/opengnm-psbc-ps5.patch` over upstream commit `a92a1228`. Its layout is
not the one `tools/*.sh` read (`sdk/` prebuilt payload plus `sources/*.tar` and
the `ps5-opengl/` source tree, against the pinned layout's `third_party/`,
`toolchain/` and `src/platform/`), so the repository does not build until an
adapter reads it; that adapter is round 7's first step, since nothing can be
re-proved without it.

**What it unlocks for the blockers.**

- **The storage image.** The fork's `PsbcDescriptorType` has
  `PSBC_DESCRIPTOR_STORAGE_IMAGE`, its layout maps it to
  `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` with a **32-byte** entry
  (`psbc_compile.c`'s stride) and its validation accepts it -- so the third
  mechanism's compiler half is already written, where our pinned tree needed a
  patch for it. The remaining 18 parked features on 16 rows are exactly this
  descriptor.
- **The descriptor words, validated.** ps5-opengl's own runtime builds a storage
  image's 8-word (32-byte) descriptor in `ps5_storage_image_view_descriptor`
  (`src/gallium/ps5/ps5_screen.c`): the same word 0 address-high and word 1
  format/width shape our sampled descriptor already writes, plus its own kind and
  level fields, and `ps5_resource_storage_image_descriptor_owned` checks a
  descriptor against it word for word. That is the reference the driver's
  storage-image path needs, from a runtime whose GL 4.6 image path passed its
  own native checks.
- **A second reading of the texel buffer.** `ps5_texel_buffer_descriptor` builds
  the same V# our rounds 5 and 6 proved, field for field: `state.stride = texel
  size`, `size / texel_size` elements, the format's swizzle, `OOB_SELECT` 0,
  `has_desc_resource_level` true and **no `add_tid`** -- the independent
  confirmation of round 5's two fixes.
- **ACO.** The fork's ACO differs from the pinned tree's in a handful of files
  and carries a `num_waves == 0` guard in `aco_live_var_analysis.cpp` ("fixed
  registers can lower occupancy below the workgroup minimum") plus a
  `buffer_scratch` spilling mode (`aco_ir.cpp`, `aco_spill.cpp`,
  `aco_isel_helpers.cpp`). `schedule_program` itself is byte-identical, so
  whether that guard is what our SIGFPE needs is a measurement, not a reading:
  the first step of the ACO round becomes compiling the same shaders with the
  0.3.0 fork and looking for the fault.
- **Metadata.** The fork's shader metadata is version **14** and carries the
  compute fields our `patch-compute-metadata.py` adds to the pinned tree (ACO's
  rsrc1/2/3, VGPR/SGPR counts, LDS, wave size, workgroup size), so that patch
  becomes obsolete -- and the driver's metadata reader has to be checked against
  the new field set.
- **New vertex formats.** `R64_FLOAT`, the `R64G64*` family and
  `R11G11B10_FLOAT` joined `PsbcVertexFormat`. None is a required Vulkan format
  (`B10G11R11_UFLOAT_PACK32` already has an entry), so the vertex blocker stays
  closed without them.

**What moving costs, now measured rather than predicted (round 16).**
`tools/check-sdk-fork-migration.sh` assembles the fork's compiler in
`build/sdk-fork` from the SDK's own pins and runs this project's compiler patches
against it. Its answers, from the run recorded in docs/M5_PHASE_C.md:

- the base is upstream `PS4-OpenGNM/opengnm-psbc` at
  `a92a1228ea3a64e4be9f0e61c2a65a5aa7ffed92`, and `toolchain/opengnm-psbc-ps5.patch`
  (`sha256 a7c73aef...`, the hash this project already pins) applies to it
  cleanly -- the assembled tree's own hash is `a27cbecc8c11761da04af6b8e089905b93252e65`,
  the SDK's recorded `patched_tree`, so the assembly is the compiler the SDK built;
- `patch-vertex-formats.py` **holds** against it (26 enum values and 26 switch
  cases: every layout of rounds 1 to 3);
- `patch-descriptor-types.py` **holds** (2 enum values, 3 CLI names, the type
  mapping, the validation line and the entry strides: our two texel-buffer types,
  on top of the fork's own `PSBC_DESCRIPTOR_STORAGE_IMAGE`);
- `patch-fragment-inputs.py` **does not hold** -- its anchor needs moving, as
  predicted (`expected one fragment-input anchor`);
- `patch-compute-metadata.py` **does not hold and is dropped**: its anchor is the
  v8 metadata define, and the fork's schema is `PSBC_SHADER_METADATA_VERSION 14u`
  with the compute fields already in it.

So the work list is one moved anchor, one dropped patch, two patches that carry
over unchanged, and a metadata version 8 to 14 bump. The compiler changes, so
every probe package, provenance file and golden in this repository is rebuilt and
every battery re-run before any claim stands: a compiler swap is a shared change,
and the rules treat it like one.
