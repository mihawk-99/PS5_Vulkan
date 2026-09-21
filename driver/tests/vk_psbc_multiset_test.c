/*
 * PS5 Vulkan driver - R7 test: one descriptor set layout per descriptor set.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * R7 (PS5_VULKAN_REQUESTSv2.md, docs/M5_PHASE_C.md); built and run directly by
 * tools/check-driver.sh (see ps5vk_test.h).
 *
 * The compiler is what used to be single-set: it built one flat set-0 table,
 * rejected any binding whose set was not 0, and handed RADV's ABI num_sets = 1,
 * while the ABI already declares one descriptor-set pointer per set bit
 * (src/amd/vulkan/radv_shader_args.c). probes/v0-multiset is a fragment stage
 * that reads a uniform block at set 0 binding 0 and an image sampler at set 1
 * binding 0 -- two sets that differ in kind -- so this program can ask for the
 * metadata and see both pointers, or the refusal.
 *
 * Only the direct build has the compiler in its address space: the loader build
 * links the Khronos loader, not the driver, and the console build has no probe
 * set to read. Both report the same pass with no checks, as every test with a
 * PC-only half does.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps5vk_test.h"

#if defined(PS5VK_TEST_DIRECT) && defined(__linux__)

#include "psbc_compile.h"

/* A probe set's SPIR-V file, in words: probes/v0-multiset/pixel.spv. */
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

/* The pipeline's fragment options, as driver/ps5vk_pipeline.c builds them, with
 * the caller's descriptor bindings (which is what R7 changes there). */
static PsbcResult
compile_pixel(const uint32_t *spirv, size_t bytes, const PsbcDescriptorBinding *bindings,
              uint32_t count, PsbcShaderOutput *output)
{
   PsbcCompileOptions options = {
      .target = PSBC_TARGET_PS5,
      .stage = PSBC_STAGE_FRAGMENT,
      .entrypoint = "main",
      .optimise = true,
      /* probes/v0-multiset is compiled with --address32-hi 2. */
      .address32_hi = 2,
   };
   if (count != 0)
      memcpy(options.descriptor_bindings, bindings, count * sizeof(bindings[0]));
   options.descriptor_binding_count = count;
   return psbc_compile_shader(spirv, bytes, &options, output);
}

#endif /* PS5VK_TEST_DIRECT && __linux__ */

