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
