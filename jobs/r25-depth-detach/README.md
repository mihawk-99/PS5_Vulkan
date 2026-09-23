# R25: depth state must end with the attachment

The vkQuake opaque HUD appears outside the world viewport, while transparent
HUD elements over the world disappear. The isolated probe renders overlapping
rectangles with depth, then draws them again into the same colour attachment
without depth in the same submission. The second draw must follow draw order;
the detached depth image must retain the first pass's data.

Before the fix, PID 243 has 518,400 wrong colour pixels, all in the overlap, and
zero wrong depth samples. The depth-control register was emitted only when a
depth attachment was bound, leaving the previous pass's test active afterward.
CmdEndRendering now explicitly writes DB_DEPTH_CONTROL=0 through the existing
AGC register helper before leaving a depth/stencil rendering. The next bound
draw writes its own state. This also closes command-buffer/submission boundaries.

PID 244: 439 PASS, zero FAIL. Five complete 8,294,400-pixel colour checks,
four complete depth checks, and four complete swapchain-copy comparisons pass.
Both D32 and D16 detached overlays are correct; original depth and no-depth
regressions pass. PID 245 adds passing stencil and depth-bias readbacks; its
three FAIL-labelled messages belong to the deliberate nonzero-clamp refusal.
Twenty-four new draw/flip submissions compare exactly on the host. See
replay-notes.txt for the failed fixture and replay setup, retained separately.

Run check.py after-readback.txt and replay.py from the repository root.
The before/after/stencil records and source hashes are retained here; command
captures live in golden/r25-depth-detach{,-before} and golden/r25-depth-stencil.
