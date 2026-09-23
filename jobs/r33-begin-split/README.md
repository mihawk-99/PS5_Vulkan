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
