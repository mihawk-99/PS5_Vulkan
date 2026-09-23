/* Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ps5vk_private.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ps5vk_blit_execute(const struct ps5vk_memory_copy *copy);

uint64_t legacy_tiled_texel_offset(uint32_t x, uint32_t y, uint32_t level_width,
   uint32_t tile_width, uint32_t tile_height, uint32_t element_bytes,
   uint32_t samples, uint64_t *swizzle);

static uint64_t reference_address(const struct ps5vk_image_copy_side *side,
                                  int32_t x, int32_t y)
{
   if (!side->tiled)
      return side->address + (uint64_t)y * side->row_pitch + (uint64_t)x * 4;
   uint64_t swizzle;
   uint64_t tile = legacy_tiled_texel_offset(x, y, side->level_width,
      side->tile_width, side->tile_height, side->element_bytes, side->samples, &swizzle);
   return side->address + tile + (swizzle ^ side->tile_xor);
}

int main(void)
{
   const size_t bytes = 2 * 1024 * 1024;
   uint8_t *source = malloc(bytes), *destination = malloc(bytes), *expected = malloc(bytes);
   assert(source && destination && expected);
   uint32_t random = 0x12345678;
   for (size_t i = 0; i < bytes; i++) {
      random = random * 1664525u + 1013904223u;
      source[i] = random >> 24;
   }
   unsigned maps = 0;
   const unsigned widths[] = {32, 128, 256, 512, 3840};
   for (unsigned w = 0; w < sizeof(widths)/sizeof(widths[0]); w++) {
      for (unsigned samples = 1; samples <= 4; samples += 3) {
         struct ps5vk_image_copy_side side = {.address = 0x10000, .level_width = widths[w],
            .tile_width = samples == 1 ? 128 : 64, .tile_height = samples == 1 ? 128 : 64,
            .element_bytes = 4, .samples = samples, .tiled = true, .tile_xor = 0x8400};
         for (unsigned y = 0; y < 256; y++) {
            for (unsigned x = 0; x < widths[w]; x++) {
               assert(ps5vk_image_copy_address(&side,x,y,4,0) == reference_address(&side,x,y));
               maps++;
            }
         }
      }
   }
   printf("PASS: %u addresses match the pre-optimization map\n", maps);
   unsigned cases = 0;
   for (unsigned tiled = 0; tiled < 4; tiled++) {
      for (unsigned shift = 0; shift < 4; shift++) {
         for (unsigned size = 1; size <= 256; size *= 2) {
            memset(destination, 0xa5, bytes);
            memset(expected, 0xa5, bytes);
            struct ps5vk_memory_copy copy = {
               .blit = true, .linear = true,
               .source_side = {.address = (uintptr_t)source, .row_pitch = 2112,
                  .level_width = 528, .tile_width = 128, .tile_height = 128,
                  .element_bytes = 4, .samples = 1, .tiled = tiled & 1,
                  .tile_xor = shift & 1 ? 0x4800 : 0},
               .destination_side = {.address = (uintptr_t)destination, .row_pitch = 1088,
                  .level_width = 272, .tile_width = 128, .tile_height = 128,
                  .element_bytes = 4, .samples = 1, .tiled = tiled & 2,
                  .tile_xor = shift & 1 ? 0x8400 : 0},
               .source_x = shift, .source_y = 3 - shift,
               .destination_x = 3 - shift, .destination_y = shift,
               .width = size, .height = size,
               .source_texel_bytes = 4, .destination_texel_bytes = 4,
               .source_format = VK_FORMAT_R8G8B8A8_UNORM,
               .destination_format = VK_FORMAT_R8G8B8A8_UNORM,
               .source_step_x = 2.0f, .source_step_y = 2.0f,
            };
            /* Independent byte oracle and original map from commit 9f9f395;
             * this checks rounding, offsets and untouched destination bytes. */
            for (unsigned y = 0; y < size; y++) {
               for (unsigned x = 0; x < size; x++) {
                  uint64_t dst = reference_address(&copy.destination_side,
                     copy.destination_x + x, copy.destination_y + y);
                  size_t offset = dst - (uintptr_t)destination;
                  assert(offset + 4 <= bytes);
                  for (unsigned channel = 0; channel < 4; channel++) {
                     float value = 0;
                     for (unsigned dy = 0; dy < 2; dy++) {
                        for (unsigned dx = 0; dx < 2; dx++) {
                           uint64_t src = reference_address(&copy.source_side,
                              copy.source_x + 2*x + dx, copy.source_y + 2*y + dy);
                           assert(src >= (uintptr_t)source && src + 4 <= (uintptr_t)source + bytes);
                           value += ((const uint8_t *)(uintptr_t)src)[channel] * 0.25f;
                        }
                     }
                     expected[offset + channel] = (uint8_t)(value + 0.5f);
                  }
               }
            }
            ps5vk_blit_execute(&copy);
            assert(memcmp(destination, expected, bytes) == 0);
            cases++;
         }
      }
   }
   free(source); free(destination); free(expected);
   printf("PASS: %u random-colour reductions, row/tile combinations, offsets, tails and untouched bytes\n", cases);
   return 0;
}
