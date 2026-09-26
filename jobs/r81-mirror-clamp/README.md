# R81: VK_KHR_sampler_mirror_clamp_to_edge

LRPS2's device-creation wrapper enables this extension on every device it
creates, whether the device lists it or not, so its hardware renderer could not
create a device here (VK_ERROR_EXTENSION_NOT_PRESENT). The address mode is one
field of the sampler word: SQ_TEX_MIRROR_ONCE_LAST_TEXEL, the value RADV
writes. The driver now lists the extension and encodes the mode, and refuses it
on a device that did not enable the extension, as Vulkan requires. (My fork of
LRPS2 also stops asking for it unconditionally: it now checks the device's
list first.)

The probe (`r81-mirror-clamp`, c7-mip's shaders) samples a 4x4 texture over
texel indices -8 to 11 on both axes, nearest, in three 4K frames: clamp to
edge and mirrored repeat as controls, then mirror clamp to edge, which mirrors
an index once about zero and clamps beyond. The three differ left of and above
the texture, so a mode read as another fails there. Every pixel is checked
except those within the reported subtexel precision of a texel boundary.

PS5 PID 170: all three frames match in every checked pixel (the corner reads
texel 0, texel 0 and texel 3 in turn), and R57's border-colour case passes
unchanged in the same run. Host: the C4 texture test refuses the mode without
the extension, creates the sampler with it, and checks the descriptor carries
mirror-once on U and V.

Reproduce from the driver root:

    python3 jobs/r81-mirror-clamp/check.py jobs/r81-mirror-clamp/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw log is the
ignored Klog_Logs/r81-mirror-clamp.log.
