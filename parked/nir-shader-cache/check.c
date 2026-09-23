/* Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ps5vk_private.h"
#include "ps5vk_shader_cache.h"
#include "compiler/nir/nir.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
   assert(argc == 2);
   nir_shader *nir = ps5vk_nir_noop_fragment();
   assert(nir);
   PsbcCompileOptions options = {.target = PSBC_TARGET_PS5, .stage = PSBC_STAGE_FRAGMENT,
      .entrypoint = "main", .optimise = true, .rasterization_samples = 1};
   struct ps5vk_shader_cache_key key, other;
   if (getenv("PS5VK_SHADER_CACHE_DIR") && *getenv("PS5VK_SHADER_CACHE_DIR")) {
      assert(ps5vk_shader_cache_nir_key(nir, &options, &key));
      nir_shader *clone = nir_shader_clone(NULL, nir);
      assert(clone);
      clone->info.name = ralloc_strdup(clone, "different debug name and address");
      assert(ps5vk_shader_cache_nir_key(clone, &options, &other));
      assert(!memcmp(key.digest, other.digest, sizeof(key.digest)));
      clone->info.fs.uses_discard = true;
      assert(ps5vk_shader_cache_nir_key(clone, &options, &other));
      assert(memcmp(key.digest, other.digest, sizeof(key.digest)));
      options.rasterization_samples = 4;
      assert(ps5vk_shader_cache_nir_key(nir, &options, &other));
      assert(memcmp(key.digest, other.digest, sizeof(key.digest)));
      options.rasterization_samples = 1;
      ps5vk_nir_free(clone);
      printf("key ");
      for (unsigned i = 0; i < sizeof(key.digest); i++) printf("%02x", key.digest[i]);
      puts("");
   }
   PsbcShaderOutput output = {0};
   bool aborted = false;
   assert(ps5vk_compile_shader_deep(nir, NULL, 0, &options, &output, &aborted) == PSBC_RESULT_OK);
   assert(!aborted && output.machine_code_size);
   FILE *file = fopen(argv[1], "wb"); assert(file);
   assert(fwrite(&output.metadata, sizeof(output.metadata), 1, file) == 1);
   assert(fwrite(output.machine_code, 1, output.machine_code_size, file) == output.machine_code_size);
   assert(fwrite(output.data, 1, output.size, file) == output.size);
   assert(!fclose(file));
   psbc_free_output(&output); ps5vk_nir_free(nir);
   puts("PASS: NIR keys and compiler output");
   return 0;
}
