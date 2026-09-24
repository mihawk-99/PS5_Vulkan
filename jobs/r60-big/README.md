# R60: pixel stages larger than 20 KiB; submitting a refused command buffer

Dolphin's ubershader pipelines were refused ("shaders of 24652 bytes do not fit
before the linked context"): each pipeline's stage workspace put the linked
context at 0x5000 and the uniforms at 0x6000, which capped the shaders at
20 KiB. Dolphin then submitted the command buffer whose recording had been
refused, and the Vulkan runtime's assert in vk_queue_submit_add_command_buffer
aborted the title.

- Shaders that end before 0x5000 keep the runner's layout word for word, so
  every existing golden is unchanged. Larger ones place the linked context on
  the next 4 KiB region after their code and the uniforms on the one after, in
  a workspace grown in 64 KiB steps. The draw reads both from the pipeline.
- vkQueueSubmit(2) refuses a command buffer that is not executable with
  VK_ERROR_UNKNOWN and a sentence, and runs nothing in that submission.

The probe's pixel shader is generated (shaders/r60/big.frag): a thousand terms
the compiler cannot fold feed a test with a fixed outcome, so its 32 KiB of code
must draw (0.25, 0.5, 0.75, 1) on every pixel.

PS5 PID 337: PASS, zero mismatches over all 8294400 pixels (0xffbf8040), with
r59-fragcoord beside it. The submission replays on the host word for word; the
host AGC model now replays linked areas from any 4 KiB boundary of a captured
stage, not only 0x5000.

Host gate: driver/tests/vk_v0_topology_test.c creates and draws the 32 KiB stage,
then records a command buffer the driver refuses (a primary executing a
primary): vkEndCommandBuffer fails and vkQueueSubmit returns VK_ERROR_UNKNOWN.

Reproduce from the driver root:

    python3 jobs/r60-big/check.py jobs/r60-big/readback.txt
    python3 jobs/r60-big/replay.py
