/*
 * PS5 Vulkan driver - triangles through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phases B7, B8, C1, C3 and C4 (docs/M5_PHASE_B.md,
 * docs/M5_PHASE_C.md). An ordinary Vulkan 1.0 program that draws three-vertex
 * triangles at 3840x2160: instance, device, colour targets, image views, render
 * passes, framebuffers, one or two pipelines from the caller's SPIR-V, two
 * command buffers and a fence. Its draws reach the queue in one of several
 * groupings, and a caller may also give it an indexed draw's geometry, a
 * uniform buffer, and (Phase C4) a texture its pixel shader samples -- either
 * an image the harness uploads the texels into, or one the frame renders them
 * into and then samples in the same command buffer.
 *
 * The program draws into one of two outputs:
 * - an R8G8B8A8_UNORM image on its own host-visible memory (B7, B8), cleared
 *   to zero before each draw and left mapped, so the caller reads the frame
 *   back;
 * - the display, as the Vulkan Tutorial draws to a window (C1): VK_KHR_display
 *   gives the display and its mode, VK_KHR_surface a plane-0 surface, and
 *   VK_KHR_swapchain a FIFO swapchain of B8G8R8A8_UNORM images. Each draw
 *   acquires an image with an image-available semaphore its submission waits
 *   on, and signals a render-finished semaphore that ps5vk_triangle_present
 *   waits on when it presents the image.
 *
 * Shared by the PC tests driver/tests/vk_b7_draw_test.c, vk_b8_groups_test.c,
 * vk_c1_present_test.c, vk_c4_texture_test.c and vk_c4_rtt_test.c and the
 * console test runner (src/diagnostics.cpp, AGC_VULKAN_DRIVER). Every Vulkan
 * function comes from the caller's GetInstanceProcAddr: the loader's, or the
 * driver's own on the console.
 */

#ifndef PS5VK_TRIANGLE_H
#define PS5VK_TRIANGLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5VK_TRIANGLE_WIDTH 3840
#define PS5VK_TRIANGLE_HEIGHT 2160
#define PS5VK_TRIANGLE_MAX_PIPELINES 2
/* The longest mip chain a frame may sample (Phase C7). */
#define PS5VK_TRIANGLE_MAX_MIP_LEVELS 16
/* The array layers a frame's texture may name (D1's two-layer probe, and room
 * for a cube's six). */
#define PS5VK_TRIANGLE_MAX_ARRAY_LAYERS 6
/* The most vertex attributes a frame's geometry may declare: two for every set
 * before Phase C7, and three for the mip probe's LOD-carrying vertices. */
#define PS5VK_TRIANGLE_MAX_ATTRIBUTES 4
/* Colour targets: the program's image, or the swapchain's images. */
#define PS5VK_TRIANGLE_MAX_IMAGES 2

/* The colour attachments one rendering may declare: the number this device
 * advertises as VkPhysicalDeviceLimits.maxColorAttachments, which a case reads
 * and passes rather than writing 4 into the test (input->color_attachment_count,
 * the MRT probe in src/diagnostics.cpp). */
#define PS5VK_TRIANGLE_MAX_TARGETS 4

typedef PFN_vkVoidFunction (*ps5vk_get_instance_proc_addr)(VkInstance instance, const char *name);

/* How the program reports each step: its name, whether it succeeded, the
 * VkResult it returned and a description. Driver messages arrive as steps
 * named "vk_message", failed for warnings and errors: Mesa reports why the
 * driver returned an error as a warning. */
struct ps5vk_triangle_report {
   void *context;
   void (*step)(void *context, const char *name, bool passed, int result, const char *detail);
};

/* One pipeline's SPIR-V. */
struct ps5vk_triangle_shaders {
   const uint32_t *vertex_spirv;
   size_t vertex_bytes;
   const uint32_t *pixel_spirv;
   size_t pixel_bytes;
};

/* Where the program draws. */
enum ps5vk_triangle_output {
   /* Its own mapped R8G8B8A8_UNORM image (B7, B8). */
   PS5VK_TRIANGLE_OUTPUT_IMAGE,
   /* The display's swapchain images, presented (C1). */
   PS5VK_TRIANGLE_OUTPUT_DISPLAY,
};

/* A program: what it draws with, and where. Fields were added to its end with
 * every phase (C2's geometry, C3's uniform buffer, C4's texture), and a caller
 * that has no use for the last ones leaves them out; C zero-fills them, which
 * is what tells the program they are not there. Neither GCC nor Clang accepts
 * that quietly under -Wextra's -Wmissing-field-initializers, and every build
 * here makes warnings fatal, so the warning is turned off for the translation
 * units that include this header: the tests that pass no texture have to keep
 * compiling byte for byte as they are. */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

/* The push-constant bytes a frame may upload: VkPhysicalDeviceLimits'
 * maxPushConstantsSize, which the driver reports as 128. */
#define PS5VK_TRIANGLE_MAX_PUSH_CONSTANTS 128

/* The images R7's vkQuake shape samples: one per combined image sampler at set
 * 0's bindings 0, 1 and 2. */
#define PS5VK_TRIANGLE_MULTISET_TEXTURES 3

