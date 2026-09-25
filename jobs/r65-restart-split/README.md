# R65: every submission starts with primitive restart off

After R64, Wind Waker drew correctly except for thin triangles that ran from
the top-left corner of the screen to the minimap, and the minimap's panel
spilling to the left edge. The same FIFO log played by desktop Dolphin had
neither. Bisecting it by object range narrowed them to objects 310-319, the
minimap. Every screenshot of that range differed from desktop Dolphin (mean
difference 0.43-0.52 per channel, 15 of 15). With Dolphin drawing lists instead
of restart strips, all 15 matched (0.12-0.20). With Dolphin's software vertex
loader, or with its line and point draws skipped, nothing changed.

Three driver A/B switches then separated the causes, over the same range:

| Restart enable | Objects 310-319 |
| --- | --- |
| R64: written when it changes, behind SQ_NON_EVENT | 0.43-0.52, broken |
| VS_PARTIAL_FLUSH instead of SQ_NON_EVENT | 0.43-0.52, broken |
| written with SET_UCONFIG_REG instead of a table load | 0.43-0.52, broken |
| written before every indexed draw | 0.12-0.20, correct |

So the GPU lost the enable while the recording held it on. The driver runs a
copy (vkCmdCopyBuffer, vkCmdFillBuffer, a CPU image copy and the like) at a
point where it splits the command buffer's submission (ps5vk_queue.c), and the
console starts every submission with VGT_MULTI_PRIM_IB_RESET_EN off. Restart
strips recorded after such a copy with no draw between them ran without
restart, and their restart indices were fetched as vertices: out-of-range
fetches at the origin, the corner of Dolphin's 2D projection.

The probe draws R64's 2138-index restart strips in two render passes of one
command buffer, the second loading what the first drew, with a scratch buffer
filled between the passes (a split), so no draw falls between the two passes'
draws. Before the fix (PID 501, before-readback.txt) that frame failed: 469597
of the strips' pixels wrong and 1365728 drawn where nothing should be. Its two
controls passed: a second pass that clears (the clear is a draw, which writes the
enable) and the same strips without restart across the split. The clearing
control is not kept: it is the one frame without the scratch buffer, so the
console placed its tables 0x4000 lower than the PC replay pins them.

The fix: a split recorded since the enable was last written leaves it off
(ps5vk_draw.c, ps5vk_cmd_buffer_restart_after_splits), so the first restart draw
after a split turns it on again. A third frame checks the other half of that
statement: across a split, a draw through a pipeline without restart fetches
vertex 0xffff with no write before it, which it could not if the GPU had kept
restart on. Command buffers without restart record the words they did before.

Console: PID 506, all three frames PASS with zero mismatches over 8294400
pixels each; R58, R62, R63 and R64 pass in the same run. The golden and
readback.txt are that run's. In the game, the minimap and the corner triangles
now match desktop Dolphin (../PS5_RetroArch, docs/PHASE_LOG.md).

Host gate: driver/tests/vk_v0_topology_test.c draws the restarted quad in two
passes with a copy between them and asserts three restart changes (on, on again
in the step after the split and before its draw, off at the end), each behind an
SQ_NON_EVENT.

The harness (driver/tests/ps5vk_triangle.c) gained split_between_passes: the
fill between two passes, a loading second pass and, with a second pipeline, one
pipeline a pass.

Reproduce from the driver root:

    python3 jobs/r65-restart-split/check.py jobs/r65-restart-split/readback.txt
    python3 jobs/r65-restart-split/replay.py

Console: deploy the runner while idle, run tools/ps5_console.py battery
PPSA99988 with this queue.txt, then restore jobs/regression/queue.txt.
