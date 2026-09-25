# R71: dual-source blending

Resident Evil 4 drew its whole frame in its haze colour: the characters as flat
silhouettes, only a bush and the HUD correct. A FIFO log of the scene recorded
on the console plays correctly in desktop Dolphin, so the commands were right;
desktop Dolphin reproduces the console's frame exactly with its Vulkan backend's
bSupportsDualSourceBlend forced off, and correctly with logic ops forced off
instead. The driver reported neither dualSrcBlend nor
maxFragmentDualSrcAttachments, so Dolphin took its fallback, which RE4's
destination-alpha haze does not survive.

The compiler already exports a pixel shader's second colour (location 0, index
1) to MRT1 with MRT0's format and sets CB_SHADER_MASK for both (psbc_compile.c,
RADV's mrt0_is_dual_src). The driver now reports dualSrcBlend and one dual-source
attachment, and maps SRC1_COLOR, ONE_MINUS_SRC1_COLOR, SRC1_ALPHA and
ONE_MINUS_SRC1_ALPHA to CB_BLEND0_CONTROL's 15 to 18 (gfx103's BLEND_SRC1_* are
numbered as Vulkan's).

The probe (r71-dual-source) is the constant-blend frame with the second source as
the factor: the pixel stage writes the vertex colour and {0.25, 0.5, 0.75, 1.0},
and the pipeline blends with the SRC1 factors, so the frame must be v0-blend-
constant's 0xff88586c. The probe build checks the compiler packed
SPI_SHADER_COL_FORMAT 0x44 (FP16 for MRT0 and MRT1).

Console: PID 630, r71-dual-source PASS -- all 8294400 pixels 0xff88586c, maximum
channel error 0 -- with m2-solid, v0-blend-constant, m4-blend and c4-rtt in the
same run. Resident Evil 4 from its save state draws Leon, the car and the forest
as desktop Dolphin does, at full speed.

Host gate: driver/tests/vk_b6_pipeline_test.c builds the r71-dual-source pipeline
with the SRC1 factors and finds its packages equal the probe's hardware-run
ones; vk_b2_device_test.c asserts the feature and the limit.

Reproduce from the driver root: deploy the runner while idle, run
tools/ps5_console.py battery PPSA99988 with this queue.txt, then restore
jobs/regression/queue.txt.
