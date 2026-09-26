/*
 * PS5 Vulkan driver - Phase C4 test: a sampled texture uploaded through a
 * staging buffer.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C4 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the m3-texture canary set: the indexed square of four
 * R32G32_SFLOAT position and R32G32_SFLOAT texture-coordinate records (stride
 * 16, six 16-bit indices), whose pixel shader samples an RGBA8 image through a
 * combined image sampler at set 0, binding 0. The harness creates the sampled
 * image (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT), binds
 * and maps its memory, uploads the texels from a mapped staging buffer with
 * vkCmdCopyBufferToImage, creates the image view, the clamp-to-edge samplers
 * and the descriptor set, and records vkCmdBindDescriptorSets and
 * vkCmdDrawIndexed before the draw (driver/tests/ps5vk_triangle.c). The
 * m3-texture shaders were compiled for exactly that layout
 * (tools/build-probe-shaders.sh m3-texture, probes/m3-texture/compile.txt:
 * --vertex-attribute 0:r32g32_float:0:0:16:4,
 * --vertex-attribute 1:r32g32_float:0:8:16:4 and
 * --descriptor-binding 0:0:combined_image_sampler:1:0:48).
 *
 * The program uploads the 64x36 texel pattern the console's own run of that
 * canary used -- red encodes the column, green the row and blue a two-level
 * checker -- and draws two frames with it, the first nearest and the second
 * bilinear, replacing the sampler the descriptor set names with
 * ps5vk_triangle_set_texture_filter between them, as src/diagnostics.cpp's
 * c4-texture test does. Nothing renders on the PC, so this program's job is
 * the stream: that both frames record, submit and signal their fence, and that
 * the driver built the image, its view, its samplers, its staging buffer and
 * its descriptor set, and left the uploaded texels in the image memory the
 * copy wrote. The frame itself is the console's to prove: src/diagnostics.cpp's
 * c4-texture draws the same two frames through the same driver and reads the
 * square back, nearest exactly and bilinear within the recorded tolerance.
 * PS5VK_PROBES names the probes directory. The PS5 build only links; it is not
 * run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <math.h>

#include "ps5vk_test.h"
#include "ps5vk_triangle.h"

/* The first failed step of a draw, apart from driver messages, and the last
 * driver message: a refused draw's wording is what says which rule refused it
 * (the component mapping's refusal below). */
struct steps {
   const char *failed;
   int result;
   char message[192];
};

