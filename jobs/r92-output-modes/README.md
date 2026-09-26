# R92: the output modes VideoOut takes

A PAL game runs at 50 fps, which neither 59.94 Hz nor 119.88 Hz divides: on the
title's 119.88 Hz output its frames alternate two and three refreshes (FFX's
demo presents at 50 Hz, docs/PHASE_LOG.md in the RetroArch title). The driver
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
