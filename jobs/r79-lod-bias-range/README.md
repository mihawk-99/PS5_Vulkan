# R79: sampler LOD bias to +/-16

Mario Kart Wii (Dolphin) asks for sampler biases of -2.1875, -3 and -3.1875.
The device reported maxSamplerLodBias 2, Vulkan's minimum, and refused those
samplers with VK_ERROR_UNKNOWN, so every surface they textured drew black -- on
Luigi Circuit, the whole grass bank and palm tree left of the track (title
ad98b960, three refusals in 100 s). The limit was the driver's own choice:
word 2's LOD_BIAS field is signed with 8 fraction bits in 14 bits, room for
+/-32, and RADV reports 16 on the same hardware generation.

The device now reports 16 and accepts biases inside it; the encoding is R26's.
The new probe (probes/r79-lod-bias-range, shaders/c7/lod-bias-wide.frag) draws
a 512-square, ten-level chain, one grey a level, with derivatives of 1/16 a
pixel, which select LOD 5. Twelve frames change only the bias: 0, -3, -5, +4,
+3.5, -2.5, -3.5, -16, +16, -8, +8, 0. Integer biases land on their own level,
half-level ones on an exact blend of two, and +/-8 and +/-16 on the chain's
ends -- where a field too narrow for them would wrap and read a middle level.
A 2048-square chain was the first attempt; its 22 MB of staging texels is more
than the runner's heap gives, and the helper refused it before any sampler.

PS5 PID 121: every one of the twelve 8,294,400-pixel frames matches exactly;
R26's eight frames pass unchanged in the same run.

Mario Kart Wii from my save state on title dce4be84 (the new limit, before this
probe): no refusal, and the grass bank and palm tree draw. klog/mkw-base-* and
klog/mkw-r79-* in ../PS5_RetroArch hold the before and after screenshots.

Reproduce from the driver root:

    python3 jobs/r79-lod-bias-range/check.py jobs/r79-lod-bias-range/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw log is the
ignored Klog_Logs/klog-20260925-111907.log.