static void
record_step(void *context, const char *name, bool passed, int result, const char *detail)
{
   struct steps *const steps = context;
   if (passed)
      return;
   if (strcmp(name, "vk_message") == 0) {
      printf("  (driver: %s)\n", detail);
      /* The driver's own reason comes first and the result's name after it, so
       * only the first message is kept: it is the one that names the rule. */
      if (steps->message[0] == '\0')
         snprintf(steps->message, sizeof(steps->message), "%s", detail);
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

/* The m3-texture canary's texture: 64x36 RGBA8 texels, every one distinct.
 * Red encodes the column, green the row and blue a two-level checker, so a
 * displaced, mis-strided or flipped upload changes a byte of the readback.
 * src/diagnostics.cpp holds the same pattern as its texel_at/upload_texture,
 * and its readback check is what accepts the frames on the console. */
#define C4_TEXTURE_WIDTH 64
#define C4_TEXTURE_HEIGHT 36

static unsigned char kTexels[C4_TEXTURE_WIDTH * C4_TEXTURE_HEIGHT * 4];

static void
fill_texels(void)
{
   for (unsigned row = 0; row < C4_TEXTURE_HEIGHT; row++) {
      for (unsigned column = 0; column < C4_TEXTURE_WIDTH; column++) {
         /* R, G, B, A bytes, the order an R8G8B8A8_UNORM texel holds them. */
         const unsigned char texel[4] = {(unsigned char)(column * 4), (unsigned char)(row * 7),
                                         (unsigned char)(((column ^ row) & 1) != 0 ? 0xe0 : 0x20),
                                         0xff};
         memcpy(kTexels + ((size_t)row * C4_TEXTURE_WIDTH + column) * 4, texel, sizeof(texel));
      }
   }
}

/* The canary's square: position (x, y) then texture coordinate (u, v) per
 * 16-byte record, with texel row 0 at the geometry's +y edge, and two triangles
 * over six indices. Its vertex shader was compiled for exactly this layout
 * (tools/build-probe-shaders.sh m3-texture: location 0 R32G32_SFLOAT at offset
 * 0, location 1 R32G32_SFLOAT at offset 8, stride 16). src/diagnostics.cpp
 * holds the same records as kTextureVertices and kIndices, and its readback
 * check is what accepts the frame on the console. */
#define C4_VERTEX_STRIDE 16
#define C4_VERTEX_COUNT 4
#define C4_INDEX_COUNT 6

static const float kVertices[C4_VERTEX_COUNT * 4] = {
   -0.5f, -0.5f, 0.0f, 1.0f, /* 0 bottom left */
   0.5f,  -0.5f, 1.0f, 1.0f, /* 1 bottom right */
   0.5f,  0.5f,  1.0f, 0.0f, /* 2 top right */
   -0.5f, 0.5f,  0.0f, 0.0f, /* 3 top left */
};

static const uint16_t kIndices[C4_INDEX_COUNT] = {0, 1, 2, 2, 3, 0};

int
main(void)
{
   test_begin("C4 texture");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex =
      probes ? read_spirv(probes, "m3-texture", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "m3-texture", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the m3-texture SPIR-V");
#else
   uint32_t *const vertex = NULL;
   uint32_t *const pixel = NULL;
#endif

   if (vertex && pixel) {
      struct steps steps = {0};
      const struct ps5vk_triangle_report report = {&steps, record_step};
      const VkVertexInputAttributeDescription attributes[2] = {
         {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
      };
      fill_texels();
      const struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         /* The square and its six indices, drawn with the position and
          * texture-coordinate attributes the m3-texture vertex shader declares. */
         kVertices, C4_VERTEX_COUNT, C4_VERTEX_STRIDE, kIndices, C4_INDEX_COUNT,
         2, {attributes[0], attributes[1]},
         /* Directly, not staged: the upload this test is about is the texture's
          * vkCmdCopyBufferToImage. */
         false,
         /* No uniform buffer: this set's one descriptor is the sampler below. */
         NULL, 0, 0,
         /* The canary's texels, uploaded into a sampled image, with the
          * canary's first frame nearest. */
         kTexels, C4_TEXTURE_WIDTH, C4_TEXTURE_HEIGHT, false,
      };
      /* Zeroed first: a create that fails before it fills the program in
       * leaves the checks below and ps5vk_triangle_finish with nothing to
       * read or release. */
      struct ps5vk_triangle triangle = {0};

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      unsigned frames = 0;
      if (status == PS5VK_TRIANGLE_OK)
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      if (status == PS5VK_TRIANGLE_OK)
         frames++;
      check(status == PS5VK_TRIANGLE_OK,
            "the textured square records, submits and signals its fence");
      check(steps.failed == NULL, "no step of the draw failed before the fence");
      if (status == PS5VK_TRIANGLE_OK) {
         /* The objects this phase adds: the sampled image, its view, its
          * samplers, the staging buffer the texels came from and the set that
          * names the view and sampler. */
         check(triangle.texture_image != VK_NULL_HANDLE && triangle.texture_view != VK_NULL_HANDLE,
               "the draw sampled an image through an image view");
         check(triangle.texture_samplers[0] != VK_NULL_HANDLE &&
                  triangle.texture_samplers[1] != VK_NULL_HANDLE &&
                  triangle.texture_set != VK_NULL_HANDLE &&
                  triangle.texture_set_layout != VK_NULL_HANDLE,
               "the draw bound a combined image sampler at set 0, binding 0");
         check(triangle.texture_staging_buffer != VK_NULL_HANDLE,
               "the texels reached the image from a staging buffer");

         /* vkCmdCopyBufferToImage is the only thing that wrote the image
          * memory, and 64 RGBA8 texels are exactly the 256-byte row the driver
          * pads its rows to, so the readback must be the pattern byte for byte
          * (driver/ps5vk_image.c, PS5VK_IMAGE_STORAGE_ROWS). */
         const size_t texel_bytes = sizeof(kTexels);
         const bool read_back = triangle.texture_mapped != NULL &&
                                triangle.texture_bytes >= texel_bytes;
         check(read_back, "the copy left the texture in the mapped image memory");
         if (read_back) {
            const size_t bad = first_mismatch(triangle.texture_mapped, kTexels, texel_bytes);
            check(bad == texel_bytes,
                  "the uploaded texels are the canary's pattern, byte for byte");
            if (bad != texel_bytes)
               printf("  (texel byte %zu is %02x, not %02x)\n", bad,
                      ((const unsigned char *)triangle.texture_mapped)[bad], kTexels[bad]);
         }

         /* The second frame the canary drew: the same square, sampled with the
          * linear sampler the descriptor set is repointed at. */
         check(ps5vk_triangle_set_texture_filter(&triangle, true),
               "the sampler the descriptor set names changes between frames");
         status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
         if (status == PS5VK_TRIANGLE_OK)
            frames++;
      }
      check(frames == 2, "two frames submitted the textured square with different samplers");
      if (status == PS5VK_TRIANGLE_OK) {
         /* R79: the range is word 2's, +/-16 as RADV reports it; Dolphin's -3.1875
          * (Mario Kart Wii) was refused while the device reported +/-2. */
         check(ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, -2.0f) &&
                  ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, -0.5f) &&
                  ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, 0.5f) &&
                  ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, 2.0f) &&
                  ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, -3.1875f) &&
                  ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, -16.0f) &&
                  ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, 16.0f),
               "sampler biases of both signs and fractions inside the advertised range create");
         check(!ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, 16.01f) &&
                  !ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, -16.01f) &&
                  !ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, NAN) &&
                  !ps5vk_triangle_set_texture_lod_bias(&triangle, 0, 4, INFINITY),
               "out-of-range and non-finite sampler biases are refused");
         /* R57: clamp-to-border with each of Vulkan 1.0's border colours. The
          * float and integer forms of a colour share its BORDER_COLOR_TYPE. */
         static const VkBorderColor borders[] = {
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_INT_TRANSPARENT_BLACK,
            VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,      VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,      VK_BORDER_COLOR_INT_OPAQUE_WHITE,
         };
         bool bordered = true;
         for (unsigned index = 0; index < sizeof(borders) / sizeof(borders[0]); index++)
            bordered = bordered && ps5vk_triangle_set_texture_border(
                                      &triangle, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                      borders[index]);
         check(bordered, "clamp-to-border samplers with each core border colour create");
         /* The last one, opaque white, drawn: word 8 carries clamp-border (6)
          * on U and V and clamp-to-edge (2) on W, and word 11 three words on
          * carries BORDER_COLOR_TYPE 2 in bits 30-31. */
         const bool border_drawn =
            bordered && ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW) ==
                           PS5VK_TRIANGLE_OK;
         check(border_drawn, "a frame samples through the clamp-to-border sampler");
