/*
 * PS5 Vulkan driver - Phase D2 test: a compute dispatch through the API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase D2 (docs/M5_REFERENCE.md); built and run through the loader
 * and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Runs the compute program driver/tests/ps5vk_compute.c over the V0-compute
 * probe's shader (probes/c0/dispatch.spv), which writes 0xa5a5a5a5 over the
 * storage buffer it is bound: one host-visible buffer, one descriptor, one
 * compute pipeline from the caller's SPIR-V, one dispatch. Nothing executes on
 * the PC, so this program's job is the stream -- that the compute pipeline is
 * accepted, that the dispatch records, submits and signals its fence, and that
 * the readback is the buffer's own contents -- while the console's readback is
 * the word the shader wrote (the runner's d2-compute case keeps that check).
 * tools/check-driver.sh compares the submission with the console's own run of
 * it (golden/d2-compute). PS5VK_PROBES names the probes directory. The PS5 build
 * only links; it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "ps5vk_compute.h"
#include "ps5vk_test.h"

/* The word the probe's shader writes and the buffer starts at. */
#define PS5VK_COMPUTE_INITIAL_WORD UINT32_C(0)
#define PS5VK_COMPUTE_EXPECTED_WORD UINT32_C(0xa5a5a5a5)

/* The first failed step of the program, apart from driver messages. */
struct steps
{
   const char *failed;
   int result;
};

static void record_step(void *context, const char *name, bool passed, int result,
                  const char *detail)
{
   struct steps *const steps = context;
   if (passed)
      return;
   if (strcmp(name, "vk_message") == 0)
   {
      printf("  (driver: %s)\n", detail);
      return;
   }
   printf("  (%s failed: VkResult %d%s%s)\n", name, result, detail[0] ? ", " : "", detail);
   if (!steps->failed)
   {
      steps->failed = name;
      steps->result = result;
   }
}

#if defined(__linux__)
/* A SPIR-V file, in words. */
static uint32_t *read_spirv(const char *probes, const char *file, size_t *bytes)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s", probes, file);
   FILE *const stream = fopen(path, "rb");
   if (!stream)
      return NULL;
   fseek(stream, 0, SEEK_END);
   const long length = ftell(stream);
   fseek(stream, 0, SEEK_SET);
   uint32_t *words = length > 0 && length % 4 == 0 ? malloc((size_t)length) : NULL;
   if (words && fread(words, 1, (size_t)length, stream) != (size_t)length)
   {
      free(words);
      words = NULL;
   }
   fclose(stream);
   *bytes = words ? (size_t)length : 0;
   return words;
}
#endif

int main(void)
{
   test_begin("D2 compute");
   size_t spirv_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const spirv = probes ? read_spirv(probes, "c0/dispatch.spv", &spirv_bytes) : NULL;
   check(spirv != NULL, "PS5VK_PROBES holds the dispatch SPIR-V");
#else
   uint32_t *const spirv = NULL;
#endif

   if (spirv != NULL)
   {
      struct steps steps = {0};
      const struct ps5vk_compute_input input = {
         .get_instance_proc_addr = GET_PROC,
         .spirv = spirv,
         .spirv_bytes = spirv_bytes,
         .entry_point = "main",
         .initial_word = PS5VK_COMPUTE_INITIAL_WORD,
         .expected_word = PS5VK_COMPUTE_EXPECTED_WORD,
         .report = {&steps, record_step},
      };
      /* Two dispatches, as the console's case runs them: vkCmdDispatch over the
       * counts the command names, then vkCmdDispatchIndirect over the counts the
       * buffer holds. */
      for (unsigned frame = 0; frame < 2; frame++)
      {
         struct ps5vk_compute_input run = input;
         run.indirect = frame == 1;
         struct ps5vk_compute compute;
         const bool ran = ps5vk_compute_run(&run, &compute);
         check(ran, frame == 1 ? "the indirect dispatch creates, records, submits and waits"
                               : "the compute program creates, records, submits and waits");
         check(steps.failed == NULL, "no step of the dispatch failed before its fence");
         if (ran)
         {
            /* The PC's host layer completes the submission without running the
               workgroup, so the readback there is the buffer's own contents; the
               console's is the word the shader wrote, which its own case checks. */
            check(compute.result_word == PS5VK_COMPUTE_EXPECTED_WORD ||
                     compute.result_word == PS5VK_COMPUTE_INITIAL_WORD,
                  "the readback is the shader's word or the buffer's own");
         }
         ps5vk_compute_finish(&compute);
      }
   }

   free(spirv);
   (void)spirv_bytes;
   return test_finish();
}
