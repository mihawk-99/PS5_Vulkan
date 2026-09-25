/*
 * PS5 Vulkan driver - the topologies a pipeline is linked as.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R6 and R8 of the vkQuake port's requests (docs/M5_PHASE_C.md); built and run
 * through the loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * A topology reaches the hardware through AGC's link, which takes AMD's DI_PT_*
 * value and writes it into VGT_PRIMITIVE_TYPE. The PC model replays the link's
 * outputs instead of computing them, so what the console's link is told is read
 * here through the driver's debug API (ps5vk_debug_pipeline_primitive_type):
 *
 *   line list        DI_PT_LINELIST  2
 *   triangle list    DI_PT_TRILIST   4
 *   triangle strip   DI_PT_TRISTRIP  6  (R6 first passed 5, which is the fan)
 *
 * and each frame is recorded and submitted through the same harness the runner
 * case v0-strip draws with, so the frames the console reads back are the ones
 * this program records: a strip of three vertices and a strip of four, drawn with
 * VkCmdDraw from a bound vertex buffer -- the DRAW_INDEX_AUTO in the submission
 * carries the frame's own vertex count -- and the quad as an indexed list. A line
 * list's draw records the line's own three words, VGT_GS_OUT_PRIM_TYPE LINESTRIP,
 * PA_SU_LINE_CNTL's width 1.0 and PA_SC_LINE_CNTL 0, and a PA_SU_SC_MODE_CNTL with
 * no cull bits even when the pipeline asked to cull both faces (cullMode is a
 * polygon's); a triangle's draw records none of the three. The pixels are the
 * console's to prove (runner cases v0-strip and v0-lines). PS5VK_PROBES names the
 * probes directory, which holds the m3-vertex package the frames use.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

struct steps {
   const char *failed;
   int result;
   const char *detail;
};

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   struct steps *const steps = context;
   if (!passed && steps->failed == NULL) {
      steps->failed = name;
      steps->result = result;
      steps->detail = detail;
   }
}

#if defined(__linux__)
/* A SPIR-V file of a probe set, in words. */
static uint32_t *
read_spirv(const char *probes, const char *set, const char *stage, size_t *bytes)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s/%s.spv", probes, set, stage);
   FILE *const file = fopen(path, "rb");
   if (!file)
      return NULL;
   fseek(file, 0, SEEK_END);
   const long length = ftell(file);
   fseek(file, 0, SEEK_SET);
   uint32_t *words = length > 0 && length % 4 == 0 ? malloc((size_t)length) : NULL;
   if (words && fread(words, 1, (size_t)length, file) != (size_t)length) {
      free(words);
      words = NULL;
   }
   fclose(file);
   *bytes = words ? (size_t)length : 0;
   return words;
}
#endif

/* The m3-vertex layout: a position and a colour, 24-byte records. Every vertex
 * has the same colour, as in the runner case, so a frame is its coverage alone. */
#define TOPOLOGY_STRIDE 24
static const float kVertices[4][6] = {
   {-1.0f, -1.0f, 0.5f, 1.0f, 0.25f, 1.0f},
   {-1.0f, 1.0f, 0.5f, 1.0f, 0.25f, 1.0f},
   {1.0f, -1.0f, 0.5f, 1.0f, 0.25f, 1.0f},
   {1.0f, 1.0f, 0.5f, 1.0f, 0.25f, 1.0f},
};
static const uint16_t kConsistent[6] = {0, 1, 2, 1, 3, 2};

/* The two draw packets as the PC's AGC model encodes them
 * (host/agc/agc_host.cpp): DRAW_INDEX_AUTO is PKT3(0x2d, 1) with the vertex count
 * first, DRAW_INDEX_2 PKT3(0x27, 4) with the index count first. */
#define DRAW_INDEX_AUTO_HEADER 0xc0012d00u
#define DRAW_INDEX_2_HEADER 0xc0042700u

