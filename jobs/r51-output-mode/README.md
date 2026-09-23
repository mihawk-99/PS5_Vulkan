# R51: the driver selects 120 Hz, falls back to 60 Hz, and says which

Until now the display reported one mode, 3840x2160 at a nominal 60000 mHz, and
the high-frame-rate output was only an opt-in probe that configured it and put
it back.

## The change (driver/ps5vk_wsi.c)

- **Modes, truthfully.** `vkGetDisplayModePropertiesKHR` reports 3840x2160 at
  **119880 mHz first**, when offered, then at **59940 mHz** -- the measured
  119.881 and 59.941 Hz, not nominal rates. The 119.88 Hz mode is offered only
  when the title's own `/app0/sce_sys/param.json` declares `attribute3` 0x80040
  (without it the console refuses the mode, 0x80290016) and
  `sceVideoOutIsOutputSupported(15)` says the output supports it. Settled once,
  on a short-lived handle, without changing the output.
- **Selection.** A swapchain on a surface made from the 119.88 Hz mode configures
  `sceVideoOutConfigureOutput(15)` on its fresh VideoOut handle, before the flip
  rate. A refusal is not an error: the swapchain presents at 59.94 Hz and one
  line says so.
- **Restore.** `ConfigureOutput(1)` before the handle closes, since a
  high-frame-rate output outlives the process that asked for it.
- The R31 output-mode probe is removed: it restored the mode at each of its
  stages, which real selection supersedes. The vblank cadence probe stays.

## Verification

Host: build clean, 11 gates, check-driver, check-shader-cache PASS (the PC stubs
offer no high-frame-rate output, so the PC sees the 59940 mHz mode only).

Console, VRR on (port evidence m6-r51-output-mode):
- vkQuake, which declares the metadata and takes the first 4K mode:
  `[ps5vk] output: 119.88 Hz selected`; walking the start map 119.88-119.89 FPS,
  every frame 8.29-8.40 ms; readback correct.
- The same build with `attribute3` 0: no selection, the 59.94 Hz mode only, a
  steady 59.94 FPS, every frame 16.60-16.76 ms.
- Runner (no metadata): present/readback tests identical to the pre-change driver
  (7/8 PASS; c1-flip needs an earlier test to open VideoOut, as before); mode
  reported as 3840x2160 at 59940 mHz.

Not exercised on the console: a refusal of `ConfigureOutput(15)` despite the
metadata and the support query (no setup here produces one); the code path
logs and presents at 59.94 Hz.
