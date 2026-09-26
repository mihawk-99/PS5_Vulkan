# R85: the line width as dynamic state

LRPS2's hardware renderer declares VK_DYNAMIC_STATE_LINE_WIDTH on its
pipelines, and the driver refused every draw with dynamic state outside its
list, so the renderer's first submissions failed to record. Without wideLines,
Valid Usage holds every line width at 1.0 -- the width the driver already
programs for a line list (PA_SU_LINE_CNTL 8, R8) -- so the state is accepted,
and a width other than 1.0 set on a line pipeline is refused at the draw by
name rather than drawn at 1.0.

Host: vk_v0_topology_test.c draws two lines with the width set by the draw to
1.0 (the same line words as the static frame) and refuses 2.0 by name; loader,
direct and PS5 link pass.

PS5 PID 181: v0-lines' new fifth frame, the width set by the draw, is word for
word the static line frame (its frame hash and 2,460 lit pixels), and the
culling frames pass. The case's reference comparison still fails as it has
since its first console run: lines and rectangles alike draw the reference's
2,460 pixels elsewhere in the frame (docs/M5_PHASE_C.md, R8's open item). That
is not what this round measures and is unchanged by it.

Reproduce from the driver root:

    python3 jobs/r85-line-width/check.py jobs/r85-line-width/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw log is the
ignored Klog_Logs/r85-line-width.log.