#if defined(PS5VK_TEST_DIRECT)
/* The count of the submission's last packet with this header, or 0 for none:
 * the frame's own draw is its last one, after the pass's clear, which vk_meta
 * draws as a DRAW_INDEX_AUTO of its own. */
static uint32_t
last_draw_count(VkDevice device, uint32_t header)
{
   uint32_t dwords = 0;
   const uint32_t *const words = ps5vk_debug_last_submission(device, &dwords);
   uint32_t count = 0;
   for (uint32_t at = 0; words != NULL && at + 1 < dwords; at++) {
      if (words[at] == header)
         count = words[at + 1];
   }
   return count;
}
#endif

#if defined(PS5VK_TEST_DIRECT)
/* The value the draw's register tables give a context register, and whether any
 * of them names it: the tables live in the driver's chunks
 * (ps5vk_debug_table_chunks), the frame's own draw's last, after vk_meta's clear.
 * Only the direct build reads them: the loader build sees the driver through its
 * ICD, which exports no debug symbol (driver/ps5vk_icd.map). */
static bool
recorded(VkDevice device, uint16_t offset, uint32_t *value)
{
   ps5vk_debug_stage chunks[8] = {{0}};
   const uint32_t count = ps5vk_debug_table_chunks(device, chunks, 8);
   bool found = false;
   /* Newest chunk first: walk them oldest first so the last record wins, as it
    * does in the hardware's table load. */
   for (uint32_t chunk = count < 8 ? count : 8; chunk-- > 0;) {
      const uint32_t *const words = chunks[chunk].address;
      const size_t records = chunks[chunk].bytes / (2 * sizeof(uint32_t));
      for (size_t record = 0; record < records; record++) {
         if ((words[2 * record] & 0xffffu) == offset) {
            *value = words[2 * record + 1];
            found = true;
         }
      }
   }
   return found;
}
#endif

#if defined(PS5VK_TEST_DIRECT)
/* R64: the changes of VGT_MULTI_PRIM_IB_RESET_EN in the last submission -- the
 * one-record uconfig table loads (PM4 0x64), which nothing else records -- how
 * many of them follow an EVENT_WRITE of SQ_NON_EVENT, and how many come before
 * the submission's DRAW_INDEX_2. */
struct restart_writes {
   unsigned count;
   unsigned after_event;
   unsigned before_draw;
};

static struct restart_writes
restart_writes_in(const uint32_t *words, uint32_t dwords)
{
   struct restart_writes found = {0};
   bool drawn = false;
   for (uint32_t at = 0; words != NULL && at < dwords;) {
      const uint32_t header = words[at];
      if ((header >> 30) != 3u) {
         at++;
         continue;
      }
      const uint32_t count = ((header >> 16) & 0x3fffu) + 1u;
      const uint32_t opcode = (header >> 8) & 0xffu;
      drawn = drawn || opcode == 0x27u;
      if (opcode == 0x64u && count == 4u && at + 4u < dwords && words[at + 4u] == 1u) {
         found.count++;
         found.after_event += at >= 2u && words[at - 2u] == 0xc0004600u && words[at - 1u] == 0u;
         found.before_draw += !drawn;
      }
      at += 1u + count;
   }
   return found;
}

static struct restart_writes
restart_writes(VkDevice device)
{
   uint32_t dwords = 0;
   const uint32_t *const words = ps5vk_debug_last_submission(device, &dwords);
   return restart_writes_in(words, dwords);
}
#endif

struct frame {
   const char *name;
   VkPrimitiveTopology topology;
   uint32_t vertex_count;
   const uint16_t *indices;
   uint32_t link;
   /* The count the frame's own draw packet carries: vertices for a
    * DRAW_INDEX_AUTO, indices for a DRAW_INDEX_2. */
   uint32_t draw_count;
   VkCullModeFlags cull;
   /* R58: the pipeline's primitiveRestartEnable, for an indexed strip. */
   bool restart;
};

