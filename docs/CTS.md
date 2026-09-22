# Conformance testing (Phase E1)

E1 runs subsets of the Khronos Vulkan CTS against this driver on the console.
It is the gate every version rise depends on: [M5_REFERENCE.md](M5_REFERENCE.md)
rule 1 says a rise is its own commit and B2 asserts that version's
requirements, but the CTS subset is what turns the assertion into evidence.

This file is the recipe (the pin, the pieces, the pitfalls and the acceptance
policy) and the plan the work follows now that rung 1.0 is closed: "The plan:
semantic validation after rung 1.0" below.

## What is claimed, and what is not

Nothing here is a conformance claim. A CTS run is evidence for one version's
requirement list, recorded in the phase log with the tag, the case selection
and the captured log. Formal Khronos conformance is a separate submission this
project does not attempt.

## The plan: semantic validation after rung 1.0

Rung 1.0 is closed: every capability the device reports has been proven on the
console by the steps the phase logs record, and the three audits are green. This
is the next phase, **CTS-style semantic validation**: the CTS is run against the
driver so that what is advertised is not merely exercised by this repository's
probes, but *semantically correct*. The rule that shapes everything below is
that a run is judged only against the capability set the device itself reports,
and a capability is advertised only once its coverage passes.

### Selection: filter, do not hope

- Take a machine-readable dump of the device's own reporting from the *same
  build* the run uses: `VkPhysicalDeviceFeatures`, the properties and limits,
  the exposed extensions, the queue families and their flags, the sample counts,
  and the format-feature table (`vkGetPhysicalDeviceFormatProperties`, which the
  runner's V0-formats probe already walks and
  [V0_FORMATS_AUDIT.md](V0_FORMATS_AUDIT.md) records). **That dump exists now**:
  the runner's `device-report` case walks it with public Vulkan calls and
  `tools/collect-device-report.py` collects it into
  `conformance_inventory/device_report.json`, completeness-checked against the
  Vulkan headers, with `tools/check-runner-cases.sh` refusing an inventory that a
  fresh run does not reproduce. It now carries the format-feature table for all
  184 core formats and the image-format answers for twelve probed combinations each,
  which is the whole surface a case's requirements are read against.
- Select cases from the CTS's own case-list files and commit them as the run's
  manifest with its hash, so the selection is regenerable and reviewable.
- A case whose requirements name an unadvertised feature, extension, limit,
  format or sample count is **excluded by the manifest**: not run, not counted,
  and recorded with the evidence line that excludes it.
- Fix the framework's iteration count and random seed per run, so two runs of one
  manifest and one driver build are comparable.
- No command the device reports support for may refuse at run time. The commands
  still refused by name (`vkCmdWriteTimestamp`, `vkCreateBufferView`) are
  unadvertised capabilities until they are implemented; the run has to show them
  as a *reporting* gap, never as a runtime refusal.

### Order: the smallest groups first

| Group | What a failure there means |
| --- | --- |
| API and object lifetime | the object model itself: creation, destruction, handle validity, reset and re-record |
| Memory and buffer binding | allocation, binding, mapping, and non-coherent memory's flush and invalidate |
| Descriptors | set layout, writing, binding, rebinding and dynamic offsets |
| Draw | the drawing API's semantics beyond this repository's canaries |
| Compute | dispatch, storage buffers, workgroup semantics |
| Images and layout transitions | layouts, barriers' image scopes, blits, copies and resolves |
| Render passes and framebuffers | attachment load and store, subpasses, framebuffer creation |
| Synchronization: fences, semaphores, events, barriers | ordering and visibility, the hardest failure class to attribute |
| Queries | occlusion, availability bits, reset and copy semantics |
| Formats | the required tables, their feature bits and their behaviour per format |

Each group is its own manifest and its own console launch, so a crash costs one
group: the runner's queue model (`hold`, `exit`, one job line per case) already
gives the payload a resumable selection.

### Every result is one of four things

