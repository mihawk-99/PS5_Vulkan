# R89: a non-indexed draw's first vertex, and a first instance

LRPS2's hardware renderer met the driver's refusal of both: "a first vertex on a
non-indexed draw, or a first instance, needs a runner probe". Each is a user-data
word of RADV's vertex ABI, placed by the compiler beside the base vertex.

- **First vertex.** vkCmdDraw's firstVertex is written into the base-vertex user
  data, where an indexed draw's vertexOffset goes. The shader adds it to the
  vertex ID for gl_VertexIndex and for the vertex fetch alike.
- **First instance.** Where a stage reads the instance index, the compiler
  reports a start-instance user-data word (`start_instance_valid`), and the
  driver writes the draw's firstInstance there. gl_InstanceIndex adds it to the
  instance ID. Per-instance vertex input is still refused at pipeline creation,
  so no per-instance fetch depends on it yet.
- **The vertex-buffer record count.** The probe's first run on the console
  (PID 218) drew the first quad and nothing for the second: 0 of 2,073,600
  square pixels from vertex 6, no fault. A non-indexed draw's vertex-buffer
  record held the draw's vertex count, so vertices 6 to 11 were out of bounds
  and read as zeros. It now holds the first vertex plus the count. With no
  first vertex that is the value the golden frames compare, so every golden
  stands.

The harness (driver/tests/ps5vk_triangle.c) gains a first vertex, a draw vertex
count and a first instance for its draws, direct and indirect.

PS5 PID 219, 6 of 6:

- `r89-first-vertex`: two quads' triangle lists in one buffer, drawn from vertex
  0 and from vertex 6. Each frame's square is its own quad's colour on all
  2,073,600 pixels, gradients within 1.
- `r89-first-instance`: c2-instancing's boxes drawn with firstInstance 1 (two
  instances) and 2 (one). Exactly the boxes of the instances each draw starts
  at are drawn, and the others stay at the clear. These draws are indexed, so
  vkCmdDrawIndexed's firstInstance is what they carry.
- Controls: c2-base-vertex, c2-instancing, c2-indexed and b7-triangle pass.

Reproduce from the driver root:

    python3 jobs/r89-first-vertex-instance/check.py jobs/r89-first-vertex-instance/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw log is the
ignored Klog_Logs/r89-first-vertex-instance.log.