/* R58: one quad, restarted: the strip's four vertices, the all-ones index, and
 * the same four again. */
static const uint16_t kRestarted[9] = {0, 1, 2, 3, 0xffff, 0, 1, 2, 3};

static void
draw_frame(const struct frame *frame, const uint32_t *vertex, size_t vertex_bytes,
           const uint32_t *pixel, size_t pixel_bytes)
{
   struct steps steps = {0};
   const struct ps5vk_triangle_report report = {&steps, record_step};
   struct ps5vk_triangle_input input = {0};
   input.get_instance_proc_addr = GET_PROC;
   input.pipeline_count = 1;
   input.shaders[0] = (struct ps5vk_triangle_shaders){vertex, vertex_bytes, pixel, pixel_bytes};
   input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
   input.report = &report;
   input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
   input.vertex_data = kVertices;
   input.vertex_count = frame->vertex_count;
   input.vertex_stride = TOPOLOGY_STRIDE;
   input.index_data = frame->indices;
   input.index_count = frame->indices == NULL ? 0u : frame->restart ? 9u : 6u;
   input.primitive_restart = frame->restart;
   input.attribute_count = 2;
   input.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
   input.attributes[1] =
      (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8};
   input.primitive_topology = frame->topology;
   input.rasterization_cull_mode = frame->cull;
   struct ps5vk_triangle triangle = {0};
   enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
   if (status == PS5VK_TRIANGLE_OK)
      status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
   char what[192];
   snprintf(what, sizeof(what), "%s: the frame records, submits and signals its fence",
            frame->name);
   check(status == PS5VK_TRIANGLE_OK && steps.failed == NULL, what);
   if (steps.failed != NULL)
      printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
             steps.detail != NULL ? steps.detail : "");
