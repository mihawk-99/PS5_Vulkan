# R90: image clears on the GPU, and a step end that flushes every cache

LRPS2's hardware renderer clears its render targets and its D32_SFLOAT_S8_UINT
depth with vkCmdClearColorImage and vkCmdClearDepthStencilImage, outside a
render pass, every frame. Both were the CPU's: a clear wrote every texel through
the image's map at a submission split point, and each split point waited for the
GPU to drain. At 6x (2880x2160) that held God of War II's demo to 70-78% with
MTVU on.

- **The clears are a draw.** An attachment of one level and one layer, cleared
  whole, is cleared by vk_meta's clear draw (driver/ps5vk_draw.c,
  `ps5vk_meta_clear_colour` and `ps5vk_meta_clear_depth`), with the
  application's bound state saved and restored around it as for the blits.
  Anything else keeps the CPU path. So does a clear that names the stencil
  aspect without the depth one: vk_meta renders it with a stencil attachment
  and no depth attachment, which the driver's rendering refuses.
- **A step ends with every cache flushed.** Each step of a submission ended with
  the runner's two packets: event 45, the colour caches' flush, and the
  completion marker at the bottom of the pipe (event 40, L2 written back). No
  depth cache was flushed at all, so a CPU copy at the split point after a
  depth clear read what memory held before it. The completion is now one
  RELEASE_MEM of CACHE_FLUSH_AND_INV_TS_EVENT (event 20) with the same cache
  actions: colour and depth caches flushed, L2 written back, then the marker,
  as RADV's fences are. The host model completes that marker as it did the
  other, and the golden comparison takes either event in the marker and
  compares every other word.

The new case, `r90-image-clears`, clears 636x358 attachments -- no tile
multiple, so a clear that misses an edge shows -- to two values in turn and
reads every texel back at the split point after each: R8G8B8A8_UNORM,
B8G8R8A8_UNORM, R32G32B32A32_SFLOAT, R32_SFLOAT, R32_UINT, R16_UINT, R8G8_UNORM
and D32_SFLOAT. D32_SFLOAT_S8_UINT has a pattern uploaded into both planes, then
a depth-only clear (LRPS2's), a stencil-only one and, over the pattern again, a
clear of both, each followed by a readback of both aspects.

Its first two runs are kept, because they established what the round owed:

- **PID 230** (step completion at the bottom of the pipe): the second D32_SFLOAT
  clear read 46,269 of 227,688 texels as the first clear's 0.25, and the
  D32_SFLOAT_S8_UINT depth clears read part of the pattern back. The stencil
  plane was right each time. Eight-byte and one-byte colour formats also
  failed (below).
- **PID 231** (CACHE_FLUSH_AND_INV_TS_EVENT at the step end): every depth and
  stencil check exact. R16G16B16A16_UNORM and _SFLOAT still missed the same 664
  texels, first at (604,64), and R8_UNORM the same texels from (8,356), in both
  runs. Texels the same in both runs point to a map, not a cache. The counts
  are exactly the texels whose place would lie past the image's edge in a
  partial tile if the hardware twisted one address bit that the CPU's one- and
  eight-byte maps (AddrLib's rows, never measured on the console) do not: x bit
  5 on odd tile rows for the eight-byte tile, y bit 1 where x bit 3 is set for
  the one-byte tile. If so, the CPU path was placing those texels wrongly all
  along (its clears, copies and readbacks of such images), and the GPU clear
  is not the cause. R91 measures the two maps; until then the case leaves
  those formats out.

PS5 PID 233, 6 of 6 (queue.txt): `r90-image-clears` 24 of 24 checks, each
227,688 of 227,688 texels; c7-clear's tiled attachment cleared on the GPU and
read back exactly; c5-depth-clear, v0-stencil-clear, v0-colour-clear and
c1-clear pass. No GPU fault.

PS5 PID 235, 29 of 29 (regression/queue.txt): R88's battery of existing cases on
the new step end -- the triangle, indexed, staging, instancing, uniforms and
dynamic uniforms, texture, render to texture, depth, stencil, depth-stencil
aspects, MSAA and resolve, compute, storage images, descriptor arrays, sampled,
storage and texel-buffer formats, targets, subpasses, multiview, subgroups, mip
upload, depth blit, uniform indexing, an occlusion query, events and the device
report. No GPU fault.

With R90 in the RetroArch title, God of War II's demo at 6x with MTVU ran at
98-99% in every window after the boot, where it had run at 70-78%.

Reproduce from the driver root:

    python3 jobs/r90-gpu-image-clears/check.py

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with queue.txt and then
regression/queue.txt (the runner reads a queue only by that name). The raw logs
are the ignored Klog_Logs/r90-queue.log, r90-regression.log and
r90-gpu-image-clears.log / -2.log (PIDs 230 and 231).
