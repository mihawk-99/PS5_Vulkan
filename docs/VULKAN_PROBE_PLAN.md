# PS5 Vulkan probe plan

This repository is a Vulkan 1.0 driver for the PS5 and the probe harness that
keeps its advertised capabilities honest. Applications reach the console's GPU
through the native AGC interface; the driver is built on that backend, and every
capability it reports is proved by a console run before it is claimed, never
inferred from AGC API availability.

This is the top-level plan: what the project must prove, the rules that
constrain how, and where the detail lives. It is deliberately short, and it is
read at the start of every session. Current step, next actions and last
verified runs are in [VULKAN_PROBE_ACTIVE.md](VULKAN_PROBE_ACTIVE.md); run
evidence is in the phase logs (`M5_PHASE_A.md`, `M5_PHASE_B.md`,
`M5_PHASE_C.md`).

## Gates

A requirement is proved by a gate, not by a plausible call path: the host build
with its lint, test and link checks; the three requirement audits (commands,
limits, formats, each with `--check`); the runner case check against the PC
goldens; and the console probe that exercises that path through the driver and
the runtime. Every gate records exact return codes, hashes, sizes, addresses,
cleanup results and a narrow status. A pass at one gate does not imply another,
and a capability is advertised only after its own console probe. The exact
commands are the table in `AGENTS.md` and step 5 of `README.md`.

## Milestone map

| Milestone | What it proves | Detail |
| --- | --- | --- |
| M1 | AGC and AGC-driver module loading, symbol resolution, direct memory, register defaults, private command encoding | [PROBE_MILESTONES.md](PROBE_MILESTONES.md) |
| M2 | Shaders compiled from this repository's GLSL draw an exact 4K frame | [PROBE_MILESTONES.md](PROBE_MILESTONES.md) |
| M3 | Uniform, vertex/index and sampled-texture resources reach the shader | [PROBE_MILESTONES.md](PROBE_MILESTONES.md) |
| M4 | Clear and depth testing, blending, and render to texture | [PROBE_MILESTONES.md](PROBE_MILESTONES.md) |
| M5 A | SPIR-V compiles to an AGC package on the console, byte-identical to the PC's | [M5_REFERENCE.md](M5_REFERENCE.md) |
| M5 B | A headless Vulkan program through the driver, up to command buffers | [M5_REFERENCE.md](M5_REFERENCE.md) |
| M5 C | The Vulkan Tutorial ladder, one driver step per chapter | [M5_REFERENCE.md](M5_REFERENCE.md) |
| M5 D | Breadth and benchmarks: dynamic state, compute, more formats | [M5_REFERENCE.md](M5_REFERENCE.md) |
| M5 E | Conformance subsets and real applications (RetroArch, emulators) | [M5_REFERENCE.md](M5_REFERENCE.md) |
| Versions | The device reports the highest version whose every requirement has been proved on the console: 1.0 now, with 1.1's and 1.2's conditional rows still open and 1.4 the goal. Each rise is its own commit, gated by a CTS subset, and rung 1.0 is followed by the staged CTS semantic-validation plan | [M5_REFERENCE.md](M5_REFERENCE.md), [CTS.md](CTS.md) |

Every M2-M4 test runs in one installed title, the runner `PPSA99988`; the
per-test titles that first passed are retired. The development loop, the
runner queues and the PC-side golden comparison are described in
[PROBE_MILESTONES.md](PROBE_MILESTONES.md).

The version ladder gives every requirement an owner and a step id, and it is what
orders the work: a probe of unknown AGC behaviour belongs on the critical path
rather than at the end of a phase, because whether the console can do something
at all decides what the driver may report. No rung is reported until every
requirement of that rung carries its own console evidence.

## Invariants

Each of these was established by a console run and constrains changes to
GPU-facing code. The evidence — build, run id, commit — is in
[HARDWARE_FINDINGS.md](HARDWARE_FINDINGS.md); read that before changing how the
driver or the runner programs the GPU.

- Indirect register tables are dereferenced by the GPU, so they must live in
  GPU-mapped direct memory. A table on the stack faults.
- `sceAgcDcb*` helpers return the start of the packet they wrote;
  `command.up` is the end of the stream.
- Colour render targets are tiled RGBA8, not row-major; CPU access must convert
  the layout.
- `CB_COLOR0_INFO` SWAP_ALT makes the target B8G8R8A8, the byte order VideoOut
  scans out; SWAP_STD swaps the channels on screen.
- A wait-until-safe packet must name the buffer the frame renders into, or the
  GPU waits on the buffer that is already on screen.
