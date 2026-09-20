# AGC entry points the console's titles import

Stable reference. The `sceAgc*` entries a real PS5 title imports, which is the
candidate list for a probe: a name that does not exist fails a title's load and
names at most the first unresolved symbol, so a good list is worth real money
and a speculative one costs a launch per batch.

## Where the list comes from, and why it is trustworthy

The names come from the NID table of the KytyPS5 emulator
(`src/libs/libAgcDriver.cpp`, `LIB_FUNC("<nid>", <function>)`). NIDs are the
guest import identifiers: a hash of the name, and this repository already
implements that hash (`tooling/native/libc_builder.cpp`, `compute_nid`). Taking
their NIDs and computing the name each one hashes to therefore *proves* the
name; it also catches their own labels, which are wrong for 15 of their 156
entries. Kyty is GPL-2.0, so nothing is copied from it: names and NIDs are
interface facts, and the names are derived here with this repository's own
code. Their implementations are read-only reference, and their GPU model is a
hypothesis that a console probe has to settle like any other.

The whole table decodes to 141 names; the ones below are the entries that
matter to the steps this project has open. Each is `sceAgc<label> <nid>`.

## The event queue: how a title learns a flip happened

This driver polls VideoOut's flip status after submitting a flip
(`ps5vk_queue_flip`). A title instead asks AGC for an event queue, and the
driver's own flip helper is the packet-level form of one submission.

| Entry | NID |
| --- | --- |
| `sceAgcDriverAddEqEvent` | `w2rJhmD+dsE` |
| `sceAgcDriverDeleteEqEvent` | `DL2RXaXOy88` |
| `sceAgcDriverGetEqEventType` | `5CdQTZIQPxM` |
| `sceAgcDriverGetEqContextId` | `Zw7uUVPulbw` |
| `sceAgcDcbSetFlip` | `YUeqkyT7mEQ` |
| `sceAgcDcbEventWrite` | `aJf+j5yntiU` |

`P-flip-event` in `M5_REFERENCE.md` is the step that would replace the poll
loop with those events.

## Submitting more than one stream at a time

| Entry | NID |
| --- | --- |
| `sceAgcDriverSubmitAcb` | `gSRnr79F8tQ` |
| `sceAgcDriverSubmitCommandBuffer` | `b4fpgH5ZXxQ` |
| `sceAgcDriverSubmitMultiAcbs` | `HF3YllT3mXU` |
| `sceAgcDriverSubmitMultiCommandBuffers` | `Fj7r9EHzF38` |
| `sceAgcDriverSubmitMultiDcbs` | `6UzEidRZwkg` |

This driver submits one DCB per submission (`sceAgcDriverSubmitDcb`); a title
that queues several command buffers or indirect buffers in one call uses these.

## Dispatch

| Entry | NID |
| --- | --- |
| `sceAgcCbDispatch` | `k3GhuSNmBLU` |
| `sceAgcDcbDispatchIndirect` | `CtB+A9-VxO0` |

`V0-compute` hand-built `DISPATCH_DIRECT` because the recorded helper surface
had no dispatch entry; `sceAgcDcbDispatchIndirect` is the helper form for a
dispatch whose grid comes from memory, which D2's Vulkan compute wants.

## The register defaults are versioned

| Entry | NID |
| --- | --- |
| `sceAgcGetRegisterDefaults2` | `2JtWUUiYBXs` |
| `sceAgcGetRegisterDefaults2Internal` | `wRbq6ZjNop4` |

This project calls `sceAgcGetRegisterDefaults` and reads one table of 16 target
registers with its count at `0x20` (`host/ps5/ps5_host.cpp`). Kyty's model of
the versioned getter describes four tables with the count at `0x38`, which is
probably the versioned difference and is worth a probe before anything relies
on the block's shape beyond the target registers.

## Using this list

Import the entries a step needs, in the order the step needs them: a load that
fails names its first missing symbol, so the order of the batch decides what a
failed launch tells us. The four event-queue entries and the two defaults are
the cheapest next batch; the submission family is what a title that queues
several command buffers needs.
