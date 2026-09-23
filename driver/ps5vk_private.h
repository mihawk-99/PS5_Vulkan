/*
 * PS5 Vulkan driver - private declarations.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B2 (docs/M5_PHASE_B.md). The driver is built on Mesa's
 * common Vulkan runtime (tools/build-vulkan-runtime.sh). Every object embeds
 * its runtime base first, and ps5vk_entrypoints.{c,h}, generated from vk.xml
 * with --prefix ps5vk (tools/build-driver.sh), name the entry points this
 * driver implements; the runtime's common entry points fill the rest.
 */

#ifndef PS5VK_PRIVATE_H
#define PS5VK_PRIVATE_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "c11/threads.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/u_dynarray.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_common_entrypoints.h"
#include "vk_descriptor_set_layout.h"
#include "vk_device.h"
#include "vk_framebuffer.h"
#include "vk_device_memory.h"
#include "vk_graphics_state.h"
#include "vk_image.h"
#include "vk_instance.h"
#include "vk_log.h"
#include "vk_meta.h"
#include "vk_physical_device.h"
#include "vk_pipeline_layout.h"
#include "vk_queue.h"
#include "vk_sync.h"

#include "ps5vk_debug.h"
#include "psbc_compile.h"
#include "ps5vk_entrypoints.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vulkan 1.0, at both levels, because that is what this driver implements:
 * the commands, features and formats of 1.1 and later are not here, so an
 * instance that claimed them would hand an application entry points whose
 * objects it cannot honour. The two versions must agree -- the CTS's
 * dEQP-VK.api.info.extension_core_versions reads them together and failed
 * while the instance claimed 1.3, because its version graph does not treat a
 * later version as supporting an earlier one -- and the runner's device-report
 * case asserts the agreement on every gate (docs/M5_PHASE_C.md, round 6).
 *
 * This is separate from the *ICD interface* version, which is what the loader
 * negotiates with vk_icdNegotiateLoaderICDInterfaceVersion
 * (driver/ps5vk_instance.c) and which is unaffected by this. */
#define PS5VK_INSTANCE_API_VERSION VK_API_VERSION_1_0
#define PS5VK_DEVICE_API_VERSION VK_API_VERSION_1_0
#define PS5VK_DRIVER_VERSION VK_MAKE_VERSION(0, 2, 0)

/* GPU-visible addresses. The shader compiler combines 32-bit pointers with a
 * fixed high word (--address32-hi 2, probes/m3/bindings.txt), and every console
 * mapping recorded so far lies at high word 2 (0x20001c000, 0x200200000), so
 * GPU-visible memory must lie in that word's 4 GiB window. */
#define PS5VK_ADDRESS_HIGH_WORD UINT64_C(2)
#define PS5VK_ADDRESS_WINDOW_BYTES (UINT64_C(1) << 32)

/* GPU-visible direct memory as the test runner allocates it: type 12, mapped
 * for CPU and GPU read and write (protection 0x33), in pages of 16 KiB. */
#define PS5VK_DIRECT_MEMORY_TYPE 12
#define PS5VK_MAP_PROTECTION 0x33
#define PS5VK_DIRECT_PAGE_BYTES UINT64_C(0x4000)

/* VkPhysicalDeviceLimits::maxMemoryAllocationCount, enforced per device. */
#define PS5VK_MAX_MEMORY_ALLOCATIONS 4096

/* Buffer size and bind alignment, equal to the minimum buffer offset
 * alignments (ps5vk_buffer.c). */
#define PS5VK_BUFFER_ALIGNMENT UINT64_C(256)

/* Push constants reach a shader as a driver-owned uniform buffer at one
 * reserved binding of set 0, because the compiler does not report where it
 * would otherwise place them (docs/M5_PHASE_C.md, C1b question 1). The NIR
 * path writes the load in libpsbc's standalone slot form, which the compiler
 * places at PSBC_GALLIUM_UBO_BINDING_BASE plus the slot
 * (lower_gallium_ubo_index). */
#define PS5VK_PUSH_CONSTANT_SLOT 0
#define PS5VK_PUSH_CONSTANT_BINDING (PSBC_GALLIUM_UBO_BINDING_BASE + PS5VK_PUSH_CONSTANT_SLOT)

/* VkPhysicalDeviceLimits::maxPushConstantsSize and maxVertexInputBindings
 * (ps5vk_physical_device.c). */
#define PS5VK_MAX_PUSH_CONSTANT_BYTES 128
#define PS5VK_MAX_VERTEX_BINDINGS 16

/* Whether [address, address + bytes) lies inside the address window. */
static inline bool
ps5vk_address_range_valid(uint64_t address, uint64_t bytes)
{
   return bytes != 0 && address >> 32 == PS5VK_ADDRESS_HIGH_WORD &&
          bytes <= PS5VK_ADDRESS_WINDOW_BYTES - (address & UINT32_MAX);
}

/* A GPU-visible direct-memory allocation, mapped for the CPU and the GPU at
 * one address inside the address window (ps5vk_direct_memory.c). */
struct ps5vk_direct_mapping {
   /* sceKernelAllocateDirectMemory's start, or -1 before it succeeds. */
   int64_t start;
   /* The allocation size, a whole number of direct-memory pages. */
   size_t bytes;
   /* The CPU and GPU address, or NULL before it is mapped. */
   void *address;
};

/* ps5vk_direct_mapping_create's result for memory mapped outside the window. */
#define PS5VK_DIRECT_OUTSIDE_WINDOW INT32_C(-1)

/* Allocates and maps bytes of direct memory at alignment. Returns 0, the
 * failing sceKernel result, or PS5VK_DIRECT_OUTSIDE_WINDOW; on failure
 * nothing stays allocated. */
int32_t
ps5vk_direct_mapping_create(struct ps5vk_direct_mapping *mapping, size_t bytes, size_t alignment);

/* Unmaps and releases whatever of mapping exists. */
void
ps5vk_direct_mapping_destroy(struct ps5vk_direct_mapping *mapping);

/* Evicts the CPU cache lines of a range: before the GPU reads what the CPU
 * wrote, and before the CPU reads what the GPU wrote. */
void
ps5vk_flush_cpu_cache(const void *address, size_t bytes);

struct ps5vk_instance {
   struct vk_instance vk;
};

/* The one VideoOut display and its one mode (ps5vk_wsi.c): their addresses
 * are the VkDisplayKHR and VkDisplayModeKHR handles. */
struct ps5vk_display {
   uint32_t unused;
};

struct ps5vk_physical_device {
   struct vk_physical_device vk;
   struct ps5vk_display display;
   struct ps5vk_display mode;
};

/* A CPU-signalled binary sync object for fences and semaphores (ps5vk_sync.c). */
struct ps5vk_sync {
   struct vk_sync base;
   mtx_t lock;
   cnd_t changed;
   bool signaled;
};

extern const struct vk_sync_type ps5vk_sync_type;

/* An event (ps5vk_sync.c, Phase B5): the same host flag a binary sync object
 * is, with Vulkan's event semantics. vkSetEvent and vkResetEvent set and clear
 * it from the host; vkCmdSetEvent, vkCmdResetEvent and vkCmdWaitEvents do the
 * same from a submission, where the queue reaches them at a split point, so an
 * event's place in the command order is its place in the split order. The lock
 * is what makes the flag safe to set from another thread while a submission
 * waits on it. */
struct ps5vk_event {
   struct vk_object_base base;
   mtx_t lock;
   bool signaled;
};

/* A pipeline cache (ps5vk_pipeline_cache.c, Phase B6): AGC compiles a pipeline
 * when it is created and the driver stores nothing between runs, so the cache
 * object is the empty cache Vulkan allows an implementation to have, and this
 * struct is only its identity: vkGetPipelineCacheData reports no bytes and a
 * merge has nothing to do. */
struct ps5vk_pipeline_cache {
   struct vk_object_base base;
};

/* The steps one submission reaches the GPU as: one per split, which the copies
 * and the render-to-texture samples record (ps5vk_cmd_buffer_split). The
 * runner's capture reads them all, so a golden holds a whole submission and not
 * just its last piece. A submission that records more steps than this keeps the
 * first PS5VK_MAX_SUBMISSION_STEPS of them, which is what the capture reports. */
#define PS5VK_MAX_SUBMISSION_STEPS 8

struct ps5vk_queue_profile {
   bool enabled;
   uint64_t since, frames, steps, queue_ns, flush_ns, gpu_ns, flip_ns, flush_bytes;
   uint64_t copy_ns, sync_wait_ns, sync_signal_ns;
};

struct ps5vk_queue {
   struct vk_queue vk;
   /* The GPU-visible submission buffer (ps5vk_queue.c); its last word is the
   * completion marker. */
   struct ps5vk_direct_mapping submission;
   /* The value the last submission's completion marker carried. */
   uint32_t marker_value;
   /* The words of the last submission, which stay in the buffer above until
    * the next one: what ps5vk_debug_last_submission reports to the runner's
    * capture. */
   uint32_t last_words;
   /* Where the last submission's words start: a submission split by copies or
    * by a render-to-texture sample (ps5vk_cmd_buffer_split) leaves its last
    * piece there, and the runner's capture reads it
    * (ps5vk_debug_last_submission). */
   const uint32_t *last_stream;
   /* Every step of the last submission, in the order the GPU ran them. */
   struct {
      const uint32_t *stream;
      uint32_t words;
   } steps[PS5VK_MAX_SUBMISSION_STEPS];
   uint32_t step_count;
   /* The words of those steps as they were submitted, end packets included,
    * for the steps of a split submission: the step after one lands its own
    * words on that one's end packets in the submission buffer, so a capture
    * that read the buffer afterwards would see two steps mixed. Owned by the
    * queue, and grown over submissions (ps5vk_queue.c). */
   uint32_t *step_capture;
   size_t step_capture_words;
   struct ps5vk_queue_profile profile;
};