#if defined(PS5VK_TEST_DIRECT)
   if (status == PS5VK_TRIANGLE_OK) {
      const uint32_t link = ps5vk_debug_pipeline_primitive_type(triangle.pipelines[0]);
      snprintf(what, sizeof(what), "%s: the link is told DI_PT %u", frame->name, frame->link);
      check(link == frame->link, what);
      if (link != frame->link)
         printf("  (the pipeline links as %u)\n", link);
      const bool indexed = frame->indices != NULL;
      const uint32_t count =
         last_draw_count(triangle.device, indexed ? DRAW_INDEX_2_HEADER : DRAW_INDEX_AUTO_HEADER);
      snprintf(what, sizeof(what),
               indexed ? "%s: the submission's last draw is a DRAW_INDEX_2 of %u indices"
                       : "%s: the submission's last draw is a DRAW_INDEX_AUTO of %u vertices",
               frame->name, frame->draw_count);
      check(count == frame->draw_count, what);
      if (count != frame->draw_count)
         printf("  (its count is %u)\n", count);
      /* The line's three words, and the rasterizer word without cull bits. */
      const bool line = frame->topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      static const struct {
         uint16_t offset;
         uint32_t value;
         const char *name;
      } kLineWords[] = {
         {0x29b, 1u, "VGT_GS_OUT_PRIM_TYPE is LINESTRIP"},
         {0x282, 8u, "PA_SU_LINE_CNTL's width is 1.0 (8 in 12.4 half-width)"},
         {0x2f7, 0u, "PA_SC_LINE_CNTL is 0: non-strict lines, no diamond test"},
      };
      for (size_t at = 0; at < sizeof(kLineWords) / sizeof(kLineWords[0]); at++) {
         uint32_t value = 0;
         const bool found = recorded(triangle.device, kLineWords[at].offset, &value);
         if (line) {
            snprintf(what, sizeof(what), "%s: %s", frame->name, kLineWords[at].name);
            check(found && value == kLineWords[at].value, what);
            if (!found || value != kLineWords[at].value)
               printf("  (0x%03x %s 0x%08x)\n", kLineWords[at].offset,
                      found ? "holds" : "is not recorded; would be", value);
         } else if (kLineWords[at].offset != 0x29b) {
            /* The link's own records name 0x29b for every pipeline; the two line
             * registers are the line's alone. */
            snprintf(what, sizeof(what), "%s: records no 0x%03x", frame->name,
                     kLineWords[at].offset);
            check(!found, what);
         }
      }
      /* R58: a restart draw loads VGT_MULTI_PRIM_IB_RESET_INDX (context record
       * 0x103) as all ones and turns on the uconfig VGT_MULTI_PRIM_IB_RESET_EN
       * record (0x24b), which the command buffer puts back to 0 at its end; no
       * other frame records either. */
      {
         uint32_t index = 0;
         const bool index_found = recorded(triangle.device, 0x103, &index);
         uint32_t enable = UINT32_MAX;
         const bool enable_found = recorded(triangle.device, 0x24b, &enable);
         if (frame->restart) {
            snprintf(what, sizeof(what),
                     "%s: the reset index is all ones and the enable is put back to 0",
                     frame->name);
            check(index_found && index == 0xffffffffu && enable_found && enable == 0u, what);
         } else {
            snprintf(what, sizeof(what), "%s: records no primitive restart state", frame->name);
            check(!index_found && !enable_found, what);
         }
         /* R64: the enable changes twice, on before the draw and off after it
          * (at vkEndCommandBuffer), and each change follows an SQ_NON_EVENT:
          * a bare write after a restart draw took effect in the middle of it
          * on the console (jobs/r64-restart-strips). */
         const struct restart_writes writes = restart_writes(triangle.device);
         snprintf(what, sizeof(what),
                  frame->restart ? "%s: restart is turned on before the draw and off after it, "
                                   "each change behind an SQ_NON_EVENT"
                                 : "%s: the submission changes no restart state",
                  frame->name);
         check(frame->restart ? writes.count == 2u && writes.after_event == 2u &&
                                   writes.before_draw == 1u
                              : writes.count == 0u,
               what);
         if (frame->restart && (writes.count != 2u || writes.after_event != 2u ||
                                writes.before_draw != 1u))
            printf("  (%u changes, %u behind the event, %u before the draw)\n", writes.count,
                   writes.after_event, writes.before_draw);
      }
      if (line) {
         uint32_t value = UINT32_MAX;
         const bool found = recorded(triangle.device, 0x205, &value);
         snprintf(what, sizeof(what), "%s: PA_SU_SC_MODE_CNTL is recorded without cull bits",
                  frame->name);
         check(found && (value & 3u) == 0u, what);
      }
   }
#endif
   if (status != PS5VK_TRIANGLE_IN_FLIGHT)
      ps5vk_triangle_finish(&triangle);
}

#if defined(PS5VK_TEST_DIRECT)
/* R65: the restarted quad in two render passes with a copy between them, the
 * second pass loading what the first drew. The copy splits the submission, and
 * the GPU starts every submission with restart off, so the second pass's draw
 * has to turn it on again: three changes in all (on, on again after the split,
 * off at the end), each behind an SQ_NON_EVENT, the second in the step after the
 * split and before its draw. */
