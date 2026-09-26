# LRPS2 against this driver: the gap list and the round plan

LRPS2 is libretro's PCSX2 core. I am porting it into the PS5 RetroArch title
(../PS5_RetroArch/docs/LRPS2_PORT.md). This page compares what its Vulkan
renderers ask for with what the driver reports
(conformance_inventory/device_report.json, regenerated after R79). It also
orders the driver rounds that close the gaps. Every hard requirement becomes a
general, conformant driver feature, proven by a runner probe on the console.
None of them is special-cased for LRPS2.

Revision audited: my fork ../PS5_LRPS2 at 6d14775ead86 (2026-09-25), files
`pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp`, `VKBuilders.cpp`, `VKEntryPoints.inl`,
`tfx.glsl`, `pcsx2/GS/parallel-gs/`, and the frontend's
`gfx/common/vulkan_common.c`.

## Status (2026-09-25)

R81-R84 are done and proven on the console (docs/M5_PHASE_C.md, jobs/r81-* to
jobs/r84-*): the device reports Vulkan 1.1 with its core commands, multiview
and basic compute subgroups, lists VK_KHR_sampler_mirror_clamp_to_edge,
renders and blends the 16-bit UNORM targets, and samples and copies both
aspects of D32_SFLOAT_S8_UINT. H1-H5 are closed. With them LRPS2's hardware
renderer creates its device on the console; its draws then met two refusals:
dynamic line width (R85, done) and a non-indexed draw with a first vertex or a
first instance (R89, done). R86-R88 gave it a 12 GiB heap.
The report below is the one this plan was made from.

## What the driver reported when I audited it

- Instance and device API version 1.0. Instance extensions:
  `KHR_get_physical_device_properties2`, `KHR_surface`, `KHR_display`,
  `EXT_debug_report` and `EXT_debug_utils`. Device extensions:
  `KHR_swapchain` only.
- Features on: `dualSrcBlend`, `robustBufferAccess` and `samplerAnisotropy`. Every
  other 1.0 feature is off, including `fullDrawIndexUint32`, `imageCubeArray`,
  `independentBlend`, `geometryShader`, `largePoints`, `wideLines`, `logicOp`,
  `depthClamp`, `fragmentStoresAndAtomics`, `shaderInt16`,
  `shaderClipDistance` and `textureCompressionBC`.
- Limits at or near the 1.0 minimum where they matter here:
  `maxComputeSharedMemorySize` 16384, `maxComputeWorkGroupInvocations` 128,
  `maxUniformBufferRange` 16384, `maxPerStageDescriptorStorageImages` and
  `maxPerStageDescriptorStorageBuffers` 4, `maxBoundDescriptorSets` 4 and
  `maxPushConstantsSize` 128.
- One heap of 12 GiB, the whole direct-memory pool, with VK_EXT_memory_budget
  (R88). Type 0 is device-local, and type 1 is device-local, host-visible and
  host-coherent.

## The hardware renderer (GSDeviceVK), the shipping path

### Hard: the renderer does not start without these

| # | Requirement | Where LRPS2 asks | Driver today |
|---|---|---|---|
| H1 | **Vulkan 1.1 instance.** `get_application_info_vulkan` asks for `apiVersion` 1.1, and RetroArch refuses to create the instance ("Core requests apiVersion 1.1, but it is not supported by loader") when `vkEnumerateInstanceVersion` reports less. | GSDeviceVK.cpp:135, vulkan_common.c:955 | 1.0 |
| H2 | **The 1.1 core entry points** `vkGetPhysicalDeviceFeatures2`, `vkGetPhysicalDeviceProperties2` and `vkGetPhysicalDeviceMemoryProperties2`, loaded by their core names as required. | VKEntryPoints.inl:60-62 | KHR aliases only, on a 1.0 instance |
| H3 | **VK_KHR_sampler_mirror_clamp_to_edge.** The core's `vkCreateDevice` wrapper adds it to every device unconditionally, so device creation fails with `VK_ERROR_EXTENSION_NOT_PRESENT`. No GS sampler uses the mode. | GSDeviceVK.cpp:159 | absent; the address mode is refused (ps5vk_image.c) |
| H4 | **R16G16B16A16_UNORM as a colour attachment.** This is the "ColorClip" target for colour-clamp emulation. `CheckFeatures` requires `SAMPLED_IMAGE \| COLOR_ATTACHMENT` for every format from Color to PrimID and gives up otherwise. | GSDeviceVK.cpp:1788-1802 | optimal features 0xdc01: sampled and transfer, no colour attachment |
| H5 | **D32_SFLOAT_S8_UINT sampled.** With a stencil buffer (chosen whenever D32S8 is a depth attachment and framebuffer fetch is off), the depth format must be `SAMPLED_IMAGE \| DEPTH_STENCIL_ATTACHMENT`: the renderer samples its depth target (depth conversion, `test_and_sample_depth`). | GSDeviceVK.cpp:1760-1774, 1790 | 0x200: attachment only |