/* One AGC register-table record: register offset and value. */
struct ps5vk_agc_register {
   uint16_t offset;
   uint16_t padding;
   uint32_t value;
};

/* The 16 CB_COLORi_* registers of one colour target (ps5vk_draw.c). */
#define PS5VK_TARGET_REGISTER_COUNT 16

/* The colour attachments this driver binds: **four**, which is the number it
 * advertises (VkPhysicalDeviceLimits.maxColorAttachments, ps5vk_physical_device.c)
 * and the specification's floor. One owner, two readers: the driver reports this
 * number and programs up to it, so the two cannot drift. */
#define PS5VK_MAX_COLOR_TARGETS 4

/* The 16 DB_* registers of a depth target and the DB_DEPTH_CONTROL word a draw
 * sets from its pipeline's depth state (ps5vk_draw.c, Phase C5). */
#define PS5VK_DEPTH_REGISTER_COUNT 16
#define PS5VK_DEPTH_CONTROL_REGISTER 0x200

/* The three stencil registers a draw whose pipeline tests stencil adds beside
 * the depth ones (ps5vk_draw.c, ps5vk_stencil_registers): DB_STENCIL_CONTROL
 * (R_02842C, the six front and back face operations), DB_STENCILREFMASK and
 * DB_STENCILREFMASK_BF (R_028430/R_028434, the reference, compare mask, write
 * mask and op value of each face). The offsets are the depth target's own
 * space -- (register - 0x28000) / 4 -- the way DB_DEPTH_CONTROL's 0x200 is
 * R_028800's (docs/HARDWARE_FINDINGS.md, the stencil entry). */
#define PS5VK_STENCIL_REGISTER_COUNT 3
#define PS5VK_STENCIL_CONTROL_REGISTER 0x10b
#define PS5VK_STENCIL_REFMASK_REGISTER 0x10c
#define PS5VK_STENCIL_REFMASK_BF_REGISTER 0x10d

/* The rasterizer registers a four-sample draw adds to its context table
 * (ps5vk_draw.c, ps5vk_multisample_registers): the sample count and distance,
 * the coverage mask, the four sample-location registers, the centroid
 * priorities, the rasterizer's MSAA enable and DB_EQAA (Phase C8). A
 * one-sample draw adds none: AGC's own context defaults already say one
 * sample, which is what every frame before C8 ran. */
#define PS5VK_MULTISAMPLE_REGISTER_COUNT 11

/* A colour target a command buffer renders into: its mapped memory, which the
 * CPU and the GPU share, and for a swapchain image the VideoOut handle and
 * buffer whose wait packet starts the submission (video is -1 otherwise). */
struct ps5vk_render_target {
   void *address;
   size_t bytes;
   int video;
   uint32_t buffer_index;
};

/* One side of a copy or a blit: where its texels are (ps5vk_image.c). A
 * row-layout side is linear with its level's padded row pitch; a tiled side
 * walks the map of its element size, whose block grid the level's width
 * strides by. */
struct ps5vk_image_copy_side {
   uint64_t address;
   uint64_t row_pitch;
   uint32_t level_width;
   /* A packed mip tail combines its origin with the texel swizzle by XOR. */
   uint32_t tile_xor;
   /* The tile the map walks: 128x128 texels of a four-byte element, 256x128 of
    * a two-byte one, and 64x64 of the sixteen-byte texel a four-sample image
    * has (ps5vk_tile_extent). */
   uint32_t tile_width;
   uint32_t tile_height;
   /* The element's own size in bytes: two or four, which selects the map. */
   uint32_t element_bytes;
   /* One or four: a four-sample side's texel is four samples in four 0x4000-byte
    * planes inside the tile, which ps5vk_image_copy_address's sample names
    * (Phase C8). */
   uint32_t samples;
   /* Whether the side is a depth image, whose tile map differs from a colour
    * one's: ps5vk_image_copy_address walks ps5vk_tiled_depth_offset for it
    * (Phase C5's readbacks proved that map on the console). */
   bool depth;
   bool tiled;
};

/* What a recorded event action does when the queue reaches its split
 * (ps5vk_sync.c records them, ps5vk_queue.c executes them). */
enum ps5vk_event_action {
   PS5VK_EVENT_ACTION_NONE = 0,
   PS5VK_EVENT_ACTION_SET,
   PS5VK_EVENT_ACTION_RESET,
   PS5VK_EVENT_ACTION_WAIT,
};

/* One split of a command buffer: a CPU copy it recorded, a scaled blit it has
 * to resample, or -- with no bytes and no addresses -- a synchronization split
 * with nothing to copy (ps5vk_cmd_buffer_split). after_words is where the split
 * falls in that buffer's words: the GPU has to finish everything recorded
 * before it before the CPU work, and everything recorded after it has to see
 * what that work wrote (ps5vk_queue.c). */
struct ps5vk_memory_copy {
   uint64_t source;
   uint64_t destination;
   uint64_t bytes;
   uint32_t after_words;
   /* A fill (vkCmdFillBuffer, Phase C2): the queue writes fill_value over
    * destination .. destination + bytes instead of copying, and source is then
    * unused. */
   bool fill;
   uint32_t fill_value;
   /* A copy whose texels are four-byte and whose bytes must be swapped as they
    * move: an upload into, or a readback out of, an image whose format's storage
    * is reversed (ps5vk_format.storage_reversed). Zero means no swap, which is
    * every other record; a nonzero value is the texel's size and the four bytes
    * are reversed one texel at a time. */
   uint32_t reverse_texel_bytes;
   /* A clear (vkCmdClearColorImage and vkCmdClearDepthStencilImage): the queue
    * writes clear_texel over the region of destination_side that
    * destination_x/y and width/height name, through the side's own texel map. */
   bool clear;
   uint8_t clear_texel[16];
   /* A copy whose source this process owns: vkCmdUpdateBuffer snapshots the
    * application's data, and the command buffer frees it when it is reset or
    * destroyed, because the application may free its own copy the moment the
    * call returns. */
   void *owned_source;
   /* A scaled blit (vkCmdBlitImage with rectangles of different sizes): the
    * queue resamples the region texel by texel where a copy is one memcpy per
    * run (ps5vk_queue.c, ps5vk_blit_execute). */
   bool blit;
   bool linear;
   /* A resolve (vkCmdResolveImage): the queue averages the four sample words of
    * each source texel into the destination's one (ps5vk_resolve_execute). */
   bool resolve;
   /* An upload into tiled storage (vkCmdCopyBufferToImage into an image whose
    * texels are placed by the measured map, C7): the queue walks the region's
    * texels, reading source_pitch apart and writing each at
    * ps5vk_image_copy_address of destination_side, because a tiled row is not a
    * run of bytes. width and height are the region's, destination_x and
    * destination_y its first texel, and destination_texel_bytes the texel's. */
   bool image_write;
   /* An image copy (vkCmdCopyImage, a one-to-one vkCmdBlitImage, or a readback
    * into a buffer): the queue walks the region's runs through both sides'
    * maps at the split point (ps5vk_image_copy_execute). The two sides carry
    * the placement, source_x and source_y the region's first source texel,
    * destination_x and destination_y its first destination texel, width and
    * height its size, and source_texel_bytes the element the runs step by. One
    * record holds the whole region: the recording used to expand it into one
    * record a run, which a 4K four-sample depth copy (one run a texel) does not
    * have the memory for. */
   bool image_copy;
   uint64_t source_pitch;
   /* A query-result copy (vkCmdCopyQueryPoolResults): the queue writes one
    * result per query, query_count of them from query_first, into destination
    * with query_stride bytes between them, as query_flags ask
    * (ps5vk_query_execute). */
   bool query;
   const struct ps5vk_query_pool *query_pool;
   uint32_t query_first;
   uint32_t query_count;
   uint64_t query_stride;
   uint32_t query_flags;
   struct ps5vk_image_copy_side source_side;
   struct ps5vk_image_copy_side destination_side;
   /* The source rectangle's first texel, where the destination rectangle
    * starts, its size, and the source texels one destination texel covers per
    * axis (negative in a mirrored direction). */
   int32_t source_x;
   int32_t source_y;
   uint32_t destination_x;
   uint32_t destination_y;
   uint32_t width;
   uint32_t height;
   /* The two sides' texel sizes and formats: a nearest blit between them is a
    * decode of the sampler's fetch into the destination's four bytes, and a
    * filtered one needs the same four-byte format on both sides
    * (ps5vk_image.c, ps5vk_blit_execute). */
   uint32_t source_texel_bytes;
   uint32_t destination_texel_bytes;
   VkFormat source_format;
   VkFormat destination_format;
   float source_step_x;
   float source_step_y;
   /* An event action (vkCmdSetEvent and neighbours, Phase B5): the queue sets,
    * resets or waits for this event's flag at this split point, which is what
    * puts the event's place in a submission's order into the split order. A
    * record with an action carries nothing else. */
   struct ps5vk_event *event;
   uint8_t event_action;
};

