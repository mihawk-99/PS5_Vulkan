# R28: split CPU copies and synchronization out of queue timing

Default-off profiling now adds copy_ms, sync_wait_ms and sync_signal_ms.
copy_ms includes CPU-side split actions (copies, mip blits, fills, clears,
queries and events), including their own cache flushes. All three are included
in queue_ms; they are not independent times to add to that total. Existing
flush_ms measures target flushing outside the copy loop. No packet, wait or
copy behavior changes. Normal launches still need no profiling flag.

PS5 game PID 261, identity d584176e…, completes two 1,200-frame phases and
writes two PNGs, then exits normally. Seven steady start intervals average
24.095 ms copy, 33.222 ms queue, 5.954 ms target flush, 2.754 ms native-submit/
marker, and 14.415 ms flip. Three steady E1M1 intervals average 0.041 ms copy,
6.905 ms queue, 3.974 ms target flush, 2.577 ms submit/marker, and 7.431 ms flip.
Both report 0.021 ms each for wait and signal. First mixed interval omitted
in each phase. The start map's water mip generation is the next candidate;
these aggregate counters alone do not identify an individual copy operation.

Explicit rebuild, full host/cache suite, eleven gates, port five gates/shader
scan and template relink pass. With PS5VK_PROFILE=1, the five R27 submissions
still replay exactly. Deployed ELF read twice/all load segments match; final
trace and both PNGs read twice identically. Native exit, 7,082,496 audio frames,
zero errors; console idle. Configurations and profiling flag restored absent.
Port evidence m6-copy-profile holds the application run. To reproduce, use its
autoexec with a title-local ps5vk-profile.txt, following the deployed proof and
listener-before-launch procedure. Metrics are recorded in metrics.txt.
