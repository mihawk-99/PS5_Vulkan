# R86: GPU-visible memory outside the 4 GiB address window

The driver kept every GPU-visible allocation in one 4 GiB window, high word 2
(0x2_0000_0000 - 0x2_FFFF_FFFF), and capped the heap at 4 GiB. The reason is
the shader compiler's ABI: it combines 32-bit pointers with that fixed high
word (`address32_hi` 2), so what shaders reach through such a pointer -- the
register tables, push constants, the vertex-buffer table and code -- has to lie
there. VkDeviceMemory is not reached that way. Buffers and images bound to it
reach the GPU through descriptors (V# and T#), target registers and packets,
all of which carry 48-bit addresses. So whether it can lie elsewhere was a
question about the console, which this round answers.

The probe:

- `r86-wide-map` maps 64 KiB of type-12 direct memory for CPU and GPU read and
  write (0x33, the driver's protection) at hints outside the window. It
  submits nothing, so a mapping the GPU could not reach cannot fault here. A
  mapping with no address is the control: the kernel puts it in the window,
  where the GPU is known to use it.
- `r86-wide-*` rerun existing Vulkan tests with every VkDeviceMemory mapping
  placed from 0x10_0000_0000 up, through a test-only switch in the driver's
  debug API (`ps5vk_debug_device_memory_base`). The tests' own checks decide
  the result, and each counts only if its memory really lay outside the
  window. They run only when `r86-wide-map` found the kernel recording such a
  mapping as it records a window one: a GPU fault froze the screen and ended
  the title in B8.

PS5 PID 194, 8 of 8:

- The kernel maps GPU-visible memory at the hint itself at 0x1_0000_0000,
  0x4_0800_0000, 0x10_0000_0000 and 0x80_0000_0000, and records protection
  0x33 there exactly as for the window control; the CPU reads back what it
  wrote.
- From 0x10_0000_0000 the GPU wrote a storage buffer and read indirect
  dispatch counts (d2-compute), wrote storage images (d2-compute-images), read
  vertex and index buffers (c2-indexed) and a sampled texture (c4-texture),
  rendered into a texture and sampled it (c4-rtt), and depth-tested against a
  depth attachment (c5-depth). Every readback was exact, with 2 to 7 of each
  test's allocations outside the window. The log has no GPU fault.
- d2-compute afterwards passed with the placement back in the window.

So the 4 GiB window binds only what shaders reach through 32-bit pointers.
VkDeviceMemory can live anywhere the kernel maps GPU-visible memory, which is
what lets the heap grow past 4 GiB.

Reproduce from the driver root:

    python3 jobs/r86-wide-memory/check.py jobs/r86-wide-memory/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw log is the
ignored Klog_Logs/r86-wide-memory.log.