/* One region of vkCmdCopyImage or vkCmdBlitImage, in the shape both entry
 * points share: one level and one layer of each image, the texel the region
 * starts at on each side, and its extent (ps5vk_image.c, Phase C7). */
struct ps5vk_image_copy {
   VkImageSubresourceLayers source;
   VkImageSubresourceLayers destination;
   VkOffset3D source_offset;
   VkOffset3D destination_offset;
   VkExtent3D extent;
   /* A blit's source rectangle -- source_offset to source_end, which the
    * destination extent above scales onto -- and the two images' formats and
    * texel sizes. A mirrored blit's source_end is below its source_offset,
    * which the transform's sign keeps. */
   VkOffset3D source_end;
   VkFormat source_format;
   VkFormat destination_format;
   uint32_t source_texel_bytes;
   uint32_t destination_texel_bytes;
};

/* A vertex buffer bound for the next draws (vkCmdBindVertexBuffers2); a
 * binding that was never bound has size 0. */
struct ps5vk_vertex_buffer {
   uint64_t address;
   uint64_t size;
};

/* The descriptor sets this driver binds: **four**, which is the number it
 * advertises (VkPhysicalDeviceLimits.maxBoundDescriptorSets,
 * ps5vk_physical_device.c), and it advertises exactly what it binds.
 *
 * Three caps exist and they are a deliberate split, not a disagreement to be
 * reconciled: the compiler wrapper caps a *program* at eight sets
 * (PSBC_MAX_DESCRIPTOR_SETS, tooling/psbc/patch-descriptor-sets.py), the
 * compiler core at thirty-two (MAX_SETS, radv_constants.h), and the driver at
 * the specification's floor of four. R7 made the mechanism general on the
 * compiler side while the driver stayed at that floor, so an application that
 * one day needs more than four sets is a driver change -- widen this constant
 * and the tables it sizes -- and not another compiler project. Do not "fix" the
 * difference by narrowing the wrapper or by widening this past what is proved:
 * more than four is refused by name (ps5vk_CmdBindDescriptorSets,
 * ps5vk_draw.c). */
#define PS5VK_DESCRIPTOR_SET_COUNT 4

struct ps5vk_descriptor_set;

enum ps5vk_pipeline_stage {
   PS5VK_PIPELINE_STAGE_VERTEX,
   PS5VK_PIPELINE_STAGE_PIXEL,
   PS5VK_PIPELINE_STAGE_COUNT,
};

/* A command buffer (ps5vk_cmd_buffer.c, ps5vk_draw.c). */
/* One chunk of register tables: room for about 200 draws' tables
 * (ps5vk_cmd_buffer.c). */
#define PS5VK_TABLE_CHUNK_BYTES UINT64_C(0x40000)

/* One GPU-visible chunk of a command buffer's register tables. It is on two
 * lists: its command buffer's, and the device's, which the console test
 * runner reads for its capture (ps5vk_debug.h, ps5vk_debug_table_chunks). */
struct ps5vk_table_chunk {
   struct ps5vk_direct_mapping mapping;
   struct ps5vk_table_chunk *next_in_buffer;
   struct ps5vk_table_chunk *next_in_device;
};

#define PS5VK_DYNAMIC_UNIFORM_COUNT 8

struct ps5vk_cmd_buffer {
   struct vk_command_buffer vk;
   /* The PM4 words recorded into it, which submission copies into the
   * queue's buffer. */
   struct util_dynarray words;
   /* GPU-visible chunks holding the register tables the words point at, the
    * chunk in use and the bytes used in it. */
   struct ps5vk_table_chunk *table_chunks;
   struct ps5vk_table_chunk *table_chunk;
   size_t table_bytes_used;
   /* The bound graphics pipeline, or NULL. */
   struct ps5vk_pipeline *pipeline;
   /* The descriptor sets bound for the next draws, by set index
    * (vkCmdBindDescriptorSets; ps5vk_descriptor_set.c), and the dynamic offset
    * each was bound with (VkBindDescriptorSetsInfo.pDynamicOffsets, D1). */
   struct ps5vk_descriptor_set *descriptor_sets[PS5VK_DESCRIPTOR_SET_COUNT];
   uint32_t descriptor_set_offsets[PS5VK_DESCRIPTOR_SET_COUNT][PS5VK_DYNAMIC_UNIFORM_COUNT];
   /* The compute pipeline vkCmdBindPipeline bound for the next dispatches
    * (Phase D2). */
   struct ps5vk_pipeline *compute_pipeline;
   /* Every colour target rendered to (struct ps5vk_render_target): submission
    * evicts their CPU cache lines and starts with the wait packets of
    * swapchain buffers (ps5vk_queue.c). */
   struct util_dynarray targets;
   /* The CPU copies and synchronization splits recorded into it, in record
    * order: the queue submits the words before each of them and waits for
    * those to have run before the words after it do (ps5vk_queue.c). A record
    * with no bytes is a split with no copy: a draw that samples a target an
    * earlier draw in the same command buffer rendered into (ps5vk_draw.c). */
   struct util_dynarray copies;
   /* Whether a rendering is active, its target's extent and CB_COLOR0
    * registers. */
   bool rendering;
   VkExtent2D target_extent;
   /* One row per colour attachment the rendering declares, in attachment order:
    * the draw copies as many rows into its context stream as the rendering has
    * attachments (ps5vk_draw.c). */
   struct ps5vk_agc_register target_registers[PS5VK_MAX_COLOR_TARGETS]
                                           [PS5VK_TARGET_REGISTER_COUNT];
   /* How many of those rows the last begin-rendering filled: the rendering's
    * colour attachment count, or 1 when it has none and every draw still
    * programs AGC's defaults (ps5vk_draw.c). The draw's context stream reserves
    * and copies this many rows. */
   uint32_t colour_attachment_count;
   /* The rasterizer registers a four-sample target needs, and how many of them
    * this rendering has: zero for a one-sample one (Phase C8). */
   uint32_t multisample_count;
   struct ps5vk_agc_register multisample_registers[PS5VK_MULTISAMPLE_REGISTER_COUNT];
   /* The rendering's depth attachment, when it has one: the DB registers every
    * draw's table then carries, after the colour target's (Phase C5). */
   bool depth_bound;

   /* Whether that attachment carries a stencil plane. Vulkan ignores the
    * stencil test when the rendering has no stencil attachment, so a draw
    * programs the stencil state words and DB_DEPTH_CONTROL's stencil bits only
    * when this is set (round 12). */
   bool stencil_bound;
   /* The depth attachment's format, which decides the words a depth-biased
    * pipeline's draw records: only the D32 float depth word is measured, so a
    * bias through another depth format is refused by name at the draw
    * (ps5vk_draw.c, R1). VK_FORMAT_UNDEFINED when no depth attachment is
    * bound, where Vulkan makes the bias inert. */
   VkFormat depth_format;
   struct ps5vk_agc_register depth_registers[PS5VK_DEPTH_REGISTER_COUNT];
   /* The rendering's attachment, for the clears vk_meta draws into it. */
   struct vk_meta_rendering_info render;
   /* Vertex buffers bound for the next draws. */
   struct ps5vk_vertex_buffer vertex_buffers[PS5VK_MAX_VERTEX_BINDINGS];
   /* The push-constant bytes recorded so far; a draw whose pipeline reads
    * push constants copies them into a GPU-visible buffer. */
   uint8_t push_constants[PS5VK_MAX_PUSH_CONSTANT_BYTES];
   /* The index buffer bound for the next indexed draws, its type, the bytes
    * its binding covers. Each indexed draw writes its INDEX_TYPE register
    * (ps5vk_draw.c). The size is what V0-robust clamps a draw's index
    * count to, so a count past the bound fetches no index outside it. */
   struct {
      uint64_t address;
      uint64_t size;
      VkIndexType type;
   } index_buffer;
};

/* A command-buffer state a meta operation replaces, kept across it: Mesa's
 * meta code binds its own pipeline, vertex buffer, descriptor sets, push
 * constants and dynamic state, and the application's must survive
 * (ps5vk_draw.c). */
struct ps5vk_meta_saved_state {
   struct ps5vk_pipeline *pipeline;
   struct ps5vk_vertex_buffer vertex_buffers[PS5VK_MAX_VERTEX_BINDINGS];
   struct ps5vk_descriptor_set *descriptor_sets[PS5VK_DESCRIPTOR_SET_COUNT];
   uint32_t descriptor_set_offsets[PS5VK_DESCRIPTOR_SET_COUNT][PS5VK_DYNAMIC_UNIFORM_COUNT];
   uint8_t push_constants[PS5VK_MAX_PUSH_CONSTANT_BYTES];
   struct vk_dynamic_graphics_state dynamic;
};

extern const struct vk_command_buffer_ops ps5vk_cmd_buffer_ops;

