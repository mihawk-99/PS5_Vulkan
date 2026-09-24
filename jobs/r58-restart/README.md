# R58: primitive restart on indexed triangle strips

Dolphin's Vulkan backend draws its strips with primitiveRestartEnable, and the
driver refused every such pipeline, 101 of them in Wind Waker's first 30
seconds (../PS5_RetroArch, 2026-09-24). Primitive restart is core Vulkan 1.0 for
strip topologies.

Restart is the uconfig register VGT_MULTI_PRIM_IB_RESET_EN (0x3092c, table
record 0x24b) with the context register VGT_MULTI_PRIM_IB_RESET_INDX (0x2840c,
record 0x103) set to all ones. From GFX9 only the index type's own bits are
compared, so the one value serves 16- and 32-bit indices. R58 bracketed each
restart draw with the enable, 1 before and 0 right after; R64
(jobs/r64-restart-strips) measured that trailing write taking effect in the
middle of a long draw. Since R64 the enable is command-buffer state as RADV
keeps it: written only when a draw needs the other value, each write behind an
SQ_NON_EVENT, and put back to 0 at vkEndCommandBuffer. A command buffer that
never restarts records exactly the words it did before R58. Restart on a list
topology is VK_EXT_primitive_topology_list_restart, which is not exposed, and
stays refused.

The probe draws two quads, left and right with a gap between them, as one
indexed strip {0,1,2,3, 0xffff, 4,5,6,7}; the control draws the same quads as
one strip joined by degenerate triangles through a pipeline without restart.

PS5 PID 300: both frames PASS with zero mismatches over all 8294400 pixels; the
gap is the clear colour 0xffff8040 and both quads are 0xff40ff80, so the two
images are identical. v0-strip and r57-border pass beside it. Both submissions
replay on the host word for word. Re-captured with R64's words (PID 449): both
frames PASS again, and the golden and readback here are that run's.

Host gate: driver/tests/vk_v0_topology_test.c draws a restarted quad and checks
that its tables load the all-ones reset index and leave the enable at 0, that
the enable changes twice, each change behind an SQ_NON_EVENT (R64), and that no
other topology's draw records either register.

Reproduce from the driver root:

    python3 jobs/r58-restart/check.py jobs/r58-restart/readback.txt
    python3 jobs/r58-restart/replay.py

Console: deploy the runner while idle, run tools/ps5_console.py battery
PPSA99988 with this queue.txt, then restore jobs/regression/queue.txt.
