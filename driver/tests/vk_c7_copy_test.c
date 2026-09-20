/*
 * PS5 Vulkan driver - Phase C7 test: an image copy and a one-to-one blit.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C7 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the m3-texture canary's square twice, over two programs and two
 * devices. The first copies its uploaded texels into a second, tiled image with
 * vkCmdCopyImage and samples that image; the second does the same with the
 * one-to-one vkCmdBlitImage. Both copies land in the destination's measured
 * tile layout -- 128x128-texel blocks of 0x10000 bytes with the XOR bit map
 * inside a block that the C7 address map fitted to the byte
 * (docs/HARDWARE_FINDINGS.md) -- which is what this program checks: the
 * destination's mapped memory must hold the caller's 64x36 pattern, texel for
 * texel, at the addresses that map names. That is the half a PC can prove; the
 * frame itself is the console's, which reads the square back and requires the
 * canary's pattern in it (src/diagnostics.cpp, `c7-copy`).
 *
 * tools/check-driver.sh compares both submissions with the console's own run of
 * it (golden/c7-copy). PS5VK_PROBES names the probes directory. The PS5 build
 * only links; it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

/* The first failed step of a draw, apart from driver messages. */
struct steps {
   const char *failed;
   int result;
};

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   struct steps *const steps = context;
   if (passed)
      return;
   if (strcmp(name, "vk_message") == 0) {
      printf("  (driver: %s)\n", detail);
      return;
   }
   printf("  (%s failed: VkResult %d%s%s)\n", name, result, detail[0] ? ", " : "", detail);
   if (!steps->failed) {
      steps->failed = name;
      steps->result = result;
   }
}

#if defined(__linux__)
/* A SPIR-V file of a probe set, in words. */
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

/* The m3-texture canary's texture: 64x36 RGBA8 texels, every one distinct.
 * src/diagnostics.cpp holds the same pattern (texel_at), and its readback check
 * is what accepts the frames on the console. */
#define C7_TEXTURE_WIDTH 64
#define C7_TEXTURE_HEIGHT 36

static unsigned char kTexels[C7_TEXTURE_WIDTH * C7_TEXTURE_HEIGHT * 4];

static void
fill_texels(void)
{
   for (unsigned row = 0; row < C7_TEXTURE_HEIGHT; row++) {
      for (unsigned column = 0; column < C7_TEXTURE_WIDTH; column++) {
         /* R, G, B, A bytes, the order an R8G8B8A8_UNORM texel holds them. */
         const unsigned char texel[4] = {(unsigned char)(column * 4), (unsigned char)(row * 7),
                                         (unsigned char)(((column ^ row) & 1) != 0 ? 0xe0 : 0x20),
                                         0xff};
         memcpy(kTexels + ((size_t)row * C7_TEXTURE_WIDTH + column) * 4, texel, sizeof(texel));
      }
   }
}

/* Where a texel of the tiled destination lands: the map the driver's copy
 * writes with (driver/ps5vk_image.c, ps5vk_tiled_texel_offset) and the one the
 * console's readbacks decode (src/diagnostics.cpp, tiled_rgba8_offset). */
#define C7_TILE_TEXELS 128
#define C7_TILE_BYTES 0x10000

static size_t
tiled_offset(uint32_t x, uint32_t y)
{
   const size_t sx = x;
   const size_t sy = y;
   const size_t local = ((sy << 4) & 0x70u) ^ ((sy << 5) & 0xf00u) ^ ((sy << 9) & 0x1000u) ^
                        ((sy << 8) & 0x4000u) ^ ((sx << 2) & 0x0cu) ^ ((sx << 5) & 0x380u) ^
                        ((sx << 4) & 0x400u) ^ ((sx << 6) & 0x800u) ^ ((sx << 9) & 0xa000u);
   const size_t blocks_per_row = (C7_TEXTURE_WIDTH + C7_TILE_TEXELS - 1u) / C7_TILE_TEXELS;
   const size_t block = (sy / C7_TILE_TEXELS) * blocks_per_row + sx / C7_TILE_TEXELS;
   return block * C7_TILE_BYTES + local;
}

/* Every texel of the copied image is the caller's texel of the same coordinate,
 * at the address the tiled map names, or the index of the first one that is
 * not. */
static size_t
first_tiled_mismatch(const void *copied)
{
   const unsigned char *const bytes = copied;
   for (unsigned row = 0; row < C7_TEXTURE_HEIGHT; row++) {
      for (unsigned column = 0; column < C7_TEXTURE_WIDTH; column++) {
         const size_t at = tiled_offset(column, row);
         const unsigned char *const want =
            kTexels + ((size_t)row * C7_TEXTURE_WIDTH + column) * 4;
         for (size_t byte = 0; byte < 4; byte++) {
            if (bytes[at + byte] != want[byte])
               return (size_t)row * C7_TEXTURE_WIDTH + column;
         }
      }
   }
   return (size_t)C7_TEXTURE_WIDTH * C7_TEXTURE_HEIGHT;
}