static void
restart_across_split(const uint32_t *vertex, size_t vertex_bytes, const uint32_t *pixel,
                     size_t pixel_bytes)
{
   struct steps steps = {0};
   const struct ps5vk_triangle_report report = {&steps, record_step};
   struct ps5vk_triangle_input input = {0};
   input.get_instance_proc_addr = GET_PROC;
   input.pipeline_count = 1;
   input.shaders[0] = (struct ps5vk_triangle_shaders){vertex, vertex_bytes, pixel, pixel_bytes};
   input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
   input.report = &report;
   input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
   input.vertex_data = kVertices;
   input.vertex_count = 4;
   input.vertex_stride = TOPOLOGY_STRIDE;
   input.index_data = kRestarted;
   input.index_count = 9;
   input.primitive_restart = true;
   input.attribute_count = 2;
   input.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
   input.attributes[1] =
      (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8};
   input.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
   input.two_passes = true;
   input.split_between_passes = true;
   struct ps5vk_triangle triangle = {0};
   enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
   if (status == PS5VK_TRIANGLE_OK)
      status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
   check(status == PS5VK_TRIANGLE_OK && steps.failed == NULL,
         "restart across a split: the frame records, submits and signals its fence");
   if (status == PS5VK_TRIANGLE_OK) {
      ps5vk_debug_stage parts[4] = {{0}};
      const uint32_t count = ps5vk_debug_submission_steps(triangle.device, parts, 4);
      struct restart_writes total = {0};
      struct restart_writes after = {0};
      for (uint32_t at = 0; at < count && at < 4; at++) {
         const struct restart_writes found = restart_writes_in(
            parts[at].address, (uint32_t)(parts[at].bytes / sizeof(uint32_t)));
         total.count += found.count;
         total.after_event += found.after_event;
         if (at == 1)
            after = found;
      }
      check(count >= 2, "restart across a split: the copy splits the submission into steps");
      check(total.count == 3 && total.after_event == 3,
            "restart across a split: three restart changes, each behind an SQ_NON_EVENT");
      check(after.before_draw >= 1,
            "restart across a split: the step after the split turns restart on before its draw");
      if (total.count != 3 || total.after_event != 3 || after.before_draw < 1)
         printf("  (%u steps, %u changes, %u behind the event, %u before the second step's draw)\n",
                count, total.count, total.after_event, after.before_draw);
   }
   if (status != PS5VK_TRIANGLE_IN_FLIGHT)
      ps5vk_triangle_finish(&triangle);
}
#endif

#if defined(__linux__)
/* R60: a pixel stage whose code is larger than the runner's fixed stage layout
 * held (32 KiB; the layout's limit was 20 KiB, and Dolphin's ubershaders were
 * refused at 24 KiB) creates, draws and signals its fence. Then a command buffer
 * whose recording the driver refused: its end fails, and submitting it returns
 * a result instead of reaching the runtime's assert. */
static void
big_shader_and_refused_submit(const uint32_t *vertex, size_t vertex_bytes, const uint32_t *pixel,
                              size_t pixel_bytes)
{
   static const float quad[] = {-1, -1, 0.25f, 0, 1, -1, 0.75f, 0, 1, 1, 0.75f, 0, -1, 1, 0.25f, 0};
   static const uint16_t quad_indices[] = {0, 1, 2, 2, 3, 0};
   struct steps steps = {0};
   const struct ps5vk_triangle_report report = {&steps, record_step};
   struct ps5vk_triangle_input input = {0};
   input.get_instance_proc_addr = GET_PROC;
   input.pipeline_count = 1;
   input.shaders[0] = (struct ps5vk_triangle_shaders){vertex, vertex_bytes, pixel, pixel_bytes};
   input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
   input.report = &report;
   input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
   input.vertex_data = quad;
   input.vertex_count = 4;
   input.vertex_stride = 16;
   input.index_data = quad_indices;
   input.index_count = 6;
   input.attribute_count = 2;
   input.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
   input.attributes[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32_SFLOAT, 8};
   struct ps5vk_triangle triangle = {0};
   enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
   if (status == PS5VK_TRIANGLE_OK)
      status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
   check(status == PS5VK_TRIANGLE_OK && steps.failed == NULL,
         "a 32 KiB pixel stage creates, draws and signals its fence");
   if (steps.failed != NULL)
      printf("  (first failed step: %s, result %d: %s)\n", steps.failed, steps.result,
             steps.detail != NULL ? steps.detail : "");
   if (status == PS5VK_TRIANGLE_OK) {
#define PROC(name) ((PFN_##name)triangle.get_instance_proc_addr(triangle.instance, #name))
      const VkCommandBufferAllocateInfo allocate = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = triangle.pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      VkCommandBuffer refused = VK_NULL_HANDLE;
      const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      VkResult ended = VK_SUCCESS, submitted = VK_SUCCESS;
      if (PROC(vkAllocateCommandBuffers)(triangle.device, &allocate, &refused) == VK_SUCCESS &&
          PROC(vkBeginCommandBuffer)(refused, &begin) == VK_SUCCESS) {
         /* A primary executing a primary: refused by name
          * (ps5vk_CmdExecuteCommands, "executes secondary command buffers"). */
         PROC(vkCmdExecuteCommands)(refused, 1, &refused);
         ended = PROC(vkEndCommandBuffer)(refused);
         const VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &refused,
         };
         submitted = PROC(vkQueueSubmit)(triangle.queue, 1, &submit, VK_NULL_HANDLE);
         PROC(vkFreeCommandBuffers)(triangle.device, triangle.pool, 1, &refused);
      }
      check(ended != VK_SUCCESS,
            "a command buffer whose recording was refused fails vkEndCommandBuffer");
      check(submitted == VK_ERROR_UNKNOWN,
            "submitting it returns VK_ERROR_UNKNOWN instead of aborting");
#undef PROC
   }
   if (status != PS5VK_TRIANGLE_IN_FLIGHT)
      ps5vk_triangle_finish(&triangle);
}
#endif