/* Space for bytes of register tables in cmd_buffer's GPU-visible chunks, or
 * NULL after recording VK_ERROR_OUT_OF_DEVICE_MEMORY. The result is aligned
 * to alignment, which descriptors need: a descriptor names a 256-byte aligned
 * address (ps5vk_buffer.c). */
void *
ps5vk_cmd_buffer_table(struct ps5vk_cmd_buffer *cmd_buffer, size_t bytes, size_t alignment);

/* Records a synchronization split with no copy at this point in cmd_buffer's
 * words: the queue submits the words recorded before it, waits for that step to
 * complete, and only then runs the words recorded after it (ps5vk_queue.c).
 * False, and the command buffer records VK_ERROR_OUT_OF_HOST_MEMORY, when there
 * is no memory for the record. */
bool
ps5vk_cmd_buffer_split(struct ps5vk_cmd_buffer *cmd_buffer);

/* Records that cmd_buffer cannot encode a command: logs why, and recording
 * ends with result at vkEndCommandBuffer. */
void PRINTFLIKE(6, 7)
ps5vk_cmd_buffer_error(struct ps5vk_cmd_buffer *cmd_buffer, VkResult result,
                       const char *command, const char *file, int line, const char *format, ...);
#define ps5vk_cmd_buffer_refuse(cmd_buffer, result, ...)                                         \
   ps5vk_cmd_buffer_error(cmd_buffer, result, __func__, __FILE__, __LINE__, __VA_ARGS__)

struct ps5vk_device {
   struct vk_device vk;
   struct vk_device_dispatch_table command_dispatch;
   /* The one queue of the one queue family, when the application requested it. */
   struct ps5vk_queue queue;
   bool queue_initialized;
   /* Live VkDeviceMemory objects, at most PS5VK_MAX_MEMORY_ALLOCATIONS. */
   int32_t memory_allocation_count;
   /* VideoOut, while a swapchain owns it (ps5vk_wsi.c). */
   struct ps5vk_video_out *video_out;
   /* Mesa's shared meta operations, which draw the colour clears
    * (ps5vk_draw.c); initialised with the device. */
   struct vk_meta_device meta;
   bool meta_initialized;
   /* The 16-byte block the last draw's push constants were copied into, and its
    * size in bytes: what vkCmdPushConstants reaches a stage through, kept for
    * the debug API so a probe can assert the upload rather than infer it from
    * pixels (R9, ps5vk_debug.h). Zero before the first draw. */
   void *push_constant_block;
   uint32_t push_constant_bytes;
   /* The 16-byte descriptor entry the reserved set-0 binding got in the last
    * draw's table -- its address, the stride word and all -- so a probe can read
    * the whole chain back (R9's answer needed exactly that). */
   const uint32_t *push_constant_descriptor;
   /* Where the last draw programmed the push-constant pointer and what it wrote
    * there, for the debug API: UINT32_MAX when the stage read none. */
   uint32_t push_constant_user_data_dword;
   uint32_t push_constant_user_data_stage;
   uint32_t push_constant_user_data_low;
   uint32_t push_constant_user_data_high;
   /* The tables the last draw built, one entry per set each stage read, with
    * the dword each pointer was written to (R7, ps5vk_debug_descriptor_tables).
    * The count is reset at the start of every draw, so a probe reads the frame's
    * last one. */
   uint32_t descriptor_table_count;
   ps5vk_debug_table descriptor_tables[PS5VK_PIPELINE_STAGE_COUNT * PS5VK_DESCRIPTOR_SET_COUNT];
   /* The last begin-rendering's colour attachments, for the debug API: how many
    * it declared and the CB_COLORi_BASE address word each one's row carries
    * (R7 step 1b, ps5vk_debug_colour_targets). */
   uint32_t target_attachment_count;
   uint32_t target_base_offsets[PS5VK_MAX_COLOR_TARGETS];
   uint32_t target_base_values[PS5VK_MAX_COLOR_TARGETS];
   /* The pipelines whose stage mapping exists, newest first: what
    * ps5vk_debug_pipeline_stages reports to the runner's capture. */
   struct ps5vk_pipeline *stages;
   /* The command buffers' table chunks, newest first: where the register
    * tables a submission names live (ps5vk_debug_table_chunks). */
   struct ps5vk_table_chunk *table_chunks;
   /* The live buffers, newest first: the addresses a submission names, which
    * a capture has to carry so a PC rebuild can pin them
    * (ps5vk_debug_buffers). */
   struct ps5vk_buffer *buffers;
};

/* One direct-memory allocation, mapped for the CPU and the GPU while it lives. */
struct ps5vk_device_memory {
   struct vk_device_memory vk;
   /* vk.size rounded up to the direct-memory page. */
   struct ps5vk_direct_mapping direct;
};

/* A range of one allocation; vk.device_address is its GPU address once bound. */
struct ps5vk_buffer {
   struct vk_buffer vk;
   /* The memory the buffer is bound to, or NULL before it is bound. */
   struct ps5vk_device_memory *memory;
   /* The device's live buffers, newest first, for the runner's capture
    * (ps5vk_debug.h, ps5vk_debug_buffers). */
   struct ps5vk_buffer *next_in_device;
};

/* A view of a buffer's bytes as texels (vkCreateBufferView, ps5vk_buffer.c):
 * the buffer, the format its bytes are read as, and the range the view names.
 * Nothing samples one yet -- that is a uniform or storage texel buffer, Phase
 * D2, and the descriptor-set layout refuses those bindings by name -- but the
 * command has its path, and the format a view may name is the same reporting
 * rule the descriptor path follows: a format carrying a texel-buffer feature. */
struct ps5vk_buffer_view {
   struct vk_object_base base;
   struct ps5vk_buffer *buffer;
   VkFormat format;
   uint64_t offset;
   uint64_t range;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_buffer_view, base, VkBufferView, VK_OBJECT_TYPE_BUFFER_VIEW)

/* How an image's texels are stored (ps5vk_image.c). */
enum ps5vk_image_storage {
   /* Each level row-major, rows padded to 256 bytes: images that are not
    * attachments. */
   PS5VK_IMAGE_STORAGE_ROWS,
   /* Each level in 64 KiB tiles, rounded up to 2 MiB: attachments. */
   PS5VK_IMAGE_STORAGE_TILES,
};

struct ps5vk_image {
   struct vk_image vk;
   enum ps5vk_image_storage storage;
   /* The memory requirements: storage size and bind alignment. */
   uint64_t size;
   uint64_t alignment;
   /* The memory the image is bound to and its GPU address, once bound; a
    * swapchain image has an address from its creation and no memory. */
   struct ps5vk_device_memory *memory;
   uint64_t address;
   /* Where a stencil-bearing depth image's stencil plane starts, from the
    * image's address: the one-byte plane AddrLib computes beside the depth
    * surface in the same 64 KiB Z_X swizzle, at the base alignment it reports
    * (ps5vk_image_stencil_plane). Zero for every format without a stencil,
    * which is what tells a draw to program DB_STENCIL_INFO disabled. */
   uint64_t stencil_offset;
   /* A swapchain image's VideoOut handle and buffer index; video is -1 for
    * every other image. */
   int video;
   uint32_t buffer_index;
};

/* A sampler: the one state the M3 texture canary ran (ps5vk_image.c). word is
 * word 10 of the 48-byte combined image-sampler descriptor a draw writes
 * (ps5vk_draw.c), the only descriptor word the sampler decides. */
struct ps5vk_sampler {
   struct vk_object_base base;
   /* Word 10 of the combined image-sampler descriptor: the filter pair and the
    * mip filter, which vkCreateSampler encodes (ps5vk_image.c). */
   uint32_t word;
   /* Word 9: the LOD range minLod to maxLod reaches. The single-level path
    * keeps the canary's own word (ps5vk_draw.c, ps5vk_write_image_descriptor);
    * a view with more than one level writes this one. */
   uint32_t lod_word;
   /* Word 8: the three 3-bit address modes, U in bits 0-2, V in 3-5 and W in
    * 6-8, which vkCreateSampler encodes from addressModeU/V/W (R2,
    * ps5vk_image.c). The value the M3 texture canary's descriptor carried is
    * clamp-to-edge on all three axes. */
   uint32_t address_word;
};

/* SPIR-V's header, OpMemoryModel, OpCapability, OpEntryPoint, OpExecutionMode
 * and the execution models, which the capability refusal, the entry-point and
 * the local-size checks read (ps5vk_pipeline.c, ps5vk_compute.c). */
#define PS5VK_SPIRV_MAGIC 0x07230203u
#define PS5VK_SPIRV_HEADER_WORDS 5
#define PS5VK_SPIRV_OP_MEMORY_MODEL 14
#define PS5VK_SPIRV_OP_ENTRY_POINT 15
#define PS5VK_SPIRV_OP_CAPABILITY 17
#define PS5VK_SPIRV_OP_DECORATE 71
/* OpDecorate's own decorations, for the input-attachment scan. */
#define PS5VK_SPIRV_DECORATION_BINDING 33
#define PS5VK_SPIRV_DECORATION_DESCRIPTOR_SET 34
#define PS5VK_SPIRV_DECORATION_INPUT_ATTACHMENT_INDEX 43
/* The input attachments one pipeline stage may read: Vulkan's own per-stage
 * limit is 4 in the core profile this device reports
 * (VkPhysicalDeviceLimits::maxPerStageDescriptorInputAttachments), and the
 * table is sized from the advertised number rather than from it. */