int
main(void)
{
   test_begin("PSBC multi-set");
#if !defined(PS5VK_TEST_DIRECT)
   printf("  (the loader build links no compiler; the direct build is the one that checks this)\n");
#elif !defined(__linux__)
   printf("  (only the PC build can read a probe set's SPIR-V; the console runs the driver)\n");
#else
   const char *const probes = getenv("PS5VK_PROBES");
   size_t pixel_bytes = 0;
   uint32_t *const pixel =
      probes ? read_spirv(probes, "v0-multiset", "pixel", &pixel_bytes) : NULL;
   check(pixel != NULL, "PS5VK_PROBES holds the v0-multiset SPIR-V");
   if (pixel == NULL) {
      return test_finish();
   }

   /* probes/v0-multiset's two bindings, exactly as the probe set declares them:
    * a 16-byte uniform block in set 0 and an image sampler in set 1. */
   const PsbcDescriptorBinding bindings[2] = {
      {.set = 0, .binding = 0, .type = PSBC_DESCRIPTOR_UNIFORM_BUFFER,
       .array_size = 1, .offset = 0, .stride = 16},
      {.set = 1, .binding = 0, .type = PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,
       .array_size = 1, .offset = 0, .stride = 48},
   };

   PsbcShaderOutput output;
   memset(&output, 0, sizeof(output));
   PsbcResult result = compile_pixel(pixel, pixel_bytes, bindings, 2, &output);
   check(result == PSBC_RESULT_OK, "a fragment stage that reads two sets compiles");
   uint32_t two_set_sgprs = 0;
   if (result == PSBC_RESULT_OK) {
      const PsbcShaderMetadata *const metadata = &output.metadata;
      two_set_sgprs = metadata->user_sgpr_count;
      const bool both =
         metadata->descriptor_sets_valid[0] && metadata->descriptor_sets_valid[1];
      check(both, "the compiler reports a descriptor-set pointer for set 0 and set 1");
      check(!both || metadata->descriptor_sets_user_data_dword[0] !=
                        metadata->descriptor_sets_user_data_dword[1],
            "the two sets' pointers are two different user-data dwords");
      bool none_above = true;
      for (uint32_t set = 2; set < PSBC_MAX_DESCRIPTOR_SETS; ++set)
         none_above = none_above && !metadata->descriptor_sets_valid[set];
      check(none_above, "no set above 1 claims a pointer it was not given");
      check(metadata->descriptor_set0_valid &&
               metadata->descriptor_set0_user_data_dword ==
                  metadata->descriptor_sets_user_data_dword[0],
            "the v14 set-0 field names the same dword as the per-set array");
      check(metadata->descriptor_binding_count == 2 &&
               metadata->descriptor_bindings[0].set == 0 &&
               metadata->descriptor_bindings[1].set == 1,
            "the metadata carries both declared bindings and their sets");
      printf("  (set 0 at user-data dword %u, set 1 at %u, %u user SGPRs, %zu bytes of code)\n",
             metadata->descriptor_sets_user_data_dword[0],
             metadata->descriptor_sets_user_data_dword[1], metadata->user_sgpr_count,
             output.machine_code_size);
   } else {
      printf("  (the two-set compile returned %d)\n", (int)result);
   }

   /* What the wrapper's set cap is *not*: a measurement. The real bound is the
    * ABI's user data -- one dword per set pointer in this build's 32-bit-pointer
    * form, out of the 32 user SGPRs a non-compute stage has -- so the third set
    * is declared here and must cost one user-data dword, no more than two if the
    * ABI ever asks for a 64-bit pointer. The shader reads two sets, so this is
    * also the case of a set the caller declares and the shader never
    * dereferences: the wrapper still names it in the mask, so it gets a pointer. */
   PsbcDescriptorBinding third[3] = {bindings[0], bindings[1],
                                     {.set = 2, .binding = 0,
                                      .type = PSBC_DESCRIPTOR_UNIFORM_BUFFER,
                                      .array_size = 1, .offset = 0, .stride = 16}};
   memset(&output, 0, sizeof(output));
   result = compile_pixel(pixel, pixel_bytes, third, 3, &output);
   check(result == PSBC_RESULT_OK, "a third declared set compiles");
   if (result == PSBC_RESULT_OK) {
      const PsbcShaderMetadata *const metadata = &output.metadata;
      check(metadata->descriptor_sets_valid[2],
            "a set the caller declared and the shader does not read still gets a pointer");
      const uint32_t per_set =
         metadata->user_sgpr_count >= two_set_sgprs
            ? metadata->user_sgpr_count - two_set_sgprs
            : 0;
      check(per_set == 1 || per_set == 2,
            "one more set costs one user-data dword (two for a 64-bit pointer ABI)");
      printf("  (three sets: %u user SGPRs, so %u per set pointer)\n",
             metadata->user_sgpr_count, per_set);
   } else {
      printf("  (the three-set compile returned %d)\n", (int)result);
   }

   /* The other half of the rule the compiler now enforces: a set index past the
    * wrapper's own cap is refused, and so is a pair of sets whose tables need
    * more binding slots than the layout blob holds (set 0's highest binding is
    * 64 -- 65 slots -- and set 1's is 63 -- 64 -- 129 together). Both are
    * refusals before any lowering, which the standalone path reports as an
    * internal error: the caller passed options it cannot honour. */   PsbcDescriptorBinding past_cap[1] = {bindings[0]};
   past_cap[0].set = PSBC_MAX_DESCRIPTOR_SETS;
   memset(&output, 0, sizeof(output));
   result = compile_pixel(pixel, pixel_bytes, past_cap, 1, &output);
   check(result == PSBC_RESULT_INTERNAL_ERROR,
         "a binding in a set past the wrapper's cap is refused, not truncated");

   PsbcDescriptorBinding over_budget[2] = {bindings[0], bindings[1]};
   over_budget[0].binding = 64;
   over_budget[1].binding = 63;
   memset(&output, 0, sizeof(output));
   result = compile_pixel(pixel, pixel_bytes, over_budget, 2, &output);
   check(result == PSBC_RESULT_INTERNAL_ERROR,
         "two sets whose tables need 129 slots are refused, not overlapped");

   free(pixel);
#endif
   return test_finish();
}
