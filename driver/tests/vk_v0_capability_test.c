/*
 * PS5 Vulkan driver - R10 test: a shader the compiler cannot lower is refused,
 * not fatal.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Port request R10, second ask (docs/REQUESTS_RESPONSE.md, docs/M5_PHASE_C.md).
 * A console title died minutes into start-up on a shader this fork's compiler
 * could not lower: the SPIR-V front end printed "SPIRV parsing FAILED" or ACO
 * printed "Unimplemented intrinsic" and then the process was gone -- no
 * VkResult, no refusal sentence, nothing the application could act on. The
 * driver now turns both into a refusal (driver/ps5vk_pipeline.c,
 * ps5vk_compute.c), and this program is the measurement, run on the host
 * without a console cycle:
 *
 *   1. the m2 pixel shader, unmodified: created, so the harness and the
 *      compiler work at all;
 *   2. the same module with OpCapability PhysicalStorageBufferAddressesEXT
 *      (4472) added: vkCreateGraphicsPipelines returns VK_ERROR_UNKNOWN with a
 *      sentence naming the capability -- and **stderr is empty**, which is what
 *      says the compiler never ran (a refusal after it ran would carry the
 *      compiler's own error text);
 *   3. the same module with OpCapability StorageImageWriteWithoutFormat (46)
 *      added: created, because this compiler does lower that one. The table is
 *      measured per capability rather than a blanket "declares something";
 *   4. the same module with OpCapability PhysicalStorageBufferAddresses (5347)
 *      added *and the logical addressing model left alone*: created, because
 *      that is the measurement behind the check -- the front end rejects the
 *      addressing model, not the declaration;
 *   5. the same module with its OpMemoryModel addressing model changed to
 *      PhysicalStorageBuffer64 (5348): refused by name before the compiler
 *      runs. This is the port's own skinning/mesh-interpolate/lightmap shape;
 *   6. and the guard behind all of it: a module whose addressing model the
 *      driver does not pre-check because no capability announces it -- an
 *      unknown capability 999, which the front end fails on -- comes back as
 *      VK_ERROR_UNKNOWN with a sentence naming the shader's declared
 *      capabilities, the process still alive, and the compiler's own
 *      "SPIR-V parsing FAILED" on stderr where it belongs.
 *
 * The console half is src/diagnostics.cpp's v0-capability case: the same
 * injected modules through a pipeline on the console, where the abort used to
 * kill the title. PS5VK_PROBES names the probes directory; the PS5 build only
 * links, it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

/* The SPIR-V opcodes this test writes into a module, and the capability values
 * it adds: the driver's own constants are private to the driver, so the test
 * states them where a reader can see what it injects (SPIR-V 1.0, chapter 3). */
#define CAPABILITY_OPCODE 17u
#define MEMORY_MODEL_OPCODE 14u
#define CAPABILITY_SHADER 1u
#define CAPABILITY_SAMPLED_BUFFER 46u
#define CAPABILITY_PHYSICAL_STORAGE_BUFFER_ADDRESSES 5347u
#define CAPABILITY_PHYSICAL_STORAGE_BUFFER_ADDRESSES_EXT 4472u
#define CAPABILITY_UNKNOWN 999u
#define ADDRESSING_MODEL_LOGICAL 0u
#define ADDRESSING_MODEL_PHYSICAL_STORAGE_BUFFER_64 5348u

/* The first failed step of a creation, and the first driver message: the
 * sentence is what says which rule refused the shader. */
struct steps {
   const char *failed;
   int result;
   char message[256];
};

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   struct steps *const steps = context;
   if (passed)
      return;
   if (strcmp(name, "vk_message") == 0) {
      printf("  (driver: %s)\n", detail);
      if (steps->message[0] == '\0')
         snprintf(steps->message, sizeof(steps->message), "%s", detail);
      return;
   }
   printf("  (%s failed: VkResult %d%s%s)\n", name, result, detail[0] ? ", " : "", detail);
   if (!steps->failed) {
      steps->failed = name;
      steps->result = result;
   }
}

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

