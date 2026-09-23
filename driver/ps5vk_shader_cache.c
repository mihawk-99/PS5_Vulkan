/* Persistent compiler outputs, shared by graphics and compute.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Called under the existing compiler mutex. Save after each successful compile,
 * so a later application crash preserves completed work. Cache failures are
 * misses, never Vulkan errors. NIR meta shaders use Mesa serialization as input.
 */
#include "ps5vk_shader_cache.h"
#include "ps5vk_cache_build.h"
#include "util/detect_os.h"
#include "util/mesa-blake3.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_MAX_BYTES (16u * 1024u * 1024u)
struct cache_header {
   unsigned char key[32];
   unsigned char checksum[32];
   uint64_t data_bytes, code_bytes;
   PsbcShaderMetadata metadata;
};

bool
ps5vk_shader_cache_key(const uint32_t *words, size_t size, const PsbcCompileOptions *options,
                       struct ps5vk_shader_cache_key *key)
{
   const char *directory = getenv("PS5VK_SHADER_CACHE_DIR");
#if !DETECT_OS_LINUX
   if (directory == NULL)
      directory = "/app0/ps5vk-shader-cache";
#endif
   if (!directory || !*directory || !words || !size)
      return false;
   if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
      static bool warned;
      if (!warned) {
         fprintf(stderr, "[ps5vk] shader cache directory unavailable; compiling normally\n");
         warned = true;
      }
      return false;
   }
   struct mesa_blake3 hash;
   _mesa_blake3_init(&hash);
#if DETECT_OS_LINUX
   const char *build = PS5VK_CACHE_HOST_BUILD;
#else
   const char *build = PS5VK_CACHE_PS5_BUILD;
#endif
   _mesa_blake3_update(&hash, build, strlen(build));
   _mesa_blake3_update(&hash, &size, sizeof(size));
   _mesa_blake3_update(&hash, words, size);
   const char *entry = options->entrypoint ? options->entrypoint : "main";
   _mesa_blake3_update(&hash, entry, strlen(entry) + 1);
   /* Hash values, never process pointers or structure padding. */
#define FIELD(name) _mesa_blake3_update(&hash, &options->name, sizeof(options->name))
   FIELD(target); FIELD(stage); FIELD(optimise); FIELD(ngg);
   FIELD(omit_implicit_primitive_id); FIELD(primitive_id_per_primitive);
   FIELD(ps5_global_streamout); FIELD(ps5_global_primitive_query);
   FIELD(split_vertex_instances); FIELD(force_accelerated_dot); FIELD(primitive_type);
   FIELD(provoking_vtx_last); FIELD(flat_input_vertex_valid); FIELD(flat_input_vertex);
   FIELD(address32_hi); FIELD(vertex_attribute_count);
   for (uint32_t i = 0; i < options->vertex_attribute_count; i++) {
      FIELD(vertex_attributes[i].location); FIELD(vertex_attributes[i].binding);
      FIELD(vertex_attributes[i].format); FIELD(vertex_attributes[i].offset);
      FIELD(vertex_attributes[i].stride); FIELD(vertex_attributes[i].alignment);
      FIELD(vertex_attributes[i].instance_divisor);
   }
   FIELD(descriptor_binding_count);
   for (uint32_t i = 0; i < options->descriptor_binding_count; i++) {
      FIELD(descriptor_bindings[i].set); FIELD(descriptor_bindings[i].binding);
      FIELD(descriptor_bindings[i].type); FIELD(descriptor_bindings[i].array_size);
      FIELD(descriptor_bindings[i].offset); FIELD(descriptor_bindings[i].stride);
   }
   FIELD(rasterization_samples); FIELD(spi_shader_col_format);
   FIELD(color_is_int8); FIELD(color_is_int10); FIELD(gallium_buffer_arrays);
   FIELD(compute_private_buffer); FIELD(compute_buffer_spills);
   FIELD(specialization_entry_count); FIELD(specialization_data_size);
   for (uint32_t i = 0; i < options->specialization_entry_count; i++) {
      FIELD(specialization_entries[i].constant_id);
      FIELD(specialization_entries[i].offset); FIELD(specialization_entries[i].size);
   }
#undef FIELD
   if (options->specialization_data_size)
      _mesa_blake3_update(&hash, options->specialization_data, options->specialization_data_size);
   _mesa_blake3_final(&hash, key->digest);
   char hex[BLAKE3_HEX_LEN];
   _mesa_blake3_format(hex, key->digest);
   const int length = snprintf(key->path, sizeof(key->path), "%s/%s.bin", directory, hex);
   return length > 0 && (size_t)length < sizeof(key->path);
}