struct ps5vk_triangle_input {
   ps5vk_get_instance_proc_addr get_instance_proc_addr;
   /* One or two pipelines; a frame's second draw uses the second, or the
    * first again when there is one. */
   uint32_t pipeline_count;
   struct ps5vk_triangle_shaders shaders[PS5VK_TRIANGLE_MAX_PIPELINES];
   /* The load operation of a frame's first render pass:
    * VK_ATTACHMENT_LOAD_OP_DONT_CARE draws; others show what the driver does
    * with them. A second render pass loads the attachment. */
   VkAttachmentLoadOp load_op;
   const struct ps5vk_triangle_report *report;
   enum ps5vk_triangle_output output;
   /* An indexed draw's geometry (Phase C2), or zeros to draw a triangle with
    * no vertex bindings as the B7/B8/C1 sets do. vertex_data is vertex_count
    * records of vertex_stride bytes in the layout the pipeline declares;
    * index_data is index_count 16-bit indices. */
   const void *vertex_data;
   uint32_t vertex_count;
   uint32_t vertex_stride;
   const uint16_t *index_data;
   uint32_t index_count;
   /* The vertex input layout of the geometry above, as the probe set's
    * shaders were compiled for it: one binding of vertex_stride bytes.
    * Zero attribute_count keeps the empty vertex input the triangle sets
    * use. */
   uint32_t attribute_count;
   VkVertexInputAttributeDescription attributes[PS5VK_TRIANGLE_MAX_ATTRIBUTES];
   /* Stage the geometry through a buffer copy instead of writing it into the
    * buffers the draw binds (Phase C2): the data goes into a mapped staging
    * buffer, one command buffer copies it into the vertex and index buffers,
    * and the frame follows in the next command buffer of the same submission,
    * which the driver splits at the copy. */
   bool stage_geometry;
   /* A uniform buffer the application binds at set 0, binding 0 (Phase C3),
    * or NULL for a program that reads no descriptors. The bytes are written
    * into a mapped buffer and the set is updated with it before the draws. */
   const void *uniform_data;
   uint32_t uniform_bytes;
   /* The stages whose shaders read the uniform buffer (Phase C3): a binding
    * counts for a stage only if the pipeline layout declares it for that stage
    * (driver/ps5vk_pipeline.c, ps5vk_descriptor_options), so a caller whose
    * vertex shader reads it passes VK_SHADER_STAGE_VERTEX_BIT. Zero for a
    * caller that passes no uniform data. */
   VkShaderStageFlags uniform_stages;
   /* The texture the caller's pixel shader samples (Phase C4), or NULL for a
    * program that samples nothing. texture_data is texture_width *
    * texture_height RGBA8 texels, row-major with the top row first and no
    * padding between rows; the harness uploads them into a sampled image
    * through a mapped staging buffer whose copy the driver performs at a
    * submission split point (vkCmdCopyBufferToImage), and binds the image view
    * and a sampler at set 0, binding 0 -- where the m3-texture canary's pixel
    * shader reads its combined image sampler
    * (probes/m3-texture/bindings.txt). */
   const void *texture_data;
   uint32_t texture_width;
   uint32_t texture_height;
   /* Whether the sampler the frames start with blends the four texels around
    * each sample (VK_FILTER_LINEAR) or takes the nearest one: the two filters
    * the canary's two frames ran. ps5vk_triangle_set_texture_filter changes it
    * between frames. */
   bool texture_bilinear;
   /* Whether the image the frame samples is one the frame itself renders into
    * (Phase C4's render-to-texture case), instead of the caller's texels
    * copied into an image: ps5vk_triangle_draw then records two render passes
    * into ONE command buffer, the first drawing the caller's texels over the
    * whole of a second colour image and the second the frame, whose pixel
    * shader samples that image. One command buffer is what the case is about:
    * the driver has to put its colour barrier between the render and the
    * sample, because the barrier that ends every submission orders a render in
    * an earlier submission but not one earlier in the same stream
    * (driver/ps5vk_draw.c, driver/ps5vk_queue.c). The flag needs texture_data
    * as well -- the texels the first pass draws are the caller's -- and the
    * m3-texture geometry whose pass can cover a whole target, because both
    * passes draw the caller's one pipeline: 16-byte records of a position and
    * a texture coordinate, which create() refuses otherwise. False, which is
    * what a caller that leaves the field out passes, records and submits
    * exactly what a frame did before this field existed. */
   bool texture_is_rendered;
   /* The depth attachment the frame renders through (Phase C5), or false for a
    * frame with no depth. The harness creates a D32_SFLOAT image the size of
    * the program's target on its own memory, gives both render passes and every
    * framebuffer the depth attachment, and writes the depth state below into
    * every pipeline. */
   bool depth;
   /* The depth state of the pipelines: the test, the write and the comparison.
    * The M4 canary ran ALWAYS for its depth-writing clear and LESS for its
    * rectangles (HARDWARE_FINDINGS.md). A caller that leaves all three unset
    * gets a pipeline with no depth state at all, which is what every frame
    * before Phase C5 had. */
   bool depth_test;
   bool depth_write;
   VkCompareOp depth_compare_op;
   /* What a frame's first render pass does with the depth attachment and the
    * value it clears to: VK_ATTACHMENT_LOAD_OP_CLEAR is a vk_meta draw of the
    * whole attachment, which is the clear C5's console criterion names
    * (Phase C1b's clear design). The second pass loads what the first left. */
   VkAttachmentLoadOp depth_load_op;
   float depth_clear_value;
   /* The depth attachment's format: D32_SFLOAT unless the caller names another
    * (round 15's D16 probe). */
   VkFormat depth_format;
   /* Phase C5's image clear: record vkCmdClearDepthStencilImage over the whole
    * depth image, with depth_clear_value, before the pass. The pass must load
    * rather than clear for the command's own write to be what the frame's depth
    * test reads. */
   bool depth_clear_image;
   /* How many instances the frame's draws run (Phase C2); 0 means one. */
   uint32_t instance_count;
   /* Whether each draw sets the viewport and scissor itself, with the same
    * full-target rect the pipeline declares, instead of leaving them to the
    * pipeline: false for every frame before Phase C2's instancing probe, whose
    * stream must not change. */
   bool explicit_viewport;
   /* The scissor the frame's pipeline declares (Phase V0-query). use_scissor
    * takes scissor exactly as it stands -- including a zero extent, the region
    * no fragment reaches, which is what an occlusion query must count nothing
    * for -- and the default draws with the whole target, as every frame before
    * that phase did. */
   bool use_scissor;
   VkRect2D scissor;
   /* The occlusion query the frame's draws run inside (Phase V0-query), or
    * VK_NULL_HANDLE for a frame that records none: the program records
    * vkCmdBeginQuery before its first draw and vkCmdEndQuery after its last,
    * so the query counts the whole frame (driver/ps5vk_query.c). */
   VkQueryPool query_pool;
   uint32_t query;
   /* Phase V0-query: whether the frame also records vkCmdCopyQueryPoolResults
    * for its query, into a host-visible buffer of the harness's own, so the
    * caller can compare the copy's result with vkGetQueryPoolResults'. */
   bool query_copy;
   /* The image's array layers (D1): 0 or 1 for the single-layer image every
    * frame before this step used. Two or more makes the image a 2D array, its
    * layers filled one copy each out of the staging buffer's level-major,
    * layer-major texels, and its view a 2D array the descriptor's DEPTH and
    * BASE_ARRAY fields name. */
   uint32_t texture_layers;
   /* Whether the image is a cube (D1): six layers and
    * VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, which makes its view a cube and the
    * descriptor's TYPE field 11 rather than a 2D array's 13. Requires
    * texture_layers 6. */
   bool texture_cube;
   /* Whether a tiled chain's texels reach it through the caller's own
    * vkCmdCopyBufferToImage instead of the caller writing the storage itself
    * (Phase C7): the image then carries TRANSFER_DST and the frame's upload
    * command buffer records one copy a level, which is the driver's tiled
    * upload. Zero keeps the probe-written chain. */
   bool texture_upload;
   /* The mip chain the sampled image is created with (Phase C7): 0 or 1 for the
    * single level every frame before that phase sampled, and N > 1 for a chain
    * whose level L holds texture_level_colours[L] in every texel, in R, G, B, A
    * byte order (four bytes per level, level 0 first). A solid level is what
    * lets a probe name the level a sample read. */
   uint32_t texture_levels;
   const uint8_t *texture_level_colours;
   /* The levels the sampled image's VIEW names (Phase C7): 0 for every level
    * the image has, which is what a chain's frames ask for, and 1 for the
    * control that samples level 0 through a single-level view. */
   uint32_t texture_view_levels;
   /* The first level the view names (Phase C7): 0 with the whole chain, and k
    * for a view of level k alone, which is how a frame can ask the hardware for
    * one level with no LOD selection at all. */
   uint32_t texture_view_base_level;
   /* The base vertex an indexed draw adds to every index it fetches (Phase C2):
    * 0 for every frame before that step's probe. */
   int32_t base_vertex;
   /* Whether the samplers' mipmapMode is LINEAR, which blends the two levels
    * around a fractional LOD, or NEAREST; and the maxLod they reach. */
   bool texture_mip_linear;
   float texture_max_lod;
   /* The range the uniform buffer's descriptor binds (V0-robust): 0 binds the
    * whole buffer, which is what every frame before that probe did, and a
    * shorter range is a bound a shader's read can fall outside of. The buffer
    * itself still holds uniform_bytes. */
   uint32_t uniform_range_bytes;
   /* The VkFormat the sampled image is created with (V0-formats):
    * VK_FORMAT_UNDEFINED keeps the R8G8B8A8_UNORM every frame before that probe
    * sampled, and texture_data holds that format's texels, tightly packed. */
   VkFormat texture_format;
   /* The uniform texel buffer the caller's pixel shader fetches from
    * (V0-formats' descriptor-type rows), or NULL for a program that fetches
    * none. texel_buffer_data is texel_buffer_bytes of texel_buffer_format's
    * texels, tightly packed; the harness creates a buffer holding them, a
    * VkBufferView of it in that format and the set 0 binding 0
    * VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER descriptor the shader reads --
    * the binding tooling/psbc/patch-descriptor-types.py gives the compiler and
    * driver/ps5vk_draw.c writes as a hardware V#. texel_buffer_bytes is what
    * the view's range and the V#'s element count are computed from. */
   const void *texel_buffer_data;
   uint32_t texel_buffer_bytes;
   VkFormat texel_buffer_format;
   /* Whether the view asks for the buffer with VK_WHOLE_SIZE instead of the same
    * bytes written out. Vulkan defines the sentinel as "from offset to the end of
    * the buffer", the two forms name the same texels, and an application that
    * writes the sentinel meets whichever refusal the literal reading produces --
    * vkQuake's palette-octree view did (R3 of that port's requests,
    * driver/ps5vk_buffer.c). The host test takes the explicit form and the console
    * case the sentinel, so both are covered by one flag. */
   bool texel_buffer_whole_size;
   /* The storage image the caller's pixel shader stores into (V0-formats'
    * descriptor-type rows), or VK_FORMAT_UNDEFINED for a program that stores
    * nothing. The harness creates a storage_image_width x storage_image_height
    * row-layout image of that format with VK_IMAGE_USAGE_STORAGE_BIT, its view
    * and the set 0 binding 0 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE descriptor the
    * shader writes through; the caller reads the stores out of
    * storage_image_mapped once the frame has completed. */
   VkFormat storage_image_format;
   uint32_t storage_image_width;
   uint32_t storage_image_height;
   /* Whether the buffer view is a *storage* texel buffer
    * (VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT and
    * VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) instead of a uniform one: the
    * pixel stage then stores into it, and the caller reads what it stored out
    * of texel_buffer_mapped after the draw. False, which is what a caller that
    * leaves the field out passes, keeps the uniform fetch the round-5 probe
    * makes. */
   bool texel_buffer_storage;
   /* Where in the bound range a dynamic uniform buffer's descriptor starts
    * (Phase D1): 0 keeps the static type every earlier frame used, and a
    * nonzero offset declares the binding dynamic. */
   uint32_t uniform_dynamic_offset;
   /* The count the frame's draws ask for, when it differs from the buffers it
    * bound (V0-robust): 0 draws the vertices and indices the buffers hold, and
    * a larger count is a draw that reaches past its bound. */
   uint32_t draw_vertex_count;
   uint32_t draw_index_count;
   /* Whether the sampled image is stored the way the driver stores an
    * attachment -- 64 KiB tiles of 128x128 texels, one level after another --
    * instead of packed rows (Phase C7's tiled chain). Such an image is created
    * with the colour-attachment usage that makes the driver choose that
    * storage, and the library records no copy into it: the driver refuses a
    * copy into tiled storage, so a caller that wants one fills the texels
    * itself through ps5vk_debug_image_storage (src/diagnostics.cpp,
    * run_vulkan_tiled_mip_frames). */
   bool texture_tiled;
   /* Whether the frame copies the texels it uploaded into a second, tiled image
    * and samples THAT image (Phase C7's vkCmdCopyImage), or does the same with
    * the one-to-one vkCmdBlitImage (texture_blitted). The tiled destination is
    * single-level, of the caller's texture extent and format, with the
    * colour-attachment usage that makes the driver store it in the measured
    * tile layout, and its memory is mapped so a caller can read what the copy
    * left there. False, which is what a caller that leaves the fields out
    * passes, records a frame with no copy at all. */
   bool texture_copied;
   bool texture_blitted;
   /* Whether the frame blits the middle half of its uploaded texels over the
    * whole tiled destination -- a two-times upscale, which is the scaled blit
    * the copy above cannot do (Phase C7's resampler) -- and whether that blit
    * filters linearly instead of taking the nearest texel. */
   bool texture_blit_scaled;
   bool texture_blit_linear;
   /* The sample count of the colour target and its passes (Phase C8): 0 or
    * VK_SAMPLE_COUNT_1_BIT for the one-sample target every earlier frame drew
    * into, and VK_SAMPLE_COUNT_4_BIT for a four-sample one. */
   VkSampleCountFlagBits samples;
   /* Whether a frame's draws go through an indirect buffer (Phase C2's
    * vkCmdDrawIndirect and vkCmdDrawIndexedIndirect) instead of naming their
    * counts themselves: false for every frame before that step, whose stream
    * must not change. The harness fills a buffer of its own with the same
    * parameters it would have passed and binds it for the draw, so a frame
    * recorded this way produces the packets a direct draw of the same geometry
    * does -- which is what the console's own frame is compared against. */
   bool indirect;
   /* Whether a frame's draw goes through a secondary command buffer the
    * primary executes (Phase B8, vkCmdExecuteCommands): the draws are the same
    * ones a direct frame records, so the stream the driver builds has to be the
    * same -- which is what the console's own frame is compared against. A frame
    * that uploads or needs two command buffers records directly (the harness
    * refuses the combination). */
   /* The component mapping of the sampled image's view. Zeroed --
    * VK_COMPONENT_SWIZZLE_IDENTITY, which is what every frame before this field
    * existed passed -- is the identity; anything else is the mapping Vulkan
    * applies to what a combined image sampler returns, and the driver refuses it
    * while its descriptor carries only the format's own selectors
    * (driver/ps5vk_draw.c, docs/M5_PHASE_C.md). */
   VkComponentMapping texture_components;
   /* Whether the four-sample target is *resolved* into the program's own image
    * before the frame ends (Phase C8): the frame renders into a four-sample
    * image of its own and then records vkCmdResolveImage into the mapped
    * one-sample image a caller reads back. False, which is what a caller that
    * leaves the field out passes, renders into the program's image as before. */
   bool resolve_output;
   /* Whether a frame's draw goes through a secondary command buffer the primary
    * executes (Phase B8, vkCmdExecuteCommands): the draws are the same ones a
    * direct frame records, so the stream the driver builds has to be the same --
    * which is what the console's own frame is compared against. A frame that
    * uploads, needs two command buffers or presents records directly (the
    * harness refuses the combination). It is the struct's last field so every
    * earlier caller's positional initializer still names what it did. */
   bool secondary;
   /* The format of the colour attachment the frame renders into (V0-formats'
    * colour targets), or VK_FORMAT_UNDEFINED for the R8G8B8A8_UNORM image every
    * earlier frame used. The harness creates the target image and its view with
    * it, and the driver programs the CB_COLOR0_INFO word
    * ps5vk_find_colour_format names for it (driver/ps5vk_image.c); a format with
    * no such word is refused at render-pass or pipeline creation. The caller
    * reads the mapped target back itself, through the tiled map one 32-bit texel
    * a word. It is the struct's last field for the same reason `secondary` is:
    * every earlier caller's positional initializer still names what it did. */
   VkFormat target_format;
   /* The colour-blend state of every pipeline the frame creates (V0-formats'
    * colour targets), or false for the opaque pipeline every frame before it
    * used: a blending frame clears its attachment to clear_colour and draws its
    * source over it, and the driver turns the state into CB_BLEND0_CONTROL and
    * picks the export format for the target. A caller that blends sets all six
    * fields. Appended at the end for the same reason as target_format. */
   bool blend;
   VkBlendFactor blend_src_colour;
   VkBlendFactor blend_dst_colour;
   VkBlendOp blend_op_colour;
   VkBlendFactor blend_src_alpha;
   VkBlendFactor blend_dst_alpha;
   VkBlendOp blend_op_alpha;
   /* The colour a frame's first render pass clears its attachment to when the
    * load operation is CLEAR, or all zeros for PS5VK_TRIANGLE_CLEAR_WORD's own
    * colour, which is what every frame before V0-formats' colour targets cleared
    * to. A blending frame clears to its destination colour and draws its source
    * over it with a blending pipeline. Appended at the end for the same reason
    * as target_format. */
   float clear_colour[4];
   /* The four blend constants the CONSTANT_COLOR, ONE_MINUS_CONSTANT_COLOR,
    * CONSTANT_ALPHA and ONE_MINUS_CONSTANT_ALPHA factors read. All zeros -- what
    * a caller that leaves them out passes -- is the state every frame before the
    * constant factors were programmable had. Appended at the end for the same
    * reason. */
   float blend_constants[4];
   /* The stencil test of each pipeline (round 12), one entry a pipeline like
    * shaders above: a caller whose frame writes the stencil plane with one
    * pipeline and tests it with the next passes two different states. All zeros
    * -- what a caller that leaves the field out passes -- is a pipeline with the
    * stencil test off, which is every frame before round 12. A pipeline whose
    * test is on gets the same state on both faces: the probes draw front-facing
    * triangles, and a two-sided frame is what a later round would add. A frame
    * with no depth attachment binds none of this: the driver ignores the stencil
    * test when the rendering has no stencil plane, which is what Vulkan asks.
    * Appended at the end for the same reason as target_format. */
   /* What a frame's first render pass does with the stencil plane of a
    * depth/stencil attachment (round 12), or VK_ATTACHMENT_LOAD_OP_DONT_CARE --
    * what a caller that leaves the field out passes, and the only legal value
    * for a format with no stencil. The pass a later draw uses loads the plane,
    * as the depth attachment's own passes do. */
   VkAttachmentLoadOp stencil_load_op;
   struct ps5vk_stencil_state {
      bool test;
      VkStencilOp fail_op;
      VkStencilOp pass_op;
      VkStencilOp depth_fail_op;
      VkCompareOp compare_op;
      uint32_t reference;
      uint32_t compare_mask;
      uint32_t write_mask;
   } stencil[PS5VK_TRIANGLE_MAX_PIPELINES];
   /* Whether the frame records *two* render passes in one command buffer, each
    * clearing the attachment and drawing the frame's geometry (R6): what a
    * renderer that renders offscreen and then draws its target records, and the
    * shape whose two passes corrupted the heap (docs/M5_PHASE_C.md). A frame
    * with a resolve resolves between the two passes, where an application that
    * resolves its multisampled pass before the main one records it. False is
    * the one pass every earlier frame recorded, and the field is appended at
    * the end for the same reason as target_format. */
   bool two_passes;
   /* R2: whether the frame's two samplers are created with
    * `texture_address_mode` (the next field) instead of the clamp-to-edge every
    * earlier frame ran, and which mode that is -- one mode on all three axes,
    * which is the shape a probe varies. Appended at the end for the same reason
    * as target_format. */
   bool texture_address_mode_set;
   VkSamplerAddressMode texture_address_mode;
   /* R1: the rasterization state every pipeline of the frame is created with --
    * cullMode and rasterizerDiscardEnable, which are core Vulkan 1.0. The zero
    * values are the unculled, undiscarded state every earlier frame ran, so a
    * caller that leaves them out records the stream it did before. Appended at
    * the end for the same reason as target_format. */
   VkCullModeFlags rasterization_cull_mode;
   bool rasterization_discard;
   /* R1's depth bias: whether every pipeline of the frame enables one, and the
    * three factors of VkPipelineRasterizationStateCreateInfo's own bias state,
    * which the driver programs for a D32 float depth attachment. False is the
    * unbiased state every earlier frame ran, and the factors are ignored while
    * it is false (Valid Usage), so a caller that leaves them out records the
    * stream it did before. Appended at the end for the same reason as
    * target_format. */
   bool depth_bias_enable;
   float depth_bias_constant;
   float depth_bias_slope;
   float depth_bias_clamp;
   /* R5: whether the frame's resolve destination -- the program's own one-sample
    * image -- declares the usage the specification asks for, TRANSFER_DST and
    * SAMPLED with no colour-attachment bit, instead of the colour-attachment
    * usage every earlier frame's target has. This driver stores such an image in
    * rows and refuses to resolve into it, which is what the request's probe
    * shows; the flag means nothing without resolve_output, where the four-sample
    * image is what the framebuffers name. Appended at the end for the same
    * reason as target_format. */
   bool resolve_destination_transfer_only;
   /* R8: the frame's pipelines declare VK_DYNAMIC_STATE_DEPTH_BIAS and every
    * draw sets its bias with vkCmdSetDepthBias -- the first draw from
    * depth_bias_first, the second from depth_bias_second, each a
    * {depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor} triple --
    * vkCmdSetDepthBias's own argument order. A driver that ignored
    * the upload would use the first triple for both draws. False, and the
    * zeroed triples, are the state every earlier frame ran. Appended at the end
    * for the same reason as target_format. */
   bool dynamic_depth_bias;
   float depth_bias_first[3];
   float depth_bias_second[3];
   /* R9: the pipeline layout declares a push-constant range of this many bytes
    * for push_constant_stages, and every draw uploads them with
    * vkCmdPushConstants -- the first from push_constant_first, the second from
    * push_constant_second. Zero declares no range, which is every frame before
    * this (no probe or test of this driver has ever used a push constant).
    * Appended at the end for the same reason as target_format. */
   uint32_t push_constant_bytes;
   VkShaderStageFlags push_constant_stages;
   uint8_t push_constant_first[PS5VK_TRIANGLE_MAX_PUSH_CONSTANTS];
   uint8_t push_constant_second[PS5VK_TRIANGLE_MAX_PUSH_CONSTANTS];
   /* R8/R9: how many indices the first of a frame's two draws takes, the second
    * the rest of them: the two draws must then be told apart by what each drew
    * (one half of the target each, say), not only by the depth they wrote. Zero
    * gives both draws the whole index buffer, which is what the two-draw
    * groupings did before. Appended at the end for the same reason. */
   uint32_t first_draw_indices;
   /* R7: the pipeline layout declares two descriptor set layouts -- both empty,
    * so no binding reaches the shaders and the frame is a valid pipeline whose
    * *set count* is the thing under test. R7 gives every set its own table, so a
    * layout that declares two of them draws; a binding a stage reads in a set
    * past the four this driver advertises is refused by name where the compiler
    * options are built (driver/ps5vk_pipeline.c, ps5vk_descriptor_options). False
    * is the one-set (or no-set) layout every earlier frame ran. Appended at the
    * end for the same reason as target_format. */
   bool two_descriptor_sets;
   /* R7's vkQuake shape: three combined image samplers at **set 0**'s bindings 0,
    * 1 and 2 -- vkQuake's collapsed texture sets -- and the caller's uniform
    * block at **set 1**'s binding 0, which is the layout its world and md5
    * pipelines declare. texture_data is binding 0, texture_data_second binding 1
    * and texture_data_third binding 2, each texture_width x texture_height
    * RGBA8 texels that the *caller* copies into the mapped image memory the
    * frame reports in multiset_mappings and flushes itself (the console's
    * clflush, src/diagnostics.cpp); uniform_data holds the block set 1 names.
    * False keeps the layout every earlier phase uses, where the uniform is set 0
    * and the one texture set 1 when both are present. */
   bool textures_in_first_set;
   const void *texture_data_second;
   const void *texture_data_third;
   /* What the frame's samplers ask for anisotropically: the device reports
    * maxSamplerAnisotropy 1.0, where the flag can only be 1.0 and is a no-op, so
    * a frame that sets it has to render the isotropic frame exactly
    * (driver/ps5vk_image.c, ps5vk_CreateSampler; the probe is
    * src/diagnostics.cpp's v0-sampler-anisotropy). False and 0.0 are what every
    * earlier phase created. */
   bool sampler_anisotropy;
   float sampler_max_anisotropy;
   /* The colour attachments the rendering declares: 1 is the single target every
    * earlier phase drew into, and 2..PS5VK_TRIANGLE_MAX_TARGETS render into that
    * many images at once. The caller passes the device's own advertised
    * VkPhysicalDeviceLimits.maxColorAttachments, so the probe varies the count
    * against what the device says. */
   uint32_t color_attachment_count;
   /* R2's separated form: the texture's view and its sampler in **two** sets
    * instead of one combined descriptor -- set 0 binding 0 a bare sampled image,
    * set 1 binding 0 a bare sampler -- the shape a renderer keeps when one sampler
    * serves many textures (the port's GUI pipeline). The driver tells the compiler
    * the combined type at each half's own index and the instruction reads the half
    * it needs from that binding's entry, so the frame this draws has to be the
    * combined frame's, texel for texel (driver/ps5vk_descriptor_set_layout.c,
    * driver/ps5vk_draw.c). */
   bool separated_texture_pair;
   /* The topology the frame's pipelines declare: VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    * -- which is what every phase before this one used -- unless the caller names
    * another. A strip draws one triangle per vertex after the second with the
    * hardware alternating the winding, so a strip of a quad's four vertices is the
    * two triangles a list of its six indices draws (R6 of the port's requests). */
   VkPrimitiveTopology primitive_topology;
};