/* The texel the scaled blit's 2x upscale of the middle half puts in destination
 * texel (x, y): the source texel the sample point names for a nearest blit, the
 * ideal bilinear blend of the four around it for a filtered one. The mapping is
 * the one the probe's checker uses (src/diagnostics.cpp, scaled_blit_texel). */
static unsigned
blit_clamp(double value, unsigned first, unsigned last)
{
   const long at = (long)floor(value);
   return (unsigned)(at < (long)first ? (long)first : (at > (long)last ? (long)last : at));
}

static void
blit_source_texel(unsigned column, unsigned row, unsigned char *bytes)
{
   memcpy(bytes, kTexels + ((size_t)row * C7_TEXTURE_WIDTH + column) * 4, 4);
}

static void
scaled_blit_texel(unsigned x, unsigned y, bool filtered, unsigned char *out)
{
   const unsigned source_x0 = C7_TEXTURE_WIDTH / 4;
   const unsigned source_y0 = C7_TEXTURE_HEIGHT / 4;
   const unsigned last_x = C7_TEXTURE_WIDTH - source_x0 - 1;
   const unsigned last_y = C7_TEXTURE_HEIGHT - source_y0 - 1;
   const double sample_x = (double)source_x0 + (double)x * 0.5 - 0.25;
   const double sample_y = (double)source_y0 + (double)y * 0.5 - 0.25;
   unsigned char a[4] = {0};
   unsigned char b[4] = {0};
   unsigned char c[4] = {0};
   unsigned char d[4] = {0};
   if (!filtered) {
      blit_source_texel(blit_clamp(sample_x + 0.5, source_x0, last_x),
                        blit_clamp(sample_y + 0.5, source_y0, last_y), out);
      return;
   }
   const double x0 = floor(sample_x);
   const double y0 = floor(sample_y);
   const double fx = sample_x - x0;
   const double fy = sample_y - y0;
   blit_source_texel(blit_clamp(x0, source_x0, last_x), blit_clamp(y0, source_y0, last_y), a);
   blit_source_texel(blit_clamp(x0 + 1.0, source_x0, last_x), blit_clamp(y0, source_y0, last_y), b);
   blit_source_texel(blit_clamp(x0, source_x0, last_x), blit_clamp(y0 + 1.0, source_y0, last_y), c);
   blit_source_texel(blit_clamp(x0 + 1.0, source_x0, last_x), blit_clamp(y0 + 1.0, source_y0, last_y),
                     d);
   for (unsigned byte = 0; byte < 4; byte++) {
      /* The colour channels blend; alpha is opaque in every source texel. */
      const double top = (double)a[byte] * (1.0 - fx) + (double)b[byte] * fx;
      const double bottom = (double)c[byte] * (1.0 - fx) + (double)d[byte] * fx;
      const double value = top * (1.0 - fy) + bottom * fy + 0.5;
      out[byte] = byte == 3 ? 0xff
                            : (unsigned char)(value < 0.0 ? 0.0 : (value > 255.0 ? 255.0 : value));
   }
}

/* How many of the scaled destination's texels are the colour the reference
 * names: a filtered blit within one level either way, a nearest one exactly. */
static size_t
scaled_blit_matches(const void *copied, bool filtered)
{
   const unsigned char *const bytes = copied;
   size_t matching = 0;
   for (unsigned row = 0; row < C7_TEXTURE_HEIGHT; row++) {
      for (unsigned column = 0; column < C7_TEXTURE_WIDTH; column++) {
         unsigned char want[4] = {0};
         scaled_blit_texel(column, row, filtered, want);
         const size_t at = tiled_offset(column, row);
         bool same = true;
         for (unsigned byte = 0; byte < 4; byte++) {
            const unsigned char got = bytes[at + byte];
            same = same && (filtered ? (got + 1u >= want[byte] && want[byte] + 1u >= got)
                                     : got == want[byte]);
         }
         if (same)
            matching++;
      }
   }
   return matching;
}

/* The canary's square: position (x, y) then texture coordinate (u, v) per
 * 16-byte record, with texel row 0 at the geometry's +y edge, and two triangles
 * over six indices. Its vertex shader was compiled for exactly this layout
 * (tools/build-probe-shaders.sh m3-texture), which is the set src/diagnostics.cpp's
 * c7-copy frames draw with. */
#define C7_VERTEX_STRIDE 16
#define C7_VERTEX_COUNT 4
#define C7_INDEX_COUNT 6

