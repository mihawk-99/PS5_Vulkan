# R92: the output modes VideoOut takes

A PAL game runs at 50 fps, which neither 59.94 Hz nor 119.88 Hz divides: on the
title's 119.88 Hz output its frames alternate two and three refreshes (a PAL
PS2 demo presents at 50 Hz, docs/PHASE_LOG.md in the RetroArch title). The driver
knows two output mode selectors, 15 (119.88 Hz, from the public ps5-opengl
runtime, R51) and 1 (the default back). This round asks whether VideoOut has
another, a 50 or 100 Hz output in particular.

`r92-output-modes` asks `sceVideoOutIsOutputSupported` about every selector from
0 to 63, which changes nothing. Each supported one is then configured on a fresh
handle with `sceVideoOutConfigureOutput`, the vblank period timed over 120
vblanks, the default restored and the handle closed. Nothing is registered or
flipped.

- **Only 1 and 15 are supported.** Every other selector is refused: 4, 7, 8,
  12-14 and 16-19 with 0x80290016, the code 15 gets from a title that does not
  declare it, and the rest with 0x8029001E. The answers are the same with the
  declaration and without it.
- **15 is 119.88 Hz only for a title that declares it.** Without attribute3
  0x80040 (the plain runner) 15 is refused with 0x80290016, on a handle that
  had waited on vblanks (PID 260) and on a fresh one (PID 261). The runner
  built with sce_sys/param-runner-hfr.json, which declares it, configures 15
  and measures 8,341.6 us (119.881 Hz), then 16,683 us again after the default
  is restored (PID 262). The default is 16,683.3 us (59.940 Hz).

So no output rate a 50 fps game divides is reachable through these selectors,
and the title paces PAL games on 119.88 Hz. Whatever 4, 7, 8, 12-14 and 16-19
are, they would need a declaration or parameters no measurement here names.

Reproduce from the driver root:

    python3 jobs/r92-output-modes/check.py

Console: build the driver and the runner, deploy while idle with
PARAM_PATH=sce_sys/param-runner-hfr.json, run tools/ps5_console.py battery
PPSA99988 with queue.txt, then deploy the plain runner again. The raw logs are
the ignored Klog_Logs/r92-output-modes-1.log (PID 260), -2.log (261) and
-3.log (262).

## FIFO timing workload

`queue-timing-hfr.txt` and `queue-timing-vrr.txt` run `present-timing` with
`param-runner-hfr.json` and `param-runner-vrr.json` respectively. The ps5vk
runner draws and presents 30 warmup frames followed by 360 timed frames. It
records elapsed microseconds, the minimum and maximum per-iteration times and
the rate in millihertz. Any failed draw, present, or clock read fails the case.
The Mesa/RADV smoke title's display-only selection runs the same frame counts
with GPU clears and FIFO presents, and also checks the last frame's readback.
The workloads differ, so their rates are not a cross-driver benchmark.
Both log the actual `/app0/sce_sys/param.json` attribute3 value when readable,
or report it unavailable. Record console model, firmware and TV separately;
the samples do not query them.

Both use the drivers' FIFO acquire/flip pacing; a finite number of frames may
still be in flight at either boundary. The timing is steady-state
draw/present-loop throughput, not an independent display scanout measurement.
Keep the WSI idle-period and mode-selection logs alongside the timing, and use
the console/TV output to establish the actual output mode. No timing result is
assumed until a console run has been captured.

On 2026-10-29 an attempted baseline RADV HFR run on an original PS5
(firmware 5.50, LG C1) did **not** reach the workload. The
registered `PPSA99014` slot retained its original metadata except attribute3
`0x80040`. The baseline eboot SHA-256 was
`ce939aafc865d57524c4c292bcabaaf566f19960dcbb5fa1e79c9ff9f2365c9e`.
The first launch failed at PRX loading with the slot's original libc:
[loader log](evidence/baseline-radv-hfr-loader-failure.log). After installing
the matching artifact libc, launch was rejected as already running
(`0x8094000c`) although the control service reported no running application:
[retry log](evidence/baseline-radv-hfr-launch-rejected.log).
After a reboot and deployment of the matching libc, the
[baseline](evidence/baseline-radv-hfr-reboot.log) and
[patched](evidence/patched-radv-hfr-reboot.log) RADV HFR runs each passed
19 checks. Both reported attribute3 `0x80040` and 119.88 Hz not offered;
their 360-frame FIFO throughput was 59.946 and 59.764 presents/s respectively.
These are not scanout measurements. The TV may have been off during these
runs, so the display state and negotiated capabilities are
unconfirmed; neither run qualifies HFR or VRR. No VRR run was made.
All 13 original slot files were restored and their remote SHA-256 hashes
matched the backup; the control service reported `count=0`.

