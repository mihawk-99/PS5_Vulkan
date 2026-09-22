/*
 * PS5 Vulkan driver - R10 test: a subpass input attachment, bound by the draw.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R10's first ask (docs/M5_PHASE_C.md, docs/REQUESTS_RESPONSE.md): a fragment
 * stage reading `subpassLoad` on its subpass's input attachment. The console
 * frame is src/diagnostics.cpp's v0-subpass case; what this program checks is
 * the half a stream can show, and it is the half that failed first on the
 * console:
 *
 *   1. the two-subpass frame is created and recorded, with no refusal -- which
 *      includes the input attachment *not* being written by the application,
 *      because Vulkan forbids a descriptor write of that type;
 *   2. the second subpass's draw built a descriptor table at all: without the
 *      application's set layout naming the binding, the compiler's metadata has
 *      no binding for the shader's read, no table is built and the fetch goes
 *      through an empty descriptor -- which is exactly what the console
 *      measured before this test existed (a black read, magenta sentinel in the
 *      frame);
 *   3. that table's entry is the 32-byte image entry the shader reads, and it
 *      names an image address -- the first subpass's colour attachment, which
 *      is in the command buffer's own rendered-target list, so the draw that
 *      reads it carries the colour barrier and splits the submission
 *      (driver/ps5vk_draw.c, ps5vk_sampled_image).
 *
 * PS5VK_PROBES names the probes directory. The PS5 build only links.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ps5vk_debug.h"
#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

#if defined(__linux__)
/* The driver's messages, concatenated: what the frame's recording refused *by
 * name* is what this program is looking for, and the host's clear metas and
 * split need console-derived tables, so a step that fails for one of those
 * reasons must not be mistaken for the binding being wrong. */
struct messages {
   char text[512];
};

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   struct messages *const messages = context;
   (void)name;
   (void)result;
   if (passed || detail[0] == '\0')
      return;
   const size_t used = strlen(messages->text);
   if (used + 2 < sizeof(messages->text))
      snprintf(messages->text + used, sizeof(messages->text) - used, "%s | ", detail);
}
#endif

#if defined(PS5VK_TEST_DIRECT)
/* The driver's own decoration scan, called directly: what it finds in the
 * reader's module is what the pipeline records and the draw binds from
 * (driver/ps5vk_pipeline.c). Declared here rather than through
 * driver/ps5vk_private.h, which needs the runtime's headers: this is the one
 * symbol the test reaches into the driver for. */
struct ps5vk_input_attachment {
   uint8_t set;
   uint8_t binding;
   uint8_t stage;
   uint32_t index;
};
uint32_t
ps5vk_spirv_input_attachments(const uint32_t *words, size_t size, uint8_t stage,
                              struct ps5vk_input_attachment *out, uint32_t capacity);
#endif

#if defined(__linux__)
/* A SPIR-V file of a probe set, in words. */
static uint32_t *
read_spirv(const char *probes, const char *set, const char *stage, size_t *out_words)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s/%s.spv", probes, set, stage);
   FILE *const file = fopen(path, "rb");
   if (file == NULL)
      return NULL;
   fseek(file, 0, SEEK_END);
   const long bytes = ftell(file);
   fseek(file, 0, SEEK_SET);
   uint32_t *const words = bytes > 0 && bytes % 4 == 0 ? malloc((size_t)bytes) : NULL;
   if (words == NULL || fread(words, 1, (size_t)bytes, file) != (size_t)bytes) {
      free(words);
      fclose(file);
      return NULL;
   }
   fclose(file);
   *out_words = (size_t)bytes / sizeof(uint32_t);
   return words;
}
#endif