#define PS5VK_MAX_INPUT_ATTACHMENTS 8
/* Room for the bindings one shader declares: PSBC_MAX_DESCRIPTOR_BINDINGS is the
 * compiler's own cap and no probe here declares more than a handful. */
#define PS5VK_MAX_SPIRV_BINDINGS 32
#define PS5VK_SPIRV_OP_EXECUTION_MODE 16
#define PS5VK_SPIRV_EXECUTION_MODE_LOCAL_SIZE 17
#define PS5VK_SPIRV_EXECUTION_MODEL_VERTEX 0
#define PS5VK_SPIRV_EXECUTION_MODEL_FRAGMENT 4
#define PS5VK_SPIRV_EXECUTION_MODEL_COMPUTE 5

/* Room for a shader's declared capability list in a refusal's sentence: the
 * most any deployed shader declares is four, and each name and number is under
 * forty bytes (ps5vk_pipeline.c, R10). */
#define PS5VK_CAPABILITY_LIST_BYTES 256

/* The compiler and AGC's shader creation are serialised by one mutex, which the
 * graphics and compute paths share (ps5vk_pipeline.c, ps5vk_compute.c). */
extern once_flag ps5vk_compile_once;
extern mtx_t ps5vk_compile_mutex;
void
ps5vk_compile_mutex_init(void);

/* One compile, run on a thread of this repository's own with a stack the
 * application's thread size cannot shrink (ps5vk_pipeline.c). aborted, when it
 * is not NULL, comes back true when the compiler raised instead of returning --
 * a shader it cannot lower (R10). */
PsbcResult
ps5vk_compile_shader_deep(struct nir_shader *nir, const uint32_t *words, size_t size,
                          const PsbcCompileOptions *options, PsbcShaderOutput *output,
                          bool *aborted);

#define PS5VK_MAX_USER_DATA 16
bool
ps5vk_cmd_buffer_shader_resources(struct ps5vk_cmd_buffer *cmd_buffer,
                                  const struct ps5vk_pipeline *pipeline,
                                  const PsbcShaderMetadata *const *metadata,
                                  const VkShaderStageFlags *stage_bits, uint32_t stage_count,
                                  uint32_t user_data[][PS5VK_MAX_USER_DATA], bool *colour_barrier);

/* A pipeline's descriptor bindings across sets for one stage bit, into the
 * compiler options (ps5vk_pipeline.c); a compute pipeline calls it with
 * VK_SHADER_STAGE_COMPUTE_BIT. */
VkResult
ps5vk_descriptor_options(struct ps5vk_device *device, const struct vk_pipeline_layout *layout,
                         VkShaderStageFlags stage_bit, PsbcCompileOptions *options);

/* R9: a stage's VkSpecializationInfo into the compiler options, which carry it to
 * spirv_to_nir (tooling/psbc/patch-specialization.py); a NULL or empty one
 * specialises nothing. Valid usage is checked here and refused by name, because
 * an entry outside the data would be read past its end (ps5vk_pipeline.c). The
 * options point at the application's arrays, which outlive the compile. */
VkResult
ps5vk_specialization_options(struct ps5vk_device *device, const VkSpecializationInfo *info,
                             const char *stage_name, PsbcCompileOptions *options);

/* Whether the command buffer has recorded a write to the buffer range: the
 * indirect draw and dispatch read their parameters when they are recorded, so
 * one whose parameters the same command buffer writes is refused
 * (ps5vk_draw.c, ps5vk_compute.c). */
bool
ps5vk_cmd_buffer_writes_range(const struct ps5vk_cmd_buffer *cmd_buffer, uint64_t address,
                              uint64_t bytes);

/* Releases a pipeline of either bind point: its compiled stages, its shader
 * stage mapping and a compute pipeline's own code mapping (ps5vk_pipeline.c). */
void
ps5vk_pipeline_free(struct ps5vk_device *device, struct ps5vk_pipeline *pipeline,
                    const VkAllocationCallbacks *allocator);

/* A shader module: the SPIR-V words, compiled when a pipeline uses them. */
struct ps5vk_shader_module {
   struct vk_object_base base;
   size_t size;
   uint32_t words[];
};

/* Whether a module is a well-formed SPIR-V instruction stream in host byte order
 * that declares an entry point named name for the execution model
 * (ps5vk_pipeline.c; the compute path checks its own stage with it). */
bool
ps5vk_spirv_has_entry_point(const struct ps5vk_shader_module *module, uint32_t model,
                            const char *name);

/* R10: the bindings a stage reads as input attachments, from the module's own
 * InputAttachmentIndex, DescriptorSet and Binding decorations
 * (ps5vk_pipeline.c). The draw fills them from the subpass's input attachment
 * rather than from a descriptor write, which Vulkan does not allow for this
 * type. */
struct ps5vk_input_attachment
{
   uint8_t set;
   uint8_t binding;
   uint8_t stage;
   uint32_t index;
};
uint32_t
ps5vk_spirv_input_attachments(const uint32_t *words, size_t size, uint8_t stage,
                              struct ps5vk_input_attachment *out, uint32_t capacity);

/* The (set, binding) pairs a module declares, from its own decorations: what a
 * stage may legitimately build a table entry for. */
struct ps5vk_spirv_binding
{
   uint8_t set;
   uint8_t binding;
};
uint32_t
ps5vk_spirv_bindings(const uint32_t *words, size_t size, struct ps5vk_spirv_binding *out,
                     uint32_t capacity);

/* R10: what this compiler has no path for, which a shader is refused for before
 * the compiler runs (ps5vk_pipeline.c). The graphics and compute paths both
 * call ps5vk_spirv_refusal and write its reason into their refusal;
 * ps5vk_spirv_capability_list fills in the declared set for a refusal that
 * cannot name the one thing at fault. */
bool
ps5vk_spirv_refusal(const uint32_t *words, size_t size, char *reason, size_t reason_size);
const char *
ps5vk_capability_name(uint32_t value);
void
ps5vk_spirv_capability_list(const uint32_t *words, size_t size, char *out, size_t out_size);

/* A query pool: one direct-memory mapping the GPU writes counters into and the
 * CPU reads them from, two eight-byte counters per query. An occlusion query
 * samples the z-pass counter at vkCmdBeginQuery and vkCmdEndQuery and
 * vkGetQueryPoolResults subtracts and scales them; a timestamp query has one
 * counter, where vkCmdWriteTimestamp has the GPU write its clock
 * (ps5vk_query.c). */
struct ps5vk_query_pool {
   struct vk_object_base base;
   VkQueryType type;
   uint32_t count;
   struct ps5vk_direct_mapping mapping;
};

/* Descriptor-set table entry sizes the shader compiler reads, as the probe
 * shaders compiled and the hardware ran them (tools/build-probe-shaders.sh). */
#define PS5VK_UNIFORM_BUFFER_DESCRIPTOR_BYTES 16
/* A texel buffer's descriptor is a buffer descriptor too, with the view's
 * format in word 3 (driver/ps5vk_draw.c, PS5VK_TEXEL_BUFFER_FORMAT). */
#define PS5VK_TEXEL_BUFFER_DESCRIPTOR_BYTES 16
/* A storage image's entry is a 32-byte image descriptor and no sampler, where a
 * combined image sampler's is that same descriptor with a 16-byte sampler after
 * it (ps5-opengl-sdk-0.3.0's fork validates the same strides). */
#define PS5VK_STORAGE_IMAGE_DESCRIPTOR_BYTES 32
/* A storage buffer's table entry (Phase D2): the size the compiler takes for
 * one, the stride the V0-compute probe declared binding 0 with
 * (src/diagnostics.cpp, kStorageBufferStride). */
#define PS5VK_STORAGE_BUFFER_DESCRIPTOR_BYTES 16
#define PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES 48

/* One binding of a descriptor set layout and its place in the set's table. */
struct ps5vk_descriptor_binding {
   VkDescriptorType type;
   uint32_t count;
   VkShaderStageFlags stages;
   /* Table offset and entry size in bytes; stride 0 marks a type no pipeline
    * can use yet. */
   uint32_t offset;
   uint32_t stride;
   uint32_t dynamic_index;
   uint32_t record_index;
};

/* A descriptor set layout: bindings indexed by binding number. */
struct ps5vk_descriptor_set_layout {
   struct vk_descriptor_set_layout vk;
   uint32_t binding_count;
   uint32_t table_bytes;
   uint32_t descriptor_count;
   struct ps5vk_descriptor_binding bindings[];
};

/* What vkUpdateDescriptorSets recorded for one binding of a set: the GPU
 * address the descriptor names, the bytes of the buffer the binding covers,
 * and the type it was written with. The type stays
 * VK_DESCRIPTOR_TYPE_MAX_ENUM while no write names the binding, which is what
 * a draw refuses. The descriptor's own words are the draw's to build: the
 * entry size, element count and flags come from the compiler's metadata
 * (ps5vk_draw.c). A combined image sampler write records the view and sampler
 * it names in place of the address and size, and the draw builds its 48 bytes
 * from the image the view names and the sampler's word. */
