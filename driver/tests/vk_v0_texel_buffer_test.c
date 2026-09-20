/*
 * PS5 Vulkan driver - blocker round 5: the uniform texel buffer's V#.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The descriptor type tooling/psbc/patch-descriptor-types.py gives the
 * compiler is only half of a texel buffer: the other half is the 16-byte buffer
 * descriptor the draw writes into the stage's set-0 table
 * (driver/ps5vk_draw.c, PS5VK_TEXEL_BUFFER_*), which both halves of the
 * mechanism share -- the uniform fetch a samplerBuffer makes (blocker round 5)
 * and the store an imageBuffer makes (round 6). That descriptor is what the
 * hardware reads, and it is the one part of the path a console frame cannot show
 * the words of: a wrong word there fetches zeros, which is what the probe's first
 * two console runs measured.
 *
 * This test builds both programs the probes build -- probes/v0-texel-buffer's
 * shaders with a uniform buffer view and probes/v0-texel-buffer-store's with a
 * storage one, four texels of R8G8B8A8_UNORM at set 0, binding 0 -- draws them,
 * and reads the table chunk each draw wrote back through the driver's own debug
 * export (driver/ps5vk_debug.h): the V# must name the view's buffer at stride
 * four, count four, the format's DST_SEL selectors and FORMAT word and the
 * RESOURCE_LEVEL bit RADV's own texel-buffer descriptor sets, and it must NOT
 * set ADD_TID_ENABLE, which adds the thread's id to the element index and walks
 * the access off the end of a four-texel view. The storage program also has to
 * get past vkCreateBufferView's reporting rule, whose bit that draw's usage asks
 * for.
 *
 * Built and run through the loader and directly by tools/check-driver.sh (see
 * ps5vk_test.h); the PS5 build only links, it is not run. The frames themselves
 * are the console's to prove: src/diagnostics.cpp's v0-formats-texel-buffer
 * fetches four texels, one per quarter of the target, and reads them back, and
 * v0-formats-texel-buffer-store stores them and reads them out of the buffer's
 * own memory.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ps5vk_debug.h"
#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

/* The eight_8_8_8_UNORM format word the register database numbers
 * (docs/V0_FORMATS_AUDIT.md's table), and the DST_SEL word for its four
 * channels. */
#define V0_TEXEL_FORMAT_WORD 56u
#define V0_TEXEL_DST_SEL 0x0facu
#define V0_TEXEL_BYTES 4u
/* The storage image the case creates and the eight words its descriptor holds:
 * a 64x2 R8G8B8A8_UNORM row-layout image, the round-7 probe's shape. */
#define V0_IMAGE_WIDTH 64u
#define V0_IMAGE_HEIGHT 2u

/* What the draw's V# must hold, as the four words: the address is checked
 * against the live buffer's own separately. */
struct v_short {
   uint32_t words[8];
   bool found;
};

/* The four levels the probe's buffer holds, in the format's own order: red,
 * green, blue and alpha of each texel. */
static const unsigned char kTexels[4 * V0_TEXEL_BYTES] = {
   0x20, 0x20, 0x20, 0xff, 0x40, 0x40, 0x40, 0xff,
   0x60, 0x60, 0x60, 0xff, 0x7f, 0x7f, 0x7f, 0xff,
};

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   (void)context;
   if (passed)
      return;
   printf("  (%s failed: VkResult %d%s%s)\n", name, result, detail[0] ? ", " : "", detail);
}

#if defined(__linux__)
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
#endif

#if defined(PS5VK_TEST_DIRECT)
#if defined(PS5VK_TEST_DIRECT)
/* The first storage image descriptor in a table chunk: the word whose TYPE field
 * (bits 28-31) is the register database's 2D kind. */
static struct v_short
find_image_descriptor(const uint32_t *words, size_t dwords)
{
   struct v_short found = {{0, 0, 0, 0}, false};
   for (size_t index = 0; index + 7 < dwords; index += 4) {
      const uint32_t word3 = words[index + 3];
      if (((word3 >> 28) & 0xfu) == 9u && (word3 & 0xfffu) == V0_TEXEL_DST_SEL) {
         memcpy(found.words, words + index, sizeof(found.words));
         found.found = true;
         break;
      }
   }
   return found;
}
#endif

/* The first texel-buffer V# in a table chunk: the word whose FORMAT field and
 * RESOURCE_LEVEL bit are the ones above. */