int
main(void)
{
   test_begin("V0 topology");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
   uint32_t *const vertex =
      probes ? read_spirv(probes, "m3-vertex", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m3-vertex", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m3-vertex SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#endif

   static const struct frame kFrames[] = {
      {"three vertices, strip", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 3, NULL, 6, 3,
       VK_CULL_MODE_NONE},
      {"three vertices, list", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 3, NULL, 4, 3,
       VK_CULL_MODE_NONE},
      {"quad, strip", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 4, NULL, 6, 4, VK_CULL_MODE_NONE},
      {"quad, indexed list", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 4, kConsistent, 4, 6,
       VK_CULL_MODE_NONE},
      {"two lines", VK_PRIMITIVE_TOPOLOGY_LINE_LIST, 4, NULL, 2, 4, VK_CULL_MODE_NONE},
      {"two lines, both faces culled", VK_PRIMITIVE_TOPOLOGY_LINE_LIST, 4, NULL, 2, 4,
       VK_CULL_MODE_FRONT_AND_BACK},
      {"quad, indexed strip with primitive restart", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 4,
       kRestarted, 6, 9, VK_CULL_MODE_NONE, true},
   };
   for (size_t at = 0; vertex && pixel && at < sizeof(kFrames) / sizeof(kFrames[0]); at++)
      draw_frame(&kFrames[at], vertex, vertex_bytes, pixel, pixel_bytes);
#if defined(PS5VK_TEST_DIRECT)
   if (vertex && pixel)
      restart_across_split(vertex, vertex_bytes, pixel, pixel_bytes);
#endif

#if defined(__linux__)
   {
      size_t big_vertex_bytes = 0;
      size_t big_pixel_bytes = 0;
      uint32_t *const big_vertex =
         probes ? read_spirv(probes, "r60-big", "vertex", &big_vertex_bytes) : NULL;
      uint32_t *const big_pixel =
         probes ? read_spirv(probes, "r60-big", "pixel", &big_pixel_bytes) : NULL;
      check(big_vertex && big_pixel, "PS5VK_PROBES holds the r60-big SPIR-V");
      if (big_vertex && big_pixel)
         big_shader_and_refused_submit(big_vertex, big_vertex_bytes, big_pixel, big_pixel_bytes);
      free(big_vertex);
      free(big_pixel);
   }
#endif

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