- AGC leaves the screen, window, generic and viewport scissors at 16384x16384
  with clipping disabled, so no default clips a 4K target.
- A submission can complete without a flip: `RELEASE_MEM` event 40 after the
  last packet carries the completion value, and the marker follows rendering,
  which makes marker-based fences safe for readback.
- A shader object's register tables carry no user-data registers, so a draw
  that follows another pipeline must write its own vertex user data.
- Blending needs more than `CB_BLEND0_CONTROL`: the pixel shader must export
  the target's blend format, and every draw writes `CB_COLOR_CONTROL`
  `0x00cc0011`.
- Sampling a target drawn earlier in the same stream needs the colour barrier
  (`RELEASE_MEM` event 45) before the sample.
- 32-bit shader pointers combine a buffer's address with a fixed high word, so
  every resource must stay inside that 4 GiB window.
- The ELF converter takes the RELRO file offset from its lowest section; a
  misaligned segment makes the console loader refuse the executable.
- A PM4 `INDIRECT_BUFFER` into title memory faults the GPU, so the driver
  copies command buffers into one stream instead.

## BC-250 and upstream AMD work

The AMD BC-250 is a useful external reference because it is a PS5-derived AMD
board with a separately exposed Linux graphics stack. It is not an identical
PS5 runtime target: community documentation places BC-250 configurations
around `GFX1013`, while LLVM models `gfx1013` and `gfx1030` as distinct AMDGPU
targets with different feature sets. The PS5 itself exposes a custom RDNA-based
GPU and unified GDDR6 memory, but applications reach it through the native AGC
interface rather than Linux's `amdgpu`/DRM interface.

Useful upstream work to study or selectively reuse, subject to its licenses,
includes:

- LLVM AMDGPU instruction definitions and target-specific code generation;
- Mesa NIR lowering and ACO shader compilation;
- RADV's Vulkan state translation and validation logic;
- resource formats, descriptors, barriers, synchronization, and pipeline
  concepts;
- BC-250 shader and Vulkan experiments as an external comparison platform.

The following are not drop-in components for PS5:

- Linux kernel `amdgpu` and DRM code;
- Linux buffer allocation and ioctl paths;
- RADV queue submission;
- firmware interfaces and BC-250-specific memory/display handling.

The long-term architecture is therefore:

```text
Vulkan frontend
      -> NIR/ACO shader translation
      -> PS5-specific resource and shader packaging
      -> AGC command/register backend
      -> PS5 GPU
```

BC-250 work can provide an AMD/GFX reference and an offline shader oracle, but
the PS5 probe canary must continue to establish the console-specific AGC ABI,
memory rules, shader package format, command encoding, and synchronization
behavior. That layer, and the Vulkan frontend above it, are now in place, and
every rule in them came from a console run rather than from an upstream
assumption.

References: [PS5 specifications](https://blog.playstation.com/2020/03/18/unveiling-new-details-behind-the-hardware-technical-specs/),
[BC-250 hardware documentation](https://elektricm.github.io/amd-bc250-docs/hardware/specifications/),
[LLVM AMDGPU usage](https://llvm.org/docs/AMDGPUUsage.html),
[Mesa RADV documentation](https://docs.mesa3d.org/drivers/radv.html), and
[PS5 OpenGL](https://github.com/blackbearreloaded/ps5-opengl).

## Reference files

This is the index for everything the plan refers to. These files are read on
demand, except the active file, which is read at the end of a session start.

| File | Read it when |
| --- | --- |
| [VULKAN_PROBE_ACTIVE.md](VULKAN_PROBE_ACTIVE.md) | Every session: current step, next actions, last verified runs |
| [PROBE_MILESTONES.md](PROBE_MILESTONES.md) | Working on the M1-M4 canaries, the runner or the console tooling |
| [M5_REFERENCE.md](M5_REFERENCE.md) | Working on the driver: phase steps, workflow, version ladder, PC accelerator |
| [HARDWARE_FINDINGS.md](HARDWARE_FINDINGS.md) | Touching GPU-facing code, or when an invariant above needs its evidence |
| [CTS.md](CTS.md) | Building or running the conformance subset that gates a version rise |
| [NATIVE_TOOLING.md](NATIVE_TOOLING.md) | Building, ccache, the converter and the FSELF commands |
| [TESTING.md](TESTING.md) | Host tests and the local CI reproduction |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Deploying a title and capturing a console run |
| [GETTING_STARTED.md](GETTING_STARTED.md) | A fresh host |
