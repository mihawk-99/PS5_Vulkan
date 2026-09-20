/*
 * PS5 Vulkan driver - V0-formats test: seventeen required sampled formats.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase V0-formats (docs/V0_FORMATS_AUDIT.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the m3-texture canary's square once per format, seventeen frames over
 * seventeen devices, each sampling a 256x4 image of that format whose every
 * texel holds one value chosen so the decode is exact: the same seventeen
 * frames src/diagnostics.cpp's `v0-formats-sampled` draws on the console, with
 * the same texels and the same linear filter, in the same order. Each format's
 * image goes through the harness (VK_IMAGE_USAGE_SAMPLED_BIT |
 * VK_IMAGE_USAGE_TRANSFER_DST_BIT, a staging buffer and vkCmdCopyBufferToImage,
 * a view and the descriptor set the pixel shader samples through), and the
 * format is what the driver's format table carries into the combined image
 * sampler descriptor: the FORMAT field's GFX10 IMG_DATA_FORMAT and word 3's
 * DST_SEL channel selectors, which is where a one- or two-channel format's fill
 * rule comes from (driver/ps5vk_image.c, driver/ps5vk_draw.c).
 *
 * Nothing renders on the PC, so this program's job is the stream: that every
 * format's frame records, submits and signals its fence, that the driver built
 * the image, its view, its samplers and its descriptor set, and that the
 * upload left that format's texels in the image memory byte for byte. The frame
 * itself is the console's to prove: src/diagnostics.cpp's v0-formats-sampled
 * reads each square back and requires the colour the texel decodes to, and
 * tools/check-driver.sh compares every submission below with the console's own
 * run of it (golden/v0-formats-sampled), descriptor word for descriptor word.
 * PS5VK_PROBES names the probes directory. The PS5 build only links; it is not
 * run.
 */

#define _POSIX_C_SOURCE 200809L

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

/* The first byte where actual and expected differ, or bytes when they are the
 * same, so a failure names the byte of the upload that went wrong. */
static size_t
first_mismatch(const void *actual, const void *expected, size_t bytes)
{
   const unsigned char *const have = actual;
   const unsigned char *const want = expected;
   for (size_t index = 0; index < bytes; index++) {
      if (have[index] != want[index])
         return index;
   }
   return bytes;
}

/* One format of the twenty-two, with the value its every texel holds: the bytes
 * of one texel in the order the format stores them, tightly packed.
 * src/diagnostics.cpp's sampled_formats holds the same twenty-two in the same
 * order, and its readback check is what accepts them on the console. */
#define V0_FORMATS 27
#define V0_FORMAT_TEXEL_BYTES 16

struct v0_format {
   VkFormat format;
   uint32_t texel_bytes;
   const unsigned char *texel;
};

/* The 4-byte layout, which the M3 texture canary and C4 proved already.
 * Distinct channels, so a byte order a format does not have shows up: RGBA8
 * stores R, G, B, A and A8B8G8R8 stores A, B, G, R. The sRGB bytes 0x89, 0xbc
 * and 0xe1 linearise to 0x40, 0x80 and 0xc0. */
static const unsigned char kTexelRgba8[4] = {0x40, 0x80, 0xc0, 0xff};
static const unsigned char kTexelA8B8G8R8Unorm[4] = {0xff, 0xc0, 0x80, 0x40};
static const unsigned char kTexelSrgb[4] = {0x89, 0xbc, 0xe1, 0xff};
/* Round 13's byte-reversed sRGB form: the same red, green and blue stored back
 * to front, which is the order its memory has (B, G, R, A). */
static const unsigned char kTexelB8G8R8A8Srgb[4] = {0xe1, 0xbc, 0x89, 0xff};
/* The packed byte-reversed sRGB form: its Vulkan layout puts red last, and the
 * driver stores its texels the other way round (round 17). */