#if defined(PS5VK_TEST_DIRECT)
         if (border_drawn) {
            ps5vk_debug_table tables[4] = {{0}};
            const uint32_t count = ps5vk_debug_descriptor_tables(triangle.device, tables, 4);
            bool encoded = false;
            for (uint32_t table = 0; table < count; table++)
               for (size_t word = 0; word + 3 < tables[table].bytes / sizeof(uint32_t); word++)
                  encoded = encoded || ((tables[table].words[word] & 0x1ffu) == 0xb6u &&
                                        tables[table].words[word + 3] >> 30 == 2u);
            check(encoded, "the descriptor carries clamp-border on U and V and an opaque white "
                           "border type");
         }
#endif
         check(!ps5vk_triangle_set_texture_border(
                  &triangle, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                  VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK),
               "mirror-clamp-to-edge is refused on a device that did not enable its extension");
      }
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);

      /* R81: a device that enables VK_KHR_sampler_mirror_clamp_to_edge creates
       * the sampler, and its descriptor's word 8 carries mirror-once (3) on U
       * and V and clamp-to-edge (2) on W: 0x9b. */
      {
         struct steps mirror_steps = {0};
         const struct ps5vk_triangle_report mirror_report = {&mirror_steps, record_step};
         static const char *const mirror_extensions[] = {"VK_KHR_sampler_mirror_clamp_to_edge"};
         struct ps5vk_triangle_input mirrored = input;
         mirrored.report = &mirror_report;
         mirrored.device_extensions = mirror_extensions;
         mirrored.device_extension_count = 1;
         struct ps5vk_triangle mirror_triangle = {0};
         enum ps5vk_triangle_status mirror_status =
            ps5vk_triangle_create(&mirror_triangle, &mirrored);
         const bool mirror_created =
            mirror_status == PS5VK_TRIANGLE_OK &&
            ps5vk_triangle_set_texture_border(&mirror_triangle,
                                              VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                                              VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
         check(mirror_created, "with its extension enabled, a mirror-clamp-to-edge sampler creates");
         if (mirror_created)
            mirror_status = ps5vk_triangle_draw(&mirror_triangle, PS5VK_TRIANGLE_ONE_DRAW);
         check(mirror_created && mirror_status == PS5VK_TRIANGLE_OK,
               "a frame samples through the mirror-clamp-to-edge sampler");
#if defined(PS5VK_TEST_DIRECT)
         if (mirror_created && mirror_status == PS5VK_TRIANGLE_OK) {
            ps5vk_debug_table tables[4] = {{0}};
            const uint32_t count = ps5vk_debug_descriptor_tables(mirror_triangle.device, tables, 4);
            bool encoded = false;
            for (uint32_t table = 0; table < count; table++)
               for (size_t word = 0; word < tables[table].bytes / sizeof(uint32_t); word++)
                  encoded = encoded || (tables[table].words[word] & 0x1ffu) == 0x9bu;
            check(encoded, "the descriptor carries mirror-once on U and V");
         }
#endif
         if (mirror_status != PS5VK_TRIANGLE_IN_FLIGHT)
            ps5vk_triangle_finish(&mirror_triangle);
      }

      /* A view whose component mapping is not the identity: Vulkan applies the
       * mapping to what a combined image sampler returns, and the driver
       * composes it onto the format's own DST_SEL selectors in the descriptor
       * (driver/ps5vk_draw.c, ps5vk_compose_dst_sel). B, G, R, A over RGBA8's
       * X, Y, Z, W selectors is Z, Y, X, W: 0xf2e, where the identity is 0xfac.
       * The frame is its own program, created after the two above so the
       * addresses they landed on are the ones the golden holds. */
      {
         struct steps mapped_steps = {0};
         const struct ps5vk_triangle_report mapped_report = {&mapped_steps, record_step};
         struct ps5vk_triangle_input mapped = input;
         mapped.report = &mapped_report;
         mapped.texture_components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_B,
                                                          VK_COMPONENT_SWIZZLE_G,
                                                          VK_COMPONENT_SWIZZLE_R,
                                                          VK_COMPONENT_SWIZZLE_A};
         struct ps5vk_triangle mapped_triangle = {0};
         enum ps5vk_triangle_status mapped_status =
            ps5vk_triangle_create(&mapped_triangle, &mapped);
         check(mapped_status == PS5VK_TRIANGLE_OK,
               "a view with a component mapping other than the identity is created");
         if (mapped_status == PS5VK_TRIANGLE_OK)
            mapped_status = ps5vk_triangle_draw(&mapped_triangle, PS5VK_TRIANGLE_ONE_DRAW);
         check(mapped_status != PS5VK_TRIANGLE_FAILED && mapped_steps.failed == NULL,
               "sampling a view whose component mapping is not the identity draws");
#if defined(PS5VK_TEST_DIRECT)
         if (mapped_status != PS5VK_TRIANGLE_FAILED) {
            /* Word 3 of the combined image sampler: the 2D kind in bits 28-31 and
             * the composed selectors in bits 0-11. */
            ps5vk_debug_table tables[4] = {{0}};
            const uint32_t count = ps5vk_debug_descriptor_tables(mapped_triangle.device, tables, 4);
            bool composed = false;
            for (uint32_t table = 0; table < count; table++)
               for (size_t word = 0; word < tables[table].bytes / sizeof(uint32_t); word++)
                  composed = composed || (tables[table].words[word] >> 28 == 0x9u &&
                                          (tables[table].words[word] & 0xfffu) == 0xf2eu);
            check(composed, "the descriptor carries the mapping composed onto the format: 0xf2e");
         }
#endif
         if (mapped_status != PS5VK_TRIANGLE_IN_FLIGHT)
            ps5vk_triangle_finish(&mapped_triangle);
      }
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
