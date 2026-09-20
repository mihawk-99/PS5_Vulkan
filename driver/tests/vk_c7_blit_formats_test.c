/*
 * PS5 Vulkan driver - Phase C7 test: a blit of every reported sampled format.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C7 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the m3-texture canary's square once per sampled format the driver
 * reports, sixteen frames over sixteen devices. Each frame's texture is solid
 * in that format's own texel -- the value src/diagnostics.cpp's sampled_formats
 * carries, whose decode V0-formats measured on the console -- and the frame
 * blits it, scaled, into a tiled R8G8B8A8_UNORM destination it then samples.
 * What this program checks is the resampler's decode: every destination texel
 * must hold the colour the format's fetch decodes to, which is what each
 * format's BLIT_SRC rests on. The frames themselves are the console's to prove;
 * tools/check-driver.sh compares every submission with the console's own run of
 * them (golden/c7-blit-formats). PS5VK_PROBES names the probes directory. The
 * PS5 build only links; it is not run.
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

/* Where a texel of a tiled image lands: the map the driver's resampler writes
 * with (driver/ps5vk_image.c, ps5vk_tiled_texel_offset). The destination is the
 * solid texture's own size, so the block grid strides by that. */
#define C7_TILE_TEXELS 128
#define C7_TILE_BYTES 0x10000
#define C7_SOLID_WIDTH 256
#define C7_SOLID_HEIGHT 4

static size_t
tiled_offset(uint32_t x, uint32_t y)
{
   const size_t sx = x;
   const size_t sy = y;
   const size_t local = ((sy << 4) & 0x70u) ^ ((sy << 5) & 0xf00u) ^ ((sy << 9) & 0x1000u) ^
                        ((sy << 8) & 0x4000u) ^ ((sx << 2) & 0x0cu) ^ ((sx << 5) & 0x380u) ^
                        ((sx << 4) & 0x400u) ^ ((sx << 6) & 0x800u) ^ ((sx << 9) & 0xa000u);
   const size_t blocks_per_row = (C7_SOLID_WIDTH + C7_TILE_TEXELS - 1u) / C7_TILE_TEXELS;
   const size_t block = (sy / C7_TILE_TEXELS) * blocks_per_row + sx / C7_TILE_TEXELS;
   return block * C7_TILE_BYTES + local;
}

/* The canary's square: position (x, y) then texture coordinate (u, v) per
 * 16-byte record, and two triangles over six indices, as the m3-texture set
 * declares them (tools/build-probe-shaders.sh m3-texture). */
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

/* Phase C7's per-format blits: the sampled formats the console's
 * c7-blit-formats frames blit into a tiled R8G8B8A8_UNORM destination, with the
 * colour each one's fetch decodes to -- the expectation V0-formats' probe
 * measured on the hardware. The destination's four bytes come from the
 * resampler's decode, so this is what each format's BLIT_SRC rests on. The set
 * mirrors the console case's own table (src/diagnostics.cpp, sampled_formats),
 * including round 17's packed byte-reversed sRGB row, whose image memory holds
 * the R, G, B, A order the driver's storage convention gives it. */
#define C7_FORMATS 55

struct c7_format {
   VkFormat format;
   uint32_t texel_bytes;
   unsigned char texel[16];
   unsigned char expected[3];
};

