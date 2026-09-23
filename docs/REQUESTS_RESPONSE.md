# Answers to `PS5_VULKAN_REQUESTS.md`

Status of each request, with the case, the golden and the console run that proves
it, written for the requesting project. Six one-per-item commits (`8f4dc2d` R6,
`2199518` R2, `cfab0b9` R1, `4564091` R3, `0f162ce` R5, `0a7c23a` R4) plus the
depth-bias follow-up (`44ae678`), on top of the compiler-fork migration
(`8b311e3`). Each item was re-checked at HEAD before it was started; none had
been closed by the migration.

| Request | Status | The case | Golden, queue | Console evidence |
| --- | --- | --- | --- | --- |
| R6 split submissions corrupt the heap | **closed** | `v0-two-passes` | `golden/v0-two-passes`, `jobs/v0-two-passes/queue.txt` | guard fired before the fix (pid 308), three of three shapes after it (pid 310, 313) |
| R2 `vkCreateSampler` takes one configuration | **closed** for the address modes | `v0-sampler-address` | `golden/v0-sampler-address`, `jobs/v0-sampler-address/queue.txt` | pid 315 |
| R1 rasterization state refuses three core things | **closed**, one named gap | `v0-cull`, `v0-depth-bias` | `golden/v0-cull`, `golden/v0-depth-bias`, `jobs/*/queue.txt` | pid 323 (cull), pid 367 (depth bias, re-run in the pid 369 sweep); host gate `c5_depth_bias` |
| R3 stencil clear takes the depth value | **closed** at the driver's boundary | `v0-stencil-clear` | `golden/v0-stencil-clear`, `jobs/v0-stencil-clear/queue.txt` | pid 327 (before), 329 (after) |
| R5 resolve destination must be a colour attachment | **reworded**, not tiled; both halves of the probe proved | `v0-resolve-usage` | `jobs/v0-resolve-usage/queue.txt` | pid 369 |
| R4 three blind spots | **answered**: the audits now say what they cannot see | -- | -- | -- |

## R6 -- closed, with a sharper dose-response than the request guessed

The copy-step capture was sized for `split_count` steps while a recording can
write `split_count + 1` step end-markers, so the last step's words landed up to 16
words (64 bytes) past the heap allocation at submit. The new guard is what proved
it on the console before the fix, with its own numbers: `a split submission's step
capture needs 165 words where the buffer holds 149 (133 words, 1 splits)`. After
the fix the same three shapes pass (`v0-two-passes`: one pass, two passes, two
passes with a resolve between; 40 frames each, a second submit every frame; the
process alive with the heap intact).

**The dose is where a copy's recording splits, not the number of passes or
draws**: "two passes" without the resolve passes and "two passes, resolve between"
died, because the resolve is what leaves the last split mid-stream. The request's
useful second measurement -- four clears and no scene draws -- was not built as a
shape; the guard's own numbers answer what it was for.

## R2 -- closed for the address modes, deliberately nothing else

`addressModeU/V/W` now reach the descriptor (U bits 0-2, V 3-5, W 6-8) in
ps5-opengl's own encoding (REPEAT 0, MIRROR_REPEAT 1, CLAMP_TO_EDGE 2). REPEAT is
what a zeroed `VkSamplerCreateInfo` holds, which is why the default itself was
being refused. CLAMP_TO_BORDER and MIRROR_CLAMP_TO_EDGE are refused by name (the
second is an extension, not core Vulkan 1.0). Everything else the request asked
not to widen is still refused by name: mixed filter pairs, a non-zero
`mipLodBias`, `anisotropyEnable` (the message now also says the device reports
`maxSamplerAnisotropy` 1.0, so an application that honours the limit never asks),
`compareEnable`, a border colour other than transparent black, and unnormalized
coordinates. `v0-sampler-address` samples a four-group texture across u 0 -> 4.

## R1 -- closed: cull, discard and depth bias are programmed

- `cullMode` and `rasterizerDiscardEnable` were the round's first half: the words
  are PA_SU_SC_MODE_CNTL's CULL_FRONT/CULL_BACK/FACE and PA_CL_CLIP_CNTL's
  DX_RASTERIZATION_KILL, and `v0-cull` reads 132 / 65 / 67 / 0 drawn pixels (no
  cull, back, front, discard; 65 + 67 complementary). The FACE bit is what makes
  `cullMode = BACK` remove the face the specification names with this driver's
  flipped-Y viewport.
- `depthBiasEnable` is now programmed too, and what closing it cost was not the
  six words: ps5-opengl's block (0x2de..0x2e3) does **nothing** without
  PA_SU_SC_MODE_CNTL's POLY_OFFSET_FRONT/BACK_ENABLE (bits 11/12) -- the first
  console run recorded all six and moved no pixel. The console also measured the
  unit (4096 units = 0.00049 of depth, one unit being 2^-23) by reading the biased
  depth out of the plane.
- **The one gap.** The hardware's clamp register (`PA_SU_POLY_OFFSET_CLAMP`)
  measured inert on this path: a 2e-5 clamp left a 0.00049 pull intact and changed
  no pixel of a ramp. The driver therefore caps the constant factor itself, and
  the slope half's clamp is not implementable at pipeline creation because the
  slope multiplies the polygon's own depth gradient. `v0-depth-bias` measures the
  cap (0.0002 past the clear: 2880 of 2880 samples kept unclamped, none clamped,
  the capped factor in the recorded table). The `polygonMode` and `depthClampEnable`
  refusals are untouched, as the request asked: both are gated by feature bits this
  device reports false.

## R3 -- closed at the driver's boundary, with the discriminating measurement

Measured before the fix: clearing a combined depth-stencil attachment with depth
1.0 left **0 of 65536** stencil bytes zero and depth 0.5 left **65536 of 65536**,
so the plane follows the depth member's value rather than being clamped or
defaulted -- the follow-up the request asked for. After the fix both depths read
65536 zero bytes. The fix is `WORKAROUND(R3)` at the `vk_meta_clear_rendering`
call: the rendering-info copy's stencil attachment carries the stencil value in
the member `vk_meta` reads, with the upstream line that retires it in the comment.
It is deliberately not a patch to Mesa: the driver links a prebuilt runtime whose
sources this repository does not own, and the defect is upstream's.

## R5 -- reworded, not tiled, with the request's own probe run

