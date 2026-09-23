# R33: what a vkBeginCommandBuffer spends its time on

R32 measured 2.44-2.48 ms a frame inside `vkBeginCommandBuffer`, two and a half
times the cost of encoding every draw. R33 splits it, default-off like the rest
of the profile, and changes no packet, wait, flip or copy.

## The instrument

Appended to the single `[ps5vk] profile2` write:

- `secondary_begins/frame`, `call_begin_secondary_ms`: begins of secondary
  command buffers and their time. Only a secondary keeps a Mesa `vk_cmd_queue`
  (its commands are encoded when a primary executes it), and resetting that
  queue is `vk_free_queue` + `linear_free_context` + a new linear context with a
  64 KiB minimum buffer.
- `resets/frame`, `reset_common_ms`, `reset_driver_ms`: inside
  `ps5vk_cmd_buffer_reset`, Mesa's `vk_command_buffer_reset` (which holds the
  queue reset) apart from this driver's `ps5vk_cmd_buffer_clear_state`.

The summary buffer grew from 1024 to 2048 bytes: the two lines together were
already close to 1024 and would otherwise have been truncated silently.

Host: `tools/build-driver.sh` 0 errors/warnings (logs read), `build/gates.sh`
11 PASS, `tools/check-driver.sh` PASS (golden replays unchanged with profiling
off), `tools/check-shader-cache.sh` PASS.

## Console result (2026-09-23, trimmed R32 fixture, profiling on)

Port evidence `m6-r33-begin-split` (identity ce0ea9a8) and `m6-r33-span-cache`
(identity 28ecb4f8, the port's allocator change being the only difference):

| per frame, E1M1 steady windows | before   | span cache |
| ------------------------------ | -------- | ---------- |
| begins / secondary begins      | 30 / 26  | 30 / 26    |
| `call_begin_ms`                | 4.3      | 2.8        |
| `reset_common_ms` (Mesa)       | 2.45     | 0.92-0.97  |
| `reset_driver_ms`              | 0.62     | 0.62       |

**A frame begins 30 command buffers, 26 of them secondaries** - not the four to
fourteen assumed from `PCBX_NUM`. The Mesa part of each reset was ~82 us, and
the port routes every allocation of 32 KiB or more to `mmap`, so each
secondary's 64 KiB linear context cost an `munmap` and an `mmap` per frame. The
port now keeps a bounded cache of released spans; the same reset is ~31 us.

**It did not change the frame rate.** With profiling off, the same fixture on
the same driver gives E1M1 34.18 vs 34.19 FPS and start 25.41 vs 26.51 without
and with the cache (`m6-r33-control-noprofile`, `m6-r33-span-cache-noprofile`).
It is less work, not a crossed vblank.

Caveats. The first run stalled (multi-second `frame_max_ms`, 4-13 FPS) and only
its counts and per-call costs are quoted. Both profiled runs show exactly one
long hitch in every ten-second window (`periods=... 64:1`), at the report's
cadence, and their traces show the driver's single summary `fputs` split
mid-line by the audio thread's line and, at the harness close, cut off
mid-write. The report write itself is therefore the leading suspect for the
"unattributed" stall; it is timed next rather than asserted here.

## R34: the report write is the stall, and a system call costs 20 us

Two more default-off fields and a one-time line, still one write each:
`last_write_ms` (the previous summary's `fputs` duration, carried across the
window reset), `clock_ns`, and `[ps5vk] cost probe`, which times 10,000 calls of
each time source between two clock reads when profiling is enabled.

**The once-per-window hitch of every profiled run is this driver's own summary
write.** `last_write_ms` equals the next window's `frame_max_ms` each time:
1606/1657, 2098/2146, 1726/1773, 3124/3397, 5783/5817, 5331/5373, 5830/5874 ms.
One ~1.9 KB `fputs` to `/app0/trace.txt` takes 1.6-5.8 s. The report runs inside
`ps5vk_queue_flip` after the present is confirmed, so that time lands in the next
frame's period. Every window-mean millisecond from a profiled run since R31 is
inflated by it; per-call costs and counts are not. Port evidence
`m6-r34-report-write`.

**Every system call costs ~20 us on this console** (port evidence
`m6-r34-cost-probe`):

| 10,000 calls each               | ns per call |
| ------------------------------- | ----------- |
| `clock_gettime` (os_time)       | 20,278.8    |
| `getpid`                        | 20,123.6    |
| `sceKernelReadTsc`              | 11.8        |
| `sceKernelGetProcessTimeCounter`| 12.1        |
| uncontended mutex lock+unlock   | 16.3        |

The TSC ticks at its reported rate: 32,167,462 ticks over 20.150 ms = 1.5964 GHz
against 1,596,300,232 Hz reported. Consequences: each probe pair of this
profile adds ~20-40 us, so R32's "23 us a draw" and much of R33's begin cost are
the clock reads themselves; and anything, in the driver or the engine, that
enters the kernel per frame (`mmap`, a contended lock, a condition wake,
`usleep`, `write`, a clock read) pays ~20 us each time.

## R36: the profile is nearly free, and the first trustworthy split

Profile timestamps now come from `ps5vk_profile_now()`: the TSC converted to
nanoseconds on the console (12 ns a read), `os_time_get_nano` on the PC. The
summary leaves as one `write(2)` on `fileno(stderr)`. A first version wrote to
`STDERR_FILENO` and its lines reached neither the trace nor the kernel log (port
build d1b39125): the port's `freopen` does not keep descriptor 2 on this libc.

Profiled E1M1 work is now 23.74 ms against 23.67 unprofiled, and the summary
write 1.05 ms against R34's 1.6-5.8 s. Port evidence `m6-r36-cheap-profile`:

| per frame                   | E1M1   | start map |
| --------------------------- | ------ | --------- |
| period                      | 29.25  | 37.60     |
| application (`app_pre_ms`)  | 17.20  | 17.21     |
| queue                       | 6.53   | 20.17     |
|   flush                     | 3.92   | 5.82      |
|   CPU copies                | 0      | 11.55     |
|   submit + marker poll      | 2.53   | 2.69      |
| flip                        | 5.51   | 0.21      |
| `vkBeginCommandBuffer`      | 0.33   | 0.23      |
| all draw encoding           | 0.075  | 0.091     |

R32's 2.44 ms of begins and 23 us per draw, and most of R33's begin cost, were
the 20 us clock reads. The E1M1 period is a constant 29.2 ms (frame_min 29.168)
and the start map's is not a multiple of 16.68 ms either, so the display is not
presenting on a fixed 60 Hz grid during gameplay; a variable-refresh model with a
~48 Hz floor (20.83 ms) and 8.33 ms scan, 20.83 + 8.33 = 29.17 ms, fits every
measured period, and predicts E1M1 work below ~20.8 ms presents without the
wait. That is a model, to be tested by cutting work, not a finding.
