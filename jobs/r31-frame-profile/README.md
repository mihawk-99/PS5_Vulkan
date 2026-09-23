# R31: separate application, submission, polling and presentation time

R28's counters made four things share one number. `queue_ms` covered the CPU
cache flush, the CPU copies and the submit-plus-marker wait together; `gpu_ms`
is not GPU execution but native submission plus completion-marker polling;
`flip_ms` is the driver's whole presentation path. Nothing measured the
application's own CPU time, and nothing could say whether a frame's period is
the work in it or the display's refresh.

R31 adds default-off instrumentation for exactly those, and changes no packet,
wait, flip or copy. With `/app0/ps5vk-profile.txt` absent and `PS5VK_PROFILE`
unset every timing call is skipped.

## What the second summary line reports

`[ps5vk] profile2` is printed after the existing `[ps5vk] profile` line, one per
ten-second window, so the R28 field names and meanings are unchanged.

- `app_pre_ms`, `app_post_ms`: the application's own time from the previous
  present's return to the next submission, and from that submission's return to
  the present. With `queue_ms` and `flip_ms` these four parts are the whole
  frame period, and `residual_ms` is their sum against the measured
  present-to-present period. A residual that is not about zero means the split
  is incomplete and the attribution above it should not be trusted.
- `app_after_present_ms`, `app_after_acquire_ms`, `app_after_submit_ms`: the
  same application time attributed by which instrumented entry point last
  returned, so it separates the engine's work before a frame, the recording of
  the frame, and what follows the submission. Taken at
  `vkAcquireNextImageKHR`, `vkQueueSubmit` and `vkQueuePresentKHR`.
- `submit_ms`, `poll_ms`, `polls/step`, `poll_first`: the submission call
  (`sceAgcDriverSubmitDcb` plus `sceAgcSuspendPoint`) apart from the marker poll
  loop, the number of marker checks per step that found the marker unset, and
  the share of steps whose first check already saw it. `submit_ms + poll_ms`
  reconstructs `gpu_ms`, which is what tells GPU execution from a 1 ms sleep.
- `flip_status/present`, `flip_status_ms`, `vblank/present`, `vblank_ms`,
  `vblank_first`: what the presentation wait is made of. `flip_status/present`
  is one more than `vblank/present` on success, and `flip_status_ms` is what
  that extra `sceVideoOutGetFlipStatus` costs after each vblank.
- `frame_ms`, `frame_min_ms`, `frame_max_ms`, `periods=`: the interval between
  two confirmed presents, and its histogram in 4 ms buckets (bucket 4 is one
  60 Hz vblank, 8 is two, 16 is four). A period that clusters on bucket
  boundaries is quantised to the refresh.
- `presents=N/M`: presents of swapchain image 0 and image 1.

## The refresh cadence probe

`/app0/ps5vk-vblank-probe.txt`, or `PS5VK_VBLANK_PROBE` in the environment, makes
`ps5vk_video_out_open` time 60 consecutive bare `sceVideoOutWaitVblank` calls
after the framebuffers are registered. The interval between two returns is one
refresh period; the first call is discarded because it returns at the next
vblank whatever the phase. It prints mean, min, max and the equivalent rate, so
the display's actual cadence is measured rather than taken from the 60000 the
mode reports. It delays startup by the refresh times 60 - about a second at
60 Hz - and is off otherwise. On the PC its stubs return immediately, so it
measures nothing there.

## Host verification

`bash tools/build-driver.sh`: 23 sources, 0 warnings, host and PS5.
`bash build/gates.sh`: lint, test, audit-commands, audit-limits, audit-formats,
migration, mip-layout, psbc-link, probe-packages, vulkan-runtime, runner-cases -
all PASS. `bash tools/check-driver.sh`: loader, direct and PS5-link runs,
negative tests, the golden submission replays and the c1 present test - PASS,
so the recorded streams are unchanged with profiling off.
`bash tools/check-shader-cache.sh`: PASS.

Note for anyone reading a build's exit status: `tools/build-driver.sh` returned
0 for a build that had failed to compile, and `build/gates.sh` reports PASS/FAIL
in its own output rather than propagating a failure. Both were read from their
output, not their exit status.

## What the application does per frame, read from the port

Established by reading `PS5_vkQuake` while the console was unreachable, because
it decides where the next change goes. This is the workload the driver is
serving, not a claim about it.

- The order is: previous frame's end-rendering task, the fence wait and reset,
  recording every primary command buffer, **then** `vkAcquireNextImageKHR`,
  one `vkQueueSubmit` and `vkQueuePresentKHR` (`gl_vidsdl.c:3281-3292`,
  `:4001`, `:4167`, `:4199`). Acquisition comes *after* recording, which is why
  the three gap slots are named for what they measure.
- `r_scale` is not a resolution knob on this port. Every target is created at
  `vid.width x vid.height`, and the drawable is the console's one 3840x2160
  mode, so `r_scale 1` renders at 4K and `r_scale 2` would not lower it either -
  it only selects a block-replicating screen effect and a sampler LOD bias. Any
  recorded "scale 2 gives no FPS gain" is therefore not evidence about fill rate.
- The one `vkCmdBlitImage` in the engine is the water warp mip chain
  (`gl_warp.c:274`, four blits per visible warp texture per frame), and this
  driver runs blits on the CPU. That is the start map's 24 ms `copy_ms`.
- `r_gpulightmapupdate` is on, so lightmaps are scheduled as compute dispatches
  and the CPU upload path is not taken; `R_FlushDynamicBuffers` is a no-op here;
  `vkDeviceWaitIdle` and the 33 MB readback exist only on the screenshot path.
- The engine cap is `host_maxfps`, the console's own config sets it to 200, and
  `vid_maxframelatency` needs a present-wait extension this driver does not
  advertise, so neither is what holds the frame rate down.
- `host_speeds` prints `tot / server / gfx / snd` per rendered frame and has
  never been captured. The port's `r29b-autoexec.cfg` is the first fixture that
  enables it, and `Con_Printf` output does reach the kernel log - "Map checklist"
  is in `klog/r29-perf-final-trace.txt`. So the baseline run returns the engine's
  own split alongside this profile, for free. Its samples run at `r_tasks 0`
  while the steady phases run at `r_tasks 1`, so the two must not be averaged
  together.

## Not yet measured

No console run. The R29 game baseline and this profile2 run are both pending:
the console stopped answering on 2121/3232/9111 at 12:36 UTC on 2026-09-23,
after a game session held it (`procs` reported `title=PPSA99010 count=1`) and
before anything was staged. Nothing was uploaded, launched or closed.

The port's own fixture and scripts for the run are `PS5_vkQuake/build/`:
`r29b-preserve.py` snapshots and restores the user's configuration files
(present or absent, contents included) because `r29b-stage.py` and
`r24-collect.py` were written when they were absent and `r24-collect.py`
restores them by deleting them; `r29b-collect.py` collects without deleting;
`profile-metrics.py` turns a capture into per-phase means and reproduces
`jobs/r28-copy-profile/metrics.txt` exactly from its capture; `r29-run.sh` is
the whole sequence with the snapshot first and the restore last.
