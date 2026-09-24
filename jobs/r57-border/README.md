# R57: clamp-to-border samplers and the three border colour types

Dolphin's libretro core creates its static samplers with
VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, and the driver refused every border
mode by name, so Dolphin's Vulkan object cache failed to initialise
(../PS5_RetroArch, 2026-09-24). Clamp-to-border and the six border colours are
core Vulkan 1.0.

The address-mode field of descriptor word 8 is the hardware's SQ_TEX_CLAMP, whose
values the driver already used for repeat (0), mirrored repeat (1) and
clamp-to-edge (2); clamp-to-border is its 6, as in RADV's public radv_tex_wrap.
The colour is word 11's BORDER_COLOR_TYPE, bits 30-31: transparent black 0,
opaque black 1, opaque white 2, the float and integer forms sharing a type as in
RADV's radv_tex_bordercolor. Custom border colours (VK_EXT_custom_border_color)
and mirror-clamp-to-edge (VK_KHR_sampler_mirror_clamp_to_edge) are not exposed
and stay refused.

The probe draws a full-screen quad sampling a 4x4 texture of one colour with
coordinates from -0.5 to 1.5: the middle quarter is the texture and the rest is
the border. Frame 0 is the clamp-to-edge control; frames 1-4 are transparent
black, opaque black, float opaque white and integer opaque white.

The first console run had the right colour in every corner and 5998 mismatched
pixels per border frame: the one-pixel ring 0.001 texel from the texture's edge.
That is inside the precision the device reports (subTexelPrecisionBits 4, 1/16
texel), where Vulkan allows either side, so I changed the probe to skip pixels
within 1/16 texel of the edge (512160 of 8294400 per border frame) and compare
every other one exactly.

PS5 PID 290: 5 of 5 frames PASS, zero mismatches; the control frame compares
all 8294400 pixels. c7-mip-nearest, run beside it with the same shaders, passes.
All five submissions replay on the host word for word.

Host gate: driver/tests/vk_c4_texture_test.c creates all six border colours,
checks the descriptor's clamp-border and opaque-white words, draws through the
sampler, and checks that mirror-clamp-to-edge is still refused.

Reproduce from the driver root:

    python3 jobs/r57-border/check.py jobs/r57-border/readback.txt
    python3 jobs/r57-border/replay.py

Console: deploy the runner while idle, then run tools/ps5_console.py battery
PPSA99988 with this queue.txt, and restore jobs/regression/queue.txt afterwards.
Captures are in golden/r57-border.