H2 is H1's consequence: under a 1.1 instance on a 1.1 device the core names
are the right ones. H3 is also a conformance bug in the core, because it enables
an extension it never checked for. I will make the core enable it only when the
device lists it, and implement it in the driver as well, where it is a
one-field sampler mode (the hardware's mirror-once).

### Accuracy and performance: the renderer runs without these, less well

| # | Feature | What LRPS2 does without it | Weight |
|---|---|---|---|
| A1 | Barriers inside a render pass on a subpass self-dependency, input attachments aliasing the colour attachment in `GENERAL` layout ("texture barriers") | On by default (`OverrideTextureBarriers` is not 0). Without them, blending, FBMASK, DATE and every read of the target it draws to are wrong. | **correctness of maximum blending (Profiles 2, 4, 7)**. Input attachments are in the driver since the PPSSPP work. Barriers in a pass need a probe. |
| A2 | Every copy, upload and readback on the GPU | Works, but a copy the driver runs on the CPU splits the submission and spins until the GPU is idle. Rogue Leader's warm attract sequence loses about 30% of its render thread to this. | **speed at every profile** |
| A3 | Driver memory the GPU reads without a CPU cache flush (write-combined or coherent) | Works, but `ps5vk_flush_cpu_cache` runs for every draw's tables, registers and constants: 11-14% of Dolphin's render thread in Rogue Leader. PS2 games issue more draws. | **speed** |
| A4 | `VK_EXT_provoking_vertex` (last) | The renderer reorders flat-shaded primitives on the CPU. | speed, some accuracy |
| A5 | `VK_KHR_shader_draw_parameters` (`vs_expand`: sprites, points and lines expanded in the vertex shader from a storage buffer) | Expansion on the CPU. | speed (GS thread) |
| A6 | `largePoints`, `wideLines` covering the upscale factor | Vertex expansion instead. | speed |
| A7 | `VK_EXT_line_rasterization` (`bresenhamLines`) | Its log warns of "rendering inaccuracies" in lines. | accuracy |
| A8 | `geometryShader`, for `gl_PrimitiveID` in the fragment shader | Accurate destination-alpha testing uses full barriers per primitive instead of the primitive-ID pass, so it is correct but slower. | speed in DATE-heavy scenes; large round |
| A9 | `VK_EXT_rasterization_order_attachment_access` (framebuffer fetch) | Uses A1's barriers. | speed; not on AMD's hardware model, so not planned |
| A10 | Pipeline creation at 10-20 ms | LRPS2 builds a pipeline per new state combination, and first-run stutter follows. Its own cache and the driver's shader cache hide it from the second run. | Profile 8 |
| A11 | Surface compression (DCC, HTILE) | Bandwidth at 6x (3840x2688 targets). | Profile 4 speed, measured first |

Not needed by the hardware renderer: `fullDrawIndexUint32` (16-bit indices),
`independentBlend`, `logicOp`, `depthClamp`, `shaderClipDistance`,
`fragmentStoresAndAtomics`, `imageCubeArray` and `textureCompressionBC` (texture
replacement packs only). Its UBOs, push constants (128 bytes or less) and three
descriptor sets fit the current limits. Dynamic states are viewport, scissor,
blend constants and line width. Shaders are GLSL 4.60, compiled at run time
with `GL_EXT_samplerless_texture_functions`, plus
`GL_ARB_shader_draw_parameters` under A5.

## paraLLEl-GS, Profile 5's compute stress

`gs_renderer.cpp:822` refuses to start without: `descriptorIndexing`,
`timelineSemaphore`, `bufferDeviceAddress`, `storageBuffer8BitAccess`,
`storageBuffer16BitAccess`, `shaderInt16`, `scalarBlockLayout`; subgroup
basic, vote, arithmetic, ballot and shuffle operations; subgroup size control
over 4-64 invocations; and 32 KiB of compute shared memory. Granite also wants
a 1.1 instance and device. That is most of the Vulkan 1.2 feature set, and it is
the last group below: it serves one profile and no game needs it.

## The rounds, in order

Each round follows the usual shape: a probe that fails first, the mechanism,
a host regression test, console proof, evidence, the docs and a focused commit.

1. **R81 — VK_KHR_sampler_mirror_clamp_to_edge** (H3). The mode's hardware
   value, the extension listed, and a sampling probe on both sides of the
   edge.
2. **R82 — 16-bit UNORM colour targets** (H4). R16G16B16A16_UNORM, and the
   R16_UNORM and R16G16_UNORM family, rendered and blended at the export
   format their precision needs.
3. **R83 — the depth of D32_SFLOAT_S8_UINT sampled** (H5). Depth-aspect views
   of the combined format, sampled; the format advertises `SAMPLED_IMAGE`.
4. **R84 — Vulkan 1.1** (H1, H2). The instance and the device report 1.1 with
   everything 1.1 requires: the core commands (Mesa's runtime provides most
   of them), `multiview`, maintenance1-3 (negative viewport height among them),
   the subgroup properties, and `VkPhysicalDeviceVulkan11Properties`, with the
   optional 1.1 features reported off until they are done.
5. **R85 — barriers inside a render pass** (A1). A probe that reads its own
   target through an input attachment after an in-pass barrier, draw after
   draw, and a fix if it disagrees.
6. **R86 — copies on the GPU** (A2): every path that still splits a
   submission for a CPU copy.
7. **R87 — driver memory without per-draw flushes** (A3).
8. **R88 — VK_EXT_provoking_vertex** (A4), **R89 — shader draw parameters**
   (A5), **R90 — large points and wide lines** (A6), **R91 — Bresenham lines**
   (A7), taken in the order the profiles show they matter.
9. Later, measured first: `geometryShader` (A8), surface compression (A11),
   and the paraLLEl-GS group.

R81-R84 gate the hardware renderer's first frame. Until they land, the port
comes up on LRPS2's software renderer, which asks the frontend for no Vulkan
context, so the CPU side is proven apart from the driver.