| Result | Meaning | What it requires |
| --- | --- | --- |
| PASS | the case ran and passed | nothing |
| FAIL | the case ran and failed | a triage entry, and an allowlist entry while it is open |
| NOT_SUPPORTED | the case names a capability the device honestly does not advertise | the reporting evidence: the feature, limit or format dump line that excludes it |
| HARNESS/PORT | the failure is in the CTS port or this repository's runner | the log and the reason; never counted as a driver failure |

A run is red if any case is HARNESS/PORT, or if any FAIL is not in the committed
allowlist. The allowlist is a file rather than a habit: each entry names the
case, the class of defect and the regression test that will close it.

### Triage: from a FAIL to a regression test

1. Minimise to the smallest reproducible CTS case (a case-list file edit with the
   framework's selectors).
2. Classify the defect: API semantics, synchronization, memory visibility,
   format behaviour, resource lifetime, limit reporting or command encoding.
3. Reproduce it on the console with this repository's runner, not only under the
   CTS.
4. Add the permanent regression test **before** the fix, so the fix is proved by
   a test that fails without it: a runner case with its golden and its PC
   comparison, like every other step's.
5. Re-run the group and the existing golden regression suite
   (`tools/check-driver.sh`, `tools/check-runner-cases.sh`) after every fix.

### What the CTS does and does not replace

The CTS validates Vulkan semantics; the goldens validate PS5 translation
correctness. Vulkan-Samples, an application that runs, or a packet-identical
golden are integration evidence: none of them substitutes for the CTS, and a CTS
pass does not retire a golden either. Both sets of evidence are kept.

### Pitfalls

- Do not call optionally unsupported behaviour a driver bug when it is honestly
  unadvertised: that is the NOT_SUPPORTED column, with its reporting evidence.
- Do not skip failing *mandatory* Vulkan 1.0 behaviour because no current
  application uses it.
- Do not fix a CTS failure by weakening the test, hardcoding an expected output,
  special-casing a test name, or detecting CTS workloads.
- Do not conflate harness and port failures with driver failures. One case already has that
  label with its evidence: `dEQP-VK.api.info.extension_core_versions` fails for every
  extension including the loader's own, and four candidate inputs -- this driver's instance
  version, the loader's, this driver's device version (measured, not assumed) and the
  check's own comparison -- have been eliminated (`docs/M5_PHASE_C.md`, CTS round 12).
- Do not trust final framebuffer output alone: check memory visibility,
  availability bits, query semantics, object lifetime and synchronization
  ordering.
- Do not rely on record-time CPU emulation where Vulkan requires execution-time
  semantics unless the observable behaviour is fully equivalent. Indirect
  commands, GPU-written argument buffers, non-coherent memory, barriers,
  descriptor rebinding, reset and re-record, resource aliasing and destruction
  timing are where a CPU-side model is most likely to differ.
- Do not advertise a format, feature or limit until its coverage passes or the
  capability is explicitly reported unsupported.

### Completion criterion

No unexpected failures in the selected Vulkan 1.0-compatible CTS subset, for the
exact capability set the driver advertises, with every unsupported path excluded
by feature, limit and format reporting rather than by a runtime refusal. In a
run's terms: every selected case PASS or NOT_SUPPORTED, no HARNESS/PORT result,
and no allowlisted FAIL left open, with the manifest, the capability dump, the
reassembled log and the driver digest committed together.

## Pin

| What | Value |
| --- | --- |
| Upstream | [KhronosGroup/VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) |
| Tag | `vulkan-cts-1.3.8.4` |
| Commit | `a0270c1897597e6c77679870e10415398a13001c` |
| License | Apache-2.0 |

The first target is the 1.0 claim, and this is the tag the reference
implementation below pins, so its results and ours are comparable. Later rises
pin the tag that matches the version they claim, at the point they are
scheduled.

The checkout is `tools/fetch-vk-gl-cts.sh`: it clones the tag shallow into
`.deps/work/vk-gl-cts` (moved by `PS5VK_CTS_DIR`), refuses a checkout that is not
the pinned commit, and writes `conformance_inventory/cts_pin.json`. That record
carries two kinds of pin, which are not the same thing -- the CTS revision, which
the script verifies from the checkout, and the revisions the CTS's own
`external/fetch_sources.py` **declares** for glslang, SPIRV-Tools, SPIRV-Headers,
amber, jsoncpp, Vulkan-Docs and the NVIDIA video samples. Declared is not
compiled: the reference project's manifest records different commits for four of
those than the script declares. The record gains the compiled revisions when the
payload build can report them, and `make lint` checks it against this file's pin
so the two cannot drift. `tools/fetch-vk-gl-cts.sh --externals` then runs the CTS's
own `external/fetch_sources.py` inside the checkout -- glslang, SPIRV-Tools,
SPIRV-Headers, Vulkan-Docs, amber, jsoncpp and the NVIDIA video samples, 988 MB --
and adds the revisions actually checked out, which on this host equal the declared
ones for all seven.

## The harness runs on the host first

The console payload below is the destination; the harness itself is proved on this host
before any of it is cross-compiled. `tools/run-cts-host.sh '<group>'` configures and builds
`deqp-vk` from the pinned checkout with `-DDEQP_TARGET=vulkan_headless`, runs one group
against the **host build of this driver** through the Khronos loader, and writes
`stdout.log`, `results.qpa` and `summary.json` under `build/cts-host/runs/`. The host
driver is a model -- it records and replays AGC work -- so the groups that belong there are
the ones that read what the device reports and how it handles the API, which is also where
the selection is decided. The first runs and their failures are in
`conformance_inventory/cts_host_baseline.json` and `docs/M5_PHASE_C.md`.

## Why this is not a small task

The CTS is not a program to run. It is a framework that has to be built for the
console as a title, with platform pieces the SDK does not provide.
`mpereiraesaa/ps5-vulkan` (GPL-3.0-or-later, validated on firmware 12.02) has
done that work and documents it in its `UPSTREAM_CTS.md`, `cts/upstream/` and
`tools/build_upstream_cts.py`. The recipe below follows that shape, with one
difference: this repository has a real ICD, so the CTS reaches the driver
through the Khronos loader instead of a static dispatch adapter.

### Pieces

| Piece | What it must do |
| --- | --- |
| Payload entry point | Log initialisation, run identity, command line, and the test iteration loop |
| Platform adaptation | `tcu::Platform` / `vk::Platform`: threading, time, assets, logging, and the Vulkan entry points the framework calls |
| Package selection | Register only the groups the manifest names, so the payload stays inside the title's size budget |
| Log capture | Serialise the QPA log, pipe it, stream it out as base64 chunks, and reassemble it host-side |
| Runner and verifier | Launch the title, capture the log, and enforce the manifest's acceptance policy |

### Pitfalls the reference project already hit

- `__dladdr` is not exported by the public SDK stubs, and the framework needs a
  back end for it.
- libc++abi needs POSIX thread-exit destructors on this SDK.
- Its harness shares one process-scoped device across cases, so independent
  device reinitialisation per case is *not* covered. Our runner should decide
  that explicitly rather than inherit the restriction.
- A pipeline-creation-only case proves no rasterisation. Pixel results come from
  this repository's runner readbacks.

## How a run is accepted

1. Build the payload reproducibly from the pinned tag and a committed manifest,
   so the same selection can be regenerated.
2. Run it on the console, capturing klog with `tools/ps5_console.py`.
3. Verify the reassembled log against the manifest: every selected case present,
   none failing, and the log naming the CTS tag, the driver build digest and the
   console firmware.
4. Record in the phase log: tag and commit, manifest hash, driver digest,
   firmware, case count, and every excluded case with its reason.
5. Only then may that version's row in [M5_REFERENCE.md](M5_REFERENCE.md) name
   the rise.

## Provenance

The CTS is Apache-2.0 and is not distributed by this repository: it is fetched
and built under an ignored cache, the same way the pinned SDK and Mesa release
are. Any file adapted from the reference project is GPL-3.0-or-later and must
carry its copyright and SPDX header, with an entry in `NOTICE.md`, before it is
committed.
