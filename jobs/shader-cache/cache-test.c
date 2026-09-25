/* Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ps5vk_shader_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
   uint32_t words[] = {0x07230203, 0x10000, 0, 1, 0};
   uint32_t value = 7, value_copy = 7;
   PsbcSpecializationEntry spec = {3, 0, sizeof(value)}, spec_copy = spec;
   PsbcCompileOptions options = {
      .target = PSBC_TARGET_PS5, .stage = PSBC_STAGE_VERTEX, .entrypoint = "main",
      .optimise = true, .vertex_attribute_count = 1, .descriptor_binding_count = 1,
      .specialization_entry_count = 1, .specialization_entries = &spec,
      .specialization_data_size = sizeof(value), .specialization_data = &value,
   };
   struct ps5vk_shader_cache_key key, other;
   assert(ps5vk_shader_cache_key(words, sizeof(words), &options, &key));
   options.specialization_data = &value_copy;
   options.specialization_entries = &spec_copy;
   assert(ps5vk_shader_cache_key(words, sizeof(words), &options, &other));
   assert(!memcmp(key.digest, other.digest, 32));
#define CHANGED(change, restore) do { \
   change; \
   assert(ps5vk_shader_cache_key(words, sizeof(words), &options, &other)); \
   assert(memcmp(key.digest, other.digest, 32)); \
   restore; \
} while (0)
   CHANGED(value_copy++, value_copy--);
   CHANGED(spec_copy.constant_id++, spec_copy.constant_id--);
   CHANGED(words[2]++, words[2]--);
   CHANGED(options.entrypoint = "other", options.entrypoint = "main");
   CHANGED(options.optimise = false, options.optimise = true);
   CHANGED(options.stage = PSBC_STAGE_COMPUTE, options.stage = PSBC_STAGE_VERTEX);
   CHANGED(options.address32_hi++, options.address32_hi--);
   CHANGED(options.vertex_attributes[0].stride++, options.vertex_attributes[0].stride--);
   CHANGED(options.descriptor_bindings[0].offset++, options.descriptor_bindings[0].offset--);
   CHANGED(options.spi_shader_col_format++, options.spi_shader_col_format--);
#undef CHANGED
   unlink(key.path);
   PsbcShaderOutput output = {0};
   assert(!ps5vk_shader_cache_load(&key, &output));
   const char data[] = "wrapper", code[] = "machine code";
   PsbcShaderOutput source = {.data = (void *)data, .size = sizeof(data),
      .machine_code = (void *)code, .machine_code_size = sizeof(code),
      .metadata = {.version = PSBC_SHADER_METADATA_VERSION}};
   ps5vk_shader_cache_store(&key, &source);
   /* R80: the file is written by the cache's writer thread; a stored output
    * loads at once, from memory until its file is in place, and from the file
    * after the flush a device's destruction does. */
   for (int from_file = 0; from_file < 2; from_file++) {
      if (from_file)
         ps5vk_shader_cache_flush();
      assert(ps5vk_shader_cache_load(&key, &output));
      assert(output.size == sizeof(data) && output.machine_code_size == sizeof(code));
      assert(!memcmp(output.data, data, sizeof(data)) && !memcmp(output.machine_code, code, sizeof(code)));
      assert(!memcmp(&output.metadata, &source.metadata, sizeof(source.metadata)));
      psbc_free_output(&output);
   }
   FILE *file = fopen(key.path, "r+b"); assert(file);
   assert(fseek(file, -1, SEEK_END) == 0); assert(fputc(42, file) != EOF); fclose(file);
   assert(!ps5vk_shader_cache_load(&key, &output));
   assert(!output.data && !output.machine_code);
   file = fopen(key.path, "wb"); assert(file); fputs("truncated", file); fclose(file);
   assert(!ps5vk_shader_cache_load(&key, &output));
   ps5vk_shader_cache_store(&key, &source);
   assert(ps5vk_shader_cache_load(&key, &output)); psbc_free_output(&output);
   ps5vk_shader_cache_flush();
   unlink(key.path);
   puts("PASS: stable keys, input/option invalidation, exact output, corruption/truncation recovery");
   return 0;
}