static const float kVertices[C7_VERTEX_COUNT * 4] = {
   -0.5f, -0.5f, 0.0f, 1.0f, /* 0 bottom left */
   0.5f,  -0.5f, 1.0f, 1.0f, /* 1 bottom right */
   0.5f,  0.5f,  1.0f, 0.0f, /* 2 top right */
   -0.5f, 0.5f,  0.0f, 0.0f, /* 3 top left */
};

static const uint16_t kIndices[C7_INDEX_COUNT] = {0, 1, 2, 2, 3, 0};

/* How a program fills the tiled image it samples. */
enum transfer_kind {
   TRANSFER_COPY,
   TRANSFER_BLIT,
   TRANSFER_SCALED,
   TRANSFER_SCALED_LINEAR,
};

/* One program: the canary's square over the caller's texels, copied or blitted
 * into a tiled image the frames sample. */
static bool
run_transfer(const uint32_t *vertex, size_t vertex_bytes, const uint32_t *pixel, size_t pixel_bytes,
             enum transfer_kind kind)
{
   const bool blitted = kind == TRANSFER_BLIT;
   const bool scaled = kind == TRANSFER_SCALED || kind == TRANSFER_SCALED_LINEAR;
   const bool scaled_linear = kind == TRANSFER_SCALED_LINEAR;
   struct steps steps = {0};
   const struct ps5vk_triangle_report report = {&steps, record_step};
   const VkVertexInputAttributeDescription attributes[2] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
   };
   const struct ps5vk_triangle_input input = {
      GET_PROC, 1,
      {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
      VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
      kVertices, C7_VERTEX_COUNT, C7_VERTEX_STRIDE, kIndices, C7_INDEX_COUNT,
      2, {attributes[0], attributes[1]},
      /* Directly, not staged: the upload this test is about is the texture's,
       * and C7's copy follows it. */
      false,
      NULL, 0, 0,
   };
   struct ps5vk_triangle_input configured = input;
   configured.texture_data = kTexels;
   configured.texture_width = C7_TEXTURE_WIDTH;
   configured.texture_height = C7_TEXTURE_HEIGHT;
   configured.texture_bilinear = true;
   configured.texture_copied = kind == TRANSFER_COPY;
   configured.texture_blitted = blitted;
   configured.texture_blit_scaled = scaled;
   configured.texture_blit_linear = scaled_linear;
   struct ps5vk_triangle triangle = {0};

   enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &configured);
   if (status == PS5VK_TRIANGLE_OK)
      status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
   check(status == PS5VK_TRIANGLE_OK,
         scaled_linear ? "the filtered scaled blit frame records, submits and signals its fence"
                       : scaled  ? "the scaled blit frame records, submits and signals its fence"
                                 : blitted ? "the one-to-one blit frame records, submits and "
                                             "signals its fence"
                                           : "the copy frame records, submits and signals its "
                                             "fence");
   check(steps.failed == NULL, "no step of the transfer's draw failed before its fence");
   if (status == PS5VK_TRIANGLE_OK && triangle.copied != NULL) {
      const size_t total = (size_t)C7_TEXTURE_WIDTH * C7_TEXTURE_HEIGHT;
      if (scaled) {
         const size_t matching = scaled_blit_matches(triangle.copied, scaled_linear);
         check(matching == total,
               scaled_linear ? "the filtered blit's destination is the ideal 2x blend of the "
                               "middle half"
                             : "the scaled blit's destination is the middle half, 2x, exactly");
         if (matching != total)
            printf("  (%zu of %zu destination texels are the reference's)\n", matching, total);
      } else {
         const size_t bad = first_tiled_mismatch(triangle.copied);
         check(bad == total,
               blitted ? "the blit left the texels at the tiled addresses the map names"
                       : "the copy left the texels at the tiled addresses the map names");
         if (bad != total)
            printf("  (texel %zu, column %zu row %zu, is not the caller's)\n", bad,
                   bad % C7_TEXTURE_WIDTH, bad / C7_TEXTURE_WIDTH);
      }
   } else {
      check(false, "the frame's tiled destination is mapped for the readback");
   }
   if (status != PS5VK_TRIANGLE_IN_FLIGHT)
      ps5vk_triangle_finish(&triangle);
   return status == PS5VK_TRIANGLE_OK;
}

int
main(void)
{
   test_begin("C7 copy");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "m3-texture", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m3-texture", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m3-texture SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      fill_texels();
      run_transfer(vertex, vertex_bytes, pixel, pixel_bytes, TRANSFER_COPY);
      run_transfer(vertex, vertex_bytes, pixel, pixel_bytes, TRANSFER_BLIT);
      run_transfer(vertex, vertex_bytes, pixel, pixel_bytes, TRANSFER_SCALED);
      run_transfer(vertex, vertex_bytes, pixel, pixel_bytes, TRANSFER_SCALED_LINEAR);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
