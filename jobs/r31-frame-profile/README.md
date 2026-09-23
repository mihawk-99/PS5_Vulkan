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
`r29b-preserve.py` snapshots and restores my configuration files
(present or absent, contents included) because `r29b-stage.py` and
`r24-collect.py` were written when they were absent and `r24-collect.py`
restores them by deleting them; `r29b-collect.py` collects without deleting;
`profile-metrics.py` turns a capture into per-phase means and reproduces
`jobs/r28-copy-profile/metrics.txt` exactly from its capture; `r29-run.sh` is
the whole sequence with the snapshot first and the restore last.

## Console results

The console answered between 12:40 and 13:20 UTC on 2026-09-23 and was idle:
`procs` reported `count=0`, from live kernel process enumeration. Four runs were
made, each with the console's own state snapshotted first and restored exactly
afterwards -- my two configuration files, `vkQuake.cfg` (1020
bytes) and `id1/vkQuake.cfg` (2510 bytes), were unchanged at the end, and the
staged autoexec and flags were removed.

**Run A -- the controlled R29 baseline (game `e3525e30`, driver c3e51f6).** The
deployed binary itself, verified before the run by reading the served ELF twice
and comparing its whole-file hash and all five PT_LOAD segments against
`evidence/manual-r29-deployment/deployed-proof.txt` in the port: both match. No
upload, no relink, `--no-build --no-deploy`.

|                        | start map | E1M1      |
| ---------------------- | --------- | --------- |
| fps (before, R28)      | 14.81     | 29.58     |
| fps (R29)              | **19.72** | 29.58     |
| frame period           | 50.71 ms  | 33.81 ms  |
| CPU copies             | 11.62     | 0.041     |
| cache flush            | 5.96      | 3.96      |
| submit + marker        | 2.80      | 2.58      |
| flip / scanout wait    | 10.23     | 7.54      |
| flush MiB/frame        | 384.09    | 256.06    |

So R29's tile-address change is worth 14.81 -> 19.72 FPS on the start map, and
its 12.5 ms copy saving moves the period from four 60 Hz vblanks to three. E1M1
is unchanged, which is what its 0.041 ms copy cost predicted. This is the game
measurement R29 was missing; the host address benchmark was never the evidence.

**The period is quantised to whole vblanks.** `frame_min_ms` was 49.976 ms at
the start map and 33.292 ms at E1M1, against 16.683 ms a vblank: three and two.
A frame whose work is under a whole number of vblanks takes that many, so a
partial saving cannot raise FPS at all until the whole frame fits one interval
less. That is exactly what R22's deduplicated flushes (2 ms saved, no FPS gain)
and R29's rolled-back filter (1.4 ms saved, no FPS gain) observed, and it is why
an optimizer on this title must measure work, not only FPS.

**`gpu_ms` is not GPU time.** `submit_ms` -- `sceAgcDriverSubmitDcb` through
`sceAgcSuspendPoint` -- is 0.169 ms per step. The rest of the 2.8 ms `gpu_ms` is
the marker poll's 1 ms sleeps: `polls/step` 0.66 at the start map and 1.00 at
E1M1, with 34% and 0% of steps seeing the marker on the first check. The GPU
finishes within about a millisecond; the measurement could not see that before.

**Presentation waits exactly one vblank.** `vblank/present` is 1.00 in every
window and `vblank_first` is 0%: a flip is never already on screen at the first
status check, so the present path adds no extra refresh of latency. It is not a
defect; it is the next vblank doing its job.

**The display's real cadence, measured.** The opt-in probe timed 60 consecutive
bare `sceVideoOutWaitVblank` calls at VideoOut open: mean 16.6831 ms, min
16.6386, max 16.7466, spread 0.108 ms, equivalent 59.941 Hz. That is the
refresh, measured rather than taken from the 60000 millihertz the mode reports.

**The largest remaining cost is the application's own CPU, and it is
scene-independent.** `app_pre_ms` -- the time between a present returning and
the next submission being entered -- was 21.7 ms at the start map and 21.7 ms at
E1M1, in a run where the two maps' driver costs differ by a factor of three
(20.8 ms against 6.9 ms of queue time). `host_speeds`, captured for the first
time, agrees from the other side: of E1M1's 35.4 ms frame, `gfx` is 33.9 ms and
`server` is 1.6 ms, so the simulation is not the cost and the render path is.