/* The descriptor word 3's DST_SEL channel selectors, three bits each from bit
 * 0 (SQ_SEL_0 is 0, SQ_SEL_1 is 1, the components 4 to 7): one per number of
 * channels a format has. The hardware aliases the components a format does not
 * have rather than returning zero and one for them, so a sampled format's
 * selector is what supplies Vulkan's fill-in rule, not the FORMAT field
 * (HARDWARE_FINDINGS.md, the format probe's first run). */
#define PS5VK_FORMAT_SWIZZLE_R001 UINT32_C(0x0204)
#define PS5VK_FORMAT_SWIZZLE_RG01 UINT32_C(0x022c)
#define PS5VK_FORMAT_SWIZZLE_RGB1 UINT32_C(0x03ac)
#define PS5VK_FORMAT_SWIZZLE_RGBA UINT32_C(0x0fac)
/* The same four channels in the opposite memory order (A8B8G8R8): the fetch's
 * X is the memory's alpha, so the selectors are W, Z, Y, X. */
#define PS5VK_FORMAT_SWIZZLE_WZYX UINT32_C(0x0977)
/* The packed formats' channel selectors (V0-formats): the same three bits a
 * channel, X = 4, Y = 5, Z = 6, W = 7, 0.0 = 0 and 1.0 = 2, read off the
 * hardware format's memory order and the Vulkan format's own. */
#define PS5VK_FORMAT_SWIZZLE_ZYX1 UINT32_C(0x052e)
#define PS5VK_FORMAT_SWIZZLE_ZYXW UINT32_C(0x0f2e)
#define PS5VK_FORMAT_SWIZZLE_YZWX UINT32_C(0x09f5)
#define PS5VK_FORMAT_SWIZZLE_XYZ1 UINT32_C(0x05ac)

/* One entry of the format table (ps5vk_image.c): the format, the features the
 * probes proved for it, the GFX10 IMG_DATA_FORMAT the combined image sampler
 * descriptor's FORMAT field carries and the DST_SEL word its swizzle field
 * carries. 0 for the format means no recorded value, and a sampled image of
 * that format is refused (V0-formats). */
struct ps5vk_format {
   VkFormat format;
   VkFormatFeatureFlags optimal_features;
   VkFormatFeatureFlags buffer_features;
   uint32_t image_format;
   uint32_t dst_sel;
   /* Whether the image's memory holds this format's texels with their four bytes
    * in the reverse of Vulkan's order (round 17). The console applies the sRGB
    * curve to the first three *fetched* components, so a format whose red is the
    * fourth byte of its texel (`A8B8G8R8_SRGB_PACK32`, whose layout is A, B, G,
    * R) could never have its red linearised if the texels were stored as Vulkan
    * describes them. Storing them the way its `R8G8B8A8_SRGB` twin is stored
    * makes the fetch's first three components the three colour channels, and the
    * driver swaps the four bytes at every boundary where an application's bytes
    * meet the image's: the uploads, the readbacks and the two blit directions.
    * The image's layout is the driver's own (every image is created with
    * VK_IMAGE_TILING_OPTIMAL), so nothing else observes the order. False for
    * every other format, which is every entry that stops before this field. */
   bool storage_reversed;
};

const struct ps5vk_format *
ps5vk_find_format(VkFormat format);

/* A colour target's CB_COLOR0_INFO encoding: the register database's
 * V_028C70_COLOR_* data format, V_028C70_NUMBER_* number type and
 * V_028C70_SWAP_* component order, which ps5-opengl's sceGnmCreateRenderTarget
 * writes from the same source (its sceGnmDfGetRtChannelType/Order) and Mesa's
 * ac_get_cb_format/ac_translate_colorswap pick for the same formats
 * (driver/ps5vk_image.c). A format with no entry cannot be rendered into. */
struct ps5vk_colour_format {
   VkFormat format;
   uint32_t cb_format;
   uint32_t cb_number_type;
   uint32_t cb_comp_swap;
   /* The SPI_SHADER_COL_FORMAT nibble every pipeline drawing into this format
    * compiles its pixel stage for, or 0 for the ones whose export the driver
    * still leaves to the compiler's default unless the attachment blends
    * (the normalized and floating classes, ps5vk_pipeline.c). The integer
    * targets need theirs always: Mesa exports them as UINT16_ABGR or
    * SINT16_ABGR whatever the blend state. */
   uint32_t export_format;
};

const struct ps5vk_colour_format *
ps5vk_find_colour_format(VkFormat format);

/* One recorded query-result copy, run at its split point: the results the pool's
 * counters hold, written into the destination buffer (ps5vk_query.c). */
void
ps5vk_query_execute(const struct ps5vk_memory_copy *copy);

/* Where texel (x, y) of one side of a copy or a blit lives: its address plus the
 * row layout's pitch or the tiled map's offset (ps5vk_image.c). The queue's
 * resampler walks the same maps a copy does. */
uint64_t
ps5vk_image_copy_address(const struct ps5vk_image_copy_side *side, int32_t x, int32_t y,
                         uint32_t texel_bytes, uint32_t sample);

/* Where one level of a tiled image starts, from the measured chain table, or
 * UINT64_MAX for a shape the oracle has not been run for (ps5vk_image.c, C7). */
uint64_t
ps5vk_image_tiled_level_base(const struct ps5vk_image *image, uint32_t level);

/* One layer's bytes for an image whose per-layer shape the measured chain table
 * covers, or 0 for a shape it does not: a slice is a chain of its own and the
 * layers are consecutive (docs/HARDWARE_FINDINGS.md, D1). */
uint64_t
ps5vk_image_layer_bytes(const struct ps5vk_image *image);

/* Where one level of one layer of a tiled image starts, or UINT64_MAX for a
 * shape no measurement covers. */
uint64_t
ps5vk_image_tiled_layer_level_base(const struct ps5vk_image *image, uint32_t level,
                                   uint32_t layer);

/* One recorded upload into tiled storage, run at its split point: the queue
 * walks the region's texels (ps5vk_queue.c). */
void
ps5vk_image_write_execute(const struct ps5vk_memory_copy *copy);

/* One recorded image copy, run at its split point: the queue walks the region's
 * runs through both sides' maps, which is the same walk the recording used to
 * do itself -- one record a run, which a four-sample depth image (a sixteen-byte
 * texel, so one run each) needs 8.3 million of for a 4K image, more than the
 * application's allocator has (ps5vk_queue.c, ps5vk_image.c). */
void
ps5vk_image_copy_execute(const struct ps5vk_memory_copy *copy);

/* The bytes a tiled row keeps contiguous: the map's low bits are the texel's
 * low bits inside this span and no more, so a copy runs four four-byte texels,
 * eight two-byte ones, or one sixteen-byte one at a time (ps5vk_image.c). */
#define PS5VK_TILED_RUN_BYTES 16u

/* One source texel decoded to the RGBA8 bytes a UNORM destination takes: the
 * sampler's fetch for that format, converted to eight-bit UNORM. False for a
 * format with no decode, which the blit's recording refuses by name
 * (ps5vk_queue.c). */
bool
ps5vk_texel_to_rgba8(VkFormat format, const uint8_t *texel, uint8_t rgba[4]);

/* The bytes one RGBA8 texel becomes in a blit's destination format, or zero for
 * a format the driver cannot write a blit into. The recording refuses that case
 * by name (ps5vk_queue.c, ps5vk_image.c). */
uint32_t
ps5vk_rgba8_texel_bytes(VkFormat format);

/* The same encode, with the RGBA8 texel's bytes in hand: the inverse of
 * ps5vk_texel_to_rgba8's decode, so a blit into a format and the sampler's
 * fetch of it agree. Returns the bytes written into texel, zero for a format
 * with no encode. */
uint32_t
ps5vk_rgba8_to_texel(VkFormat format, const uint8_t rgba[4], uint8_t texel[16]);

struct ps5vk_descriptor_buffer {
   uint64_t address;
   uint64_t size;
   VkDescriptorType type;
   VkImageView view;
   VkSampler sampler;
   /* A uniform or storage texel buffer's write names a view, not a range: the
    * view's buffer, offset, range and format are what the draw's V# needs
    * (ps5vk_draw.c). */
   VkBufferView buffer_view;
};

/* A descriptor set: one record per array element of the layout it was allocated
 * with, and that layout itself. The layout is reference counted because
 * vkUpdateDescriptorSets and vkCmdBindDescriptorSets take no layout, and the
 * records are indexed by binding.record_index plus array element. */
struct ps5vk_descriptor_set {
   struct vk_object_base base;
   struct ps5vk_descriptor_set_layout *layout;
   /* The pool's live sets, newest first; NULL for the last one. */
   struct ps5vk_descriptor_set *next_in_pool;
   struct ps5vk_descriptor_buffer buffers[];
};

/* One descriptor size a pool was created with, and how many of it no set
 * holds yet: vkAllocateDescriptorSets takes them, and freeing or resetting a
 * set gives them back (ps5vk_descriptor_set.c). */
struct ps5vk_descriptor_pool_size {
   VkDescriptorType type;
   uint32_t remaining;
};

/* A descriptor pool. It allocates its sets from the device and is the only
 * thing that owns them, so vkDestroyDevice frees nothing itself. */