/* The colour a clearing render pass clears to: red 0x40, green 0x80, blue
 * 0xff, opaque. ps5vk_triangle.c clears to those bytes as floats, and a
 * readback of the display's B8G8R8A8_UNORM images sees them as this word,
 * which is the packing src/diagnostics.cpp's colour_word names (the offscreen
 * R8G8B8A8 images pack the other way). No probe shader draws this colour, and
 * an untouched framebuffer is zero, so a pixel holding it is one the clear
 * wrote (Phase C1b). */
#define PS5VK_TRIANGLE_CLEAR_WORD UINT32_C(0xff4080ff)

/* How a frame's draws reach the queue. */
enum ps5vk_triangle_grouping {
   /* One draw (Phase B7). */
   PS5VK_TRIANGLE_ONE_DRAW,
   /* Both draws in one render pass of one command buffer. */
   PS5VK_TRIANGLE_ONE_COMMAND_BUFFER,
   /* One command buffer per draw, both in one VkSubmitInfo. */
   PS5VK_TRIANGLE_TWO_COMMAND_BUFFERS,
   /* One command buffer per draw, one VkSubmitInfo each, one vkQueueSubmit. */
   PS5VK_TRIANGLE_TWO_SUBMIT_INFOS,
   /* One command buffer per draw, one vkQueueSubmit each. */
   PS5VK_TRIANGLE_TWO_SUBMISSIONS,
};