static const struct c7_format kFormats[C7_FORMATS] = {
   /* One row per format the console's c7-blit-formats case blits -- its
    * sampled table, whose rows are src/diagnostics.cpp's sampled_formats()
    * and whose count is what the capture's submissions are compared
    * against. Round 18 gave the packed sRGB row its blit source and this
    * table the whole console set, so the two cannot drift again. */
   {VK_FORMAT_R8G8B8A8_UNORM, 4, {0x40, 0x80, 0xc0, 0xff}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_A8B8G8R8_UNORM_PACK32, 4, {0xff, 0xc0, 0x80, 0x40}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R8G8B8A8_SRGB, 4, {0x89, 0xbc, 0xe1, 0xff}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_B8G8R8A8_SRGB, 4, {0xe1, 0xbc, 0x89, 0xff}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_A8B8G8R8_SRGB_PACK32, 4, {0xff, 0xe1, 0xbc, 0x89}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R8G8B8A8_SNORM, 4, {0x20, 0x40, 0x60, 0x7f}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_A8B8G8R8_SNORM_PACK32, 4, {0x7f, 0x60, 0x40, 0x20}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R16G16B16A16_UINT, 8, {0x00, 0x40, 0x00, 0x80, 0x00, 0xc0, 0xff, 0xff}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R8_SNORM, 1, {0x20}, {0x40, 0x00, 0x00}},
   {VK_FORMAT_R8G8_SNORM, 2, {0x20, 0x40}, {0x40, 0x80, 0x00}},
   {VK_FORMAT_R8_UNORM, 1, {0x80}, {0x80, 0x00, 0x00}},
   {VK_FORMAT_R8G8_UNORM, 2, {0x80, 0x40}, {0x80, 0x40, 0x00}},
   {VK_FORMAT_R16_UNORM, 2, {0x00, 0x80}, {0x80, 0x00, 0x00}},
   {VK_FORMAT_R16G16_UNORM, 4, {0x00, 0x80, 0x00, 0x80}, {0x80, 0x80, 0x00}},
   {VK_FORMAT_R16G16B16A16_UNORM, 8, {0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0xff, 0xff}, {0x80, 0x80, 0x80}},
   {VK_FORMAT_R16_SFLOAT, 2, {0x00, 0x38}, {0x80, 0x00, 0x00}},
   {VK_FORMAT_R16G16_SFLOAT, 4, {0x00, 0x38, 0x00, 0x38}, {0x80, 0x80, 0x00}},
   {VK_FORMAT_R16G16B16A16_SFLOAT, 8, {0x00, 0x38, 0x00, 0x38, 0x00, 0x38, 0x00, 0x3c}, {0x80, 0x80, 0x80}},
   {VK_FORMAT_R32_SFLOAT, 4, {0x00, 0x00, 0x00, 0x3f}, {0x80, 0x00, 0x00}},
   {VK_FORMAT_R32G32_SFLOAT, 8, {0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f}, {0x80, 0x80, 0x00}},
   {VK_FORMAT_R32G32B32A32_SFLOAT, 16, {0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x80, 0x3f}, {0x80, 0x80, 0x80}},
   {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, {0x08, 0xfc}, {0xff, 0x82, 0x42}},
   {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, {0x08, 0xfe}, {0xff, 0x84, 0x42}},
   {VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2, {0xff, 0x48}, {0xff, 0x88, 0x44}},
   {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, {0x00, 0x01, 0x01, 0x79}, {0x80, 0x40, 0x20}},
   {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, {0x80, 0x03, 0x1a, 0x60}, {0x80, 0x40, 0x20}},
   {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, {0x00, 0x02, 0x04, 0xc8}, {0x80, 0x40, 0x20}},
   {VK_FORMAT_R8_UINT, 1, {0x40}, {0x40, 0x00, 0x00}},
   {VK_FORMAT_R8_SINT, 1, {0x20}, {0x40, 0x00, 0x00}},
   {VK_FORMAT_R8G8_UINT, 2, {0x40, 0x80}, {0x40, 0x80, 0x00}},
   {VK_FORMAT_R8G8_SINT, 2, {0x20, 0x40}, {0x40, 0x80, 0x00}},
   {VK_FORMAT_R8G8B8A8_UINT, 4, {0x40, 0x80, 0xc0, 0xff}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R8G8B8A8_SINT, 4, {0x20, 0x40, 0x60, 0x7f}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_A8B8G8R8_SINT_PACK32, 4, {0x20, 0x40, 0x60, 0x7f}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R8_UINT, 1, {0x40}, {0x40, 0x00, 0x00}},
   {VK_FORMAT_R8_SINT, 1, {0x20}, {0x40, 0x00, 0x00}},
   {VK_FORMAT_R8G8_UINT, 2, {0x40, 0x80}, {0x40, 0x80, 0x00}},
   {VK_FORMAT_R8G8_SINT, 2, {0x20, 0x40}, {0x40, 0x80, 0x00}},
   {VK_FORMAT_R8G8B8A8_UINT, 4, {0x40, 0x80, 0xc0, 0xff}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R8G8B8A8_SINT, 4, {0x20, 0x40, 0x60, 0x7f}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_A8B8G8R8_UINT_PACK32, 4, {0x40, 0x80, 0xc0, 0xff}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_A8B8G8R8_SINT_PACK32, 4, {0x20, 0x40, 0x60, 0x7f}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R16_UINT, 2, {0x00, 0x40}, {0x40, 0x00, 0x00}},
   {VK_FORMAT_R16_SINT, 2, {0x00, 0x20}, {0x40, 0x00, 0x00}},
   {VK_FORMAT_R16G16_UINT, 4, {0x00, 0x40, 0x00, 0x80}, {0x40, 0x80, 0x00}},
   {VK_FORMAT_R16G16_SINT, 4, {0x00, 0x20, 0x00, 0x40}, {0x40, 0x80, 0x00}},
   {VK_FORMAT_R16G16B16A16_SINT, 8, {0x00, 0x20, 0x00, 0x40, 0x00, 0x60, 0xff, 0x7f}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R32_UINT, 4, {0x00, 0x00, 0x00, 0x40}, {0x40, 0x00, 0x00}},
   {VK_FORMAT_R32_SINT, 4, {0x00, 0x00, 0x00, 0x10}, {0x20, 0x00, 0x00}},
   {VK_FORMAT_R32G32_UINT, 8, {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x80}, {0x40, 0x80, 0x00}},
   {VK_FORMAT_R32G32_SINT, 8, {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20}, {0x20, 0x40, 0x00}},
   {VK_FORMAT_R32G32B32A32_UINT, 16, {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff, 0xff, 0xff}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_R32G32B32A32_SINT, 16, {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x60, 0xff, 0xff, 0xff, 0x7f}, {0x20, 0x40, 0xc0}},
   {VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, {0x00, 0x01, 0x08, 0xf0}, {0x40, 0x80, 0xc0}},
   {VK_FORMAT_B8G8R8A8_UNORM, 4, {0xc0, 0x80, 0x40, 0xff}, {0x40, 0x80, 0xc0}},
};

