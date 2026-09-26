# R83: both aspects of D32_SFLOAT_S8_UINT, sampled and copied

LRPS2 creates its depth target as D32_SFLOAT_S8_UINT with sampled and transfer
usage, samples it (depth conversion, `test_and_sample_depth`) and refuses to
start unless the format reports both. The driver reported it as a
depth-stencil attachment only.

The format's two aspects are two planes of one allocation: the depth surface,
laid out as D32_SFLOAT's is, and the one-byte stencil plane at the next 64 KiB
boundary, in the one-byte Z_X map AddrLib gives (15 terms,
ps5vk_tiled_stencil_terms; tools/check-mip-layout.sh holds them against
AddrLib). An aspect's view samples its own plane -- the stencil as R8_UINT --
and uploads, readbacks, clears and copies each take one aspect. Copies between
Z-mapped images now move the run the map keeps contiguous (two texels), which
also fixes a latent fault in D32_SFLOAT tiled copies. Stencil formats are one
level, one layer and one sample, which the format properties now say.

The probe (`r83-depth-stencil`, the r83-quadrant pixel stage) clears depth to
0.375 and stencil to 0, writes depth 0 and stencil 0x5a in the lower right
quadrant through the depth block, then samples one aspect per frame through a
view of it and reads both planes back through vkCmdCopyImageToBuffer.

PS5 PID 172: both 4K frames match in every pixel -- depth 96 of 255 outside
the quadrant and 0 in it, stencil 0 and 0x5a -- and both planes read back
exact after each frame. v0-stencil and v0-formats pass unchanged; the format's
optimal features are 0xc201. Host: vk_r83_depth_stencil_test.c checks each
aspect's upload lands in its own plane at the map's offsets, readbacks,
per-aspect clears, aspect copies and the refusal of cross-aspect copies.

Reproduce from the driver root:

    python3 jobs/r83-depth-stencil/check.py jobs/r83-depth-stencil/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw log is the
ignored Klog_Logs/r83-depth-stencil.log.
