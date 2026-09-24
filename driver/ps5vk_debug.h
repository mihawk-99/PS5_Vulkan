/*
 * PS5 Vulkan driver - test-only access to image storage.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C1 (docs/M5_PHASE_C.md). Not part of any Vulkan API.
 * Swapchain images have no VkDeviceMemory an application could map, so the
 * console test runner (src/diagnostics.cpp) reads presented frames back
 * through this function. The PC's loader driver does not export it.
 *
 * The second export is what a PC rebuild needs from a console run: the stage
 * mappings the driver compiled its pipelines into. AGC relocates each
 * pipeline's shader headers in place and writes its linked context and
 * uniforms there, and the PC model cannot compute any of that -- it replays
 * it. One run with one pipeline is covered by the capture's stage image; a
 * frame with a second pipeline, which every vk_meta clear, blit, depth clear
 * and resolve has, needs the second one too (docs/M5_PHASE_C.md, the PC
 * model's pipeline capture).
 */

#ifndef PS5VK_DEBUG_H
#define PS5VK_DEBUG_H

#include <stdbool.h>
#include <stddef.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The CPU address of an image's texel storage and its size in bytes, or NULL
 * and 0 for an image without storage. */
void *
ps5vk_debug_image_storage(VkImage image, size_t *bytes);

/* An occlusion query pool's counters: their CPU address and the mapping's size
 * in bytes, or NULL and 0 for no pool (Phase V0-query). The console test runner
 * logs them with the frame's capture because the submission's ZPASS_DONE
 * samples name these addresses, and a PC replay has to hand the pool the same
 * mapping for the two streams to compare (tools/ps5vk_log.py's region kinds). */
void *
ps5vk_debug_query_pool_storage(VkQueryPool pool, size_t *bytes);

/* One pipeline's stage mapping: GPU-visible memory AGC relocated the
 * pipeline's headers in and linked its context and uniforms in. */
typedef struct ps5vk_debug_stage {
   void *address;
   size_t bytes;
} ps5vk_debug_stage;

/* The device's live pipeline stages, in the order their pipelines created
 * them, up to capacity; returns how many there are. The console test runner
 * logs them so a PC rebuild can replay the pipelines the console ran. */
uint32_t
ps5vk_debug_pipeline_stages(VkDevice device, ps5vk_debug_stage *stages, uint32_t capacity);

/* The primitive type a graphics pipeline hands sceAgcLinkShaders -- AMD's DI_PT_*
 * value, VGT_PRIMITIVE_TYPE's (ps5vk_pipeline.c) -- or 0 for no pipeline or a
 * compute one. The PC model replays the link's outputs rather than computing
 * them, so this is where a host check reads what the console's link is told. */
uint32_t
ps5vk_debug_pipeline_primitive_type(VkPipeline pipeline);

/* The command buffers' GPU-visible table chunks, newest first, up to
 * capacity; returns how many there are. The register tables a submission
 * names live in these, so a capture of the words is only comparable with a
 * capture of the tables they point at. */
uint32_t
ps5vk_debug_table_chunks(VkDevice device, ps5vk_debug_stage *chunks, uint32_t capacity);

/* The 16 bytes the last draw's push constants were copied into, and how many of
 * them the draw's pipeline reads (R9): the block a stage's reserved set-0
 * binding points at, so a probe can assert the upload itself rather than infer
 * it from pixels. *block is NULL and *bytes zero before the first draw. */
void
ps5vk_debug_push_constants(VkDevice device, const void **block, uint32_t *bytes,
                           const uint32_t **descriptor);
/* Where the last draw programmed the push-constant pointer: the stage, the
 * user-data dword and the two words written there (R9). *dword is UINT32_MAX
 * when no stage read push constants. */
void
ps5vk_debug_push_constant_user_data(VkDevice device, uint32_t *stage, uint32_t *dword,
                                    uint32_t *low, uint32_t *high);

/* One descriptor table the last draw built (R7): the stage that reads it, the
 * set it belongs to, the user-data dword its pointer was written to, the pointer
 * as programmed and the table's size in bytes. */
