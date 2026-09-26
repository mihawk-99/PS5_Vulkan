# R84: Vulkan 1.1

LRPS2 asks for a Vulkan 1.1 instance, and RetroArch refuses to create one on a
loader or driver that reports less; its renderer then loads the 1.1 core entry
points by their core names. The driver reported 1.0.

The instance and the device now report 1.1 with what 1.1 requires: the core
commands (vkGetPhysicalDeviceFeatures2 and friends, vkBindBufferMemory2,
vkGetDescriptorSetLayoutSupport, vkUpdateDescriptorSetWithTemplate,
vkCmdDispatchBase, the external-memory queries answering "none"),
VkPhysicalDeviceVulkan11Properties, and the two 1.1 features that are not
optional:

- **Multiview.** A render pass whose subpass has a view mask draws every draw
  once per view, each into its own layer. The driver replays each draw per view
  (driver/ps5vk_draw.c, ps5vk_cmd_draw and ps5vk_select_view), pointing the
  colour and depth registers at the view's layer and writing the view into the
  user-data dword the compiler names for gl_ViewIndex. The compiler patch
  (tooling/psbc/patch-view-index.py) keeps the view index a multiview pipeline
  reads, reports where it went, accepts the MultiView and GroupNonUniform
  capabilities, and gives the pixel stage the same user-data argument (RADV
  routes a pixel stage's view index through the layer the vertex stage
  exports, which the standalone compiler never set up; the compile aborted).
  vk_meta's clears are draws, so a multiview pass's clear covers every view.
  Queries inside a multiview pass are refused by name.
- **Basic subgroup operations in compute**, with a subgroup of 32 -- compute's
  wave. The compiler tree stubs out `vk_set_subgroup_size`, so RADV's wave
  heuristics chose, and a workgroup of 64 using subgroup operations became one
  wave of 64 reporting gl_SubgroupSize 64 (PID 154: one invocation elected,
  every size and index check failed). tooling/psbc/patch-subgroup-size.py sets
  a compute stage's API, minimum and maximum subgroup size to cs_wave_size.

Probes. `r84-multiview` renders views 0 and 1 of a three-layer 4K target: the
pass clears both views, one draw of the band quad goes left in view 0 (red) and
right in view 1 (green), both stages reading gl_ViewIndex. `r84-subgroup`
(shaders/r84-subgroup/dispatch.comp) runs a 128-invocation workgroup, four
subgroups, checking gl_SubgroupSize, gl_SubgroupInvocationID, gl_SubgroupID,
gl_NumSubgroups and subgroupElect, then repeats the dispatch 32 more times.

PS5 PID 167 (the capture this directory's readback and golden/r84-multiview
come from), and PIDs 168-169 the same: device-report shows a 1.1 instance and
device; layers 0 and 1 exact in all 8,294,400 pixels and layer 2 still the
zero the harness filled it with; 34 four-subgroup dispatches exact; c4-rtt,
v0-subpass, v0-stencil and d2-compute pass; 8 of 8.

Failed runs kept for what they showed: PID 154 also reported layer 2 wrong --
the probe's mistake, not the driver's: it wrote a sentinel there before
ps5vk_triangle_draw, which zeroes the whole target before every frame. PID
155, with the subgroup size fixed and a two-subgroup workgroup, read a wrong
gl_SubgroupID in both dispatches (word 0xbad00204). Nothing has repeated it
since: the same shader passed 2 more runs and a variant that also records the
IDs it sees passed 6 (16 dispatches), and the four-subgroup probe passed 102
dispatches in 3 runs. The compiled code reads the wave's index from TG_SIZE bits 6-11, the
ordered-append ID Mesa uses on GFX6-10, valid while the dispatch initiator
leaves ordered append off, which the driver does. I keep the repeat in the
probe so that a recurrence fails the case.

Host: vk_r84_multiview_test.c draws the same frame against the replay of PID
167's capture and reads the stream back -- the clear is one DRAW_INDEX_AUTO
per view and the draw one DRAW_INDEX_2 per view, each programs CB_COLOR0_BASE
at its view's layer, none names layer 2, and one user-data dword goes from 0
to 1 in each stage. vk_b2_device_test.c checks the 1.1 version, commands,
features and properties; vk_d2_compute_test.c compiles the subgroup probe.

Reproduce from the driver root:

    python3 jobs/r84-multiview/check.py jobs/r84-multiview/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw logs are
the ignored Klog_Logs/r84-stress1.log (PID 167), r84-multiview.log (154) and
r84-multiview-2.log (155).