enum ps5vk_triangle_status {
   /* The step completed: after a draw, the target holds the frame. */
   PS5VK_TRIANGLE_OK,
   /* A step failed, and nothing is in use by the GPU. */
   PS5VK_TRIANGLE_FAILED,
   /* A submission or presentation did not complete: the GPU may still use
    * the objects, so they must not be released. */
   PS5VK_TRIANGLE_IN_FLIGHT,
};

struct ps5vk_triangle {
   /* After a draw into the program's image: the mapped image memory and its
    * size. NULL and 0 for the display, whose images are not mappable. */
   const void *target;
   size_t target_bytes;
   /* After a draw to the display: the swapchain image drawn, in images, which
    * ps5vk_triangle_present presents. */
   uint32_t image_index;

   ps5vk_get_instance_proc_addr get_instance_proc_addr;
   const struct ps5vk_triangle_report *report;
   enum ps5vk_triangle_output output;
   uint32_t pipeline_count;
   VkFormat format;
   /* The colour a frame's CLEAR load operation uses: PS5VK_TRIANGLE_CLEAR_WORD's
    * own colour, or the caller's clear_colour when it set one (V0-formats'
    * colour targets blend over it). */
   float clear_colour[4];
   void *mapped;
   size_t memory_bytes;
   VkInstance instance;
   VkDebugUtilsMessengerEXT messenger;
   VkSurfaceKHR surface;
   VkDevice device;
   VkQueue queue;
   /* The swapchain as created; a caller replacing it passes this again with
    * oldSwapchain. */
   VkSwapchainCreateInfoKHR swapchain_info;
   VkSwapchainKHR swapchain;
   /* Whether an image is acquired and drawn but not yet presented. */
   bool acquired;
   uint32_t image_count;
   VkImage images[PS5VK_TRIANGLE_MAX_IMAGES];
   VkDeviceMemory memory;
   VkImageView views[PS5VK_TRIANGLE_MAX_IMAGES];
   VkRenderPass first_pass;
   VkRenderPass load_pass;
   VkFramebuffer framebuffers[PS5VK_TRIANGLE_MAX_IMAGES];
   VkPipelineLayout layout;
   VkShaderModule vertex[PS5VK_TRIANGLE_MAX_PIPELINES];
   VkShaderModule pixel[PS5VK_TRIANGLE_MAX_PIPELINES];
   VkPipeline pipelines[PS5VK_TRIANGLE_MAX_PIPELINES];
   VkCommandPool pool;
   VkCommandBuffer commands[2];
   VkFence fence;
   VkSemaphore image_available;
   VkSemaphore render_finished;
   /* The caller's geometry, uploaded to host-visible memory (Phase C2). */
   VkBuffer vertex_buffer;
   VkDeviceMemory vertex_memory;
   void *vertex_mapped;
   VkBuffer index_buffer;
   VkDeviceMemory index_memory;
   void *index_mapped;
   /* Whether the frame's draws go through the indirect buffer below (Phase
    * C2): what the caller asked for at create time. */
   bool indirect;
   /* Whether a frame's draw goes through a secondary command buffer (Phase B8):
    * what the caller asked for at create time, and the buffer itself, which is
    * allocated with the secondary level when it is. */
   bool secondary;
   VkCommandBuffer secondary_command;
   /* The indirect buffer a frame's draws go through when the caller asked for
    * indirect draws (Phase C2): host-visible memory the recording writes the
    * parameters into, which is what vkCmdDrawIndirect reads. */
   VkBuffer indirect_buffer;
   VkDeviceMemory indirect_memory;
   void *indirect_mapped;
   /* The indices those buffers hold, which a frame's recording needs: the
    * caller's input is not kept. Zero draws three vertices, with no binding,
    * as the B7/B8/C1 sets do. */
   uint32_t vertex_count;
   uint32_t index_count;
   /* The caller's geometry when it is staged (Phase C2): the mapped staging
    * buffer the data was written into, and the copies the GPU-side buffers
    * hold after the submission, which a test reads to prove the copy ran. */
   VkBuffer staging_buffer;
   VkDeviceMemory staging_memory;
   void *staging_mapped;
   bool staged;
   const void *vertex_copied;
   const void *index_copied;
   /* The two regions a staged frame's copies move out of the staging buffer:
    * its recording needs their sizes, and the caller's input is not kept.
    * Zero when the caller did not stage. */
   VkDeviceSize vertex_bytes;
   VkDeviceSize index_bytes;
   /* The uniform buffer the caller supplied (Phase C3): its bytes live in a
    * mapped buffer at set 0, binding 0, and the pipeline layout holds the set
    * layout they were declared in. Zero handles when the caller asked for no
    * descriptors, which leaves the pipeline layout and every recording call
    * exactly as they were before Phase C3. */
   VkDescriptorSetLayout set_layout;
   VkDescriptorPool descriptor_pool;
   VkDescriptorSet descriptor_set;
   VkBuffer uniform_buffer;
   VkDeviceMemory uniform_memory;
   void *uniform_mapped;
   VkDeviceSize uniform_bytes;
   /* The texture the caller's pixel shader samples (Phase C4): the sampled
    * image and its mapped memory (the copy's destination, so a test reads back
    * what reached it), the view and the two samplers its descriptor names, the
    * staging buffer the texels were written into, and the second set that
    * names the view and sampler. Zero handles when the caller passed no
    * texture, which leaves the pipeline layout and every recording call as
    * they were before Phase C4. */
   /* The sample count of the target and its passes (Phase C8): one unless the
    * caller asked for four, set from the input before the target is created. */
   VkSampleCountFlagBits samples;
   /* Phase C8's resolve: the four-sample image the frame renders into, whose
    * view the framebuffers name, and which the frame resolves into images[0].
    * Zero handles when the caller did not ask for a resolve. */
   VkImage resolve_image;
   VkDeviceMemory resolve_memory;
   VkImageView resolve_view;
   bool resolve_output;
   /* R5: the usage the resolve destination declares, from
    * input->resolve_destination_transfer_only. */
   bool resolve_destination_transfer_only;
   /* R8's per-draw depth bias (input->dynamic_depth_bias and its triples) and
    * R9's per-draw push constants (input->push_constant_bytes and its bytes),
    * with how many draws this frame has recorded (each draw sets its state
    * before it draws, so the first draw takes the first values and the second
    * the second). */
   bool dynamic_depth_bias;
   float depth_bias_first[3];
   float depth_bias_second[3];
   uint32_t push_constant_bytes;
   VkShaderStageFlags push_constant_stages;
   uint8_t push_constant_first[PS5VK_TRIANGLE_MAX_PUSH_CONSTANTS];
   uint8_t push_constant_second[PS5VK_TRIANGLE_MAX_PUSH_CONSTANTS];
   uint32_t first_draw_indices;
   uint32_t draws_recorded;
   /* R7: the second empty set layout the pipeline layout declares, from
    * input->two_descriptor_sets. */
   VkDescriptorSetLayout second_set_layout;
   bool two_descriptor_sets;
   /* R6: whether the frame records a second render pass in the same command
    * buffer (input->two_passes). */
   bool two_passes;
   /* R2: the address mode the frame's samplers are created with
    * (input->texture_address_mode_set and input->texture_address_mode). */
   bool texture_address_mode_set;
   VkSamplerAddressMode texture_address_mode;
   /* R1: the rasterization state every pipeline of the frame is created with
    * (input->rasterization_cull_mode and input->rasterization_discard). */
   VkCullModeFlags rasterization_cull_mode;
   bool rasterization_discard;
   /* R1's depth bias, carried from the input to the pipelines the frame creates
    * (input->depth_bias_enable and the three factors). */
   bool depth_bias_enable;
   float depth_bias_constant;
   float depth_bias_slope;
   float depth_bias_clamp;
   VkImage texture_image;
   /* The format the image was created with: a depth format's view and upload
    * name the depth aspect, a colour format's the colour one (round 21). */
   VkFormat texture_format;
   VkDeviceMemory texture_memory;
   void *texture_mapped;
   size_t texture_bytes;
   VkExtent2D texture_extent;
   VkImageView texture_view;
   /* The nearest sampler first, the linear one second; the descriptor set names
    * whichever ps5vk_triangle_set_texture_filter selected. */
   VkSampler texture_samplers[2];
   /* The sampler a frame pinned a LOD range with, or VK_NULL_HANDLE
    * (ps5vk_triangle_set_texture_lod, Phase C7). */
   VkSampler texture_lod_sampler;
   /* The view a frame pointed the set at, or VK_NULL_HANDLE (Phase C7). */
   VkImageView texture_level_view;
   bool texture_bilinear;
   VkBuffer texture_staging_buffer;
   VkDeviceMemory texture_staging_memory;
   void *texture_staging_mapped;
   /* R7's vkQuake shape (input->textures_in_first_set): the three sampled images
    * set 0's bindings 0, 1 and 2 name, their views, the one set that names them
    * -- and the mappings the caller fills the texels through, with each image's
    * mapped size. The uniform block's set is the same create_uniform builds for
    * every other frame's set 0 and this shape places at index 1. Zero handles
    * when the caller asked for another layout. */
   VkImage multiset_images[PS5VK_TRIANGLE_MULTISET_TEXTURES];
   VkDeviceMemory multiset_memories[PS5VK_TRIANGLE_MULTISET_TEXTURES];
   void *multiset_mappings[PS5VK_TRIANGLE_MULTISET_TEXTURES];
   size_t multiset_bytes[PS5VK_TRIANGLE_MULTISET_TEXTURES];
   VkImageView multiset_views[PS5VK_TRIANGLE_MULTISET_TEXTURES];
   /* A frame that renders into more than one colour attachment: each
    * attachment's mapped memory and its size, in attachment order, with
    * target/target_bytes naming the first -- the readback every case already
    * uses. Zero and NULL for a one-attachment frame, whose image and memory are
    * images[0] and memory. */
   /* The colour attachments the device advertises, read from the physical device
    * when the frame is created: a case varies its attachment count up to this,
    * so the probe varies against what the device says rather than against a
    * number written into the test (src/diagnostics.cpp, v0-mrt). */
   uint32_t max_color_attachments;
   VkDeviceMemory target_memories[PS5VK_TRIANGLE_MAX_TARGETS];
   void *target_mappings[PS5VK_TRIANGLE_MAX_TARGETS];
   size_t target_memory_bytes[PS5VK_TRIANGLE_MAX_TARGETS];
   uint32_t colour_attachment_count;
   VkDescriptorSetLayout multiset_set_layout;
   VkDescriptorPool multiset_pool;
   VkDescriptorSet multiset_set;
   /* The anisotropy the frame's samplers were created with, from the input
    * above; create_texture_samplers reads it. */
   bool sampler_anisotropy;
   float sampler_max_anisotropy;
   /* The uniform texel buffer a frame's pixel shader fetches from, its view and
    * the set the frame binds (V0-formats' descriptor-type rows). Zero handles
    * when the caller passed no texel buffer, and the driver's destroys ignore
    * zero, so a program that fetches none allocates nothing new. */
   bool texel_buffer_whole_size;
   /* The sampler's own set, when a caller keeps the pair apart; the set itself
    * goes with the texture's pool above. */
   VkDescriptorSetLayout sampler_set_layout;
   VkDescriptorSet sampler_set;
   bool separated_texture_pair;
   VkBuffer texel_buffer;
   VkDeviceMemory texel_buffer_memory;
   void *texel_buffer_mapped;
   VkBufferView texel_buffer_view;
   /* The storage image a frame's pixel shader stores into, its view and the
    * memory the caller reads the stores from. Zero handles when the caller
    * passed no storage image, and the driver's destroys ignore zero. */
   VkImage storage_image;
   VkDeviceMemory storage_image_memory;
   void *storage_image_mapped;
   size_t storage_image_bytes;
   VkImageView storage_image_view;
   VkDescriptorSetLayout texture_set_layout;
   VkDescriptorPool texture_pool;
   /* The set a frame's pixel shader samples through: the uploaded image's view,
    * or -- in Phase C4's render-to-texture case -- the view of the image the
    * frame rendered into, which is the whole point of that case. The fill pass
    * of that case binds texture_source_set, below, which always names the
    * uploaded view, with the nearest sampler its exact copy needs. */
   VkDescriptorSet texture_set;
   /* The descriptor sets a frame binds, in the order the pipeline layout names
    * their layouts: the uniform buffer's, then the texture's. The uniform keeps
    * set 0 whenever the caller supplied one, as before Phase C4; a texture-only
    * caller's set is set 0, which is where the probe sets that sample declare
    * their combined image sampler. */
   VkDescriptorSetLayout set_layouts[2];
   VkDescriptorSet descriptor_sets[2];
   uint32_t set_count;
   /* Phase C4's render-to-texture case (input->texture_is_rendered): the second
    * colour image the frame draws the caller's texels into and then samples,
    * its view, the pass and framebuffer the fill pass draws over it, the set
    * that pass samples the uploaded texels through (the frame's own set names
    * the rendered image's view instead), and the full-target quad it draws.
    * Every handle is zero when the caller did not ask for the case, and the
    * driver's destroys and frees ignore zero, so a frame that does not render
    * into its texture allocates and records nothing new. */
   bool texture_is_rendered;
   VkImage rendered_image;
   VkDeviceMemory rendered_memory;
   VkImageView rendered_view;
   VkRenderPass rendered_pass;
   VkFramebuffer rendered_framebuffer;
   VkDescriptorSet texture_source_set;
   VkBuffer fill_buffer;
   VkDeviceMemory fill_memory;
   void *fill_mapped;
   /* Phase C5's depth attachment: whether the program has one and what its
    * first render pass does with it, then the image, its memory (mapped for a
    * caller's readback), its view, and what ps5vk_triangle_draw reports. */
   bool depth;
   VkAttachmentLoadOp depth_load_op;
   /* The stencil plane's own first-pass load operation, for the combined
    * depth/stencil attachment round 12 renders through. */
   VkAttachmentLoadOp stencil_load_op;
   float depth_clear_value;
   /* The depth attachment's format (input.depth_format). */
   VkFormat depth_format;
   /* Whether the frame records vkCmdClearDepthStencilImage over the depth
    * image before its pass (input.depth_clear_image). */
   bool depth_clear_image;
   /* Phase V0-query: the scissor the pipelines declare, and the occlusion query
    * each frame's draws are recorded inside (VK_NULL_HANDLE for none). */
   bool use_scissor;
   VkRect2D scissor;
   VkQueryPool query_pool;
   uint32_t query;
   /* The image's array layers (input.texture_layers), one for every frame
    * before D1. */
   uint32_t texture_layers;
   /* Whether a tiled chain's texels are uploaded by the frame's own copies
    * (input.texture_upload). */
   bool texture_upload;
   /* Phase V0-query's timestamps: the pool a frame writes its clock into --
    * query 0 before the frame's draws and query 1 after them -- when the caller
    * set one with ps5vk_triangle_set_timestamp_pool. */
   VkQueryPool timestamp_pool;
   /* The buffer a recorded vkCmdCopyQueryPoolResults wrote the query's result
    * into, mapped, when the caller asked for the copy (input.query_copy). */
   VkBuffer query_copy_buffer;
   VkDeviceMemory query_copy_memory;
   void *query_copy_mapped;
   /* Phase C7: the levels the sampled image has, whether the samplers mip the
    * chain linearly, the maxLod they reach, and where each level's texels start
    * in the staging buffer the frame copies them out of. */
   uint32_t texture_levels;
   bool texture_mip_linear;
   bool texture_tiled;
   uint32_t uniform_range_bytes;
   uint32_t uniform_dynamic_offset;
   bool uniform_dynamic;
   uint32_t draw_index_count;
   float texture_max_lod;
   /* One staging offset per (layer, level): a D1 array's layers repeat the level
    * stack (input.texture_layers). */
   VkDeviceSize texture_level_offsets[PS5VK_TRIANGLE_MAX_MIP_LEVELS *
                                      PS5VK_TRIANGLE_MAX_ARRAY_LAYERS];
   /* Phase C2's base vertex and instance count: what every draw of the next
    * frame adds to its indices and how many times it runs. */
   int32_t base_vertex;
   uint32_t instance_count;
   bool explicit_viewport;
   VkImage depth_image;
   VkDeviceMemory depth_memory;
   VkImageView depth_view;
   void *depth_mapped;
   size_t depth_bytes;
   void *depth_target;
   size_t depth_target_bytes;
   /* The rendered image's memory, mapped for the life of the program so a test
    * can read what the fill pass wrote into it: the render's own result on its
    * own, separate from what the frame's sample of it produced. NULL with
    * rendered_bytes zero when the caller did not ask for the case. */
   void *rendered;
   size_t rendered_bytes;
   /* Phase C7's copy case (input->texture_copied or texture_blitted): the tiled
    * image the frame copies or blits the uploaded texels into and then samples,
    * its view, and its memory, mapped so a caller can read the copy's bytes. */
   VkImage copied_image;
   VkDeviceMemory copied_memory;
   void *copied;
   size_t copied_bytes;
   VkImageView copied_view;
   bool texture_copied;
   bool texture_blitted;
   bool texture_blit_scaled;
   bool texture_blit_linear;
};