static const unsigned char kTexelA8B8G8R8Srgb[4] = {0xff, 0xe1, 0xbc, 0x89};
static const unsigned char kTexelSnorm[4] = {0x20, 0x40, 0x60, 0x7f};
/* The one- and two-channel SNORM forms round 6 added to the console table, and
 * the byte-reversed UNORM row: the checks below are the upload and the frame, so
 * each row's own bytes are what fill_texels writes. */
static const unsigned char kTexelR8Snorm[1] = {0x20};
static const unsigned char kTexelRg8Snorm[2] = {0x20, 0x40};
static const unsigned char kTexelB8G8R8A8Unorm[4] = {0xc0, 0x80, 0x40, 0xff};
static const unsigned char kTexelA8B8G8R8Snorm[4] = {0x7f, 0x60, 0x40, 0x20};
/* The 8-bit one- and two-channel families. */
static const unsigned char kTexelR8[1] = {0x80};
static const unsigned char kTexelRg8[2] = {0x80, 0x40};
/* The 16-bit unorm family: 0x8000 is half of the range. */
static const unsigned char kTexelR16Unorm[2] = {0x00, 0x80};
static const unsigned char kTexelRg16Unorm[4] = {0x00, 0x80, 0x00, 0x80};
static const unsigned char kTexelRgba16Unorm[8] = {0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0xff, 0xff};
/* Half floats: 0x3800 is 0.5, 0x3c00 one. */
static const unsigned char kTexelR16Float[2] = {0x00, 0x38};
static const unsigned char kTexelRg16Float[4] = {0x00, 0x38, 0x00, 0x38};
static const unsigned char kTexelRgba16Float[8] = {0x00, 0x38, 0x00, 0x38,
                                                   0x00, 0x38, 0x00, 0x3c};
/* Full floats: 0x3f000000 is 0.5. */
static const unsigned char kTexelR32Float[4] = {0x00, 0x00, 0x00, 0x3f};
static const unsigned char kTexelRg32Float[8] = {0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f};
static const unsigned char kTexelRgba32Float[16] = {0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f,
                                                    0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x80, 0x3f};
/* V0-formats' packed families: components that are not byte-aligned, so the
 * fetch is a decode. The values are src/diagnostics.cpp's, and
 * tools/packed-format-check.py is what decodes them into the colours the
 * console's frames are read against. */
static const unsigned char kTexelR5G6B5[2] = {0x08, 0xfc};
static const unsigned char kTexelA1R5G5B5[2] = {0x08, 0xfe};
static const unsigned char kTexelB4G4R4A4[2] = {0xff, 0x48};
static const unsigned char kTexelE5B9G9R9[4] = {0x00, 0x01, 0x01, 0x79};
static const unsigned char kTexelB10G11R11[4] = {0x80, 0x03, 0x1a, 0x60};
static const unsigned char kTexelA2B10G10R10[4] = {0x00, 0x02, 0x04, 0xc8};

/* Whether the image's memory holds this row's texels reversed: the format the
 * driver stores that way (driver/ps5vk_private.h, ps5vk_format.storage_reversed),
 * whose upload is checked against the storage's order rather than the caller's. */
