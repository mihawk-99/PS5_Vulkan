/*
 * PS5 Vulkan driver - a census of the paths an application exercises.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Opt-in with the rest of the driver's logging (/app0/ps5vk-log.txt,
 * ps5vk_instance.c): every distinct combination a hook reports -- an attachment's
 * format and size, a pipeline's blend and depth state, a sampled image's layout,
 * a copy's formats and aspects -- is written once to stderr, the first time it
 * is seen. A frame that renders wrong without refusing anything then says which
 * of its paths the console has measured and which it has not, which is what an
 * audit of an application's real usage needs (jobs/r54-ppsspp-audit). Off, every
 * hook is one load and a branch.
 */

#include "ps5vk_private.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool ps5vk_census_enabled;
unsigned ps5vk_ab_flags;

/* /app0/ps5vk-ab.txt, read once with the log flag: words naming which path a
 * diagnostic run replaces (ps5vk_private.h, PS5VK_AB_*). */
void
ps5vk_ab_load(void)
{
   FILE *file = fopen("/app0/ps5vk-ab.txt", "rb");
   if (file == NULL)
      return;
   char word[64];
   while (fscanf(file, "%63s", word) == 1) {
      if (!strcmp(word, "full-mask"))
         ps5vk_ab_flags |= PS5VK_AB_FULL_MASK;
      else if (!strcmp(word, "const-zero"))
         ps5vk_ab_flags |= PS5VK_AB_CONST_ZERO;
      else if (!strcmp(word, "const-half"))
         ps5vk_ab_flags |= PS5VK_AB_CONST_HALF;
      else if (!strcmp(word, "no-stencil"))
         ps5vk_ab_flags |= PS5VK_AB_NO_STENCIL;
      else if (!strcmp(word, "no-depth"))
         ps5vk_ab_flags |= PS5VK_AB_NO_DEPTH;
      else if (!strcmp(word, "sync-present"))
         ps5vk_ab_flags |= PS5VK_AB_SYNC_PRESENT;
      else if (!strcmp(word, "no-d24"))
         ps5vk_ab_flags |= PS5VK_AB_NO_D24;
      else if (!strcmp(word, "base-mip"))
         ps5vk_ab_flags |= PS5VK_AB_BASE_MIP;
      else if (!strcmp(word, "pix-center"))
         ps5vk_ab_flags |= PS5VK_AB_PIX_CENTER;
      else if (!strcmp(word, "cpu-transfers"))
         ps5vk_ab_flags |= PS5VK_AB_CPU_TRANSFERS;
      else if (!strcmp(word, "tile-padded"))
         ps5vk_ab_flags |= PS5VK_AB_TILE_PADDED;
   }
   fclose(file);
   char line[64];
   snprintf(line, sizeof(line), "[ps5vk] A/B flags 0x%x\n", ps5vk_ab_flags);
   fputs(line, stderr);
}

#define PS5VK_CENSUS_SLOTS 8192u

static pthread_mutex_t ps5vk_census_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t ps5vk_census_seen[PS5VK_CENSUS_SLOTS];
static unsigned ps5vk_census_count;

void
ps5vk_census(const char *format, ...)
{
   if (!ps5vk_census_enabled)
      return;
   char line[320];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line), format, args);
   va_end(args);
   uint64_t hash = UINT64_C(0xcbf29ce484222325);
   for (const char *c = line; *c; c++)
      hash = (hash ^ (uint8_t)*c) * UINT64_C(0x100000001b3);
   if (hash == 0)
      hash = 1;
   pthread_mutex_lock(&ps5vk_census_lock);
   unsigned slot = (unsigned)(hash % PS5VK_CENSUS_SLOTS);
   bool fresh = false;
   for (unsigned probe = 0; probe < PS5VK_CENSUS_SLOTS; probe++) {
      const unsigned at = (slot + probe) % PS5VK_CENSUS_SLOTS;
      if (ps5vk_census_seen[at] == hash)
         break;
      if (ps5vk_census_seen[at] == 0) {
         if (ps5vk_census_count + 1 < PS5VK_CENSUS_SLOTS) {
            ps5vk_census_seen[at] = hash;
            ps5vk_census_count++;
            fresh = true;
         }
         break;
      }
   }
   pthread_mutex_unlock(&ps5vk_census_lock);
   if (fresh) {
      char out[352];
      snprintf(out, sizeof(out), "[ps5vk] census: %s\n", line);
      fputs(out, stderr);
   }
}
