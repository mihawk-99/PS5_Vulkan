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

/* The command buffers' GPU-visible table chunks, newest first, up to
 * capacity; returns how many there are. The register tables a submission
 * names live in these, so a capture of the words is only comparable with a
 * capture of the tables they point at. */
uint32_t
ps5vk_debug_table_chunks(VkDevice device, ps5vk_debug_stage *chunks, uint32_t capacity);

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

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_DEBUG_H */