/* Creates every object into triangle. Unless the result is
 * PS5VK_TRIANGLE_IN_FLIGHT, ps5vk_triangle_finish must release it. */
enum ps5vk_triangle_status
ps5vk_triangle_create(struct ps5vk_triangle *triangle, const struct ps5vk_triangle_input *input);

/* Draws a frame: clears the program's image, or acquires a swapchain image;
 * records the frame's draws, submits them as grouping says and waits for the
 * fence. On the display, each draw must be presented before the next. */
enum ps5vk_triangle_status
ps5vk_triangle_draw(struct ps5vk_triangle *triangle, enum ps5vk_triangle_grouping grouping);

/* Presents the swapchain image the last draw rendered. */
enum ps5vk_triangle_status
ps5vk_triangle_present(struct ps5vk_triangle *triangle);

/* Replaces the bytes of the uniform buffer bound at set 0, binding 0, and
 * updates the descriptor set with the new range (Phase C3). False when the
 * program has no uniform buffer. */
bool
ps5vk_triangle_set_uniform(struct ps5vk_triangle *triangle, const void *data, uint32_t bytes);

/* Points the texture's descriptor set at the linear sampler (Phase C4),
 * replacing the nearest one the frames started with, or back again: the two
 * filters the m3-texture canary's two frames ran. False when the program has no
 * texture. The samplers themselves are made once, by ps5vk_triangle_create. */
