# R72: texel buffers of the 32-bit record count

Dolphin sizes its texel stream buffer as the smaller of 16 MiB and
maxTexelBufferElements, and GPU texture decoding streams each texture through
it. The driver reported Vulkan 1.0's minimum, 65536, so the buffer was 64 KiB:
Resident Evil 4 under the accurate profile put "Failed to allocate 65536 bytes
from texel buffer" on screen, a 64 KiB texture that fit only an empty buffer.

A texel buffer's descriptor holds its element count in the 32-bit NUM_RECORDS
word (range / texel bytes, ps5vk_draw.c), as RADV's does, and RADV reports
UINT32_MAX; so does this driver now.

Console: PID 649, the texel-buffer format probes (float, uint, sint, store,
atomic) PASS with r71-dual-source and m2-solid. Resident Evil 4 under
tooling/dolphin-profiles/p1-accurate.txt (GPU texture decoding on, a 16 MiB
texel buffer whose fetches reach far past element 65536) at the moment the
error showed before shows no error and draws its textures as desktop Dolphin
does, at full speed.

Host gate: vk_b2_device_test.c asserts the limit; vk_b2_buffer_view_test.c
creates a view of four million RGBA8 texels.