The refusal now names the usage bit, the storage rule and the workaround in one
sentence ("... this driver makes an image tiled from the COLOR_ATTACHMENT or
DEPTH_STENCIL_ATTACHMENT usage bit ... declare COLOR_ATTACHMENT on it to make it
tiled"). The probe the request described has been run on the console
(`v0-resolve-usage`, pid 369): the same four-sample resolve recorded twice, once
into a destination declared TRANSFER_DST and SAMPLED, which is refused (the
recording ends, which is what the case reads), and once into the same frame with
COLOR_ATTACHMENT added -- the workaround the requesting project carries as W5 --
which submits. `c8-resolve` is the regression behind the second half.

One thing the probe cannot show: the runner installs no Vulkan debug messenger, so
the sentence itself does not appear in the klog, only the refusal does. The
sentence is what an application's own messenger receives, which is how this
project found R1, R2 and R3 in the first place.

The request's other option, tiling every transfer destination, was not taken
because tiling is chosen from the usage bits and a texture upload declares
TRANSFER_DST too: it would move every sampled image's descriptor and every golden
that samples a texture -- a repository-wide re-capture for the same behaviour.

## R4 -- the audits now say what they cannot see

`command_audit.py` prints five blind spots beside the cases that cover them
(pipeline state twice, sampler state, cleared pixels, a submission's steps),
`limits_audit.py` one and `format_audit.py` two; `--check` counts are unchanged.
The matrices the request proposed exist in part: the pipeline-state matrix is
cullMode, rasterizerDiscardEnable and the depth bias (the other three fields are
feature-gated or have their own named gap), the sampler matrix is the address
modes (the rest is refused by name, not merely unmeasured), and the clear readback
with nothing drawn over it exists for the stencil plane, not for a colour target.

## What is still open, stated plainly

- The slope factor's half of `depthBiasClamp`: the hardware's clamp register is
  inert here and a driver-side cap cannot know a polygon's depth gradient.
- The request's "four clears, no scene draws" shape was not built; the mechanism
  is proved instead by the guard's numbers and by the resolve shape.
- The sampler matrix is the address modes; filters, LOD bias, anisotropy, compare
  and border colours stay single-configuration refusals.
- `v0-depth-bias` runs forty frames a shape in `v0-two-passes`, not a hundred in
  one shape; `kTwoPassFrames` is the one constant to raise.
- **The AGC package writer emits one descriptor-set pointer, set 0's**
  (`src/platform/ps5_agc_package.c:200`, `descriptor_set0_valid` /
  `descriptor_set0_user_data_dword`). A *packaged* multi-set shader would
  therefore reach an AGC-native consumer with set 0 bound and the rest nowhere.
  Nothing depends on it today: the driver compiles the application's SPIR-V and
  never reads those packages, so R7's multi-set work does not go through it, and
  the probe CLI's packages are only compared byte for byte by the runner's
  compile keyword. Blast radius: an AGC-native consumer of a multi-set shader --
  ps5-opengl's Gallium path is single-set by construction and this driver's path
  does not use packages, so no such consumer exists yet. Retirement trigger: the
  writer learns `descriptor_sets_valid[]` / `descriptor_sets_user_data_dword[]`
  at the same time as the first AGC-native consumer that binds more than set 0.

## The verification this status rests on

`tools/check-driver.sh` passes whole (288 run comparisons identical, no
`DIFFERENT` record, no fault), `build/gates.sh` passes all ten host gates on the
committed tree, and the console batteries named above each regress `v0-cull`,
`v0-stencil` and `m2-solid` as their own controls. The linked runtime was four
days older than the compiler migration and has been rebuilt; every console run
quoted here is tied to the artifact digest printed in its own section of
`docs/M5_PHASE_C.md` (the R round's runs to `b33b813c...`, the depth-bias and
resolve-usage sweep to `998037c4...`).

---

# Answers to `PS5_VULKAN_REQUESTSv2.md` (R7-R9 and the coverage note)

The second batch's evidence is of a different kind -- each item was a prediction read
from vkQuake's source -- so each was re-checked at HEAD before anything was written,
and one detail of one prediction turned out to be wrong (R7's refusal point, below).
Per item: status, the probe, the run's digest, and what the application side should do.

| Request | Status | The probe | Run, digest |
| --- | --- | --- | --- |
| R8 `VK_DYNAMIC_STATE_DEPTH_BIAS` | **closed** | `v0-dynamic-depth-bias`, plus the focused host gate `c5_depth_bias` | pid 371, `4aec17c7…` |
| the clamp decision | **closed**: refused by name in both forms | `v0-depth-bias`'s clamp frame + `v0-two-sets`' run | pid 371, `4aec17c7…` |
| R9 push constants | **confirmed open**: the path does not work from an application's SPIR-V | `v0-push-constant` (a measurement, red on purpose) + the host gate `v0_push_constant` | pid 377, `6306b4aa…` |
| R7 descriptor-set count | **confirmed open**, route chosen | `v0-two-sets` | pid 378, `084a7c84…` |
| R4's coverage note | **done**, and the claim behind it was wrong | the three refusal sentences in one run's klog | pid 380, `c9c57b40…` |

## R8 -- closed, and the clamp refused with it

Re-checked at HEAD first: `ps5vk_draw_refusal`'s dynamic-state walk refused
`VK_DYNAMIC_STATE_DEPTH_BIAS`, exactly as predicted, and every pipeline in the port
would have hit it. The bias is now the *command buffer's* state: the pipeline's three
factors reach it through `vkCmdBindPipeline` unless the pipeline declares the dynamic
state, and `depthBiasEnable` always comes from the pipeline, because Vulkan 1.0's
dynamic depth bias covers the factors only. Two measurements were needed on the way
and both are in `docs/M5_PHASE_C.md`: the enable is static (the first console run drew
nothing), and `vkCmdSetDepthBias`'s argument order is (constant, **clamp**, slope) --
the runtime's own prototype, which the harness had wrong.

The probe is the request's: one pipeline, two draws whose bias changes between them,
one half of the target each. Measured (pid 371): biasing the second draw kept 0 of 3
left samples and 3 of 3 right; biasing the first kept 3 and 0 -- mirror images, which
is what says each draw used the values set immediately before it -- and biasing both by
different amounts left two different biased depths in one image (0x3efff000 and
0x3effe000). `v0-depth-bias`, `v0-cull` and `m2-solid` regress.

**The clamp is refused by name, as decided.** A non-zero `depthBiasClamp` is refused in
the static form at the pipeline and in the dynamic form at the draw, with a sentence
that names the register, the measurement and the probe that would widen it. The static
case's clamp frame now *expects* the refusal instead of the capped draw it used to
measure, and the driver no longer caps anything silently.

## R9 -- confirmed open: push constants do not reach a shader from an application's SPIR-V

The probe is the request's own, and it says *where* the path breaks rather than only
that it does. Measured on the console (pid 377): the driver copied the second draw's
bytes into the reserved block (the debug API reads back 0,0,1,1 where the first draw's
were 1,0,0,1), the descriptor names that block with a 16-byte stride, the pixel user
data names the table -- and both halves of the target read back `0x00000000`. The
upload arrives; the stage's read does not.

The mechanism is the one `driver/ps5vk_nir.c` documents: the standalone compiler lowers
an application's `layout(push_constant)` to a user-data location `PsbcShaderMetadata`
does not report, and the driver -- which cannot see it -- writes only the reserved
binding instead. The driver's own NIR rewrite is what makes push constants work for the
stages it builds itself (Mesa's `vk_meta` clears read theirs happily, which is every
clear case in this tree), and an application's SPIR-V never passes through it.

**What the port should do:** keep W4. Its per-draw transforms have to stay in a uniform
buffer until the compile path changes. Two routes are named in `docs/M5_PHASE_C.md`:
the driver could run the application's SPIR-V through Mesa's `spirv_to_nir` and its own
rewrite before handing NIR to `psbc_compile_nir` (the path the meta stages already
take), or the SDK compiler could report the push-constant user-data location the way it
reports every other one. Neither is done; the probe was asked for first and it is what
says the work is needed.

## R7 -- confirmed, with one correction, and route (b) chosen

Confirmed with the request's probe (pid 378): a pipeline layout with two set layouts,
both empty so no binding reaches the shaders, and a graphics pipeline against it. The
pipeline is **created**, the **draw** is refused, and the sentence is

    descriptor set 1 is beyond the 1 this driver binds; sets past 0 are D1 (docs/M5_REFERENCE.md)

**One detail of the prediction was wrong**: the request expected the refusal at
pipeline creation. This driver refuses the draw instead -- creation succeeds so that
every package can be checked, which is the shape `ps5vk_draw_refusal` has always had --
and the sentence comes from the draw path, not the one the request quoted.

**The route: (b).** Implement multi-set binding within the advertised limit of four,
and have vkQuake merge its five layouts into four. Vulkan requires *at least* four sets,
so the driver must go on advertising four, which makes today's one-set behaviour a
conformance gap of R1's kind -- the report is a promise this driver does not keep yet.
The cost of multi-set is the same for (a) and (b) and is not the driver's alone: the
compiler metadata names one descriptor set per stage and a table travels in one
user-data dword, so a second set needs the compiler fork to report and emit a second
pointer and table (a patch in the style this repository already carries). Route (a)
pays that and additionally promises five, a limit nothing here has measured; route (c)
trades one application merge for collapsing the engine's whole binding interface.
Until it lands, keep the port at four or fewer layouts and expect this refusal above
that.

## The coverage note, and a claim of ours that was wrong

The second document repeats a claim *this* repository made in its R5 answer: that the
runner installs no debug messenger, so a refusal's sentence never reaches the klog.
That claim was wrong. The harness enables `VK_EXT_debug_utils` and installs a messenger
whose callback hands every warning and error to the report a case passed -- which is how
every item in both documents was found from the application side. The real gap was ours:
the one frame that had produced a refusal in that round passed `report = NULL`, which
discarded the sentence with the report.

Fixed and measured: the frames a probe expects to be refused now get a report whose
records are INFO, and one run (pid 380, `c9c57b40…`) carries all three of this batch's
refusal sentences -- the resolve's usage bit (R5), the descriptor-set count (R7) and the
clamp -- while the ten cases of the regression sweep pass together. The coverage item
stands closed with that correction, and the correction is the more useful half of it.

## Verification for this batch

`tools/check-driver.sh` passes whole with the two new host gates
(`c5_depth_bias` for R8's dynamic form, `v0_push_constant` for R9's driver half), and
the console sweeps named above each regress the list the request gave: `v0-cull`,
`v0-depth-bias`, `v0-stencil-clear`, `v0-sampler-address`, `v0-two-passes`,
`v0-resolve-usage`, `c8-resolve` and `m2-solid`. The last of those sweeps, on the
committed build (title digest `20d11775…`), runs all ten together: R8's
`v0-dynamic-depth-bias` now also *asserts* the depth each draw wrote (the halves at
`3efff000`/`3effe000` and `3f000000`/`3effe000`, the two pulls the console measured),
R7's `v0-two-sets` and R5's `v0-resolve-usage` carry their refusal sentences into the
log, and the Klog also shows the clamp's. `v0-push-constant` is the one case
that stays red, on purpose: it is R9's measurement, not a claim, and its message says
which half of the path arrived.

---

# Third batch: R9 fixed, R7's reconnaissance, R4's residual

## The two reconnaissance answers, first

**(a) The R9 fix is small, and it is done.** The route that works is the compiler-side
one, and it is three edits in one patch script, not a re-architecture: the standalone
path's shader-info pass runs *before* `radv_postprocess_nir`, which is where
`nir_lower_explicit_io` turns a push-constant variable into the loads that make
`radv_shader_info.loads_push_constants` true. So the ABI never declared
`AC_UD_PUSH_CONSTANTS`, ACO still lowered the loads through it, and the shader read an
unwritten SGPR: the silent zero. Hoisting the (idempotent) lowering above the info
pass, switching off the info pass's inlining heuristic for this path (the pointer is
the only form a driver can program when 128 bytes exceed a stage's 16 user-data
dwords), and reporting the pointer's dword in `PsbcShaderMetadata` is the whole fix;
the driver writes the block's address there. One trap is recorded in
`docs/M5_PHASE_C.md`: bumping the metadata version made the console refuse *every*
pipeline, because title builds compile ps5-opengl's package writer against the SDK's
header; the fields are appended, so the version stays 14.

**(b) R9 and R7 share the mechanism, not the change.** Both need per-stage user-data
pointers that the compiler reports and the driver writes, and R9's patch is the exact
template for that part. But R7 additionally needs the compiler wrapper's **single-set
assumption** removed: `psbc_descriptor_layout` builds one `radv_descriptor_set_layout`
into one storage and the validation rejects any binding whose `set` is not 0. R7 is
therefore N set layouts, N ABI pointers, N tables in the draw, per-set binding in
`vkCmdBindDescriptorSets`, the "beyond the 1 this driver binds" refusal replaced by an
"at most 4" one, and a two-set probe -- the larger piece by a wide margin. They are
not one change; doing R9 first was right, and it is what unblocks W4 now.

## R9 -- closed, both symptoms proved

Fixed in `8de2580`. The probe (`jobs/v0-push-constant/queue.txt`,
`golden/v0-push-constant`) records one pipeline whose fragment shader exports its
`layout(push_constant)` colour and two draws that upload different values without
recreating the pipeline, one half of the target each. On the console (pid 110, title
digest `dd035edd…`, `Klog_Logs/r9-sweep.log`): the left half holds `0xff0000ff` and the
right `0xffff0000` -- each draw's own value, 3 of 3 samples each -- and the frame
submits with no refusal, which is the second symptom the demos saw and the silent-zero
mechanism alone does not explain. The whole regression list passes with it (11 of 11),
`tools/check-driver.sh` reports 288 run comparisons identical with no `DIFFERENT`
(nothing else uses push constants, so no golden moved), and the focused host gate is
6 of 6 direct, loader and PS5 link.

**The port can revert W4.** The probe is a per-draw 16-byte transform, which is the
shape the request asked for.

## R4's residual -- closed

`v0-colour-clear` (`jobs/v0-colour-clear/queue.txt`, `golden/v0-colour-clear`) clears
the colour target to the canary `0xffff8040` (not black), records a rasterizer discard
so the clear is the only writer, and reads it back; a second frame draws the same pass
so the readback is proved to be the clear's value. Measured (pid 112, digest
`24deafbe…`): 36 of 36 samples hold the clear word in the clear-only frame and 0 of 36
in the drawn control. The audits' sampler line now names what `v0-sampler-address`
actually exercises (the address modes) and the fields that are refusals rather than
coverage, in both the command and the limits audit; the pixels-a-clear-wrote line names
both planes. All three `--check` modes still exit 0.

## R7 -- reconnaissance done, implementation not started

Status unchanged from the v2 answer: confirmed open, route (b) chosen, and the probe
still refuses with `descriptor set 1 is beyond the 1 this driver binds; sets past 0 are
D1`. What is new is the estimate above and the first concrete step: the compiler
wrapper has to grow N set layouts before anything else can move, and that is a
multi-session change with its own console cycles, not a field away from R9. The
application's own half (five world layouts into four) is independent of it and can
start whenever it likes.

---

# R7, Round 0: the three reconnaissance answers, before any code

The plan's own "isolated wrapper" case is the one that holds, and it holds more
cleanly than the plan assumed. All three answers come from the SDK tree this
repository already builds (`opengnm-psbc`, the same tree ps5-opengl links), read at
the revision the migration pinned.

**1. `descriptor_set0_storage` has no siblings anywhere -- and it does not need any.**
It is not an API parameter at all: it is the wrapper's own scratch, a stack blob in
the linked/tess path (`libpsbc/psbc_compile.c:2893`) and a field of
`psbc_compile_scratch` (`:3269`), both sized
`sizeof (radv_descriptor_set_layout) + PSBC_MAX_DESCRIPTOR_BINDINGS * sizeof
(radv_descriptor_set_binding_layout)`. The GL flow is single-set *by construction*:
`ps5_opengl`'s `ps5_compile_options` (`src/gallium/ps5/ps5_screen.c:3270`) flattens
everything a stage binds into one set -- a UBO *array* per stage
(`PSBC_GALLIUM_UBO_ARRAY_BINDING`, `array_size = num_ubos`), an SSBO array, a storage
image array, and one flat combined-image-sampler binding per texture unit
(`PS5_TESSELLATION_TEXTURE_BINDING + stage * units + unit`) -- all with `.set`
defaulting to 0. So the storage is singular because one set is all that path has ever
needed, not because the core cannot hold more. **R7 therefore changes the wrapper's
internals only: no caller-facing ownership or API change, which is one of the two
things my plan flagged as pushing the estimate up.**

**2. `desc_set_used_mask` never carries a bit above 0 in ps5-opengl's usage -- but the
ABI already declares one pointer per set.** The wrapper itself sets it, at both sites:
`stage->info.desc_set_used_mask |= 1u` (`psbc_compile.c:2957` for the linked path,
`:3647` and `:3649` for a stage and its paired stage). The declaration side is
RADV's and is already N-set: `declare_global_input_sgprs` adds "1 for each descriptor
set" by walking that mask (`src/amd/vulkan/radv_shader_args.c:150-166`), one
`add_descriptor_set(state, i)` per set bit. So nothing in the ABI blocks a second set;
the mask simply has never had one.

**3. The `binding->set != 0` rejection is a wrapper assertion, not a core limit.** It
sits in the wrapper's own validation (`psbc_compile.c:2109`), beside
`psbc_descriptor_layout` hard-coding `layout->num_sets = 1` (`:2161`) -- a wrapper that
only ever built one set. The core is N-set throughout: `struct radv_shader_layout`
carries `uint32_t num_sets` and `set[MAX_SETS]` with `MAX_SETS` = 32
(`src/amd/vulkan/radv_shader.h:263-269`, `radv_constants.h:71`), and the descriptor
lowering indexes the layout per binding's own set
(`layout->set[desc_set].layout`, `nir/radv_nir_lower_descriptors.c:74,224,282,323`).

**What that does to the plan.** The compiler work is wrapper-only, so the Round-1 risk
of a storage/ownership refactor disappearing is real: **plan for four rounds, keep the
fifth as contingency** rather than budgeting it. Two details Round 1 must carry that
the plan did not name:
  * the wrapper builds the shared layout from `options->vertex` in the linked path
    (`:2895`); the *single-stage* path builds it from that stage's own options
    (`:3513`), which is the path R7 uses, so per-stage set lists are already possible
    -- but the linked/tess path would need the union if it is ever asked for one;
  * the metadata's `descriptor_set0_valid`/`descriptor_set0_user_data_dword`
    (`libpsbc/psbc_compile.h:211-212`) become a per-set array, which is the same struct
    R9's `push_constant_*` fields sit in -- so Round 1's byte-identical check carries
    **R9's pointer dword explicitly**, and `v0-push-constant` joins Round 1's host gate
    as well as the console regression list. Metadata stays at version 14.

**Folded into the plan, unchanged in shape:** per-set tables are sized per set (not one
entry each), and the probe's two sets differ in kind -- set 0 supplies the colour the
image needs, set 1 a scale, so a driver that reads the wrong table draws a
recognisably wrong picture; four sets, not two; one commit per item; every round ends
green with gates plus a console cycle.

# R7, Round 1: the compiler builds one layout per set

The wrapper's single-set restriction is gone, verified without touching the driver. What
moved, in the order the plan set out:

**The change.** `tooling/psbc/patch-descriptor-sets.py` (2 header edits, 9 compiler edits;
metadata stays at version 14 because every field is appended): a wrapper cap
`PSBC_MAX_DESCRIPTOR_SETS` = 8 that is deliberately neither the core's 32 nor the driver's
four; validation by set index plus a **total** slot budget across sets, because each set's
table is indexed by its own binding numbers; one layout per set in one blob, each sized from
that set's own bindings, with empty layouts for sets in between so a shader that reads one
fails in the lowering rather than on a NULL pointer; every bound set's bit in
`desc_set_used_mask`, which is what the ABI turns into one pointer per set; and
`descriptor_sets_valid[]` / `descriptor_sets_user_data_dword[]` in the metadata, with set 0
mirroring the v14 singular fields.

**The probe.** `probes/v0-multiset`: set 0 binding 0 a uniform block (the colour), set 1
binding 0 a combined image sampler (the scale) -- two sets that differ in kind, so set 1's
table is sized from 48-byte image-sampler entries and its pointer is a second user-data
dword. `bindings.txt` records both bindings and `pixel_user_sgpr_count 4`. It is built with
the probe CLI, not the SDK's pinned compiler: the latter has none of this repository's
patches and refuses any binding whose set is not 0, which is precisely the refusal R7 is
about.

**The host gate.** `psbc_multiset` (new test, in `tools/check-driver.sh`'s list) compiles
that fragment stage with both bindings and asserts the compile succeeds, the metadata
carries a pointer for set 0 **and** set 1 in two different dwords, no set above 1 claims
one, the v14 field agrees with the array, and both new bounds refuse (a set past the cap;
two sets whose tables need 129 slots). Measured: 9 of 9 checks direct, "set 0 at user-data
dword 2, set 1 at 3, 4 user SGPRs, 160 bytes of code"; loader PASS; PS5 link PASS.
`v0_push_constant` (R9's dword) still passes in all three modes, and
`tools/check-driver.sh` is **288 run comparisons identical, none `DIFFERENT`**, 47 tests
PASS; `build/gates.sh` is 10 of 10.

**The byte-identical check, and one correction it turned up.** Every probe set was
recompiled with the pre-patch CLI and the post-patch CLI and the packages diffed: of the 40
sets that build, **every package is byte-identical except `probes/v0-push`** -- and that one
was already wrong before R7. It was written by a probe CLI built before the R9 compiler
patch landed (package committed 00:30 as `028dc83`, R9 fix at 08:15 as `8de2580`, CLI binary
from the previous afternoon), so it carried the pre-fix compilation: push constants inlined
into user SGPRs, 7 of them, 48 bytes of code, where the driver and the console have the
pointer form, 4 user SGPRs, 68 bytes. No queue ran `compile` for that set, and no other log
compares the two, so it stayed invisible. Round 1's rebuild corrects it, `b6_pipeline`
(which compiles the probe sets and compares packages) passes against the corrected package,
and R9's conclusions do not move: the console's draws were compiled by the driver, never
from that package. The other 39 packages are byte-identical, and the only other file churn
is the compiler-hash line in the `PROVENANCE.txt` files of the sets that use the probe CLI.

**No driver and no console work in this round**, as planned: the driver still refuses a
draw that binds more than one set, which Round 2 replaces with per-set tables sized per set
and `vkCmdBindDescriptorSets` per set, refusing past the four it advertises. Round 3 is the
console case that shows a value arriving from set 1 with a command buffer that submits.
One gap named here rather than later: the AGC package writer emits only set 0's pointer
(`src/platform/ps5_agc_package.c:200`), so a *packaged* multi-set shader would carry one
pointer; the driver compiles the application's SPIR-V and never reads those packages, so
nothing in R7 depends on it.

**On the console** the round is a regression, not a new case: the driver is unchanged and
the multi-set path is unreachable from a queue until Round 2, so `jobs/r7-round1/queue.txt`
is the standing nine-case list. Run pid 114, title digest `9adf1241…`,
`Klog_Logs/r7-round1.log`: **9 of 9 PASS** (`v0-cull`, `v0-depth-bias`,
`v0-stencil-clear`, `v0-sampler-address`, `v0-two-passes`, `v0-resolve-usage`,
`v0-push-constant`, `c8-resolve`, `m2-solid`), 1645 PASS records, and the only FAIL records
are the named non-zero `depthBiasClamp` refusal. R9's readback is unchanged through the
rebuilt compiler, which is what "the single-set output is byte-identical" has to mean on
the console.

# R7, Round 1 follow-up: the drift is a standing check, and the two numbers are explained

**1. Which packages the console's compile keyword actually covers.** The keyword appears
in exactly one of the 93 queues under `jobs/` (`jobs/compile`, which queues `all`); the
other 92 -- every per-case and per-round battery, including this round's -- recompile
nothing. Even that one run covers only the sets a *test* loads: **40 of the 43 committed
sets** are named in `src/diagnostics.cpp`, and three are named nowhere, so no queue
compiles them: `c7-diag`, `c8-sampleid` and `v0-multiset` (the new one, which Round 3
gives a case). So the answer to "are there other committed packages whose queue carries no
compile keyword" is: during 92 of 93 queues, **all of them**, and the compile run itself
leaves three out. The console cannot be the drift gate.

**2. How the 43 sets divide, and whether the ones that do not build matter.** The build
script has 43 case labels; **41 build** and **43 committed package sets** exist. The
difference is that two sets are produced by something else, and two labels are recorded in
the script as not building:

| | |
| --- | --- |
| rebuilt by `tools/build-probe-shaders.sh` and compared | 41 |
| `probes/shaders` | imported Split AGC assets ("headers and text copied byte-for-byte"), no builder in this repository |
| `c8-sampleid` | committed packages, and its label exits 2 on purpose: the SPIR-V front end rejects `SpvCapabilitySampleRateShading` |
| `v0-robust` | its label fails in the AGC writer (a uniform block over 16 bytes) and **nothing is committed** for it, so there is nothing to drift |

`c8-sampleid` does matter, in exactly this sense: it is a committed package that nothing
here can reproduce, and nothing consumes it either (no test names it, no queue runs it).
So it is *named* rather than skipped, and `probes/shaders` with it.

**The standing check is `tools/check-probe-packages.sh`**, and it is now in
`build/gates.sh` (11 gates). It rebuilds every set the build script can build into a
scratch root -- the build script gained `PS5VK_PROBE_OUTPUT_ROOT` so a check never writes
into the tree -- and compares the result with the committed files byte for byte. It prints
its coverage every run and **fails** if a committed set is neither rebuilt nor one of the
two named exceptions, so a new set cannot join `probes/` without a builder. Measured:
43 committed sets, **41 rebuilt and byte-identical**, the two named above, `probe packages:
PASS`. Both ways of failing were tested: appending a byte to `probes/m2/pixel.bin` gives
`DIFFERS from the committed package` and exit 1, and a *committed* set with no builder
gives `committed but neither rebuilt nor excepted` and exit 1. `PROVENANCE.txt` differences
are reported separately (they move whenever the probe CLI is rebuilt) and do not fail the
check.

**3. `PSBC_MAX_DESCRIPTOR_SETS = 8` is not a measurement, and the comment now says so.**
It is the size of a fixed blob, chosen small; the constant that *does* bound sets is the
ABI's user data, one dword per set pointer out of the 32 user SGPRs a non-compute stage has
(16 for compute), shared with every other argument. The host test now measures it rather
than asserting it: declaring a third set takes the fragment stage from 4 to **5 user
SGPRs, so 1 per set pointer** (`psbc_multiset` direct, 12 of 12 checks). The comment also
names the failure mode above the cap: RADV does not fail when the pointers stop fitting, it
switches the whole ABI to the *indirect* descriptor form
(`remaining_sgprs < num_desc_set`, `src/amd/vulkan/radv_shader_args.c:1013`), which this
driver does not implement -- and in that form no per-set pointer is declared, so the
metadata reports none and Round 2's driver refuses it by name rather than binding a set
nowhere.

**4. The 129-slot refusal's provenance, stated where a reader meets it.** The number is
`PSBC_MAX_DESCRIPTOR_BINDINGS` = 128: the length of the caller's own `descriptor_bindings[]`
array in `PsbcCompileOptions`, and the old *single-set* cap, now spent **across** sets --
each set's table runs to its own highest binding number, so the sum is what the layout blob
holds. That does mean a caller spreading many bindings over several sets can reach the
budget sooner than a per-set cap would allow; the header comment beside the constant says
so, and names the current consumer's use (about eight slots across three sets) so the
margin is on the record rather than implied. The refusal is stated in the validation
comment too, where the check itself is read.

**5. The AGC package writer's single pointer is now in the named-gaps list** ("What is
still open, stated plainly"), with its blast radius (a packaged multi-set shader reaching
an AGC-native consumer) and its retirement trigger (the writer learning the per-set array
at the same time as the first such consumer), rather than only in a source comment.

**Round 2's test shape is the application's**, recorded before any of it is written:
set 0 with **three** COMBINED_IMAGE_SAMPLER bindings (what "size each set's table from its
own binding count" has to mean -- a one-binding-per-set assumption passes a two-set probe
and fails this), set 1 with one buffer-type binding, and **no input attachment anywhere**:
vkQuake's real set 3 is three INPUT_ATTACHMENT bindings from its MBOIT pass, which is a
separate family with MRT and subpasses going into the next request batch. The refusal keeps
naming the advertised limit ("more than the four advertised"), not the current constant.

## The four items after R7 — 1 to 3 closed, 4 next

**1. The build hazard: closed, and closed twice over.** The Makefile fix from R7 round 4
(`driver/Makefile`'s object rule now takes `$(wildcard $(DRIVER)/*.h)`) covers an edit under
`driver/`, which is where the mixed layout came from. It cannot cover a header the build
*consumes* from outside: the runtime's installed tree and the compiler's.
`tools/build-driver.sh` now hashes every header the build reads into the same flags file the
archive guard already compares, so a change to any of them discards every object for that
target. **Probe of the fix**: appending a comment to
`.deps/native/vulkan-runtime/include/vulkan/runtime/vk_command_buffer.h` -- a header the
Makefile does not name -- recompiled **22 of 22** sources in all three targets; restoring it
byte for byte (sha256 checked) recompiled 22 of 22 again. Then a forced full rebuild (every
object and flags file removed) with the gates: `build/gates.sh` PASS, `tools/check-driver.sh`
PASS, and the driver is byte-identical (`sha256 d04fcad4…`), which is the point -- the digest
decides what is rebuilt, never what is built. **Gap left open**: `tooling/vulkan-runtime/
Makefile`'s object rules name no header at all (`$(OUT)/util/%.o: $(SRC)/vulkan/util/%.c`).
Blast radius: a wrapper object built against an older installed runtime header, silently,
which is the same failure mode one door over. Not R7's to fix.

**2. Anisotropy: closed at the reported limit, refused above it.** Status: the flag is
accepted as a no-op while `maxAnisotropy` is inside `[1, maxSamplerAnisotropy]`, which this
device reports as 1.0, and any value past it is refused with a sentence naming the limit and
the number (`driver/ps5vk_image.c`). **Probe**: `v0-sampler-anisotropy` draws the address
probe's four-group texture through a sampler with the flag at the reported maximum and through
one without it, and compares the two frames **texel for texel over the whole target**;
then a third sampler asking for 2.0, refused on both frames with the sentence in the log.
**Digest**: run **pid 138**, `Klog_Logs/v0-sampler-anisotropy.log` -- `frames_drawn` 2,
`mismatched_texels` **0**, and the refusal's sentence twice. **Golden**:
`golden/v0-sampler-anisotropy/` (two submissions, four pipelines; `m2-solid` is queued beside
the case because a driver capture carries no context register table). The claim follows the
proof: `docs/M5_REFERENCE.md`'s C4 row and its limits row now say exactly this and no more.
**Gap left open**: `samplerAnisotropy` stays **FALSE**. That is deliberate -- nothing is
filtered, so the feature is not claimed -- and an application that sets the flag anyway gets
the isotropic sampler rather than an error, which is what a conformant application at this
limit is entitled to.

**3. R7 round 3's fallout.** *(a) The driver-only-queue audit is done.* All **103** queues in
`jobs/` were classified by their cases' runner functions: **91** cases are driver cases and
**24** are runner-built. Exactly **two** queues name nothing but driver cases.
`jobs/v0-u16/queue.txt` was the real one: its golden, `golden/v0-formats-sampled-uint`, replays
only because the run it came from happened to queue `m2-solid` elsewhere -- the committed queue
would have written an unreplayable golden on the next re-capture, and the refusal ("no context
register table to take register defaults from") appears only when someone tries to replay it.
It now queues `m2-solid` after its case (so the case's own capture is unchanged) and says why.
`jobs/device-report/queue.txt` says why it is not a golden capture at all: its case creates no
device and draws nothing. Separately, **all 31 goldens the driver gate reads were replayed as
an audit and every one succeeds** -- no committed evidence is in the broken state.
*(b) The submission comparison was already in, from round 4.* `driver/tests/
vk_v0_multiset_quake_test.c` draws exactly the console case's one frame and
`tools/check-driver.sh` compares it with `compare-run`; re-verified for this document:
`identical to v0-multiset-quake-1.json: 17 packets, 6 register tables`, in both build modes.
*(c) This document is that item.* **Gap left open**: nothing enforces the rule the two queues
now document. A future driver-only queue can be written without a sibling and without a
reason, and the defect stays invisible until a replay is attempted; the blast radius is one
console cycle and one unusable golden. A guard would be "a queue whose cases are all driver
cases must name a runner-built frame or say why it is not a golden capture", which is policy
this round chose not to invent.

## Step 1 (MRT) — reconnaissance, and the one input that had to be sourced rather than guessed

**What is already in place**, read rather than assumed: `ps5vk_color_export_options`
(`driver/ps5vk_pipeline.c`) accepts up to `PS5VK_MAX_COLOR_EXPORTS` (8) and emits one
`SPI_SHADER_COL_FORMAT` nibble per attachment, so the *export* side is general already. The
pass, the framebuffer and a pipeline against it succeed on the console. `ps5vk_draw.c`
refuses `colorAttachmentCount > 1` at one place, ahead of the colour-format and
target-register checks, and the colour target's registers are the 16 `CB_COLOR0_*` offsets
in `ps5vk_target_offsets[]`, with the comment "gfx103 context register offsets:
(address - 0x28000) / 4". The advertised limit is `maxColorAttachments = 4`
(`ps5vk_physical_device.c`), and the refusal text names one, not the limit.

**The unknown that had to be settled before any code: where targets 1..3's registers are.**
They are *sourced*, not invented: `amdgfxregs.h` in the SDK tree carries the gfx103
addresses, and the same `(address - 0x28000)/4` arithmetic the table documents reproduces
them -- to the dword:

```
R_028C60_CB_COLOR0_BASE  0x028C60 -> 0x318   (the table's own first entry, so the arithmetic holds)
R_028C9C_CB_COLOR1_BASE  0x028C9C -> 0x327   (not 0x328: the per-target stride is 15 dwords, not 16)
R_028CD8_CB_COLOR2_BASE  0x028CD8 -> 0x336
R_028D14_CB_COLOR3_BASE  0x028D14 -> 0x345
R_028C70_CB_COLOR0_INFO  0x028C70 -> 0x31c   and CB_COLOR1_INFO -> 0x32b, +15 as well
R_028E40_CB_COLOR0_BASE_EXT      0x028E40 -> 0x390
R_028E44_CB_COLOR1_BASE_EXT      0x028E44 -> 0x391   (this family's stride is 1, not 15)
R_028E60_CB_COLOR0_CMASK_BASE_EXT 0x028E60 -> 0x398  and CB_COLOR1's -> 0x399
```

The two register families therefore have **different per-target strides** -- the main ten
registers (BASE, VIEW, INFO, ATTRIB, DCC_CONTROL, CMASK, FMASK, CLEAR_WORD0/1, DCC_BASE)
stride by **15 dwords**, the `_EXT` four by **1 dword within a field**, with the fields eight
dwords apart. A table built as "target 0's offsets plus one stride" would be right for one
family and silently wrong for the other, which is why the first draft of this paragraph (it
said 0x328 and one stride) was corrected here. `_ATTRIB2` and `_ATTRIB3` did not match that
naming in the header and still have to be located there before the table is written.

**The acceptance shape, fixed before the code**: the probe varies the attachment count (1, 2
and the advertised maximum, read from `maxColorAttachments` rather than written as 4) and
each attachment carries a distinguishable value, every one read back -- a driver that wrote
attachment 0's data into all of them, or wrote one twice, has to fail. R6's case joins the
battery because the colour-target dynarray is where R6's heap corruption lived and more
targets changes exactly that arithmetic. The refusal past the limit names
`VkPhysicalDeviceLimits.maxColorAttachments` and the reported number.

**State**: not started. The next commit is the draw path's per-attachment programming, the
probe, and the console run.

## Step 1a — the per-target register table, derived and validated (`134e956`)

**What moved.** `ps5vk_target_offsets` was sixteen offsets for one colour target. It is now
sixteen rows by four columns, one column per colour attachment, and every function that fills
target registers names the attachment whose row it fills (`ps5vk_default_target_registers`,
`ps5vk_target_registers`); `PS5VK_MAX_COLOR_TARGETS` (4) has one owner and two readers — the
driver reports it as `maxColorAttachments` and programs up to it — so the number the device
advertises and the number the draw honours cannot drift. The draw still programs one target
(its callers pass 0), so this commit claims no capability: it is the table and the plumbing
the loop needs.

**The two open questions, settled from the header** (`.deps/.../amdgfxregs.h`, `(address -
0x28000)/4`, the arithmetic the table's own comment documents):

1. **gfx103 confirmed.** The header carries gfx9/gfx10/gfx103/gfx11/gfx115 under the same
   names and their addresses differ; taking only the rows whose generation comment covers
   gfx103 reproduces **all sixteen** of the entries the table already carried for target 0,
   dword for dword. That is the validation: the reader agrees with the table where the table
   is known good, so its answers for targets 1-3 are trustworthy. The comments are a small
   language -- `<= gfx9, >= gfx10` covers *everything*, and two earlier readings of it got
   that wrong, once by prefix and once by treating a clause list as a single range.
2. **`_ATTRIB2` and `_ATTRIB3` located**: `0x3b0..0x3b3` and `0x3b8..0x3bb`, per-target
   stride **1**, with the fields eight dwords apart. They are not derivable from the
   fifteen-dword stride the first ten registers use.

The finished table, as the code carries it:

```
field            t0     t1     t2     t3     stride
BASE             0x318  0x327  0x336  0x345  15     BASE_EXT        0x390  0x391  0x392  0x393  1
VIEW             0x31b  0x32a  0x339  0x348  15     CMASK_BASE_EXT  0x398  0x399  0x39a  0x39b  1
INFO             0x31c  0x32b  0x33a  0x349  15     FMASK_BASE_EXT  0x3a0  0x3a1  0x3a2  0x3a3  1
ATTRIB           0x31d  0x32c  0x33b  0x34a  15     DCC_BASE_EXT    0x3a8  0x3a9  0x3aa  0x3ab  1
DCC_CONTROL      0x31e  0x32d  0x33c  0x34b  15     ATTRIB2         0x3b0  0x3b1  0x3b2  0x3b3  1
CMASK            0x31f  0x32e  0x33d  0x34c  15     ATTRIB3         0x3b8  0x3b9  0x3ba  0x3bb  1
FMASK            0x321  0x330  0x33f  0x34e  15
CLEAR_WORD0      0x323  0x332  0x341  0x350  15
CLEAR_WORD1      0x324  0x333  0x342  0x351  15
DCC_BASE         0x325  0x334  0x343  0x352  15
```

**A trap the change created and caught before committing.** The context stream's copy was
`memcpy(cx, cmd_buffer->target_registers, sizeof(cmd_buffer->target_registers))` while the
reservation above it stayed `PS5VK_TARGET_REGISTER_COUNT` records. With a row per target,
that `sizeof` is four rows and the copy would have written three of them past the space
reserved -- the exact arithmetic R6's heap corruption lived in, and silent by construction.
It is `sizeof(cmd_buffer->target_registers[0])` now, with the comment saying why and naming
the loop that will multiply the reservation and the copy together.

**Evidence**: `tools/check-driver.sh` PASS (every test, three modes, every `compare-run` case
identical to its golden -- so row 0 is byte-for-byte the offsets the frames already ran with);
`build/gates.sh` PASS; `make lint` PASS; the standing list **12 of 12** on the console (`Klog_Logs/standing-list.log`,
run_start pid 115 -- the first draft of this line said 142, and the log is what says
otherwise; the console's run counter restarted since the earlier cycles), driver sha256
`134e9564…`. The only FAILs in that log are
the two refusals the harness expects.

**Gap left open, with its mechanism: the draw's per-attachment programming.** The next commit
makes the begin-rendering path loop over the rendering's attachments (fill row *i*, track
attachment *i* in the target dynarray), multiplies the stream reservation and the copy by the
attachment count, deletes the `colorAttachmentCount > 1` refusal in favour of one naming
`VkPhysicalDeviceLimits.maxColorAttachments` read from the reported limits, and adds the
probe: attachment counts 1, 2 and the maximum, each attachment a distinguishable value, every
one read back, with R6's case in the battery. Nothing is claimed until that runs.

## Step 1b, second pass — the two reads, both negative, and one new positive

**Read 1: the clear path is general.** `ps5vk_CmdClearAttachments` hands the runtime's
`vk_meta_rendering_info` to `vk_meta_clear_attachments` (`driver/ps5vk_draw.c`), and
`cmd_buffer->render` records **every** attachment's format and write mask when the rendering
begins, so the clear is not a single-target path. The inference it was meant to check
therefore stands: attachment 1 reading zero -- with a coloured clear that also never reaches
it -- is evidence about the *registers*, not about the clear.

**Read 2: the consumer takes a count, not a fixed sixteen.**
`sceAgcDcbSetCxRegistersIndirect(&command, cx, cx_count)` receives the whole block with
`cx_count`, which `fixed` now computes from `colour_attachment_count * PS5VK_TARGET_REGISTER_COUNT`;
the draw-packet budget (`PS5VK_DRAW_MAX_WORDS`) bounds the *packets*, not the context
records, and does not change with the attachment count. So the producer and the consumer
agree: rows 1..N are reserved, copied and consumed. The asymmetry the brief predicted
between two halves of this change is **not** there.

**One new positive: the offsets for targets 1..3 are confirmed by the device itself.** The
console run that read attachment 1 as zero produced **no** "AGC's context defaults lack a
colour target register" refusal, so `ps5vk_default_register()` found records at `0x327`,
`0x336` and `0x345` in **AGC's own default table** -- which is a context-block layout, not a
register-address list. That independently confirms what step 1a derived from
`amdgfxregs.h`'s gfx103 rows: the context offsets of the second, third and fourth targets'
`CB_COLORi_BASE` really are those dwords. (The host model's replay is what lacked them, and
that is why it refused where the console did not.)

**So the chain stands as: four of five links in hand, one unmeasured.** The shader requests
four exports (`SPI_SHADER_COL_FORMAT 0x9999`, four live nibbles); the harness builds N images,
views, attachments and a pass that names them; the driver fills N register rows from the
device's own offsets; the stream reserves, copies and hands over N counted rows. What is
still unverified is that those rows, once handed over, make the hardware write targets past
the first -- the AGC/hardware semantics of `CB_COLOR_CONTROL`'s MODE, a per-`CB_COLORi_VIEW`
field, or a requirement that the export word travel in the same block.

**The measurement that settles it, and where it stopped.** The host model records the words
the driver queues, so a frame drawn with the refusal lifted can be dumped
(`PS5_HOST_SUBMISSION_DUMP`) and the target records decoded straight out of it -- no console
cycle, seconds of work. The attempt ran out of this round's budget before producing the dump
(the driver archive had not been rebuilt when the test linked, so the frame was still
refused); the tree was restored to the committed state, and the parked
`driver/tests/vk_v0_mrt_test.c` is where the assertions for the flipped expectation already
live. That dump is the first step of the next attempt, and it decides between the three
remaining shapes rather than guessing among them.

## Step 1b, third pass — the dump answered, and the register field it pointed at

**The dump was made, with its precondition checked.** The host model records the words the
driver queues, so the frame was drawn with the refusal lifted **after verifying the artifact**
(the refusal string absent from the archive the test would link) and the run's rows were read
straight out of the test's own report:

```
attachment 0: CB_COLOR_BASE offset 0x318 value 0x2004000, its mapping 0x2004000
attachment 1: CB_COLOR_BASE offset 0x327 value 0x2024000, its mapping 0x2024000
PASS each attachment's row names its own CB_COLORi_BASE register
PASS each attachment's row carries that attachment's own address
```

**That is the first branch of the decision tree: the producer and the stream are right, and
the fault is downstream.** Rows present, correctly offset (0x318 then 0x327 -- the column
step 1a derived), each carrying its own attachment's address.

**The downstream fault was then found by reading, not guessing.** `CB_COLORi_BASE_EXT`'s
field is `S_028E40_BASE_256B` -- bits 0-7 of the address's bits 40-47, the half
`CB_COLORi_BASE`'s 32 bits (`address >> 8`) cannot hold, the same split the depth target
already programs (`ps5vk_depth_registers`, `DB_Z_READ_BASE_HIGH`). The driver left that
register at AGC's default, which describes **target 0**, so a rendering into a second
attachment wrote at an address made of one target's low half and another's high half -- an
address belonging to neither image. It now programs `(address >> 40) & 0xff` per target.

**What that changed on the console, measured:** the title no longer wedges after the probe's
frame (the earlier battery stalls -- killed by PID, twice -- were this fault), and the run
continues through all three attachment counts. The readbacks still do not match, and **this
round cannot say which attachment or which word**, because the probe's own instrument was
the next thing wrong: it stopped logging at the first mismatch and reported "the rendering
neither drew nor was refused" where it meant "attachment 1 holds the wrong word". Every
attachment is now logged **before** any is judged, so the next run names the value.

**Two artefacts worth recording.** The probe's package carries the exports the shader asks
for: `SPI_SHADER_COL_FORMAT 0x9999`, four live nibbles at the legacy 32_ABGR export. And the
last console measurement was made against a build whose refusal was lifted while the source's
refusal was still present-but-disabled (`if (false && ...)`): the string grep found in the
source and the absence of it in the binary were consistent, not a stale artifact -- but it is
worth saying that the *first* reading of those two numbers was "the deploy is stale", and it
was wrong. `strings` on the built library is the check that settles it either way.

**Capability still unclaimed, refusal live again**: the writes past the first attachment do
not land yet, so a rendering into more than one is refused by name and `v0-mrt` measures the
interim. The host half `driver/tests/vk_v0_mrt_test.c` runs again -- against the replay of the
probe's *own* console capture, because its shader's register tables need that stage mapping --
and its two row assertions pass.

**Gap left open, with its blast radius and its next measurement.** The per-attachment writes.
Blast radius: MRT applications get a named refusal instead of a wrong frame. Next: one console
cycle with the instrument-fixed probe, which names the attachment and the word; then whichever
of `CB_COLOR_CONTROL`'s MODE, a per-`CB_COLORi_VIEW`/`ATTRIB2`/`ATTRIB3` field, or a
per-target `CMASK`/`FMASK` word the values point at.

## R2 — reconnaissance, and it is smaller than my own notes assumed

**The compiler does not need a new descriptor type for the separated form.**
`legacy_texture_bindings_valid` (`build/sdk-fork/assembled/libpsbc/psbc_compile.c:2062`) walks
every `nir_tex_instr` and, for **each half** of a texture instruction, requires a binding whose
type is `PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER` at
`key = sampler ? tex->sampler_index : tex->texture_index`:

```c
unsigned key = sampler ? tex->sampler_index : tex->texture_index;
found |= !binding->set && binding->binding == key &&
         binding->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER;
```

So a `texture2D` at one binding and a `sampler` at another are already expressible: tell the
compiler **combined** at both indices, at the 48-byte stride the driver already writes. Each
entry is then a complete combined descriptor of which the instruction reads its own half -- the
image words from the `SAMPLED_IMAGE` binding's entry, the sampler words from the `SAMPLER`
binding's entry -- which is what makes the acceptance ("the separated frame is texel-for-texel
the combined frame") the right test rather than a new-type test.

**What that leaves, all driver-side:**

1. `ps5vk_descriptor_stride` (`driver/ps5vk_descriptor_set_layout.c`): `SAMPLER` and
   `SAMPLED_IMAGE` → the combined 48-byte entry, `INPUT_ATTACHMENT` → the 32-byte image
   descriptor (the same shape `STORAGE_IMAGE` already has: an input attachment read is a fetch
   of an image, and the *reads* stay their own conversation with subpasses).
2. `ps5vk_descriptor_options` (`driver/ps5vk_pipeline.c`): report those three to the compiler as
   the types it already has (`COMBINED_IMAGE_SAMPLER` for the first two,
   `STORAGE_IMAGE`'s 32-byte form for the third), so the layout a Vulkan application writes is
   one the compiler accepts. The check that refuses a binding whose stride is 0 stays: it is
   what reports the next absent type by name.
3. The write path (`driver/ps5vk_descriptor_set.c`, `driver/ps5vk_draw.c`): a `SAMPLER` write
   fills its entry's sampler words, a `SAMPLED_IMAGE` write fills the image words and a default
   sampler beside them (Vulkan's own default sampler state for the half the instruction does not
   read), and an `INPUT_ATTACHMENT` write fills the image words alone.
4. The probe: the port's GUI shape -- set 0 binding 0 `SAMPLED_IMAGE`, set 1 binding 0
   `SAMPLER`, fragment stage, one texture, nearest -- and **one extra frame through a single
   `COMBINED_IMAGE_SAMPLER`** of the same texture, compared texel for texel. A frame that merely
   draws is not evidence here; the identity is.

**The ordering the port asked for is kept**: the separated pair first as one mechanism, then
`INPUT_ATTACHMENT` as a type (which is what unblocks its `basic_alphatest` pipeline, since the
check walks the layout rather than the shader's used bindings), with subpass reads and MRT each
their own rounds.

**Gap left open at this record's writing**: none of 1-4 is implemented yet. The next action is
the driver table and the write path, then the probe's two frames on the console -- and the
compiler question this reconnaissance settled is the one that could have made R2 a compiler
project, so it is recorded before the code rather than after it.

## 2026-09-22 — R8, R6's correction, R4 and the fragment-less pipeline (vkQuake port, `PS5_VULKAN_REQUESTS.md`)

Implemented and host-gated in four commits; **no console run yet**, so nothing here is claimed.
One run of `jobs/r8-lines/queue.txt` (`v0-lines`, `v0-strip`, `v0-fragmentless` and the
regression) decides all three drawing items.

| Request | Status | Commit | Console case | Host gate |
| --- | --- | --- | --- | --- |
| R8 `LINE_LIST` | implemented, unclaimed | `6d000b7` | `v0-lines` | `v0_topology` 35/35, B6 24/24 |
| R6 strip, correction | fixed, unclaimed | `15878c3` | `v0-strip` | `v0_topology` |
| fragment-less pipeline | case written, unclaimed | `bfbfc32` | `v0-fragmentless` | `v0_fragmentless` 9/9, B6 |
| R4 swapchain assert | closed (reporting, no draw) | `1414853` | -- | C1 present 16/16 |

**R8.** A line list is linked as DI_PT_LINELIST 2, compiled with the topology (the NGG vertex
stage exports two vertices a primitive, not three), and its draw records VGT_GS_OUT_PRIM_TYPE
LINESTRIP, PA_SU_LINE_CNTL width 1.0 and PA_SC_LINE_CNTL 0 (Vulkan's non-strict lines). A line
pipeline ignores cullMode, as Vulkan says it must. `wideLines` stays unclaimed: a static
`lineWidth` other than 1.0 is refused naming it; a multisampled line list is refused until one
is measured. For the port: `debug_lines` and `md5_debug` will create against this archive; the
two edits in `vkquake-edits.py` can retire once `v0-lines` passes.

**R6, a correction to our own record.** The parked strip case did not wedge the GPU: the harness
aborted on a zero-size index buffer before any frame recorded, and the "hang" was the title
gone. Behind it, the driver linked the strip as DI_PT **5**, which is the triangle **fan**; the
strip is 6. Your `warp` pipelines were created against the fan value -- a water frame drawn
with the old archive would have been a fan. Relink before you draw water.

**Fragment-less.** vkQuake's `sky_stencil` creation was yours; the frame is ours to prove: a
vertex-only pass leaves the colour target exactly the clear, writes depth 0.5 and stencil where
it rasterises, and a later EQUAL test passes only there.

**R4.** A swapchain the surface does not allow returns `VK_ERROR_UNKNOWN` with a sentence
beginning with the field -- `imageUsage`, `imageExtent`, `presentMode`, `minImageCount`,
`imageFormat`, `imageColorSpace`, `imageArrayLayers` -- and so does a plane surface with the
wrong extent or plane.

**The archive.** Rebuild `build/driver/ps5/libps5vk.ps5.a` on the reference host before
relinking; the one there predates these commits.

## 2026-09-22 — R9: specialization constants, and the `-13` the port stopped on

**The refusal that could stop a world pipeline was ours.** `ps5vk_compile_stage` (and its
compute counterpart) carried

```c
if (stage && stage->pSpecializationInfo && stage->pSpecializationInfo->mapEntryCount != 0)
   return vk_errorf(device, VK_ERROR_UNKNOWN, "specialization constants are not supported");
```

so every pipeline stage that named a non-empty `VkSpecializationInfo` was refused. That is a
Vulkan 1.0 core feature, which this driver advertises, so the refusal was a gap rather than a
boundary. It is gone; the sentence is not in the rebuilt archive.

**One caveat, because the port's log alone cannot settle it.** `-13` is `VK_ERROR_UNKNOWN`, the
code *every* refusal in this driver returns, and vkQuake prints the code and not the sentence, so
nothing captured here names `pSpecializationInfo` as the world pipeline's cause. It is the likely
one -- this was the only refusal a world draw's stages could reach that this batch changed, and
the code that returned it is exactly `VK_ERROR_UNKNOWN` -- but it is an inference, and the port's
next run is the check. If it stops again, the world-pipeline message is what the next request
should carry; there is no reason for a second round of guessing. The probe below builds five
pipelines from one module for the same reason: the feature is measured here, not assumed from
the port's symptom.

**What was actually missing, and where.** The driver was never the blocker: RADV's
`spirv_to_nir` applies specialization values already. The fork of the AGC shader compiler under
`tooling/psbc/` had a *stub* in its Mesa copy of `vk_spec_info_to_nir_spirv` and no way for a
caller to pass the entries, so an application's `constant_id` values had nowhere to arrive.
`tooling/psbc/patch-specialization.py` (four edits, metadata version stays 14) adds
`PsbcSpecializationEntry` with `VkSpecializationMapEntry`'s own layout and four fields to
`PsbcCompileOptions`, validates them, sets `stage->spec_info` before the main lowering and
resets it after, and installs Mesa's real `vk_spec_info_to_nir_spirv` (renamed in the driver's
copy, so the runtime keeps its own symbol). The driver's half is
`ps5vk_specialization_options()`: a static assertion that the two entry layouts match, a refusal
**by name** for a malformed `VkSpecializationInfo`, and the application's arrays handed over --
called from `ps5vk_compile_stage` on a copy of the options and specializing SPIR-V stages only,
never Mesa's meta stages, and from the compute path the same way.

**Measured, not inferred** (`v0-r9`, `probes/v0-spec`, `Klog_Logs/r9-spec.log`, pid 178). One
module -- a `constant_id` bool selecting the red channel and a `constant_id` int selecting green
(passing 1 gives 64/255, passing 2 gives 128/255) -- built into five pipelines, every frame read
back exactly:

| set | readback | what it proves |
| --- | --- | --- |
| none | `0xffff0000` | the shader's own default is what compiles |
| `level = 1` | `0xffff4000` | one constant of two changed, and only its channel |
| `tint_red` + `level = 2` | `0xffff80ff` | both constants took, in one stage |

`"3 of 3 constants' sets drew the colour they select and 2 of 2 invalid sets were refused"`.
The words are the constants' *values* in the readback's byte order (R 0/0/255, G 0/64/128,
B 255), so each frame says **which** set arrived rather than merely that two frames differ. The
two invalid sets -- an entry reading past `pData`, and entries with no map -- were refused rather
than silently compiled with the shader's defaults. The compiler-side half is
`driver/tests/vk_v0_spec_test.c`: the same module with no entries, with those values, and with a
hand-written twin of the literals gives 40, 48 and 48 bytes of machine code, 4 of 4 checks.

**For the port.** Nothing to change: recreate the world pipelines and they create. `constant_id`
values now reach the shader; before, a pipeline that named them did not exist at all. The one
behaviour to know is that a malformed `VkSpecializationInfo` is refused with
`VK_ERROR_UNKNOWN` and a sentence naming the stage and the entry (this is validation Vulkan asks
for), so a wrong `offset`/`size` in the application's own map entries is reported rather than
ignored.

**Honest limits.** Specialization is applied to stages the driver compiles from SPIR-V. Mesa's
own NIR meta stages (blit, clear, resolve, the empty fragment stage) are compiled without
entries, which is correct -- nothing hands them a `VkSpecializationInfo`. The map entries are
read once, at pipeline creation, as Vulkan requires (`vkCreate*Pipelines` copies what it needs;
the application may free `pData` afterwards). There is **no pipeline cache** yet -- its shim is a
zero-byte stub -- so two pipelines from one module with different values are two compiles, which
is also why this cannot be silently wrong today; when a cache lands, the constants' values belong
in its key, as RADV hashes them. That is written beside the helper, not left for the cache's
author to rediscover.

## 2026-09-22 — R10: the subpass read, the refusal, and what your other twenty shaders do

Three answers, all measured on this host except where a console run is named. The archive is
rebuilt at the end of this section with its digest.

### 1. The read: not a permanent limit. Do not delete your gamma/contrast pass.

The question was whether this fork can lower a subpass read at all, and the answer is yes, through
the path RADV uses when a driver binds input attachments as descriptors. The fork's front end
called `nir_lower_input_attachments` with `.use_ia_coord_intrin = true`, which is the tile
coordinate intrinsic its ACO has no case for; with that option false the same pass computes the
coordinate from the fragment's position and layer and reads the texel **through the input
attachment's own descriptor**, which is what this driver already writes for such a binding. One
field, `tooling/psbc/patch-subpass-input.py`.

Measured on your own shader, before anything was written: `postprocess_frag`, taken out of
`build/vkquake/generated/postprocess_frag_spv.c`, aborts the stock compiler on this host exactly as
it did on your console (exit 134, the same `@load_input_attachment_coord`) and compiles to a 324-byte
package with the patch, metadata `{"set": 0, "binding": 0, "type": 4, "stride": 32}` -- the 32-byte
image entry this driver builds. `wboit_resolve_frag` and `mboit_resolve_frag` compile the same way.

On the console, `jobs/r10-subpass/queue.txt`'s `v0-subpass` case runs the shape your UI pass has: a
two-subpass render pass, subpass 0 drawing a positional band pattern into attachment 0, subpass 1
reading it with `subpassLoad` and writing attachment 1, which the case reads back. Subpass 0's
attachment is right in its own memory, 16 of 16 samples; subpass 1's draw **reads through the input
attachment's descriptor** -- 4 of 16 samples are exactly the writer's, and they are the first
quarter-width. So the binding, the subpass lookup, the table entry and the flush all work.

**What is still wrong, named precisely**: every texel past x = 960 reads as band 0, i.e. the read
behaves as if the attachment were 960 texels wide -- `SQ_RSRC_IMG_WORD2`'s width field,
`(extent.width - 1) >> 2`, 959 for a 3840-texel image. The image is row-stored (the writer's own
mapping is linear), and the descriptor path was only ever proven on tiled images. That is a
descriptor question about a row-stored attachment, not another subpass question, and it is the next
thing on this side.

### 2. The abort is a refusal now, and it fired on your console

`ps5vk_spirv_refusal` runs before the compiler: the module's addressing model and its declared
capabilities are read from the SPIR-V and the two measured cases are refused by name -- the
`PhysicalStorageBuffer64` addressing model, which the front end itself rejects, and
`SpvCapabilityPhysicalStorageBufferAddressesEXT` (4472), whose ACO has no case for the
`@bindless_image_store` the shaders using it reach. Underneath, `ps5vk_compile_worker` guards the
compile with `SIGABRT`/`SIGTRAP`, so a shader neither the table nor anyone else knows about still
comes back as `VK_ERROR_UNKNOWN` with the sentence *"the shader compiler aborted on this shader
instead of returning a result ... (the shader declares ...)"*.

You can see it working in the port's own terms: the first console run of the round still had the
unpatched compiler in the archive, and the log shows the guard's sentence where your run showed a
dead title. `driver/tests/vk_v0_capability_test.c` is the acceptance you asked for, runnable here:
five modules built by injecting one instruction into a probe's pixel SPIR-V -- created as it stands;
refused by name **with no `SPIR-V` text on stderr at all** when 4472 is added, which is what says
the compiler never ran; created with a warning when 46 or a logical-addressing 5347 is added;
refused by name when the addressing model is `PhysicalStorageBuffer64`; and for an unknown capability
the front end fails on, the guard's sentence with the compiler's own `SPIR-V parsing FAILED` on
stderr and the process still alive.

### 3. Your twenty shaders, checked against this compiler on the host

Every deployed shader with a capability beyond `Shader`, compiled here with its own descriptor
declarations. **Fifteen compile. Six do not, and they are the two classes above**:

| shader | capabilities | verdict |
| --- | --- | --- |
| `postprocess_frag` | 40 | **compiles** (with the patch above) |
| `wboit_resolve_frag`, `mboit_resolve_frag` | 40 | **compile** |
| `wboit_resolve_msaa_frag`, `mboit_resolve_msaa_frag` | 35, 40 | **compile** |
| `draw_pic_xbr_frag`, `draw_pic_xbr_alphatest_frag` | 50 | **compile** (`textureSize` lowers; the warning is not a failure) |
| `screen_effects_{8,10}bit{,_scale}{,_sops}_comp` (6) | 46, 49, 61, 65 | **compile** |
| `update_lightmap_{8,10}bit_comp` | 49 | **compile** |
| `skinning_comp`, `skinning_8_comp`, `mesh_interpolate_comp` | 5347 | refused by name: `AddressingModelPhysicalStorageBuffer64 not supported` |
| `ray_debug_comp`, `update_lightmap_{8,10}bit_rt_comp` | 4472, 49 | refused by name: ACO has no `@bindless_image_store` |

**A correction to your scanner**, which will matter if you act on its labels: the numbers it prints
are right and four of the names are not. 35 is `SampleRateShading` (not ImageGatherExtended), 46 is
`SampledBuffer` (not StorageImageWriteWithoutFormat, which is 56), 49 is `StorageImageExtendedFormats`
(not GroupNonUniform, which is 61), and 61 is `GroupNonUniform` (not GroupNonUniformBallot, which is
64). The two that matter for your worry list are the ones you were least worried about: nothing in
your storage-image or subgroup set is missing, and the menu upscaler's `ImageQuery` lowers.

### The archive

Rebuilt on this host at the state of `5f7e910`:

```
build/driver/ps5/libps5vk.ps5.a   14 415 958 bytes
sha256 6e12550bd241c1a0ce87f7ea8319c60747c18a023fc64f71b95a9fbbd63b76f9
```

Relink and the world pipelines should keep creating where `-13` used to be, your UI pass's
`postprocess_frag` should compile and its second subpass should read its offscreen attachment -- with
the x-scale defect above still in it, which is why this reply does not claim the frame. When it is
fixed, this section gets an entry rather than a new number.

## 2026-09-22 — vkQuake R11: named recording refusals and optional framebuffer

| Request | Result | Evidence |
| --- | --- | --- |
| Name the first-frame refusal | Host B8 reproduces -13 for the same NULL inheritance framebuffer; recording errors now print caller and reason to stderr without requiring a debug messenger | `driver/tests/vk_b2_commands_test.c`, full driver check PASS |
| Accept framebuffer-free secondaries | Mesa's owned command queue defers encoding to the primary's subpass; B8 readback PASS on PID 194 | `jobs/r11-secondary/queue.txt`, `golden/r11-secondary/` |
| Preserve existing rendering | C1 4/4 frames presented/read back and C4 RTT pixel checks PASS; eleven new streams replay identically, existing goldens unchanged | `golden/r11-secondary/host-replay.txt` and its README |
| vkQuake first frame | Still requires the port's own run and on-screen confirmation; not claimed by these probes | Port `docs/ACTIVE.md` |

Archive: `build/driver/ps5/libps5vk.ps5.a`, 14,383,172 bytes,
SHA-256 `65550cae897ee2fab14224d07b7cf6766e986be21c9e5ba81359b0a0535c75ce`.
The R10 quarter-width read defect and the hardware line failure are unchanged.
The phase log separately records the host loader's deferred surface creation and
the resulting correction to C1's direct-only surface-refusal assertion.


### 2026-09-22 — R11 port result and R12 pitch request

| Request | Result | Evidence / next witness |
| --- | --- | --- |
| R11, actual vkQuake boot | Negative diagnostic criterion met; positive presentation still open. PID 195 reached 540 successful compiles and a named padded-row refusal, no frame. | Port identity `6b437103…`, `../PS5_vkQuake/evidence/m2-texture-row-pitch/`; runner PID 194 remains the secondary replay proof. |
| R12, 32-wide sampled image with 256-byte row stride | Unverified candidate parked; no deployment. Initial host PASS used the old archive and was incorrectly described as validating new code; explicit build failed on ALIGN. | Separate correction in M5_PHASE_C.md. `parked/r12-row-pitch/` records exact patch, queue, source witness and resumption plan. Mission stop rule applied. |


### 2026-09-22 — R12 resumed response

| Request | Result | Evidence |
| --- | --- | --- |
| R12, padded 32-wide sampled image | Driver hardware criterion PASS: both nearest and bilinear frames pass beside the tight 64-wide baseline. Explicit rebuild and all gates pass. Padded mip/array layouts remain guarded. Port acceptance awaits relink/boot. | PPSA99988 PID 196; golden/r12-pitch/README.md; jobs/r12-pitch/queue.txt; archive c37afdec… |


### 2026-09-22 — R12 port acceptance and R13

| Request | Result | Evidence |
| --- | --- | --- |
| R11/R12 actual port first frame | PASS: QueuePresent success and human sees Quake menu/console; M2 met. | Port 3c29641, PID 197, a779b2bd…, evidence/m2-first-frame |
| R13 bounded upload metadata | Open: named host-memory refusal during map staging; one record per row is the source candidate. Failed EndCommandBuffer is then ignored by upstream staging and triggers submit assertion. | Same boot; host record-count/byte witness is next, no fix claimed. |


### 2026-09-22 — R13 bounded upload metadata response

| Request | Result | Evidence |
| --- | --- | --- |
| R13 bounded row-upload metadata | Driver criteria PASS: one record per region; 64 uploads use 17,408 bytes; copy/format host tests and PS5 texture/mip/copy pixels pass. Port heap outcome still pending. | PID 198; jobs/r13-upload/README.md; golden/r13-upload/README.md; archive b3bb7ac9… |


## 2026-09-22 — R13 port result and user cache priority

Port PID 199, identity 28581900…, 540 successful compiles and QueuePresent=0.
Map lightmap/indirect/visibility allocations pass the prior OOM. New failures:
tiled-chain blit; one descriptor set with two dynamic offsets; indirect draw
stride assertion (upstream single draw uses zero stride). Two FTP reads match,
SHA-256 193e40957f7cc0c786ce01d54bd344729694a10eba0f52c933bfd5a66fa9a36e.
Kernel PID 199 abort/termination; console idle. Port committed evidence is
evidence/m2-r13-map-recording. These failures remain open. User reprioritized
persistent shader caching and measurable faster warm startup before rendering
work continues. No new shader-cache implementation is claimed by this entry.


## 2026-09-22 — Persistent shader cache accepted

Same port identity 78bd43a2e575089a96cf8dc561937dd7781c462fbcf051f2fa177ac0c55107b1:
PID 202 cold launch-to-first-present 30.410 seconds, 99 SPIR-V compiles,
433 cache hits, 99 stores. PID 203 warm 13.018 seconds, zero SPIR-V compiles,
532 cache hits, zero stores. Both compile eight internal NIR shaders. Times
include launch IPC and one-second trace polling, not display scanout. Each
run has a listener before launch, newest-boot identity check, two identical
final trace reads and idle closure. Paired measurement script and exact hashes:
jobs/shader-cache/README.md, cold-startup.txt and warm-startup.txt. Cold run
crashed after presentation; warm hits prove its saved shaders survived.

The same two map-recording refusals and ps5vk_cmd_draw_indirect stride assertion
repeat (already PID 199). No M3–M6 acceptance or rendering repair is claimed.
The pair was the user's cache experiment; with it complete, follow the user's
repeat-failure stop rule rather than start another rendering experiment.

Implementation and gates are recorded in M5_PHASE_C.md and jobs/shader-cache/README.md.

## 2026-09-22 — R14 single-draw stride accepted

User resumed M6 rendering work. Count=1/stride=0 now records in the shared
indirect helper; multi-draw retains its stride checks. Six host/link arms,
eleven gates, port five gates and template relink PASS. PS5 PID 204: 121 PASS,
zero FAIL, pixel readback PASS, one exact replay; closed. Evidence and commands:
jobs/r14-indirect-stride/README.md, golden/r14-indirect-stride. No port retry;
dynamic-offset and tiled-chain refusals are the next named gaps.

## 2026-09-22 — R15 independent dynamic UBO offsets accepted

PS5 PID 205: 196 PASS, zero FAIL. Corrected scalar D1 and the new pair/static
probe pass all four frames; four streams replay exactly. Full host run had
165/167 passing arms; debugger found a null set in the new binding walk, fixed
before deployment. Final nine D1/subpass/present arms and eleven gates PASS;
port five gates and template relink PASS. Title closed. Implementation,
readback, hashes and reproduction: jobs/r15-dynamic-pair/README.md and
golden/r15-dynamic-pair. No port retry; the named water mip blit remains.

### Correction: earlier D1 test wrote a mismatched descriptor type

The old shared harness wrote UNIFORM_BUFFER into its dynamic-UBO layout.
Those runs established address-offset pixels/streams, not acceptance of a
correctly typed dynamic write. R15 corrects the harness and the driver's type
check and re-runs D1 on PS5; old streams still compare unchanged. This is a
coverage correction; earlier evidence and log entries remain unmodified.


## 2026-09-22 — R16 corrected candidate parked after contradictory readback

PID 206 failed level-3 full-frame readback: 7,776,000/8,294,400 matching pixels,
while the old additive CPU witness passed. Separate entries in HARDWARE_FINDINGS.md and M5_PHASE_C.md
narrow the earlier C7 claim. The corrected shared XOR tail addressing and
actual-written-range flush pass whole-chain AddrLib queries (zero mismatches
across 87,296 and 349,184 texels) and independent shifted-coordinate CPU checks
for all 87,040 lower texels. Ten PM4 streams still replay exactly; that is not
corrected hardware pixel acceptance.

Final explicit candidate build: 14,428,778 bytes, SHA-256
8d5206d5d4fc1535c342916b71c81e57d62ae4086d14fcd993074bc4c2fc8c67.
Fifteen targeted host/link arms, cache package tests and all eleven gates PASS;
port five gates and template relink PASS. Host regression cache enabled during
check-driver. Metadata stress: 64 records, 18,432 bytes, below 32 KiB.

The user's mission requires stopping when a run contradicts an earlier claim.
Corrected code is parked in parked/r16-mip-tail.patch against b838832, with
jobs/r16-mip-blit/README.md and validation artifacts; failed hardware evidence
is golden/r16-mip-blit-before. Restore accepted R15 source/archive and relink
dependents before checkpointing. No corrected-candidate or port launch; console
idle. Next authorized cycle must prove all four mip levels on PS5 before port
relink/deployment. M6 remains open.

Checkpoint restoration completed: explicit accepted-base build is 14,427,682
bytes, SHA-256 055c7c6c1ff47829fcb3c294cc4d8bd758529a0c2dea8f238e3347ed11c17811.
Port all five gates PASS (23 captures, zero failures), identity
de7a813a51722193a7c2fb7754afda8285ec8f23c73b9e98230316cf68e6ce6c; template relink PASS
(pre-existing unused audio helper warnings). Neither was deployed or launched.
Exact restoration identities: jobs/r16-mip-blit/restored-base.txt.


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


## 2026-09-22 — R16 vkQuake consumption, PID 208

Port identity b6a1e9540a03996b94a42346de7e0868fb339b883ea0ddab6e84d1407c6f126f
links accepted driver c7f6f95; port five gates and shader scan PASS, two deployed
ELF reads match all PT_LOAD segments. PID 208 presents and reaches the Necropolis
map recording. Previous tiled-chain blit, dynamic-offset and indirect-stride
failures are absent. New named refusals: set 0 binding 2 holds three descriptors;
set 0 binding 0 needs a padded pitch of 256 texels beyond current custom-pitch
coverage. EndCommandBuffer returns -13; Quake exits 1 and takes the known SIGSYS
exit path. No stable-map/input/audio acceptance.

The lightmap compute layout declares three sampled images at binding 2. R17
must implement shared per-element storage/write/copy/emission and prove distinct
entries on host and PS5; removing its guard alone is insufficient. R18 first
needs the padded image's extent/format/mips/layers, absent from the refusal,
then a measured descriptor/layout probe. Do not assume which condition failed.
Port docs/PS5_VULKAN_REQUESTS.md records the precise next witnesses.

Two final trace reads agree: SHA-256
49f3bf8d243b8a07d8b9dcc63afd757bff1656f9a36bd334bc32e030c22a5fd4;
kernel PID 208 matches. Two-minute harness finishes count=0, console idle.
Evidence: ../PS5_vkQuake/evidence/m2-r16-map-recording (24 captures, zero
replay failures). On-screen question pending; no new visual acceptance claimed.


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


## 2026-09-23 — R18 image shape identified, vkQuake PID 210

Added image extent/format/mips/layers/view/name to the existing padded-pitch
refusal. Explicit rebuild, eleven gates, port five gates/scan and template
relink PASS; two deployed ELF reads match all PT_LOAD segments. Port identity
cd1b39c8e36e63f45a896f21399b0d542e08a384831512a4f9a356e3e6fca3da,
PID 210 presents and no longer refuses the descriptor array. R18 names a
224x195x1 VK_FORMAT_R8G8B8A8_UNORM (37), eight mip levels, one layer, 2D view,
padded pitch 256 texels. This is the exact shape the next probe must measure.
No layout fix is claimed. Port evidence/m2-r18-shape has two matching final
reads and PID-correlated exit 1/SIGSYS; the bounded run ended count=0.

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
