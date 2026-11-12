# RADV VideoOut HDR10 probe

This job checks the title metadata gate and, when HDR10 ST2084 is offered,
presents five PQ frames and reads the first and last pixel of the last image
before presentation. It also retains the existing SDR display, FIFO pacing and
replacement-swapchain checks. This readback tests the swapchain image, not the
TV's output or HDR appearance.

From the repository root, with `RADV_ARCHIVE` naming the freshly built Mesa
archive and `PS5_MESA_FORK` naming that Mesa checkout (for its headers):

```sh
export PS5_MESA_FORK=/path/to/PS5_Mesa
export RADV_ARCHIVE="$PS5_MESA_FORK/build-ps5/src/amd/vulkan/libvulkan_radeon.a"
PARAM_PATH=sce_sys/param-radv-hdr.json RADV_SMOKE_ENV=jobs/radv-hdr10/radv-display-env.txt tools/build-radv-title.sh
```

Deploy `dist/PPSA99014` and capture its klog. Expected lines include
`display hdr: title attribute 0x20000000`, `display hdr: offered`,
`PASS display hdr: formats match title attribute`,
`wsi/videoout: HDR10 PQ swapchain A2B10G10R10_UNORM_PACK32` (or
`A2R10G10B10_UNORM_PACK32`), and
`PASS display hdr: 5 PQ frames and first/last pixel readback`. The PQ clear is
R=1, G=0, B=0, A=1: packed readback is `0xc00003ff` for A2B10G10R10 or
`0xfff00000` for A2R10G10B10, at both sampled pixels.

Rebuild and deploy the negative case using the same archive:

```sh
PARAM_PATH=sce_sys/param-radv.json RADV_SMOKE_ENV=jobs/radv-hdr10/radv-display-env.txt tools/build-radv-title.sh
```

Expected: `display hdr: title attribute 0x00000000`, `display hdr: not offered`,
and `PASS display hdr: formats match title attribute`; no HDR PQ swapchain line.
Both runs must finish without FAIL. The fixtures differ only in `attribute`:
0 versus decimal 536870912 (`0x20000000`). Both use title ID PPSA99014, so
install each build separately; the build replaces that title's dist directory.

## Console observation

On an original PS5 running firmware 5.50, connected to an LG C1 with the TV
on, both builds were launched as PPSA99014 with the installed title's original
metadata identity, changing only `attribute` for HDR. The captures are
[`evidence/hdr-tv-on-run2.klog`](evidence/hdr-tv-on-run2.klog) and
[`evidence/sdr-tv-on.klog`](evidence/sdr-tv-on.klog).

| Observation | HDR attribute | Original SDR attribute |
|---|---|---|
| AvControl app | `HDR:o` | `HDR:x` |
| HDMI video line | `3840_2160P_5994 RGB444 full RGB2100_PQ 36bpp` | `3840_2160P_5994 RGB444 full RGB2100_PQ 36bpp` |
| Probe attribute / HDR formats | `0x20000000` / offered | `0x00000000` / not offered |
| HDR format checks | attribute read, formats match, offered format supported: PASS | attribute read, formats match, offered format supported: PASS |
| Mesa HDR10 PQ swapchain | `A2R10G10B10_UNORM_PACK32` | no HDR10 PQ swapchain line |
| HDR frame/readback | five PQ frames and first/last pixels PASS, both `0xfff00000` as expected | not exercised |
| `display: its modes` | PASS; 2 modes, first 59.940 Hz | PASS; 2 modes, first 59.940 Hz |
| Smoke summary | 26 passed, 0 failed | 23 passed, 0 failed |

Both runs logged `exit() exit_value=0` after the smoke summary, followed by
SIGSYS and a Syscore app-crash report on the known exit path. After both runs
the original 13 slot files were restored and SHA-256 verified, and the
control service reported `count=0`.

The earlier [`evidence/hdr-tv-on.klog`](evidence/hdr-tv-on.klog) recorded
25 passed, 1 failed because `display: its modes` rejected incomplete mode
enumeration. The probe's enumeration handling was corrected before run 2.

The RADV archive SHA-256 was
`d0077c60b0aa1ad7749e6af3c9b7c71bc6d57191a996ca438433f11c9b3117e9`;
both run 2 title executables were
`c75f4aeeaed0c601294667d315243e4fa3a8b256a81251a7ccff075c377018b2`
and their matching `libc.prx` was
`e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`.
The image readback and HDMI mode report do not measure luminance or picture
accuracy.

## Non-HDR TV (second console)

On a second console, connected to a TV that does not support HDR, the
captures are [`evidence/hdr-no-hdr-tv.klog`](evidence/hdr-no-hdr-tv.klog)
and [`evidence/sdr-no-hdr-tv.klog`](evidence/sdr-no-hdr-tv.klog).
The short pre-launch klog backlog contained no `video: port:` line, and
neither run captured an HDMI video mode line, so the output colour, bit
depth and any SDR fallback were not established by these logs. No AvControl
HDR error or conversion was logged.

| Observation | HDR attribute | SDR attribute |
|---|---|---|
| AvControl app | `HDR:o` | `HDR:x` |
| Probe attribute / HDR formats | `0x20000000` / offered | `0x00000000` / not offered |
| HDR checks | attribute read, formats match, offered format supported: PASS | attribute read, formats match, offered format supported: PASS |
| Mesa HDR10 PQ swapchain | `A2R10G10B10_UNORM_PACK32` | no HDR10 PQ swapchain line |
| HDR frames / image readback | five frames PASS; both pixels `0xfff00000`, as expected | not exercised |
| Display modes | PASS; 2 modes, first 59.940 Hz | PASS; 2 modes, first 59.940 Hz |
| Smoke summary | 26 passed, 0 failed | 23 passed, 0 failed |

Both runs logged `exit() exit_value=0` followed by the known post-exit fatal
signal. The temporary title folder was removed and the control service
reported `count=0`. A stale home-screen entry may remain.