/* A copy of words with one capability instruction added after the module's
 * first OpCapability: the SPIR-V specification's own order, where every
 * capability comes before anything but OpSource, and the first instruction of
 * every module this repository compiles is OpCapability Shader. */
static uint32_t *
with_capability(const uint32_t *words, size_t count, uint32_t capability, size_t *out_words)
{
   uint32_t *const copy = malloc((count + 2) * sizeof(uint32_t));
   if (copy == NULL)
      return NULL;
   memcpy(copy, words, 7 * sizeof(uint32_t));
   copy[7] = (2u << 16) | CAPABILITY_OPCODE;
   copy[8] = capability;
   memcpy(copy + 9, words + 7, (count - 7) * sizeof(uint32_t));
   *out_words = count + 2;
   return copy;
}

/* A copy of words whose OpMemoryModel names model as its addressing model, or
 * NULL for a module that has none. */
static uint32_t *
with_addressing_model(const uint32_t *words, size_t count, uint32_t model, size_t *out_words)
{
   uint32_t *const copy = malloc(count * sizeof(uint32_t));
   if (copy == NULL)
      return NULL;
   memcpy(copy, words, count * sizeof(uint32_t));
   for (size_t at = 5; at < count;) {
      const uint32_t word_count = copy[at] >> 16;
      const uint32_t opcode = copy[at] & 0xffff;
      if (word_count == 0 || word_count > count - at) {
         free(copy);
         return NULL;
      }
      if (opcode == MEMORY_MODEL_OPCODE && word_count >= 3) {
         copy[at + 1] = model;
         *out_words = count;
         return copy;
      }
      at += word_count;
   }
   free(copy);
   return NULL;
}

/* stderr, captured around one creation: the compiler writes its own error text
 * there (and nothing else does), so an empty capture is what says it never
 * ran. */
struct stderr_capture {
   int saved;
   char path[64];
};

static bool
capture_begin(struct stderr_capture *capture)
{
   snprintf(capture->path, sizeof(capture->path), "/tmp/ps5vk-capability-XXXXXX");
   const int file = mkstemp(capture->path);
   if (file < 0)
      return false;
   fflush(stderr);
   capture->saved = dup(STDERR_FILENO);
   if (capture->saved < 0) {
      close(file);
      return false;
   }
   if (dup2(file, STDERR_FILENO) < 0) {
      close(file);
      close(capture->saved);
      return false;
   }
   close(file);
   return true;
}

static size_t
capture_end(struct stderr_capture *capture, char *out, size_t out_size)
{
   fflush(stderr);
   dup2(capture->saved, STDERR_FILENO);
   close(capture->saved);
   size_t used = 0;
   FILE *const file = fopen(capture->path, "rb");
   if (file != NULL) {
      used = fread(out, 1, out_size - 1, file);
      fclose(file);
   }
   out[used] = '\0';
   unlink(capture->path);
   return used;
}
#endif /* __linux__ */

/* One module to create a pipeline from, what the creation must do with it, and
 * what the compiler must have written while it did. */
struct capability_case {
   const char *what;
   uint32_t *module;
   size_t words;
   /* NULL when the pipeline is created; otherwise a phrase the refusal's
    * sentence must hold. */
   const char *expect_message;
   /* NULL when the compiler must not have run (stderr empty); otherwise a
    * phrase its own failure must have written there. */
   const char *expect_stderr;
};