struct ps5vk_descriptor_pool {
   struct vk_object_base base;
   struct ps5vk_descriptor_set *sets;
   /* The live sets, which vkAllocateDescriptorSets caps at max_sets. */
   uint32_t set_count;
   uint32_t max_sets;
   uint32_t size_count;
   struct ps5vk_descriptor_pool_size sizes[];
};

/* What the application wrote into one array element of the set bound at set_index,
 * or NULL when no set is bound there or the binding holds no write
 * (ps5vk_descriptor_set.c). A draw builds its set-0 tables from it. */
const struct ps5vk_descriptor_buffer *
ps5vk_cmd_buffer_descriptor(const struct ps5vk_cmd_buffer *cmd_buffer, uint32_t set_index,
                            uint32_t binding, uint32_t element);

/* A compiled shader stage: the AGC package and the compiler's metadata. */
struct ps5vk_shader_package {
   uint8_t *data;
   size_t size;
   PsbcShaderMetadata metadata;
};

/* The stage workspace of a pipeline's AGC shader objects, as the test runner
 * lays it out (link_shader_packages): both packages' headers and code from
 * the start, the linked context records and uniform records
 * sceAgcLinkShaders writes at fixed offsets. */
#define PS5VK_STAGE_BYTES UINT64_C(0x10000)
#define PS5VK_STAGE_CONTEXT_OFFSET 0x5000
#define PS5VK_STAGE_CONTEXT_RECORDS 34
#define PS5VK_STAGE_UNIFORM_OFFSET 0x6000
#define PS5VK_STAGE_UNIFORM_RECORDS 3

/* The context and SH register tables sceAgcCreateShader leaves in a shader
 * object. */
struct ps5vk_shader_tables {
   const struct ps5vk_agc_register *cx;
   const struct ps5vk_agc_register *sh;
   uint32_t cx_count;
   uint32_t sh_count;
};

/* A pipeline's AGC shader objects, created the first time a command buffer
 * draws with it (ps5vk_pipeline.c). */
struct ps5vk_pipeline_shaders {
   mtx_t lock;
   bool attempted;
   VkResult result;
   struct ps5vk_direct_mapping stage;
   struct ps5vk_shader_tables tables[PS5VK_PIPELINE_STAGE_COUNT];
};

/* One vertex binding of a pipeline's vertex input. */
struct ps5vk_vertex_binding {
   uint32_t stride;
   bool used;
};

/* A graphics pipeline: one package per stage (ps5vk_pipeline.c). */
struct ps5vk_pipeline {
   struct vk_object_base base;
   struct ps5vk_shader_package stages[PS5VK_PIPELINE_STAGE_COUNT];
   /* SPI_SHADER_COL_FORMAT export nibbles the pixel stage was compiled for. */
   uint32_t spi_shader_col_format;
   /* The primitive type the shaders were linked as (DI_PT_TRILIST, DI_PT_TRISTRIP
    * or DI_PT_LINELIST): the strip's alternating winding is the hardware's, which
    * is why the topology reaches the link rather than a draw-time register
    * (ps5vk_pipeline.c, R6 and R8 of the port's requests). */
   uint32_t link_primitive_type;
   /* The pipeline's colour write mask: 0xf for RGBA, 0 for a pipeline that
    * writes no colour (vk_meta's depth clear is one). A draw whose pipeline
    * writes none carries CB_TARGET_MASK and CB_SHADER_MASK zeroed in its table,
    * beside the depth registers (Phase C5, ps5vk_draw.c). */
   uint32_t colour_write_mask;
   /* The pipeline's CB_BLEND0_CONTROL word (0x1e0), from the attachment's
    * colour-blend state: 0 for a pipeline that does not blend, which is the
    * state every draw before V0-formats' colour targets ran with. A draw whose
    * pipeline blends records that word and CB_COLOR_CONTROL (0x202) beside its
    * colour target's registers (ps5vk_pipeline.c, ps5vk_draw.c). */
   uint32_t blend_control;
   /* Whether that word's state reads the pipeline's blend constants: the draw
    * records CB_BLEND_RED/GREEN/BLUE/ALPHA (0x105 to 0x108) from
    * blend_constants when it is set, and records neither those four nor the
    * blend word for a pipeline that does not blend. A pipeline that blends with
    * anything else writes exactly the words it wrote before constant factors
    * were programmable, so no recorded stream changes. */
   bool blend_uses_constants;
   /* The four blend constants as the registers hold them, the float's bits. */
   uint32_t blend_constants[4];
   /* R1: the pipeline's PA_SU_SC_MODE_CNTL word (0x205), from cullMode and
    * frontFace, or 0 for a pipeline that culls nothing -- which is the state
    * every draw before R1 ran with, so a draw that does not cull records no
    * rasterizer word at all (ps5vk_draw.c). */
   uint32_t rasterizer_word;
   /* R1: whether the pipeline discards every rasterized primitive
    * (rasterizerDiscardEnable): the draw records PA_CL_CLIP_CNTL (0x204) with
    * DX_RASTERIZATION_KILL set. */
   bool discard_rasterizer;
   /* R8: whether the pipeline rasterizes lines (VK_PRIMITIVE_TOPOLOGY_LINE_LIST):
    * its draws record the line's own three words -- VGT_GS_OUT_PRIM_TYPE, the
    * width and the line control (ps5vk_draw.c) -- and no other pipeline's do. */
   bool line_rasterizer;
   /* Why command buffers cannot draw with the pipeline yet, or NULL. */
   const char *draw_refusal;
   /* What vkCmdBindPipeline puts into the command buffer's dynamic state:
    * the viewport and scissor of a pipeline that declares them statically.
    * The draw reads them from the command buffer, so a pipeline that declares
    * them dynamic leaves what vkCmdSetViewport and vkCmdSetScissor set. */
   struct vk_dynamic_graphics_state dynamic;
   /* The vertex input's bindings, indexed by binding number. */
   struct ps5vk_vertex_binding vertex_bindings[PS5VK_MAX_VERTEX_BINDINGS];
   /* The push-constant bytes the pipeline reads, 0 when it reads none, and
    * the stages that read them. */
   uint32_t push_constant_bytes;
   VkShaderStageFlags push_constant_stages;
   struct ps5vk_pipeline_shaders shaders;
   /* R10: the bindings this pipeline's stages read as input attachments, taken
    * from their modules at creation because the application may destroy a
    * module afterwards. The draw builds those entries from the subpass's input
    * attachment instead of from the application's descriptor writes, which
    * Vulkan forbids for this type (ps5vk_draw.c). */
   struct ps5vk_input_attachment input_attachments[PS5VK_MAX_INPUT_ATTACHMENTS];
   uint32_t input_attachment_count;
   /* The bind point the pipeline was created for: VK_PIPELINE_BIND_POINT_GRAPHICS
    * (zero, which is what a zeroed graphics pipeline is) or COMPUTE -- the only
    * one that fills the block below (Phase D2). */
   VkPipelineBindPoint bind_point;
   /* A compute pipeline: the compiler's ISA in a GPU-visible mapping and the
    * resource words a dispatch programs, which the compiler reports
    * (ps5vk_compute.c). */
   struct {
      struct ps5vk_direct_mapping code;
      PsbcShaderMetadata metadata;
      uint32_t code_bytes;
      uint32_t rsrc1;
      uint32_t rsrc2;
      uint32_t rsrc3;
      /* The shader's local size, from its SPIR-V: the dispatch programs it as
       * COMPUTE_NUM_THREAD_X/Y/Z, which is the workgroup's shape. */
      uint32_t local_size[3];
      /* The wave size the compiler reports, which DISPATCH_DIRECT's CS_W32_EN has
       * to agree with (the 0.2.0-era compiler reported a VGPR granule instead, and
       * the driver inferred the size from that). */
      bool wave32;
   } compute;
   /* The device's list of pipelines whose stage mapping exists, in creation
    * order, which ps5vk_debug_pipeline_stages walks for the runner's capture. */
   bool stage_registered;
   struct ps5vk_pipeline *next_stage;
};

/* A swapchain's images: VideoOut's two registered framebuffers. */
#define PS5VK_SWAPCHAIN_IMAGES 2

/* VideoOut and its framebuffers (ps5vk_wsi.c), owned by the newest swapchain
 * of a chain of replacements. */
struct ps5vk_video_out {
   int handle;
   bool registered;
   struct ps5vk_direct_mapping buffers;
   /* The buffer on screen, or UINT32_MAX before the first flip. */
   uint32_t shown;
   /* The marker of the last flip. */
   int64_t flip_marker;
};

struct ps5vk_swapchain {
   struct vk_object_base base;
   /* NULL once a newer swapchain has taken VideoOut over. */
   struct ps5vk_video_out *video;
   struct ps5vk_image *images[PS5VK_SWAPCHAIN_IMAGES];
   /* The image the application holds, or UINT32_MAX. */
   uint32_t acquired;
   bool retired;
};

