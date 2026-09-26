# R88: VkDeviceMemory outside the address window, and a 12 GiB heap

R86 showed the GPU reads and writes VkDeviceMemory outside the 4 GiB address
window, and R87 that it reaches the whole direct-memory pool. The driver still
placed every VkDeviceMemory mapping in the window and reported a 4 GiB heap.

The driver now:

- **Places VkDeviceMemory in its own region.** The region is 256 GiB at
  0x40_0000_0000, handed out in 2 MiB granules, first fit, so freed ranges are
  used again (driver/ps5vk_direct_memory.c). The kernel has the last word: a
  mapping it puts elsewhere gives its granules back, and a mapping it will not
  make at the address asked for is made at its own choice instead. Either is
  accepted when the GPU can reach it through 48-bit addresses. What shaders
  reach through 32-bit pointers -- register tables, push constants, code, the
  queue -- still has to lie in the window.
- **Reports the whole pool as the heap:** 12 GiB. An allocation larger than
  the pool is refused.
- **Lists VK_EXT_memory_budget.** The CPU draws on the same pool, so the
  budget reports what VkDeviceMemory holds (heapUsage) and how much the heap
  can give in all: that usage plus what the pool has free now (heapBudget).

The host model reports the console's 12 GiB pool, implements the two kernel
calls R86 to R88 use (`sceKernelQueryMemoryProtection`,
`sceKernelAvailableDirectMemorySize`), and its tests follow. B3 memory checks
the 12 GiB heap and a budget whose usage grows by exactly the pages mapped. B3
window accepts VkDeviceMemory above the window and still refuses a queue
buffer there. The runner-cases gate compares the device-report inventory with
everything but its `source` (the log it was collected from).

PS5 PID 196, 30 of 30:

- device-report: heap 12,884,901,888 bytes, VK_EXT_memory_budget listed.
- r88-memory, through the default placement: vkAllocateMemory gave 11.875 GiB
  before VK_ERROR_OUT_OF_DEVICE_MEMORY, all 18 allocations outside the window.
  The budget's usage went from 49,152 bytes to exactly 49,152 plus the
  11.875 GiB, and back to 49,152 once freed. The GPU wrote, inverted and
  compared every word of 11.625 GiB of it (3,120,562,176 words), and none
  differed. The pool came back to the byte.
- 28 existing cases passed with their memory in the region: triangle,
  indexed, staging, instancing, uniforms and dynamic uniforms, texture, render
  to texture, depth, stencil, depth-stencil aspects, MSAA and resolve,
  compute, storage images, descriptor arrays, sampled and texel-buffer
  formats, targets, subpasses, multiview, subgroups, mip upload, depth blit,
  uniform indexing, an occlusion query and events.
- The log has no GPU fault.

Reproduce from the driver root:

    python3 jobs/r88-device-memory/check.py jobs/r88-device-memory/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw log is the
ignored Klog_Logs/r88-device-memory.log;
conformance_inventory/device_report.json is collected from it.
