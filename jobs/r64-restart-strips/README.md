# R64: primitive restart is state, and every change of it waits behind an event

Wind Waker's 3D scene drew one huge near-camera surface whose colours changed
from frame to frame. A FIFO log of ten frames bisected it to one draw: objects
0-89 drew pixel-identically to desktop Dolphin, and object 90 added the
surface. That draw was the first with more than 2048 indices (2138 16-bit
indices, 1740 vertices, 398 short triangle strips each ended by a restart
index, at vertexOffset 271091 and firstIndex 250975). With Dolphin drawing lists
instead of restart strips, the same prefix drew correctly.

The probe draws the same shape: short strips (3 to 16 vertices, every vertex
used once and in order, in about Wind Waker's proportions), each ended by
0xffff, one strip per cell of a 40x30 grid in a colour of its own, and checks
every pixel against the layout. Its twelve frames vary the index count across
2048 (1894, 2047, 2049, 2138, 4096), the offsets, the strip shape (all quads,
all single triangles, six-vertex strips), restart itself (the same strips
joined by degenerate triangles through a pipeline without restart), a long draw
(64 instances of 4096 indices), and a change of restart between two draws of
one command buffer, the second without restart fetching vertex 0xffff.

Before the fix every restart frame failed, whatever its count, from about the
300th index on: the first strips drew, then triangles joined strips and ran to
the centre of the target (vertex (0,0), an out-of-range fetch). The control
without restart passed. R58 had bracketed each restart draw with
VGT_MULTI_PRIM_IB_RESET_EN: on before it, off with a bare uconfig write right
after it. That write took effect while the draw was still fetching indices, so
the rest of the draw fetched its restart indices as vertices. An A/B on the
console (one build, four variants of the write after the draw) measured it:

| After the restart draw | Restart frames |
| --- | --- |
| RESET_EN = 0 (R58) | all fail |
| nothing | all pass |
| SQ_NON_EVENT, then RESET_EN = 0 | all pass, the 64-instance draw included |
| VS_PARTIAL_FLUSH, then RESET_EN = 0 | all pass |

The SQ_NON_EVENT is RADV's own handling on GFX10 and GFX10.3
(radv_emit_primitive_restart; Mesa's ac_gpu_info has_prim_restart_sync_bug):
RADV writes the event before every change of the enable, and never changes it
after a draw. The 64-instance frame shows the event orders the write behind
the draw rather than delaying it.

The driver now does the same. The enable is command-buffer state: a draw
writes it only when it needs the other value (on for an indexed draw through a
restart pipeline, off for every other draw), each write behind an
SQ_NON_EVENT, and vkEndCommandBuffer puts it back to off behind one more, so
every command buffer starts and ends with restart off. A command buffer that
never restarts records exactly the words it did before R58; a run of restart
draws (all of Dolphin's) writes the enable once. R58's two frames now carry
the event before each write, and its golden is re-captured.

Console: PID 435 is the A/B (ab-readback.txt, ab_check.py). PID 449 is the fix:
all twelve frames PASS with zero mismatches over 8294400 pixels each; R58,
R57, R59-R63, r15-dynamic-pair, d1-dynamic-ubo and v0-push-constant pass in the
same run, and readback.txt and the golden are that run's. Every frame's buffers
are the same size (Dolphin's offsets plus a full 16-bit index range), so the
console places every frame's allocations alike and the PC replay can pin them.

The first run with the fix (PID 438) passed with buffers that ended at their
draws. Giving the buffers poison past the draws exposed a fault in the test
harness, not the driver: driver/tests/ps5vk_triangle.c set a frame's
draw_index_count only when the frame had a uniform buffer, so these frames drew
the whole index buffer (the driver's robustness clamp cut it at the buffer's
end) and took one poison index as a strip's last vertex. The harness now sets it
for every frame; nothing else passed a count.

Host gate: driver/tests/vk_v0_topology_test.c's restarted quad asserts that the
submission changes the enable exactly twice, on before the draw and off after
it, each change right behind an SQ_NON_EVENT, and that no other topology's
submission changes it.

The replay also needed a fix in the PC model (host/ps5/ps5_host.cpp): a
captured stage's image lines follow their own "stage" line, and the stage index
restarts with every device a test creates. The model attached each image line
to the first stage of that index, so a capture whose frames placed their stages
at different addresses overwrote frame 1's relocated headers with the later
frames' ("a created shader's register tables are out of bounds"). It now takes
the latest stage of that index. R57-R63 still replay word for word.

Reproduce from the driver root:

    python3 jobs/r64-restart-strips/check.py jobs/r64-restart-strips/readback.txt
    python3 jobs/r64-restart-strips/ab_check.py jobs/r64-restart-strips/ab-readback.txt
    python3 jobs/r64-restart-strips/replay.py

Console: deploy the runner while idle, run tools/ps5_console.py battery
PPSA99988 with this queue.txt, then restore jobs/regression/queue.txt.