VK_DEFINE_HANDLE_CASTS(ps5vk_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)
VK_DEFINE_HANDLE_CASTS(ps5vk_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)
VK_DEFINE_HANDLE_CASTS(ps5vk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)
VK_DEFINE_HANDLE_CASTS(ps5vk_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)
VK_DEFINE_HANDLE_CASTS(ps5vk_cmd_buffer, vk.base, VkCommandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_device_memory, vk.base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_buffer, vk.base, VkBuffer, VK_OBJECT_TYPE_BUFFER)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_sampler, base, VkSampler, VK_OBJECT_TYPE_SAMPLER)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_query_pool, base, VkQueryPool, VK_OBJECT_TYPE_QUERY_POOL)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_shader_module, base, VkShaderModule,
                               VK_OBJECT_TYPE_SHADER_MODULE)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_descriptor_set_layout, vk.base, VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_descriptor_set, base, VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_pipeline, base, VkPipeline, VK_OBJECT_TYPE_PIPELINE)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_swapchain, base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_event, base, VkEvent, VK_OBJECT_TYPE_EVENT)
VK_DEFINE_NONDISP_HANDLE_CASTS(ps5vk_pipeline_cache, base, VkPipelineCache,
                               VK_OBJECT_TYPE_PIPELINE_CACHE)

/* ps5vk_physical_device.c: the instance's physical-device callbacks. */
VkResult
ps5vk_physical_device_create(struct ps5vk_instance *instance,
                             struct ps5vk_physical_device **out_device);

void
ps5vk_physical_device_destroy(struct vk_physical_device *device);

/* ps5vk_queue.c: the queue and its submission buffer. */
VkResult
ps5vk_queue_init(struct ps5vk_device *device, struct ps5vk_queue *queue,
                 const VkDeviceQueueCreateInfo *info);

void
ps5vk_queue_finish(struct ps5vk_queue *queue);

/* Submits the flip of a VideoOut buffer in a stream of its own and waits
 * until VideoOut's flip status reaches marker (ps5vk_queue.c). */
VkResult
ps5vk_queue_flip(struct ps5vk_queue *queue, int video, uint32_t buffer_index, int64_t marker);

/* ps5vk_query.c: occlusion queries, which are the GPU's z-pass counter
 * sampled around a region and scaled to samples. */
VkResult
ps5vk_CreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool);

void
ps5vk_DestroyQueryPool(VkDevice device, VkQueryPool queryPool,
                       const VkAllocationCallbacks *pAllocator);

VkResult
ps5vk_GetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
                          uint32_t queryCount, size_t dataSize, void *pData, VkDeviceSize stride,
                          VkQueryResultFlags flags);

void
ps5vk_ResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
                     uint32_t queryCount);

/* ps5vk_pipeline.c: creates the pipeline's AGC shader objects on first use;
 * the result of that one attempt. */
VkResult
ps5vk_pipeline_prepare_shaders(struct ps5vk_device *device, struct ps5vk_pipeline *pipeline);

struct nir_shader;

/* ps5vk_nir.c: a clone of the NIR a meta pipeline stage carries, with push
 * constants lowered to the reserved uniform-buffer binding and the dead layer
 * output removed; NULL when it cannot be cloned. Release it with
 * ps5vk_nir_free. */
struct nir_shader *
ps5vk_nir_prepare(const struct nir_shader *source, uint32_t push_constant_bytes);

struct nir_shader *
ps5vk_nir_noop_fragment(void);

void
ps5vk_nir_free(struct nir_shader *nir);

/* ps5vk_draw.c: the device's vk_meta, with the flags this driver can honestly
 * set (docs/M5_PHASE_C.md, C1b question 2). */
VkResult
ps5vk_meta_init(struct ps5vk_device *device);

void
ps5vk_meta_finish(struct ps5vk_device *device);

/* The AGC command-buffer and submission ABI, as src/agc_abi.hpp and the test
 * runner declare them: helpers append packets at up and return their start. */
struct ps5vk_agc_command_buffer {
   uint32_t *bottom;
   uint32_t *top;
   uint32_t *up;
   uint32_t *down;
   uintptr_t callback;
   void *user_data;
   uint32_t reserved_dwords;
   uint32_t padding;
};

struct ps5vk_agc_submit_description {
   void *words;
   uint32_t word_count;
   uint8_t flag;
   uint8_t padding[3];
};

/* The callback AGC calls when a helper needs more words than a command buffer
 * has left (ps5vk_queue.c); the driver sizes its buffers so that it never
 * happens. */
uint8_t
ps5vk_agc_out_of_space(struct ps5vk_agc_command_buffer *buffer, uint32_t words, void *user_data);

/* libSceAgc and libSceAgcDriver on the console; host/agc/agc_host.cpp and
 * host/ps5/ps5_host.cpp on the PC. */
int32_t
sceAgcInit(uint32_t version);

void *
sceAgcGetRegisterDefaults(void);

int32_t
sceAgcCreateShader(void **shader, void *header, void *code);

int32_t
sceAgcLinkShaders(void *context, void *uniforms, void *reserved, void *vertex_shader,
                  void *pixel_shader, uint32_t primitive_type);

uint32_t *
sceAgcDcbSetCxRegistersIndirect(void *buffer, const void *table, uint32_t count);

uint32_t *
sceAgcDcbSetUcRegistersIndirect(void *buffer, const void *table, uint32_t count);

uint32_t *
sceAgcDcbSetShRegistersIndirect(void *buffer, const void *table, uint32_t count);

/* SET_SH_REG: count values from one SH register offset, in the stream itself.
 * A draw writes its stage's user data with it (offset 0x8c for the vertex
 * stage, 0x0c for the pixel stage), because the register tables a shader
 * object carries hold no user-data registers (console run pid 117). */
uint32_t *
sceAgcCbSetShRegisterRangeDirect(void *buffer, uint32_t offset, const uint32_t *values,
                                 uint32_t count);

/* The index-buffer packets of an indexed draw, in the order the recorded
 * streams use them: SET_UCONFIG_REG_INDEX of the index size, INDEX_BASE,
 * INDEX_BUFFER_SIZE and DRAW_INDEX_2 (host/agc/agc_host.cpp,
 * golden/runner/m3-vertex-1.json). Size 0 is UINT16, size 1 is UINT32 (R19). */
uint32_t *
sceAgcDcbSetIndexSize(void *buffer, uint8_t size, uint8_t reserved);

uint32_t *
sceAgcDcbSetIndexBuffer(void *buffer, void *indices);

uint32_t *
sceAgcDcbSetIndexCount(void *buffer, uint32_t count);

uint32_t *
sceAgcDcbDrawIndex(void *buffer, uint32_t count, void *indices, uint64_t modifier);

uint32_t *
sceAgcDcbDrawIndexAuto(void *buffer, uint32_t count, uint64_t modifier);

/* How many instances the draw that follows runs: the packet ps5-opengl's
 * runtime emits immediately before an instanced draw, and one to put the count
 * back to one after it (ps5_agc_set_instances, the AGC library's
 * sceAgcDcbSetNumInstances). */
uint32_t *
sceAgcDcbSetNumInstances(void *buffer, uint32_t count);

uint32_t *
sceAgcCbReleaseMem(void *buffer, uint8_t event, int16_t control, uint64_t a, int8_t b, void *c,
                   uint32_t d, uint64_t e, uint16_t f, uint16_t g, int8_t h, int32_t i);

int32_t
sceAgcDriverSubmitDcb(void *description);

int32_t
sceAgcSuspendPoint(void);

uint32_t
sceAgcDriverGetWaitRenderingPacketSizeInDwords(void);

/* Writes the wait packet for a VideoOut buffer at *up and advances it;
 * returns 0 on success. */
uint32_t
sceAgcDriverWaitUntilSafeForRendering(uint32_t **up, uint32_t words, uint32_t reserved,
                                      uint32_t video, int buffer_index);

uint32_t *
sceAgcDcbSetFlip(void *buffer, uint32_t video, int buffer_index, uint32_t mode, int64_t marker);

/* libSceVideoOut on the console, host/ps5/ps5_host.cpp on the PC, declared as
 * the test runner declares them; the arguments named reserved are 0 in the
 * runner, ProsperoLight and ps5-opengl. */
int
sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);

int
sceVideoOutClose(int32_t handle);

int
sceVideoOutSetFlipRate(int32_t handle, int32_t rate);

void
sceVideoOutSetBufferAttribute2(void *attribute, uint64_t pixel_format, uint32_t reserved1,
                               uint32_t width, uint32_t height, uint64_t reserved2,
                               uint32_t reserved3, uint64_t reserved4);

int
sceVideoOutRegisterBuffers2(int32_t handle, int32_t set, int32_t start, void *buffers,
                            int32_t count, void *attribute, int32_t reserved, void *option);

int
sceVideoOutUnregisterBuffers(int32_t handle, int32_t set);

int
sceVideoOutGetFlipStatus(int32_t handle, void *status);

int
sceVideoOutWaitVblank(int32_t handle);

int
sceVideoOutIsFlipPending(int32_t handle);

int
sceKernelUsleep(uint32_t microseconds);

/* The PS5 kernel: libkernel on the console, host/ps5/ps5_host.cpp on the PC.
 * Declared as the test runner declares them (src/diagnostics.cpp). */
int64_t
sceKernelGetDirectMemorySize(void);

int32_t
sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t bytes,
                              size_t alignment, int type, int64_t *start);

int32_t
sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t start,
                         size_t alignment);

int32_t
sceKernelMunmap(void *address, size_t bytes);

int32_t
sceKernelReleaseDirectMemory(int64_t start, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_PRIVATE_H */