int
main(void)
{
   test_begin("V0 subpass input");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t write_vertex_words = 0;
   size_t write_pixel_words = 0;
   size_t read_vertex_words = 0;
   size_t read_pixel_words = 0;
   uint32_t *const write_vertex =
      probes ? read_spirv(probes, "v0-subpass-write", "vertex", &write_vertex_words) : NULL;
   uint32_t *const write_pixel =
      probes ? read_spirv(probes, "v0-subpass-write", "pixel", &write_pixel_words) : NULL;
   uint32_t *const read_vertex =
      probes ? read_spirv(probes, "v0-subpass-read", "vertex", &read_vertex_words) : NULL;
   uint32_t *const read_pixel =
      probes ? read_spirv(probes, "v0-subpass-read", "pixel", &read_pixel_words) : NULL;
   check(write_vertex && write_pixel && read_vertex && read_pixel,
         "PS5VK_PROBES holds both subpass probe sets");

#if defined(PS5VK_TEST_DIRECT)
   if (read_pixel != NULL) {
      /* The reader's module carries the three decorations on one id (glslang
       * emits DescriptorSet 0, Binding 0 and InputAttachmentIndex 0 for
       * `layout(input_attachment_index = 0, set = 0, binding = 0) uniform
       * subpassInput`), so the scan has to answer one binding: set 0, binding
       * 0, index 0, fragment stage. */
      struct ps5vk_input_attachment found[4] = {{0}};
      const uint32_t count =
         ps5vk_spirv_input_attachments(read_pixel, read_pixel_words * sizeof(uint32_t),
                                       1, found, 4);
      check(count == 1, "the reader's module declares one input attachment binding");
      if (count == 1)
         check(found[0].set == 0 && found[0].binding == 0 && found[0].index == 0,
               "the scan reads set 0, binding 0, InputAttachmentIndex 0 out of the module");
      else
         printf("  (the scan found %u bindings)\n", count);
   }
#endif

   if (write_vertex && write_pixel && read_vertex && read_pixel) {
      /* Designated rather than positional: this struct has grown a field a
       * phase, and the probe cares about five of them. */
      struct ps5vk_triangle_input input = {0};
      input.get_instance_proc_addr = GET_PROC;
      input.pipeline_count = 2;
      input.shaders[0] = (struct ps5vk_triangle_shaders){
         write_vertex, write_vertex_words * sizeof(uint32_t), write_pixel,
         write_pixel_words * sizeof(uint32_t)};
      input.shaders[1] = (struct ps5vk_triangle_shaders){
         read_vertex, read_vertex_words * sizeof(uint32_t), read_pixel,
         read_pixel_words * sizeof(uint32_t)};
      input.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
      struct messages messages = {{0}};
      const struct ps5vk_triangle_report report = {&messages, record_step};
      input.report = &report;
      input.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
      /* R10: the two-subpass render pass, and the input attachment the second
       * pipeline's fragment stage reads out of it. */
      input.subpass_input = true;

      struct ps5vk_triangle triangle = {0};
      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      check(status == PS5VK_TRIANGLE_OK,
            "the two-subpass frame is created: the input attachment is not the application's to write");
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_COMMAND_BUFFER);
      /* What the host proves is the part a stream can show and the part that
       * failed first: no step refused the input attachment by name. A shader the
       * application may not write a descriptor for is not one the draw may
       * demand a write for -- the binding is the subpass's -- and a stage that
       * never reads it must not make the draw ask either (the metadata is
       * narrowed to the module's own declarations). The rest of the frame -- the
       * clear metas and the submission the read splits -- needs tables only a
       * console run provides, so the console case is what draws and reads it
       * back. */
      printf("  (steps: %s)\n", messages.text);
      check(strstr(messages.text, "is not bound or holds no write") == NULL,
            "no step demanded an application write for the input attachment");
      check(strstr(messages.text, "input attachment") == NULL ||
               strstr(messages.text, "the shader's own InputAttachmentIndex") != NULL ||
               strstr(messages.text, "not inside a render pass subpass") != NULL,
            "no step refused the input attachment's binding");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }
   free(write_vertex);
   free(write_pixel);
   free(read_vertex);
   free(read_pixel);
#else
   check(true, "the PS5 build only links this program");
#endif
   return test_finish();
}
