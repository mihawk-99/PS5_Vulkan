/*
 * PS5 Vulkan driver - compute image tables and dispatch regression.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host checks encoding only. The console's d2-compute-images case uses the
 * same harness and requires all 256 output texels to match the shader.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "ps5vk_compute.h"
#include "../ps5vk_debug.h"
#include "ps5vk_test.h"

#if defined(__linux__)
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
   test_begin("R17 descriptor array");
#if defined(__linux__)
   size_t bytes = 0;
   const char *probes = getenv("PS5VK_PROBES");
   uint32_t *spirv = probes ? read_spirv(probes, "r17-descriptor-array/dispatch.spv", &bytes) : NULL;
   check(spirv != NULL, "texture-to-storage-image SPIR-V exists");
   if (spirv) {
      struct steps steps = {0};
      const struct ps5vk_compute_input input = {
         .get_instance_proc_addr = GET_PROC, .spirv = spirv, .spirv_bytes = bytes,
         .entry_point = "main", .images = true, .descriptor_array = true, .report = {&steps, record_step},
      };
      struct ps5vk_compute compute;
      const bool ran = ps5vk_compute_run(&input, &compute);
      check(ran && steps.failed == NULL, "two sets create, dispatch once, copy back and signal");
      if (ran) {
         /* The host deliberately executes no shader: never call this a pixel pass. */
         check(compute.mismatched_texels == 256, "host execution leaves all 256 output texels unwritten");
#if defined(PS5VK_TEST_DIRECT)
         ps5vk_debug_table tables[2] = {{0}};
         const uint32_t count = ps5vk_debug_descriptor_tables(compute.device, tables, 2);
         check(count == 2, "compute writes one table per set");
         if (count == 2) {
            check(tables[0].set == 0 && tables[1].set == 1 &&
                     tables[0].user_data_dword != tables[1].user_data_dword,
                  "the two sets occupy distinct compiler-named user-data dwords");
            const uint32_t order[3] = {2, 0, 1};
            for (uint32_t element = 0; element < 3; element++) {
               size_t bytes = 0;
               const void *storage = ps5vk_debug_image_storage(compute.images[order[element]], &bytes);
               check(storage != NULL && tables[0].words[(element + 1) * 12] ==
                        (uint32_t)((uintptr_t)storage >> 8),
                     "array write/copy/partial update emits the distinct expected image address");
            }
            check(tables[0].words[0] == 0 && tables[0].words[8] != 0,
                  "separate sampler stays at binding zero before the image array");
         }
#endif
      }
      ps5vk_compute_finish(&compute);
   }
   free(spirv);
#endif
   return test_finish();
}
