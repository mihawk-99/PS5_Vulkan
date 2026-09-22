/*
 * PS5 Vulkan driver - R9 test: specialization constants reach the compiler.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R9 (the port's PS5_VULKAN_REQUESTS.md, docs/M5_PHASE_C.md). Only the direct
 * build has the compiler in its address space, so this program is a compiler-level
 * test: it compiles probes/v0-spec's fragment stage three ways through the same
 * entry the driver calls (psbc_compile_shader) and compares what comes out.
 *
 *   A. no specialization entries at all: the shader's own defaults;
 *   B. the application's VkSpecializationInfo, selecting tint_red = false and
 *      level = 1, handed over as the driver hands it over;
 *   C. probes/v0-spec-hardcoded, the same expression with those values written as
 *      literals.
 *
 * The claim is that B differs from A -- the constants are applied rather than
 * ignored -- and that B equals C, so the specialized module is the module the
 * values describe rather than merely a different one. The whole output is
 * compared, code bytes and metadata, and the register-count field is reported
 * either way so a difference says where it is.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps5vk_test.h"

#if defined(PS5VK_TEST_DIRECT) && defined(__linux__)
#include "psbc_compile.h"

/* A probe set's SPIR-V file, in words. */
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

/* The pipeline's fragment options, as driver/ps5vk_pipeline.c builds them: no
 * descriptors, and the caller's specialization info when it has one. */
static PsbcResult
compile_pixel(const uint32_t *spirv, size_t bytes, const VkSpecializationInfo *specialization,
              PsbcShaderOutput *output)
{
   PsbcCompileOptions options = {
      .target = PSBC_TARGET_PS5,
      .stage = PSBC_STAGE_FRAGMENT,
      .entrypoint = "main",
      .optimise = true,
      /* probes/v0-spec is compiled with --address32-hi 2. */
      .address32_hi = 2,
   };
   if (specialization != NULL) {
      options.specialization_entry_count = specialization->mapEntryCount;
      options.specialization_entries = (const PsbcSpecializationEntry *)specialization->pMapEntries;
      options.specialization_data_size = specialization->dataSize;
      options.specialization_data = specialization->pData;
   }
   return psbc_compile_shader(spirv, bytes, &options, output);
}

/* Whether two compiled stages are the same shader: the machine code first, since
 * that is what runs, and then the metadata the driver reads. */
static bool
same_stage(const PsbcShaderOutput *a, const PsbcShaderOutput *b)
{
   return a->machine_code_size == b->machine_code_size &&
          memcmp(a->machine_code, b->machine_code, a->machine_code_size) == 0 &&
          a->metadata.shader_register_count == b->metadata.shader_register_count &&
          a->metadata.user_sgpr_count == b->metadata.user_sgpr_count;
}
#endif /* PS5VK_TEST_DIRECT && __linux__ */

int
main(void)
{
   test_begin("V0 specialization constants");
#if !defined(PS5VK_TEST_DIRECT)
   printf("  (the loader build links no compiler; the direct build is the one that checks this)\n");
#elif !defined(__linux__)
   printf("  (only the PC build can read a probe set's SPIR-V; the console runs the driver)\n");
#else
   const char *const probes = getenv("PS5VK_PROBES");
   size_t spec_bytes = 0;
   size_t hard_bytes = 0;
   uint32_t *const spec = probes ? read_spirv(probes, "v0-spec", "pixel", &spec_bytes) : NULL;
   uint32_t *const hard =
      probes ? read_spirv(probes, "v0-spec-hardcoded", "pixel", &hard_bytes) : NULL;
   check(spec && hard, "PS5VK_PROBES holds the v0-spec and v0-spec-hardcoded SPIR-V");
   if (spec == NULL || hard == NULL) {
      free(spec);
      free(hard);
      return test_finish();
   }

   /* tint_red = false is the shader's own default; level = 1 is not, so the
    * green channel is what says the value arrived. */
   const VkBool32 values[2] = {VK_FALSE, 1};
   const VkSpecializationMapEntry entries[2] = {
      {.constantID = 0, .offset = 0, .size = sizeof(VkBool32)},
      {.constantID = 1, .offset = sizeof(VkBool32), .size = sizeof(int32_t)},
   };
   const VkSpecializationInfo specialization = {
      .mapEntryCount = 2,
      .pMapEntries = entries,
      .dataSize = sizeof(values),
      .pData = values,
   };

   PsbcShaderOutput defaults;
   PsbcShaderOutput specialized;
   PsbcShaderOutput hardcoded;
   memset(&defaults, 0, sizeof(defaults));
   memset(&specialized, 0, sizeof(specialized));
   memset(&hardcoded, 0, sizeof(hardcoded));
   const PsbcResult a = compile_pixel(spec, spec_bytes, NULL, &defaults);
   const PsbcResult b = compile_pixel(spec, spec_bytes, &specialization, &specialized);
   const PsbcResult c = compile_pixel(hard, hard_bytes, NULL, &hardcoded);
   check(a == PSBC_RESULT_OK && b == PSBC_RESULT_OK && c == PSBC_RESULT_OK,
         "the same module compiles with no constants, with the pipeline's, and hard-coded");
   printf("  (registers: defaults %u, specialized %u, hard-coded %u; code bytes %zu, %zu, %zu)\n",
          (unsigned)defaults.metadata.shader_register_count,
          (unsigned)specialized.metadata.shader_register_count,
          (unsigned)hardcoded.metadata.shader_register_count, defaults.machine_code_size,
          specialized.machine_code_size, hardcoded.machine_code_size);
   check(!same_stage(&defaults, &specialized),
         "the values the pipeline passes change the compiled stage");
   check(same_stage(&specialized, &hardcoded),
         "the specialized stage is the stage the same values written as literals compile to");
#endif
   return test_finish();
}