With the TV on and PS5 input selected, a further bounded run
on the same original PS5, firmware 5.50 and LG C1 produced:

| Driver / fixture | Baseline FIFO presents/s | Patched FIFO presents/s |
| --- | ---: | ---: |
| RADV HFR `0x80040` | [119.519](evidence/baseline-radv-hfr-tv-on.log) | [119.524](evidence/patched-radv-hfr-tv-on.log) |
| RADV VRR `0x40040` | [59.933](evidence/baseline-radv-vrr-tv-on.log) | [119.854](evidence/patched-radv-vrr-tv-on.log) |
| ps5vk HFR `0x80040` | [120.227](evidence/baseline-ps5vk-hfr-tv-on-shaders.log) | [119.879](evidence/patched-ps5vk-hfr-tv-on.log) |
| ps5vk VRR `0x40040` | [59.939](evidence/baseline-ps5vk-vrr-tv-on.log) | [119.879](evidence/patched-ps5vk-vrr-tv-on.log) |

Each measurement follows 30 warmup frames and covers 360 frames; all four
RADV runs passed 19 checks and the four ps5vk timing tests passed. Rates are
FIFO loop throughput, not independent display scanout or a cross-driver
benchmark. The first ps5vk attempt lacked the `b8-corner` shader files and
failed before timing ([log](evidence/baseline-ps5vk-hfr-tv-on.log)); the
matching files were then deployed to `/app0/probes/b8-corner/`.

In the RADV VRR baseline, VideoOut logged HDMI 2160p 119.88, VRR peg 120
range 48–120 (`ref:0x800d`), then the driver measured 20.876 ms idle vblanks
and restored 59.94 Hz with VRR peg 60 range 48–60 (`ref:0x8003`). Patched
RADV measured 20.873 ms idle and kept 119.88 Hz; its VideoOut log likewise
reported peg 120/range 48–120. The ps5vk VRR baseline logged peg 60/range
48–60; patched logged peg 120/range 48–120. Its logs do not report the idle
period. HFR runs logged 2160p 119.88 and `ref:0xd(0-0)`, no VRR peg/range,
and `unknown vrr parameter(0x80040)` with app flags HFR on and VRR off.
These are the console's output-mode observations, not independent panel
measurements or a general VRR qualification.

RADV baseline used Mesa `a8800d9130583fbd4a436c4c64ab9e33b98edf10`
(eboot `ce939aafc865d57524c4c292bcabaaf566f19960dcbb5fa1e79c9ff9f2365c9e`);
patched Mesa was based on `0eb9051bdf2e0d31afa4ff312ec4a83524afe096`
with working changes (WSI SHA-256
`515d8c0d41bf67732070b62a7e83ad4258e30a303d8b96c17064365c82864e1b`,
eboot `7654bce1b239466910e1b85c4132b96bf2908622508718805ef8d7eada6bab25`).
ps5vk baseline was `f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed` (eboot
`6e464d06303b352b50f4ddb2d94a059d5cca26dd2815af8fab0851c7216312e7`);
patched was based on `21db3c7c8d4b137dc1b1bd8aaf31f7346a11c69a` with
working changes (eboot
`7cc0ae874d2f87c2b82fe5b019d1b9d9a00b93a42c18f66f9f74c64508b8f1f8`).
Both used SDK `b83202be73e930050e00de0f1b4d9ec46c0391bf` and matching
libc SHA-256 `e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`.
The title slot's original 13 files were restored and verified by SHA-256,
the added queue, shader files and generated ps5vk cache removed, and the
control service reported `count=0`.

For ps5vk, build `tools/build-driver.sh`, then build the runner with
`APP_DEFINITIONS="AGC_LINKED_CANARY=1 AGC_TEST_RUNNER=1 AGC_OUTPUT_4K=1 AGC_LIVE_SUBMIT_ARMED=1 AGC_SHADER_COMPILER=1 AGC_VULKAN_DRIVER=1"`
and the appropriate `PARAM_PATH`. On a registered test slot, preserve its
original param identity and change only attribute3 to the fixture value;
upload the matching `sce_module/libc.prx` and `jobs/queue.txt` before launch.
For RADV, build the intended Mesa checkout's PS5 archive, then use
`PARAM_PATH=<same fixture> RADV_SMOKE_ENV=jobs/r92-output-modes/radv-display-env.txt RADV_ARCHIVE=<archive> bash tools/build-radv-title.sh`.
Deploy the matching eboot, libc and environment file. Capture klog and close
the title before switching builds. Back up and hash the existing slot first,
persist any added paths, and restore it only after the control service confirms
that no application is running.
