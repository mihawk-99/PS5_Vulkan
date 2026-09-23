# R37: flush colour targets only in memory the application has mapped

`ps5vk_queue_flush_targets` clflushed every distinct colour target before and
after every submission step: 256 MiB and 3.92 ms a frame at E1M1, 384 MiB and
5.82 ms at the start map (R36, port evidence `m6-r36-cheap-profile`), for 4K
targets the game never maps.

## Who reads a target on the CPU

- the driver's own copies, clears and uploads: each flushes what it wrote after
  writing and invalidates what it reads before reading;
- the driver's blits and resolves: they flushed their source only *after*
  reading it, relying on the post-step target flush. They now invalidate the
  whole source image before reading (`source_span` on the record);
- query pools: still flushed every step (`always`), and read through
  `vkGetQueryPoolResults`, which flushes;
- memory the application maps: still flushed every step. The first
  `vkMapMemory` of an allocation also evicts its whole range once;
- the test runner's `ps5vk_debug_image_storage`, which hands out a raw pointer
  to an image (swapchain images included): it now invalidates the image's range
  when it hands it over.

Everything else - a colour attachment in memory never mapped, or a swapchain
image, which has no memory object - is no longer flushed.

## Verification

Host: build clean (logs read), 11 gates PASS, `check-driver.sh` PASS (golden
replays unchanged), `check-shader-cache.sh` PASS. Archive `2653305e...`
(`archive.txt`); baseline archive for the comparison `880b239d...`.

Console runner battery, every runner test except the faulting `b8-indirect`, in
seven queues of at most 32 (`part*/queue.txt`; the runner reads only
`jobs/queue.txt`, and refuses more than 32 tests). The same queues on the
candidate and on the pre-change driver, per-test statuses in `candidate.txt`
and `baseline.txt`, runner records in `runs/`:

    python3 jobs/r37-mapped-flush/tests.py diff jobs/r37-mapped-flush/baseline.txt jobs/r37-mapped-flush/candidate.txt
    baseline 138 tests, 130 PASS; candidate 138 tests, 130 PASS
    identical

The eight non-passing tests are the same on both (c0-dispatch, c1-flip first in
a queue, c5-depth-noclear, c5-depth-nostate, e2-module-load, two
unknowns-depth4x measurement probes, v0-lines). `v0-mrt` stops the runner on
both drivers (no `run_end` after it; part 4), so part 4b reruns part 4 without
it. The runner left on the console is the baseline build.

Game (port evidence `m6-r37-mapped-flush`, profiled, trimmed fixture): flush
3.92 -> 0.002 ms (E1M1), 5.82 -> 0.003 ms (start); start map 26.7 -> 31.25 FPS
(work 37.3 -> 31.8 ms); E1M1 34.19 FPS in one window and 39.28 in the other
(work 21.6 / 20.8 ms). Both readbacks correct.

Display note, added after the run: the owner reports that VRR "Apply to
Unsupported Games" was only now enabled, so none of the runs above had VRR, and
the variable-refresh model in `jobs/r33-begin-split` does not explain the
29.25 ms E1M1 period. Later runs are under a different display setting.
