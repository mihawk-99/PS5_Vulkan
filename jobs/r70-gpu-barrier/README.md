# R70: GPU barriers instead of submission splits

A draw that samples an image rendered earlier in the same command buffer
carried the colour-buffer barrier (RELEASE_MEM event 45) and split the
submission there, because the barrier flushes but does not wait (C4). The wait
was the CPU's: the queue submitted the words before the split, waited for their
marker, then submitted the rest. Super Smash Bros. Melee's EFB copies split its
frames into about 105 steps a present (2.8 ms of queue time a present on my Pro,
and a whole refresh a step on a console that starts work at the next vblank,
R68), and every draw that sampled any image rendered anywhere earlier in the
command buffer paid it again.

Now the draw carries a GPU barrier: RELEASE_MEM of CACHE_FLUSH_AND_INV_TS_EVENT
(event 20, colour and depth caches flushed, vector and L1 caches invalidated)
writing a fence value, then WAIT_REG_MEM64 in the prefetch parser until the
fence holds it -- the console's own wait-until-safe packet has that form
(control 0x06000113). The queue gives each barrier a value the fence has never
held at submission, so a command buffer submitted again cannot pass a wait on a
stale value; the fence is two words in the submission buffer below the marker,
so no allocation moves. Only an image rendered since the command buffer's last
barrier needs another, and depth attachments now count as rendered targets.
Between command buffers of one submission the queue puts a barrier where the
first rendered and the second samples before a barrier of its own, and a
submission whose predecessor may still run (R69) starts by waiting on the GPU
for that step's marker.

Console: PID 614, m2-solid, c4-rtt, c4-texture and v0-subpass PASS -- c4-rtt is
the case whose barrier-without-wait read the image's last rows as zero before
C4's split (36006-40661 pixels, pids 143-145), and reads every texel now. PID 615
re-captured golden/c4-rtt as one submission (18 packets, the barrier included);
the host replay matches it. In Dolphin, Melee's steps a present fell from 105.8
to 0.07 and its queue time from 2.8 ms to 0.19 ms a present.

Host gate: driver/tests/vk_c4_rtt_test.c asserts one step whose barrier
releases and waits on the same non-zero fence value; b8_groups (two command
buffers that sample nothing) keeps its golden words.

Reproduce from the driver root: deploy the runner while idle, run
tools/ps5_console.py battery PPSA99988 with this queue.txt, then restore
jobs/regression/queue.txt.