bool
ps5vk_triangle_set_texture_filter(struct ps5vk_triangle *triangle, bool bilinear);

/* Phase V0-query: an occlusion query pool on the program's device, the query a
 * frame records inside (ps5vk_triangle_set_query, before the next
 * ps5vk_triangle_draw), the samples a query counted, and the 64-bit result a
 * frame's recorded vkCmdCopyQueryPoolResults left in the harness's buffer. */
bool
ps5vk_triangle_query_copy(const struct ps5vk_triangle *triangle, uint64_t *value,
                          uint64_t *available);

VkQueryPool
ps5vk_triangle_create_query_pool(struct ps5vk_triangle *triangle, uint32_t count);

/* Phase V0-query's timestamps: a timestamp pool on the program's device, the
 * two writes a frame records into its query 0 (before the frame's draws) and
 * query 1 (after them) once ps5vk_triangle_set_timestamp_pool names it, and the
 * ticks either read back. V0-query's probe measured the console's clock
 * (docs/M5_PHASE_C.md, pid 143), which is what these read. */
VkQueryPool
ps5vk_triangle_create_timestamp_pool(struct ps5vk_triangle *triangle, uint32_t count);

void
ps5vk_triangle_set_timestamp_pool(struct ps5vk_triangle *triangle, VkQueryPool pool);

