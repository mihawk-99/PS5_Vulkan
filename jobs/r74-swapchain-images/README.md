# R74: a swapchain has the images it asks for, up to five

A swapchain had three images whatever minImageCount asked for. At 120 Hz,
RetroArch's emulated swap interval of 2 presents each frame twice and fills the
two queued flips three images allow, so the second present of every frame waited
for a vblank on the core's thread; Dolphin's frame-stepped emulation lost 4-12%
of Wind Waker's speed under ubershaders there. A swapchain now gets its
minImageCount between three and five, and VideoOut registers five framebuffers.

`queue.txt` is C1's job: `c1-triangle` draws its four frames, then presents a
five-image replacement five times (images 3, 4, 1, 2, 0), with a capture.
`c1-flip` is the negative control and fails as it always has.

Console: PID 702, c1-triangle PASS, `agc_c1_five_images` PASS; the capture is
golden/c1-triangle. PID 700 re-captured golden/c4-rtt with jobs/c4-rtt, whose
barrier names the fence the new flip slots moved.

Host gate: driver/tests/vk_c1_present_test.c (check_replacement) makes the same
five presents and compares them with that golden, packet by packet.