typedef struct ps5vk_debug_table {
   uint32_t stage;
   uint32_t set;
   uint32_t user_data_dword;
   uint32_t address_low;
   uint32_t address_high;
   /* The table itself, as the draw allocated it: the words a caller reads
    * instead of the address the ABI carries, which is one dword in this
    * driver's 32-bit-pointer build. */
   const uint32_t *words;
   size_t bytes;
} ps5vk_debug_table;

/* The tables the last draw built, in the order it built them, up to capacity;
 * returns how many there are. One per set each stage read, because a set's table
 * is sized from that set's own bindings and its pointer is its own user-data
 * dword: a probe asserts both sets' pointers and contents from this rather than
 * inferring them from pixels (R7, docs/M5_PHASE_C.md). */
uint32_t
ps5vk_debug_descriptor_tables(VkDevice device, ps5vk_debug_table *tables, uint32_t capacity);

/* One colour attachment of the last begin-rendering, as the draw programmed it:
 * the attachment's index, the dword offset of its CB_COLORi_BASE register, and
 * the address word written there. This is what makes the per-attachment table
 * (ps5vk_draw.c, ps5vk_target_offsets) assertable on the host: the offsets are
 * the derivation and the values are the attachments, so a driver that programmed
 * one target and reused its row for the others reports one address twice, and a
 * driver that derived the wrong column reports an offset its neighbours share
 * (R7 step 1b; the probe is v0-mrt in src/diagnostics.cpp). */
typedef struct ps5vk_debug_target {
   uint32_t index;
   uint32_t base_offset;
   uint32_t base_value;
} ps5vk_debug_target;

/* The colour attachments of the last begin-rendering, in attachment order, up to
 * capacity; returns how many there were. */
uint32_t
ps5vk_debug_colour_targets(VkDevice device, ps5vk_debug_target *targets, uint32_t capacity);

/* The device's live bound buffers, newest first, up to capacity; returns how
 * many there are. A submission names their addresses -- an index buffer's
 * INDEX_BASE, a vertex-buffer table's first record -- so a capture that
 * lists them lets a replay hand the same addresses to a PC rebuild's own
 * buffers (tools/golden.py, a driver run's replay). Buffers that are not
 * bound yet have no address and are skipped. */
uint32_t
ps5vk_debug_buffers(VkDevice device, ps5vk_debug_stage *buffers, uint32_t capacity);

/* The words of the last command stream the device queued, and their count in
 * dwords, or NULL and 0 before the first submission. They stay valid until
 * the next submission replaces them. */
const uint32_t *
ps5vk_debug_last_submission(VkDevice device, uint32_t *dwords);

/* The steps of the last submission, in the order the GPU ran them, up to
 * capacity; returns how many there are. A submission is one step unless its
 * copies or a render-to-texture sample split it (ps5vk_cmd_buffer_split), and
 * the words of the steps after the first stay valid exactly as
 * ps5vk_debug_last_submission's do, so a capture reads the whole submission
 * through this. */
uint32_t
ps5vk_debug_submission_steps(VkDevice device, ps5vk_debug_stage *steps, uint32_t capacity);

/* The VideoOut handle the device presents through, or -1 when it presents
 * through none. A submission's wait packet carries it, so a replay that
 * presents has to know it (tools/golden.py, a driver run's replay). */
int
ps5vk_debug_video_handle(VkDevice device);

/* The driver's profiling clock, in nanoseconds (the TSC), and when the last
 * vkQueuePresentKHR on that clock presented, 0 before any: an application's
 * own sampling profiler reads them to tell a stalled frame from a running one
 * without a hook in its frame loop. */
uint64_t
ps5vk_debug_now_ns(void);
uint64_t
ps5vk_debug_last_present_ns(void);

/* Whether VideoOut outlives the swapchain that presents through it. Retained, a
 * destroyed swapchain leaves its last presented image on screen and the output
 * in its mode, and the next swapchain -- on any device -- presents through the
 * same framebuffers, so an application that tears its whole Vulkan context down
 * and builds it again (RetroArch, on loading or closing content) shows its last
 * frame meanwhile rather than a blank panel and an output-mode switch. Released
 * (false, the default), VideoOut closes with its swapchain, and a retained
 * output no swapchain holds is closed now, its mode restored: an application
 * that retains calls this with false before it exits. */
void
ps5vk_display_retain(bool retain);

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_DEBUG_H */