static void
checksum(const struct cache_header *header, const PsbcShaderOutput *output, unsigned char sum[32])
{
   struct mesa_blake3 hash;
   _mesa_blake3_init(&hash);
   _mesa_blake3_update(&hash, &header->data_bytes, sizeof(header->data_bytes));
   _mesa_blake3_update(&hash, &header->code_bytes, sizeof(header->code_bytes));
   _mesa_blake3_update(&hash, &header->metadata, sizeof(header->metadata));
   if (output->size)
      _mesa_blake3_update(&hash, output->data, output->size);
   _mesa_blake3_update(&hash, output->machine_code, output->machine_code_size);
   _mesa_blake3_final(&hash, sum);
}

bool
ps5vk_shader_cache_load(const struct ps5vk_shader_cache_key *key, PsbcShaderOutput *output)
{
   FILE *file = fopen(key->path, "rb");
   if (!file)
      return false;
   struct cache_header header;
   PsbcShaderOutput cached = {0};
   bool valid = fread(&header, sizeof(header), 1, file) == 1 &&
                memcmp(header.key, key->digest, sizeof(header.key)) == 0 &&
                header.data_bytes <= CACHE_MAX_BYTES && header.code_bytes > 0 &&
                header.code_bytes <= CACHE_MAX_BYTES &&
                header.metadata.version == PSBC_SHADER_METADATA_VERSION &&
                header.metadata.context_register_count <= PSBC_MAX_CONTEXT_REGISTERS &&
                header.metadata.shader_register_count <= PSBC_MAX_SHADER_REGISTERS &&
                header.metadata.input_semantic_count <= PSBC_MAX_SEMANTICS &&
                header.metadata.output_semantic_count <= PSBC_MAX_SEMANTICS &&
                header.metadata.descriptor_binding_count <= PSBC_MAX_DESCRIPTOR_BINDINGS;
   if (valid) {
      cached.size = header.data_bytes;
      cached.machine_code_size = header.code_bytes;
      cached.data = malloc(cached.size ? cached.size : 1);
      cached.machine_code = malloc(cached.machine_code_size);
      valid = cached.data && cached.machine_code &&
              fread(cached.data, 1, cached.size, file) == cached.size &&
              fread(cached.machine_code, 1, cached.machine_code_size, file) == cached.machine_code_size &&
              fgetc(file) == EOF && !ferror(file);
   }
   fclose(file);
   if (valid) {
      unsigned char sum[32];
      checksum(&header, &cached, sum);
      valid = memcmp(sum, header.checksum, sizeof(sum)) == 0;
   }
   if (!valid) {
      psbc_free_output(&cached);
      printf("[ps5vk] shader cache invalid; recompiling\n");
      return false;
   }
   cached.metadata = header.metadata;
   *output = cached;
   printf("[ps5vk] shader cache hit\n");
   return true;
}

void
ps5vk_shader_cache_store(const struct ps5vk_shader_cache_key *key, const PsbcShaderOutput *output)
{
   if (!output->machine_code || !output->machine_code_size ||
       output->size > CACHE_MAX_BYTES || output->machine_code_size > CACHE_MAX_BYTES)
      return;
   struct cache_header header = {0};
   memcpy(header.key, key->digest, sizeof(header.key));
   header.data_bytes = output->size;
   header.code_bytes = output->machine_code_size;
   header.metadata = output->metadata;
   checksum(&header, output, header.checksum);
   char temporary[1100];
   snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", key->path, (long)getpid());
   FILE *file = fopen(temporary, "wb");
   if (!file) {
      fprintf(stderr, "[ps5vk] shader cache file unavailable; compiled shader remains usable\n");
      return;
   }
   bool written = fwrite(&header, sizeof(header), 1, file) == 1 &&
                  (!output->size || fwrite(output->data, 1, output->size, file) == output->size) &&
                  fwrite(output->machine_code, 1, output->machine_code_size, file) == output->machine_code_size;
   written = fflush(file) == 0 && written;
   written = fsync(fileno(file)) == 0 && written;
   written = fclose(file) == 0 && written;
   if (written && rename(temporary, key->path) == 0)
      printf("[ps5vk] shader cache stored\n");
   else {
      unlink(temporary);
      fprintf(stderr, "[ps5vk] shader cache write failed; compiled shader remains usable\n");
   }
}