## The measurement that invalidated a run

Run B -- the same fixture on a relinked R31 build -- was 10-13% slower at the
start map with a 1.2 second stall in every ten-second window, and then collapsed
to 0.04 FPS when `host_speeds` was enabled, presenting one frame every 20
seconds until the harness closed it. It is not a rendering defect and it is not
in the submission path: every driver interval in that run was normal
(`queue_ms` 20.77, `flush_ms` 5.94, `copy_ms` 11.61, identical to the baseline),
and only the application's own time grew.

The cause is the port's trace shim. `PS5_vkQuake/src/trace.cpp` reopens stdout
and stderr unbuffered onto `/app0/trace.txt`, a file inside the title's folder,
so every stdio call from the driver is its own write to that filesystem. R31's
first summary line was formatted as one `fprintf` per field -- eighteen writes
per window -- and that cost about 12% of the frame budget. Enabling a
`host_speeds` line per frame then made every frame pay that cost, and the run
died of its own logging.

The fix is that both summary lines are now formatted into one buffer and leave
as a single write, and the file says so where it is done, because the next
person to add a field will otherwise add a write. Two consequences worth keeping:
a run that logs per frame on this console measures its logging, and the console's
own steady-state logging is not free either -- the port writes a present line and
an audio line every ten seconds whether or not anyone is profiling.

Run C, with that fix, had no stalls: its start-map windows were 190 frames in
10009-10028 ms, uniform, against run B's 177-190 frames in 10000-10443 ms.

## No regression

Run D staged the same fixture with **no profiling flag and no cadence probe**,
so the same two scenes were timed on a build with R31 compiled in but never
armed. It exited normally with two correct readbacks.

|        | run A: deployed R29, profiling on | run D: R31 build, profiling off |
| ------ | --------------------------------- | ------------------------------- |
| E1M1   | 29.58 fps / 33.81 ms              | **29.97 fps / 33.37 ms**        |
| start  | 19.72 fps / 50.71 ms              | **21.12 fps / 47.56 ms**        |

E1M1's 33.37 ms sits on the two-vblank floor of 33.33 ms. The start map's window
mean of 47.56 ms is between two and three vblanks, which is what a mix of frames
does when some of them cross under 33.33 ms and the rest do not. So R31 does not
regress the game, with or without its instrumentation armed, and the build that
was left on the console is this one.

## What is still open

- **The per-slot split of the application's ~21.7 ms is not trustworthy.** The
  gap chain is one shared structure (ps5vk_queue.c) and the engine records on
  the main thread while presenting on a worker (`r_tasks 1`), so the two
  threads close each other's gaps and the slot a stretch lands in is not
  necessarily the thread that spent it. The numbers are strongly suggestive --
  `app_after_begin_ms` 3.6 and `app_after_end_ms` 14.8, identical in both maps
  to within 1% -- but they are not evidence yet. Either make the chain
  thread-local or run the decomposition at `r_tasks 0`, where there is one
  thread and the attribution is exact.
- The whole application cost is inside a driver entry point rather than between
  them: `app_pre_ms` 21.7 exceeds the sum of the six gaps (18.7), and the
  difference is the duration of `vkGetQueryPoolResults`, every
  `vkBeginCommandBuffer`, every `vkEndCommandBuffer` and the acquire, which are
  timed as gaps and never as calls. Timing those calls themselves is the next
  instrument, and it is the one that says whether the ~20 ms is the driver's
  command-buffer bookkeeping or the engine.
- R29 remains unmeasured against R28 for E1M1 by anything other than this run's
  agreement, which is what a change to a CPU address helper should look like.
- Everything from section 12 of the assignment -- movement, firing, save/load,
  all eight shareware maps, a long soak -- is still not done.

## The display is 60 Hz, and the high-frame-rate mode is refused

The console was moved to my 4K120 Hz TV, so the display path was
measured rather than assumed. Two short runs, no fixture and no driver
profiling, with only the probe flags staged.

