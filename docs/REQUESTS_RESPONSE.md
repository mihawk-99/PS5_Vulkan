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