static unsigned char kSolidTexels[C7_SOLID_WIDTH * C7_SOLID_HEIGHT * 16];

/* Every destination texel of the tiled image holds the format's decoded colour:
 * within one level either way, and opaque. */
static bool
within_one(unsigned char a, unsigned char b)
{
   return a + 1u >= b && b + 1u >= a;
}

static size_t
format_destination_matches(const void *copied, const struct c7_format *format)
{
   const unsigned char *const bytes = copied;
   size_t matching = 0;
   for (unsigned row = 0; row < C7_SOLID_HEIGHT; row++) {
      for (unsigned column = 0; column < C7_SOLID_WIDTH; column++) {
         const size_t at = tiled_offset(column, row);
         const unsigned char *const got = bytes + at;
         if (within_one(got[0], format->expected[0]) &&
             within_one(got[1], format->expected[1]) &&
             within_one(got[2], format->expected[2]) && got[3] == 0xff)
            matching++;
      }
   }
   return matching;
}

/* One format's frame: the solid texture of that format, blitted scaled into a
 * tiled R8G8B8A8_UNORM destination by the program, and the destination's
 * decoded colour. */
static void
run_format_blit(const uint32_t *vertex, size_t vertex_bytes, const uint32_t *pixel,
                size_t pixel_bytes, const struct c7_format *format)
{
   struct steps steps = {0};
   const struct ps5vk_triangle_report report = {&steps, record_step};
   const VkVertexInputAttributeDescription attributes[2] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
   };
   const size_t count = (size_t)C7_SOLID_WIDTH * C7_SOLID_HEIGHT;
   for (size_t texel = 0; texel < count; texel++)
      memcpy(kSolidTexels + texel * format->texel_bytes, format->texel, format->texel_bytes);
   struct ps5vk_triangle_input configured = {
      GET_PROC, 1,
      {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
      VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
      kVertices, C7_VERTEX_COUNT, C7_VERTEX_STRIDE, kIndices, C7_INDEX_COUNT,
      2, {attributes[0], attributes[1]},
      false,
      NULL, 0, 0,
   };
   configured.texture_data = kSolidTexels;
   configured.texture_width = C7_SOLID_WIDTH;
   configured.texture_height = C7_SOLID_HEIGHT;
   configured.texture_bilinear = false;
   configured.texture_format = format->format;
   configured.texture_blit_scaled = true;
   struct ps5vk_triangle triangle = {0};

   enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &configured);
   if (status == PS5VK_TRIANGLE_OK)
      status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
   check(status == PS5VK_TRIANGLE_OK, "a format's blit frame records, submits and signals its fence");
   check(steps.failed == NULL, "no step of a format's blit failed before its fence");
   if (status == PS5VK_TRIANGLE_OK && triangle.copied != NULL) {
      const size_t total = (size_t)C7_SOLID_WIDTH * C7_SOLID_HEIGHT;
      const size_t matching = format_destination_matches(triangle.copied, format);
      check(matching == total,
            "the blitted destination holds the colour the format's fetch decodes to");
      if (matching != total)
         printf("  (format %d: %zu of %zu destination texels are %02x %02x %02x)\\n",
                (int)format->format, matching, total, format->expected[0], format->expected[1],
                format->expected[2]);
   } else {
      check(false, "the format frame's tiled destination is mapped for the readback");
   }
   if (status != PS5VK_TRIANGLE_IN_FLIGHT)
      ps5vk_triangle_finish(&triangle);
}


int
main(void)
{
   test_begin("C7 blit formats");
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
      for (unsigned index = 0; index < C7_FORMATS; index++)
         run_format_blit(vertex, vertex_bytes, pixel, pixel_bytes, &kFormats[index]);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
