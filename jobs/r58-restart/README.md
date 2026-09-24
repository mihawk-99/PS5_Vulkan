# R58: primitive restart on indexed triangle strips

Dolphin's Vulkan backend draws its strips with primitiveRestartEnable, and the
driver refused every such pipeline, 101 of them in Wind Waker's first 30
seconds (../PS5_RetroArch, 2026-09-24). Primitive restart is core Vulkan 1.0 for
strip topologies.

An indexed draw through a restart pipeline is bracketed by the uconfig register
VGT_MULTI_PRIM_IB_RESET_EN (0x3092c, table record 0x24b): 1 before the draw and
back to the hardware's 0 after it, with the context register
VGT_MULTI_PRIM_IB_RESET_INDX (0x2840c, record 0x103) set to all ones. From GFX9
only the index type's own bits are compared, so the one value serves 16- and
32-bit indices. This is RADV's public radv_emit_primitive_restart for GFX9-10.
Every other draw records exactly the words it did before, which is what every
existing golden holds. Restart on a list topology is
VK_EXT_primitive_topology_list_restart, which is not exposed, and stays refused.

The probe draws two quads, left and right with a gap between them, as one
indexed strip {0,1,2,3, 0xffff, 4,5,6,7}; the control draws the same quads as
one strip joined by degenerate triangles through a pipeline without restart.

PS5 PID 300: both frames PASS with zero mismatches over all 8294400 pixels; the
gap is the clear colour 0xffff8040 and both quads are 0xff40ff80, so the two
images are identical. v0-strip and r57-border pass beside it. Both submissions
replay on the host word for word.

Host gate: driver/tests/vk_v0_topology_test.c draws a restarted quad and checks
that its tables load the all-ones reset index and leave the enable at 0, and
that no other topology's draw records either register.

Reproduce from the driver root:

    python3 jobs/r58-restart/check.py jobs/r58-restart/readback.txt
    python3 jobs/r58-restart/replay.py

Console: deploy the runner while idle, run tools/ps5_console.py battery
PPSA99988 with this queue.txt, then restore jobs/regression/queue.txt.
