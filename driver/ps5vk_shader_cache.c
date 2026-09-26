/* Persistent compiler outputs, shared by graphics and compute.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Called under the existing compiler mutex. Save after each successful compile,
 * so a later application crash preserves completed work. The file is written
 * by a writer thread (R80): creating, writing and renaming a small file took
 * about 3.5 ms a stage on the console, on the thread that asked for the
 * pipeline -- Dolphin's, which draws nothing meanwhile. Until its file is in
 * place, a stored output is loaded from memory, and a device's destruction
 * waits for the writes still pending (ps5vk_shader_cache_flush). Cache failures are
 * misses, never Vulkan errors. NIR meta shaders use Mesa serialization as input.
 */
#include "ps5vk_shader_cache.h"
#include "ps5vk_cache_build.h"
#include "util/detect_os.h"
#include "util/mesa-blake3.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
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

#if DETECT_OS_LINUX
#define PS5VK_CACHE_BUILD PS5VK_CACHE_HOST_BUILD
#else
#define PS5VK_CACHE_BUILD PS5VK_CACHE_PS5_BUILD
#endif

/* The cache for this driver build: <base>/<first 16 hex digits of the build>.
 * Keys already include the build, so every build's entries were valid only for
 * it, and they piled up side by side in one directory; one directory a build
 * keeps them apart, and is exactly the set a title can ship pre-built
 * (the port's tools/shader-cache.py). The base is PS5VK_SHADER_CACHE_DIR, or on
 * the console the first line of /app0/ps5vk-shader-cache-dir.txt (a test hook,
 * default-off), or /app0/ps5vk-shader-cache. The directories are opened to the
 * FTP service (below). NULL when there is none. Called under
 * the compiler mutex, so the one-time setup needs no lock of its own. */
static const char *
ps5vk_shader_cache_directory(void)
{
   static bool done;
   static char directory[768];
   if (done)
      return directory[0] ? directory : NULL;
   done = true;
   char base[512] = {0};
   const char *const environment = getenv("PS5VK_SHADER_CACHE_DIR");
   if (environment != NULL) {
      snprintf(base, sizeof(base), "%s", environment);
   } else {
#if !DETECT_OS_LINUX
      FILE *const hook = fopen("/app0/ps5vk-shader-cache-dir.txt", "rb");
      if (hook != NULL) {
         if (fgets(base, sizeof(base), hook) != NULL)
            base[strcspn(base, "\r\n")] = '\0';
         fclose(hook);
      }
      if (!base[0])
         snprintf(base, sizeof(base), "/app0/ps5vk-shader-cache");
#endif
   }
   if (!base[0])
      return NULL;
   snprintf(directory, sizeof(directory), "%s/%.16s", base, PS5VK_CACHE_BUILD);
   const bool made = (mkdir(base, 0777) == 0 || errno == EEXIST) &&
                     (mkdir(directory, 0777) == 0 || errno == EEXIST);
   /* Open to the console's FTP service, which is not the title's user: it reads
    * entries back (harvest) and writes shipped ones in (deploy). An existing
    * base from an older driver was made 0700. */
   (void)chmod(base, 0777);
   (void)chmod(directory, 0777);
   if (!made) {
      fprintf(stderr, "[ps5vk] shader cache directory unavailable; compiling normally\n");
      directory[0] = '\0';
      return NULL;
   }
   return directory;
}