static struct v_short
find_v_short(const uint32_t *words, size_t dwords)
{
   struct v_short found = {{0, 0, 0, 0}, false};
   for (size_t index = 0; index + 3 < dwords; index += 4) {
      const uint32_t word3 = words[index + 3];
      if (((word3 >> 12) & 0x7fu) == V0_TEXEL_FORMAT_WORD && (word3 & (1u << 24)) != 0) {
         memcpy(found.words, words + index, sizeof(found.words));
         found.found = true;
         break;
      }
   }
   return found;
}
#endif

int
main(void)
{
   test_begin("V0 texel buffer");
#define V0_PROGRAMS 4
   size_t vertex_bytes[V0_PROGRAMS] = {0, 0, 0, 0};
   size_t pixel_bytes[V0_PROGRAMS] = {0, 0, 0, 0};
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   /* One row a mechanism: the uniform texel buffer a samplerBuffer fetches, the
    * storage texel buffer an imageBuffer stores through, and the storage image an
    * imageStore writes. They differ in the pixel stage's shader and in what the
    * caller binds, and in nothing else this test names. */
   const char *const sets[V0_PROGRAMS] = {"v0-texel-buffer", "v0-texel-buffer-store",
                                          "v0-image-store", "v0-image-atomic"};
   uint32_t *vertex[V0_PROGRAMS] = {NULL, NULL, NULL, NULL};
   uint32_t *pixel[V0_PROGRAMS] = {NULL, NULL, NULL, NULL};
   for (unsigned row = 0; row < V0_PROGRAMS; row++) {
      vertex[row] = probes ? read_spirv(probes, sets[row], "vertex", &vertex_bytes[row]) : NULL;
      pixel[row] = probes ? read_spirv(probes, sets[row], "pixel", &pixel_bytes[row]) : NULL;
   }
   check(vertex[0] && pixel[0] && vertex[1] && pixel[1] && vertex[2] && pixel[2] &&
            vertex[3] && pixel[3],
         "PS5VK_PROBES holds the four probe sets' SPIR-V");
#else
   uint32_t *vertex[V0_PROGRAMS] = {NULL, NULL, NULL, NULL};
   uint32_t *pixel[V0_PROGRAMS] = {NULL, NULL, NULL, NULL};
#endif

   if (vertex[0] && pixel[0] && vertex[1] && pixel[1] && vertex[2] && pixel[2] && vertex[3] &&
       pixel[3]) {
      const struct ps5vk_triangle_report report = {NULL, record_step};
      /* The two halves of the mechanism share everything but the usage, the
       * descriptor type and the pixel stage's shader: a uniform fetch and a
       * storage store, each with its own probe set. */
      for (unsigned storage = 0; storage < V0_PROGRAMS; storage++) {
         /* The fullscreen triangle: no vertex data and no attributes, so the pixel
          * stage is the only stage that reads a descriptor, and no texture: this
          * program's one descriptor is the texel buffer below. One field per name,
          * because the struct is long and positional initializers drifted the last
          * time it grew. */
         struct ps5vk_triangle_input filled = {0};
         filled.get_instance_proc_addr = GET_PROC;
         filled.pipeline_count = 1;
         filled.shaders[0] = (struct ps5vk_triangle_shaders){vertex[storage], vertex_bytes[storage],
                                                             pixel[storage], pixel_bytes[storage]};
         filled.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
         filled.report = &report;
         filled.output = PS5VK_TRIANGLE_OUTPUT_IMAGE;
         if (storage < 2) {
            filled.texel_buffer_data = kTexels;
            filled.texel_buffer_bytes = (uint32_t)sizeof(kTexels);
            filled.texel_buffer_format = VK_FORMAT_R8G8B8A8_UNORM;
            filled.texel_buffer_storage = storage != 0;
         } else {
            filled.storage_image_format = VK_FORMAT_R8G8B8A8_UNORM;
            filled.storage_image_width = V0_IMAGE_WIDTH;
            filled.storage_image_height = V0_IMAGE_HEIGHT;
         }

         struct ps5vk_triangle triangle = {0};
         enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &filled);
         if (status == PS5VK_TRIANGLE_OK)
            status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         check(status == PS5VK_TRIANGLE_OK,
               storage == 2 ? "a frame that stores into a storage image records and submits"
               : storage == 3 ? "a frame that adds to a storage image records and submits"
               : storage      ? "a frame that stores through a storage texel buffer records and submits"
                              : "a frame that fetches a uniform texel buffer records and submits");
         check(storage >= 2 ? (triangle.storage_image != VK_NULL_HANDLE &&
                               triangle.storage_image_view != VK_NULL_HANDLE &&
                               triangle.texture_set != VK_NULL_HANDLE)
                            : (triangle.texel_buffer != VK_NULL_HANDLE &&
                               triangle.texel_buffer_view != VK_NULL_HANDLE &&
                               triangle.texture_set != VK_NULL_HANDLE),
               "the harness created the buffer or image, its view and the set the pixel stage "
               "reads");
#if defined(PS5VK_TEST_DIRECT)
         /* Only the direct build reads the table back: the loader build sees the
          * driver through its ICD, which exports no such symbol
          * (driver/ps5vk_icd.map). */
         if (status == PS5VK_TRIANGLE_OK) {
            ps5vk_debug_stage chunks[4] = {{NULL, 0}};
            const uint32_t chunk_count =
               ps5vk_debug_table_chunks(triangle.device, chunks, sizeof(chunks) / sizeof(chunks[0]));
            check(chunk_count > 0 && chunks[0].address != NULL && chunks[0].bytes >= 16,
                  "the draw's set-0 table chunk is readable");
            if (chunk_count > 0 && chunks[0].address != NULL && storage >= 2) {
               /* The storage image's entry: the image descriptor's eight words. */
               const struct v_short v =
                  find_image_descriptor(chunks[0].address, chunks[0].bytes / 4);
               printf("  image descriptor: %08x %08x %08x %08x %08x %08x %08x %08x\n", v.words[0],
                      v.words[1], v.words[2], v.words[3], v.words[4], v.words[5], v.words[6],
                      v.words[7]);
               check(v.found, "the table holds a 32-byte storage image descriptor");
               check(v.words[0] != 0,
                     "the descriptor names an address (word 0 is the address high bits)");
               check(((v.words[1] >> 20) & 0x7fu) == V0_TEXEL_FORMAT_WORD,
                     "the descriptor's format word is the format entry's");
               /* The image descriptor's word 1 carries the low two bits of
                * (width - 1) at bits 30-31, the format word at 20-26 and the
                * address's top byte at 0-7. */
               check(((v.words[1] >> 30) & 3u) == ((V0_IMAGE_WIDTH - 1u) & 3u),
                     "the descriptor's low width bits are the image's");
               check((v.words[2] & 0x3fffu) == (((V0_IMAGE_WIDTH - 1u) >> 2) & 0x3fffu),
                     "the descriptor's width field is the image's");
               check((v.words[3] & 0xfffu) == V0_TEXEL_DST_SEL,
                     "the descriptor's DST_SEL selectors are the format entry's");
               check((v.words[3] >> 28) == 9u && (v.words[5] & 0x00400000u) != 0,
                     "the descriptor's kind is the untiled 2D one and its level list is "
                     "single-level");
               check(v.words[6] == 0 && v.words[7] == 0,
                     "the sampler's words are not part of a storage image's entry");
            } else if (chunk_count > 0 && chunks[0].address != NULL) {
               const struct v_short v = find_v_short(chunks[0].address, chunks[0].bytes / 4);
               printf("  V#: %08x %08x %08x %08x\n", v.words[0], v.words[1], v.words[2], v.words[3]);
               check(v.found, "the table holds a texel buffer V# with the format word and resource "
                              "level");
               check(((v.words[1] >> 16) & 0x3fffu) == V0_TEXEL_BYTES,
                     "the V#'s STRIDE field is the view's four-byte element");
               check(v.words[2] == 4, "the V#'s element count is the view's four texels");
               check((v.words[3] & 0xfffu) == V0_TEXEL_DST_SEL,
                     "the V#'s DST_SEL selectors are the format entry's");
               check((v.words[3] & (1u << 23)) == 0,
                     "the V# does not set ADD_TID_ENABLE, which would add the thread's id to the "
                     "element index");
               check((v.words[3] & (1u << 24)) != 0,
                     "the V# sets RESOURCE_LEVEL, as RADV's own texel-buffer descriptor does on GFX10");

               ps5vk_debug_stage buffers[8] = {{NULL, 0}};
               const uint32_t buffer_count =
                  ps5vk_debug_buffers(triangle.device, buffers, sizeof(buffers) / sizeof(buffers[0]));
               bool address_matches = false;
               for (uint32_t index = 0; index < buffer_count; index++) {
                  const uintptr_t address = (uintptr_t)buffers[index].address;
                  address_matches = address_matches ||
                                    ((uint32_t)address == v.words[0] &&
                                     (uint32_t)(address >> 32) == (v.words[1] & 0x3fffu));
               }
               check(address_matches,
                     "the V#'s address, both halves, is one of the device's live bound buffers");
            }
         }
   #endif
         ps5vk_triangle_finish(&triangle);
      }
      for (unsigned row = 0; row < 2; row++) {
         free(vertex[row]);
         free(pixel[row]);
      }
   }
   return test_finish();
}