**Measured refresh: 16.6831 ms, 59.941 Hz**, spread 0.049 ms over 60 intervals.
The console negotiates 4K60 with a 120 Hz panel. Nothing about the TV's
capability reaches VideoOut on its own.

`sceVideoOutIsOutputSupported(handle, 15, NULL, NULL, NULL)` returns **1**, so
the console says the mode is available. `sceVideoOutConfigureOutput(handle, 15,
NULL, NULL, NULL)` is **refused with 0x80290016**, and refused identically
before the framebuffers are registered and after them, so the point in the
port's life is not what decides it. What 0x80290016 means is not known and is
not guessed here; what is known is that the mode did not take effect, and the
probe measures the period after configuring precisely so that a return code
cannot be mistaken for a mode change.

The value 15 is a hypothesis taken from the publicly released ps5-opengl runtime
this project already vendors, not a documented constant of this driver, and the
probe records it as asked-for rather than as understood. The mode is restored in
the same call because that runtime records that a high-frame-rate port outlives
the process that opened it; a probe that could leave a panel in a mode it never
restored would be a worse instrument than no probe.

The driver reports 60000 millihertz and does not change it. A 120 Hz claim needs
the measured period to halve, and it did not.

Next for this thread, in order: read what the console offers, by asking
`IsOutputSupported` over a bounded range of modes and configuring only those it
reports -- reported support is the console's own gate, so that stays inside
supported interfaces; and settle the console-side setting, because a title
cannot select a mode the system has not enabled and that is not readable from
inside the title.

`call_begin_ms` is 2.4 ms per frame. The first version of the call timing put
the stretch chain in thread-local storage, which is wrong: the engine records on
one thread and presents on another, so two per-thread chains measure overlapping
wall-clock and their sum exceeded the frame it was meant to partition (929 ms of
"named" time in a 50 ms frame). The chain is shared again; only the enter/leave
pairing of a single call is per thread, which is what makes the call durations
valid.

## R32: the draws are not the cost

Every draw in the driver passes through `ps5vk_cmd_draw`, so one probe pair there
counts them and times them. `gap_count` on that slot is the draw count, `call_ns`
is the driver's own per-draw cost, and `gap_ns` is what the engine spends between
two draws. Steady windows of a trimmed two-map fixture, first window of each
phase dropped:

| per frame                    | start map | E1M1  |
| ---------------------------- | --------- | ----- |
| draws                        | **40.2**  | **30.3** |
| the driver's draw encoding   | 0.93 ms   | 0.71 ms |
| between two draws            | 10.20 ms  | 8.71 ms |
| `vkBeginCommandBuffer` calls | 2.44 ms   | 2.48 ms |
| `vkEndCommandBuffer` calls   | 0.61 ms   | 0.61 ms |
| acquire + query results      | 0.04 ms   | 0.04 ms |
| the application's own CPU    | 24.66 ms  | 24.68 ms |

**A Quake frame is thirty to forty draws.** Whatever costs the application
twenty-odd milliseconds a frame, it is not proportional to draw count and it is
not the driver's draw encoding: encoding every draw in the frame costs under a
millisecond, about 23 microseconds each.

What is disproportionate is `vkBeginCommandBuffer`. The driver spends 2.44 ms a
frame there -- two and a half times what it spends encoding *all* the draws --
for the handful of command buffers the engine begins. That is now the largest
driver-side item inside the application's own time, and it is the next thing to
read: `ps5vk_BeginCommandBuffer` and the `vk_command_buffer_begin` under it, for
a per-begin cost of roughly a quarter of a millisecond, which nothing that
function does on its face justifies.

Caveat, because it matters: this run was slow again, at 16-18 FPS against the
baseline's 19.7 and 29.6, with multi-second `frame_max_ms` in every window. The
counts and the relative driver costs are sound; the absolute millisecond values
are inflated by whatever it is that stalls this console run to run. The draw
count is a count, and the draw encoding is measured with the same timestamps as
everything else in the same window, so the comparison between them holds even
though the window does not.

`between_draws_ms` at 8.7-10.2 ms is the largest single stretch, but it is by
construction "everything between two draws" -- engine code and every driver call
that is not a draw -- so it narrows the search rather than naming a cause.