static bool
storage_reversed(VkFormat format)
{
   return format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

static const struct v0_format kFormats[V0_FORMATS] = {
   {VK_FORMAT_R8G8B8A8_UNORM, 4, kTexelRgba8},
   {VK_FORMAT_A8B8G8R8_UNORM_PACK32, 4, kTexelA8B8G8R8Unorm},
   {VK_FORMAT_R8G8B8A8_SRGB, 4, kTexelSrgb},
   {VK_FORMAT_A8B8G8R8_SRGB_PACK32, 4, kTexelA8B8G8R8Srgb},
   {VK_FORMAT_B8G8R8A8_SRGB, 4, kTexelB8G8R8A8Srgb},
   {VK_FORMAT_R8_SNORM, 1, kTexelR8Snorm},
   {VK_FORMAT_R8G8_SNORM, 2, kTexelRg8Snorm},
   {VK_FORMAT_B8G8R8A8_UNORM, 4, kTexelB8G8R8A8Unorm},
   {VK_FORMAT_R8G8B8A8_SNORM, 4, kTexelSnorm},
   {VK_FORMAT_A8B8G8R8_SNORM_PACK32, 4, kTexelA8B8G8R8Snorm},
   {VK_FORMAT_R8_UNORM, 1, kTexelR8},
   {VK_FORMAT_R8G8_UNORM, 2, kTexelRg8},
   {VK_FORMAT_R16_UNORM, 2, kTexelR16Unorm},
   {VK_FORMAT_R16G16_UNORM, 4, kTexelRg16Unorm},
   {VK_FORMAT_R16G16B16A16_UNORM, 8, kTexelRgba16Unorm},
   {VK_FORMAT_R16_SFLOAT, 2, kTexelR16Float},
   {VK_FORMAT_R16G16_SFLOAT, 4, kTexelRg16Float},
   {VK_FORMAT_R16G16B16A16_SFLOAT, 8, kTexelRgba16Float},
   {VK_FORMAT_R32_SFLOAT, 4, kTexelR32Float},
   {VK_FORMAT_R32G32_SFLOAT, 8, kTexelRg32Float},
   {VK_FORMAT_R32G32B32A32_SFLOAT, 16, kTexelRgba32Float},
   {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, kTexelR5G6B5},
   {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, kTexelA1R5G5B5},
   {VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2, kTexelB4G4R4A4},
   {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, kTexelE5B9G9R9},
   {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, kTexelB10G11R11},
   {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, kTexelA2B10G10R10},
};

/* The 256x4 texel image every frame samples, in the widest texel any of them
 * has: 256 texels are exactly the 256-byte row the driver pads an untiled
 * image's rows to, so the upload is row for row and the readback is the
 * pattern (driver/ps5vk_image.c, PS5VK_IMAGE_STORAGE_ROWS). */
#define V0_TEXTURE_WIDTH 256
#define V0_TEXTURE_HEIGHT 4

static unsigned char kTexels[V0_TEXTURE_WIDTH * V0_TEXTURE_HEIGHT * V0_FORMAT_TEXEL_BYTES];

static void
fill_texels(const struct v0_format *format)
{
   const size_t count = (size_t)V0_TEXTURE_WIDTH * V0_TEXTURE_HEIGHT;
   for (size_t texel = 0; texel < count; texel++)
      memcpy(kTexels + texel * format->texel_bytes, format->texel, format->texel_bytes);
}

/* The canary's square: position (x, y) then texture coordinate (u, v) per
 * 16-byte record, with texel row 0 at the geometry's +y edge, and two triangles
 * over six indices. Its vertex shader was compiled for exactly this layout
 * (tools/build-probe-shaders.sh m3-texture: location 0 R32G32_SFLOAT at offset
 * 0, location 1 R32G32_SFLOAT at offset 8, stride 16). src/diagnostics.cpp
 * holds the same records as kTextureVertices and kIndices, and its readback
 * check is what accepts the frames on the console. */
#define V0_VERTEX_STRIDE 16
#define V0_VERTEX_COUNT 4
#define V0_INDEX_COUNT 6

static const float kVertices[V0_VERTEX_COUNT * 4] = {
   -0.5f, -0.5f, 0.0f, 1.0f, /* 0 bottom left */
   0.5f,  -0.5f, 1.0f, 1.0f, /* 1 bottom right */
   0.5f,  0.5f,  1.0f, 0.0f, /* 2 top right */
   -0.5f, 0.5f,  0.0f, 0.0f, /* 3 top left */
};

static const uint16_t kIndices[V0_INDEX_COUNT] = {0, 1, 2, 2, 3, 0};

int
main(void)
{
   test_begin("V0 formats");
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
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
      };
      unsigned framed = 0;
      unsigned uploaded = 0;

      for (unsigned index = 0; index < V0_FORMATS; index++) {
         const struct v0_format *const format = &kFormats[index];
         struct steps steps = {0};
         const struct ps5vk_triangle_report report = {&steps, record_step};
         struct ps5vk_triangle_input input = {
            GET_PROC, 1,
            {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
            kVertices, V0_VERTEX_COUNT, V0_VERTEX_STRIDE, kIndices, V0_INDEX_COUNT,
            2, {attributes[0], attributes[1]},
            /* Directly, not staged: the formats are what these frames are
             * about, and every earlier texture frame uploaded the same way. */
            false,
            NULL, 0, 0,
         };
         /* The canary's texels in this format, sampled with the linear filter
          * the console's seventeen frames ran. */
         fill_texels(format);
         input.texture_data = kTexels;
         input.texture_width = V0_TEXTURE_WIDTH;
         input.texture_height = V0_TEXTURE_HEIGHT;
         input.texture_bilinear = true;
         input.texture_format = format->format;
         struct ps5vk_triangle triangle = {0};

         enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
         if (status == PS5VK_TRIANGLE_OK)
            status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         if (status == PS5VK_TRIANGLE_OK)
            framed++;
         check(status == PS5VK_TRIANGLE_OK,
               "every format's frame records, submits and signals its fence");
         check(steps.failed == NULL, "no step of a format's draw failed before its fence");
         if (status == PS5VK_TRIANGLE_OK) {
            check(triangle.texture_image != VK_NULL_HANDLE &&
                     triangle.texture_view != VK_NULL_HANDLE &&
                     triangle.texture_samplers[0] != VK_NULL_HANDLE &&
                     triangle.texture_set != VK_NULL_HANDLE,
                  "every format's frame sampled an image through a combined image sampler");
            const size_t texel_bytes =
               (size_t)V0_TEXTURE_WIDTH * V0_TEXTURE_HEIGHT * format->texel_bytes;
            /* A reversed row's image memory holds the caller's texels with their
             * four bytes swapped, which is the storage the fetch needs. */
            if (storage_reversed(format->format)) {
               static unsigned char swapped[V0_TEXTURE_WIDTH * V0_TEXTURE_HEIGHT * 16];
               const size_t texels = texel_bytes / format->texel_bytes;
               memset(swapped, 0, sizeof(swapped));
               for (size_t texel = 0; texel < texels; texel++)
                  for (unsigned byte = 0; byte < format->texel_bytes; byte++)
                     swapped[texel * format->texel_bytes + byte] =
                        kTexels[texel * format->texel_bytes + format->texel_bytes - 1 - byte];
               if (triangle.texture_mapped != NULL && triangle.texture_bytes >= texel_bytes &&
                   first_mismatch(triangle.texture_mapped, swapped, texel_bytes) == texel_bytes)
                  uploaded++;
               else
                  printf("  (format %d: the upload is not the reversed texel pattern)\n",
                         (int)format->format);
            } else if (triangle.texture_mapped != NULL && triangle.texture_bytes >= texel_bytes &&
                       first_mismatch(triangle.texture_mapped, kTexels, texel_bytes) == texel_bytes) {
               uploaded++;
            }
            else
               printf("  (format %d: the upload is not the texel pattern)\n", (int)format->format);
         }
         if (status != PS5VK_TRIANGLE_IN_FLIGHT)
            ps5vk_triangle_finish(&triangle);
         if (status == PS5VK_TRIANGLE_IN_FLIGHT)
            break;
      }
      check(framed == V0_FORMATS, "all twenty-seven formats drew");
      check(uploaded == V0_FORMATS, "every format's texels reached its image byte for byte");
      printf("  (%u of %u formats framed, %u uploaded)\n", framed, V0_FORMATS, uploaded);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
