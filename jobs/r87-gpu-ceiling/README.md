# R87: the GPU memory ceiling

R86 showed VkDeviceMemory can lie outside the 4 GiB address window, with a few
MiB per test. This round asks how much GPU-visible memory the console gives a
process, and whether the GPU reaches all of it.

- `r87-ceiling-map` maps GPU-visible direct memory (0x33) in 1 GiB, then 64 MiB,
  then 2 MiB pieces at consecutive addresses from 0x40_0000_0000 until the
  kernel refuses, and submits nothing. The CPU writes and reads back one word
  per MiB.
- `r87-ceiling` allocates Vulkan memory there (R86's placement switch), 1 GiB
  and then 128 MiB at a time, until vkAllocateMemory refuses. It gives back the
  last allocations until 256 MiB is free for the driver's own tables. A compute
  shader (shaders/r87-ceiling) then writes every word of the rest with a
  pattern seeded per 128 MiB slice, inverts every word, and compares every word
  with the inverted pattern, counting into each slice's record. There are three
  submissions, one per pass, and the CPU reads only the records. Two slices
  that reached the same memory would overwrite each other's pattern and fail
  the comparison.

PS5 PID 195, 3 of 3:

- The runner's pool is 12 GiB, 12,882,739,200 bytes of it free.
  12,880,707,584 bytes of GPU-visible memory mapped, in 56 pieces, every one at
  its address and none in the window. The kernel refused only when 2,031,616
  bytes were left, less than a 2 MiB piece. Every CPU word was right, and the
  pool came back to the byte.
- vkAllocateMemory gave 12,750,684,160 bytes (11.875 GiB, eleven 1 GiB and
  seven 128 MiB allocations) and then answered VK_ERROR_OUT_OF_DEVICE_MEMORY,
  with 122 MiB left: the rest of a 128 MiB step, and the device's own
  mappings.
- The GPU wrote, inverted and compared 12,482,248,704 bytes (93 slices,
  3,120,562,176 words). Every slice checked all its words, and none differed.
  The log has no GPU fault. The pool came back to the byte, and d2-compute
  passed afterwards.

So the GPU's ceiling is the direct-memory pool itself. The console sets no
separate limit on GPU-visible memory, and the GPU reads and writes all of it.
What a title can give the GPU is the pool less what it holds on the CPU side.
The device still reports a 4 GiB heap; raising it is the next round.

The passes took 25.9 ms (write), 53.2 ms (invert: read and write) and 25.8 ms
(compare) by the runner's CLOCK_MONOTONIC, from submission to fence. The rate
that implies is above the PS5's published 448 GB/s, so that clock has to be
cross-checked before any bandwidth is quoted from it.

Reproduce from the driver root:

    python3 jobs/r87-gpu-ceiling/check.py jobs/r87-gpu-ceiling/readback.txt

Console: build the compute probe (tools/build-compute-probe.sh r87-ceiling),
the driver and the runner, deploy while idle, then run tools/ps5_console.py
battery PPSA99988 with this queue.txt. The raw log is the ignored
Klog_Logs/r87-gpu-ceiling.log.
