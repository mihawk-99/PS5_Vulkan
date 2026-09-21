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