int
main(void)
{
   test_begin("V0 capability refusal");
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   size_t vertex_words = 0;
   size_t pixel_words = 0;
   uint32_t *const vertex = probes ? read_spirv(probes, "m2", "vertex", &vertex_words) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m2", "pixel", &pixel_words) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m2 SPIR-V");

   if (vertex && pixel) {
      size_t counts[5] = {0};
      uint32_t *const modules[5] = {
         with_capability(pixel, pixel_words, CAPABILITY_PHYSICAL_STORAGE_BUFFER_ADDRESSES_EXT,
                         &counts[0]),
         with_capability(pixel, pixel_words, CAPABILITY_SAMPLED_BUFFER, &counts[1]),
         with_capability(pixel, pixel_words, CAPABILITY_PHYSICAL_STORAGE_BUFFER_ADDRESSES,
                         &counts[2]),
         with_addressing_model(pixel, pixel_words, ADDRESSING_MODEL_PHYSICAL_STORAGE_BUFFER_64,
                               &counts[3]),
         with_capability(pixel, pixel_words, CAPABILITY_UNKNOWN, &counts[4]),
      };
      check(modules[0] && modules[1] && modules[2] && modules[3] && modules[4],
            "every injected module was built");

      const struct capability_case cases[] = {
         {"the m2 pixel shader as it stands", pixel, pixel_words, NULL, NULL},
         {"with PhysicalStorageBufferAddressesEXT added", modules[0], counts[0],
          "PhysicalStorageBufferAddressesEXT", NULL},
         {"with SampledBuffer added, a capability this compiler warns about and lowers",
          modules[1], counts[1], NULL, "SPIR-V WARNING"},
         {"with PhysicalStorageBufferAddresses declared, logical addressing", modules[2],
          counts[2], NULL, "SPIR-V WARNING"},
         {"with the PhysicalStorageBuffer64 addressing model", modules[3], counts[3],
          "PhysicalStorageBuffer64", NULL},
         {"with an unknown capability, which the front end fails on", modules[4], counts[4],
          "aborted", "SPIR-V parsing FAILED"},
      };
      for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
         struct steps steps = {0};
         const struct ps5vk_triangle_report report = {&steps, record_step};
         struct ps5vk_triangle_input input = {
            GET_PROC, 1,
            {{vertex, vertex_words * sizeof(uint32_t), cases[at].module,
              cases[at].words * sizeof(uint32_t)},
             {NULL, 0, NULL, 0}},
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
            NULL, 0, 0, NULL, 0, 0, {{0}}, false, NULL, 0, 0,
         };
         /* The compiler writes its own error text to stderr and nothing else
          * does, so what it holds is what says whether the compiler ran. */
         struct stderr_capture capture = {.saved = -1};
         const bool captured = cases[at].module != NULL && capture_begin(&capture);
         struct ps5vk_triangle triangle = {0};
         const enum ps5vk_triangle_status status =
            cases[at].module != NULL ? ps5vk_triangle_create(&triangle, &input)
                                     : PS5VK_TRIANGLE_FAILED;
         char compiler[1024] = "";
         if (captured)
            capture_end(&capture, compiler, sizeof(compiler));
         if (status != PS5VK_TRIANGLE_IN_FLIGHT)
            ps5vk_triangle_finish(&triangle);

         printf("  %s\n", cases[at].what);
         if (cases[at].expect_message == NULL) {
            check(status == PS5VK_TRIANGLE_OK,
                  "a shader this compiler lowers is created: the refusal is per measured "
                  "capability and not a blanket one");
         } else {
            const bool named = status == PS5VK_TRIANGLE_FAILED && steps.failed != NULL &&
                               strstr(steps.message, cases[at].expect_message) != NULL;
            check(named, "the creation returns an error and a sentence naming the limit");
            if (!named)
               printf("  (status %d, sentence: %s)\n", (int)status, steps.message);
         }
         if (cases[at].expect_stderr == NULL) {
            /* The compiler writes its warnings and errors as "SPIR-V ..." text
             * and nothing else here does (the host AGC shim's own line is
             * "MESA: error: ..."), so its absence is what says the compiler
             * never ran: the refusal came first. */
            const bool silent = strstr(compiler, "SPIR-V") == NULL;
            check(silent, "the compiler never ran: no SPIR-V text reached stderr");
            if (!silent)
               printf("  (stderr held: %.200s)\n", compiler);
         } else {
            check(strstr(compiler, cases[at].expect_stderr) != NULL,
                  "the compiler's own failure is on stderr, and the driver still returned a "
                  "result: the process is still here");
            if (strstr(compiler, cases[at].expect_stderr) == NULL)
               printf("  (stderr held: %.200s)\n", compiler);
         }
      }
      for (size_t at = 0; at < sizeof(modules) / sizeof(modules[0]); at++)
         free(modules[at]);
   }
   free(vertex);
   free(pixel);
#else
   (void)record_step;
   check(true, "the PS5 build only links this program");
#endif
   return test_finish();
}