bool
ps5vk_shader_cache_key(const uint32_t *words, size_t size, const PsbcCompileOptions *options,
                       struct ps5vk_shader_cache_key *key)
{
   const char *const directory = ps5vk_shader_cache_directory();
   if (!directory || !words || !size)
      return false;
   struct mesa_blake3 hash;
   _mesa_blake3_init(&hash);
   const char *build = PS5VK_CACHE_BUILD;
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
   FIELD(address32_hi); FIELD(multiview); FIELD(vertex_attribute_count);
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

/* A stored output waiting for its file: the header with its checksum and copies
 * of the two buffers. Immutable once queued, so a load reads it while the
 * writer writes it; it leaves the queue, under the lock, once its file is in
 * place. */
struct cache_job {
   struct cache_job *next;
   char path[1024];
   struct cache_header header;
   void *data;
   void *code;
};

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cache_queued = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cache_drained = PTHREAD_COND_INITIALIZER;
static struct cache_job *cache_head, *cache_tail;
static bool cache_writer_started;

static void
cache_job_free(struct cache_job *job)
{
   free(job->data);
   free(job->code);
   free(job);
}

/* A queued output for key, copied into output, or false. Under cache_lock. */
static bool
cache_pending(const struct ps5vk_shader_cache_key *key, PsbcShaderOutput *output)
{
   for (const struct cache_job *job = cache_head; job; job = job->next) {
      if (memcmp(job->header.key, key->digest, sizeof(job->header.key)) != 0)
         continue;
      PsbcShaderOutput copy = {0};
      copy.size = job->header.data_bytes;
      copy.machine_code_size = job->header.code_bytes;
      copy.data = malloc(copy.size ? copy.size : 1);
      copy.machine_code = malloc(copy.machine_code_size);
      if (!copy.data || !copy.machine_code) {
         psbc_free_output(&copy);
         return false;
      }
      if (copy.size)
         memcpy(copy.data, job->data, copy.size);
      memcpy(copy.machine_code, job->code, copy.machine_code_size);
      copy.metadata = job->header.metadata;
      *output = copy;
      return true;
   }
   return false;
}

bool
ps5vk_shader_cache_load(const struct ps5vk_shader_cache_key *key, PsbcShaderOutput *output)
{
   pthread_mutex_lock(&cache_lock);
   const bool pending = cache_pending(key, output);
   pthread_mutex_unlock(&cache_lock);
   if (pending) {
      printf("[ps5vk] shader cache hit\n");
      return true;
   }
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

/* The file itself: a temporary name, then a rename, so a reader never sees a
 * partial entry. No fsync: it cost about 0.6 ms a stage and bought only
 * power-loss durability, and a file a power cut leaves torn fails the load's
 * size, key or checksum test and is compiled again. */
static void
cache_write(const struct cache_job *job)
{
   char temporary[1100];
   snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", job->path, (long)getpid());
   FILE *file = fopen(temporary, "wb");
   if (!file) {
      fprintf(stderr, "[ps5vk] shader cache file unavailable; compiled shader remains usable\n");
      return;
   }
   bool written = fwrite(&job->header, sizeof(job->header), 1, file) == 1 &&
                  (!job->header.data_bytes ||
                   fwrite(job->data, 1, job->header.data_bytes, file) == job->header.data_bytes) &&
                  fwrite(job->code, 1, job->header.code_bytes, file) == job->header.code_bytes;
   written = fflush(file) == 0 && written;
   written = fclose(file) == 0 && written;
   if (written && rename(temporary, job->path) == 0)
      printf("[ps5vk] shader cache stored\n");
   else {
      unlink(temporary);
      fprintf(stderr, "[ps5vk] shader cache write failed; compiled shader remains usable\n");
   }
}

static void *
cache_writer(void *unused)
{
   (void)unused;
   pthread_mutex_lock(&cache_lock);
   for (;;) {
      while (!cache_head)
         pthread_cond_wait(&cache_queued, &cache_lock);
      struct cache_job *const job = cache_head;
      pthread_mutex_unlock(&cache_lock);
      cache_write(job);
      pthread_mutex_lock(&cache_lock);
      cache_head = job->next;
      if (!cache_head) {
         cache_tail = NULL;
         pthread_cond_broadcast(&cache_drained);
      }
      cache_job_free(job);
   }
   return NULL;
}

void
ps5vk_shader_cache_store(const struct ps5vk_shader_cache_key *key, const PsbcShaderOutput *output)
{
   if (!output->machine_code || !output->machine_code_size ||
       output->size > CACHE_MAX_BYTES || output->machine_code_size > CACHE_MAX_BYTES)
      return;
   struct cache_job *const job = calloc(1, sizeof(*job));
   if (!job)
      return;
   snprintf(job->path, sizeof(job->path), "%s", key->path);
   memcpy(job->header.key, key->digest, sizeof(job->header.key));
   job->header.data_bytes = output->size;
   job->header.code_bytes = output->machine_code_size;
   job->header.metadata = output->metadata;
   checksum(&job->header, output, job->header.checksum);
   job->data = malloc(output->size ? output->size : 1);
   job->code = malloc(output->machine_code_size);
   if (!job->data || !job->code) {
      cache_job_free(job);
      return;
   }
   if (output->size)
      memcpy(job->data, output->data, output->size);
   memcpy(job->code, output->machine_code, output->machine_code_size);
   pthread_mutex_lock(&cache_lock);
   if (!cache_writer_started) {
      pthread_t thread;
      cache_writer_started = pthread_create(&thread, NULL, cache_writer, NULL) == 0;
      if (cache_writer_started)
         pthread_detach(thread);
   }
   if (!cache_writer_started) {
      /* No thread: write it here, as before. */
      pthread_mutex_unlock(&cache_lock);
      cache_write(job);
      cache_job_free(job);
      return;
   }
   if (cache_tail)
      cache_tail->next = job;
   else
      cache_head = job;
   cache_tail = job;
   pthread_cond_signal(&cache_queued);
   pthread_mutex_unlock(&cache_lock);
}

void
ps5vk_shader_cache_flush(void)
{
   pthread_mutex_lock(&cache_lock);
   while (cache_head)
      pthread_cond_wait(&cache_drained, &cache_lock);
   pthread_mutex_unlock(&cache_lock);
}