bool
ps5vk_triangle_query_timestamp(struct ps5vk_triangle *triangle, VkQueryPool pool, uint32_t query,
                               uint64_t *ticks);

void
ps5vk_triangle_destroy_query_pool(struct ps5vk_triangle *triangle, VkQueryPool pool);

/* Phase C7: a sampler for the texture's set whose LOD range is min_lod to
 * max_lod, which is how a frame pins the level its sample reads whatever LOD
 * the sample would take. Returns false when no sampler could be created. */
bool
ps5vk_triangle_set_texture_lod(struct ps5vk_triangle *triangle, float min_lod, float max_lod);

/* Phase C7: points the texture's set at a view of one level of the chain, level
 * 0 being the image itself. Returns false when no view could be created. */
bool
ps5vk_triangle_set_texture_view(struct ps5vk_triangle *triangle, uint32_t level);

/* Phase C2: the base vertex every indexed draw of the next frame adds to the
 * indices it fetches, which is vkCmdDrawIndexed's vertexOffset. */
void
ps5vk_triangle_set_base_vertex(struct ps5vk_triangle *triangle, int32_t offset);

/* Binds a shorter range of the uniform buffer than the whole of it, or the
 * whole buffer again with 0 (V0-robust). */
bool
ps5vk_triangle_set_uniform_range(struct ps5vk_triangle *triangle, uint32_t bytes);

/* Raises the index count a frame's draws ask for, past what the buffer holds
 * (V0-robust). */
bool
ps5vk_triangle_set_draw_index_count(struct ps5vk_triangle *triangle, uint32_t count);

/* Moves the dynamic uniform buffer's offset (Phase D1). */
bool
ps5vk_triangle_set_uniform_offset(struct ps5vk_triangle *triangle, uint32_t offset);

/* Phase C2: how many instances the next frame's draws run, one being every
 * frame before the step's instancing probe. */
void
ps5vk_triangle_set_instance_count(struct ps5vk_triangle *triangle, uint32_t count);

void
ps5vk_triangle_set_query(struct ps5vk_triangle *triangle, VkQueryPool pool, uint32_t query);

bool
ps5vk_triangle_query_samples(struct ps5vk_triangle *triangle, VkQueryPool pool, uint32_t query,
                             uint64_t *samples);

const char *
ps5vk_triangle_grouping_name(enum ps5vk_triangle_grouping grouping);

/* Releases every object the program created. */
void
ps5vk_triangle_finish(struct ps5vk_triangle *triangle);

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_TRIANGLE_H */
