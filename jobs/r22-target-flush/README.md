# R22: remove duplicate target cache evictions

R21 measured repeated target flushing in vkQuake. A target image registered by
multiple passes, or a query pool registered by multiple writes, appears more
than once in a submission's target list. During one flush operation nothing
writes between those entries, so identical address/byte ranges need only one
eviction. Every distinct range, pre/post step flush, GPU barrier, completion
wait and CPU-copy boundary is retained. Partly overlapping ranges are untouched.

Acceptance requires the queue's repeated-pass, subpass, render-to-texture,
query/timestamp and resolve readbacks, with captured command streams replayed
exactly. Then relink and measure the same vkQuake demo workload against R21.
The profiler measures bytes actually flushed after deduplication.

Accepted on console PID 224: 1,043 PASS, zero FAIL, seven cases complete.
The repeated-pass case ran 120 frames with matching first/last folds. Both
subpass images match every pixel in two frames; RTT, queries, timestamps and
resolve readbacks pass. Fourteen captured submissions replay exactly. The
query replay allocation-order correction is explained in replay-ordering.txt;
no golden command word or comparison tolerance was changed.

PID 223 was interrupted by the 180-second harness limit during final resolve
readback. Its incomplete result is retained separately; it is not a full pass.
The identical runner completed with the 600-second harness limit in PID 224.

Port PID 225 then ran 300 seconds: 6,557 presents, 532 shader-cache hits,
zero reported game/audio errors. Both final trace reads and deployed ELF
segments match, kernel PID agrees, title closed and idle verified. The first
27 steady profile intervals contain 6,357 frames versus R21's 6,334. Weighted
flush cost fell 7.872 -> 5.788 ms/frame and 509.54 -> 373.73 MiB/frame; queue
15.202 -> 13.113 ms, native submit/marker 2.852 -> 2.854 ms and flip wait
7.787 -> 9.821 ms. Frame rate remains about 20–30 FPS; no FPS gain claimed.
R21 had 99 cold shader compiles, R22 none, so total run frame counts are not
an equivalent startup benchmark. Full port evidence: m5-r22-flush-run.

Reproduce readbacks with check.py and strict replays with replay.py from the
repository root. Host checks, eleven gates, port gates/scan and template relink
pass; archive identity is in gates.txt.
