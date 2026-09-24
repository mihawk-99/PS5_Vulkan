# R53: one VideoOut per process, offered truthfully, kept across swapchains

RetroArch showed about half a second of black whenever the menu opened or
closed, content closed or loaded, or a video setting needed a reinit. It
recreates its swapchain for each (and on content changes its whole Vulkan
context), and the driver closed VideoOut with every swapchain: drain, restore
60 Hz, unregister, close; then open, select 119.88 Hz, register two cleared
framebuffers. Every handover was an output-mode switch and a blank panel.

R51 also left an edge: the 119.88 Hz mode was offered on the strength of the
metadata and `sceVideoOutIsOutputSupported`, but selected only at swapchain
creation. Had the console refused it there, the swapchain would present at
59.94 Hz while the application, having read the mode, believed 119.88 Hz.

## The change (driver/ps5vk_wsi.c)

- **One output per process.** VideoOut and its framebuffers belong to the
  process, not to a device: a handle and a direct-memory mapping outlive any
  VkDevice, so one `struct ps5vk_video_out`, under a lock, serves every
  swapchain in turn. A second swapchain while one holds it is still refused
  (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
- **Offered only once accepted.** When the title declares high-frame-rate
  output, listing the display modes opens VideoOut and configures mode 15 there;
  the 119.88 Hz mode is listed only if the console accepted it, and the swapchain
  then presents through that same handle. A refusal closes the handle and leaves
  59.94 Hz as the only mode, so the refresh an application reads is the refresh
  the panel runs at. A swapchain on a mode other than the output's opens a fresh
  output in that mode.
- **Retention, opt-in.** `ps5vk_display_retain(bool)` (driver/ps5vk_debug.h).
  Retained, a destroyed swapchain leaves VideoOut open in its mode with its last
  image on screen, and the next swapchain takes the same framebuffers; acquire
  already starts on the buffer that is not shown, so the old image stays until
  the first new present. Released (the default, and what an application calls
  before it exits), an unowned output closes and restores the default mode.
  Titles that never call it behave as before.
- **Handover report.** One line per swapchain, after its first image that is not
  black (64 sampled texels, or its first 600 presents): whether the output was
  kept or opened, the gap since the previous present, how long until the first
  picture, and how many black presents came first. "default mode restored" when
  a close puts 60 Hz back.

## Verification

Host: build-driver, check-driver PASS, with three new c1_present checks (run
directly): a retained output outlives its swapchain and serves the next, is still
refused to a second swapchain, and closes when released.

Console, RetroArch FCEUmm with a pad script (port evidence display-retention):

| event | released (old behaviour) | retained |
| --- | --- | --- |
| menu open | output reopened, 296.7 ms gap | kept, 7.0 ms |
| menu close | reopened, 343.8 ms | kept, 6.4 ms |
| close content | reopened, 601.8 ms | kept, 279.6 ms, previous image on screen |
| run from History | reopened, 760.2 ms | kept, 437.4 ms, previous image on screen |

0 black presents after every handover. Quitting releases the output: "default
mode restored", then a clean exit. After a crash the system itself put the panel
back to 59.94 Hz (klog VideoOutStatus default=True).

vkQuake, which does not retain: "119.88 Hz selected" at mode listing, one
handover ("output opened"), 119.88 FPS at 4K.
