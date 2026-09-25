/*
 * PS5 Vulkan driver - Phase C4 test: render to texture, sampled in the same
 * command buffer.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C4 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * The m3-texture canary set again, with the image its pixel shader samples
 * being one the frame itself renders into: ps5vk_triangle_draw then records TWO
 * render passes into ONE command buffer (driver/tests/ps5vk_triangle.c,
 * input.texture_is_rendered). The first draws the caller's uploaded texels over
 * the whole of a second colour image through the same pipeline, its own
 * full-target quad and the nearest sampler; the second is the canary's square,
 * sampling that image. One command buffer is what the case is about: the driver
 * orders a render and a later sample of the same image with a GPU barrier in the
 * words before the draw that samples it (R70: RELEASE_MEM of
 * CACHE_FLUSH_AND_INV_TS_EVENT writing a fence, then WAIT_REG_MEM64 on it;
 * driver/ps5vk_draw.c, ps5vk_cmd_buffer_gpu_barrier). A flush alone leaves the
 * image's most recently written rows unflushed when the sample's first fetches
 * run, which the console measured as a run-varying band of the frame's top
 * rows reading zero while the image itself held every texel
 * (docs/M5_PHASE_C.md, C4); until R70 the wait was a submission split and the
 * queue's step marker. The barrier that ends every submission covers a render
 * in an earlier submission, whose marker the next one waits for
 * (driver/ps5vk_queue.c). The image is the program's target size because that
 * is the one colour attachment size the driver records, and the frame's own
 * pass and geometry are exactly the ones vk_c4_texture_test.c draws, so the
 * console's c4-rtt test can read the square back with the same checker the
 * console's c4-texture run passed with (src/diagnostics.cpp,
 * run_vulkan_rtt_frames).
 *
 * Nothing renders on the PC, so this program's job is the stream: that the two
 * render passes record into one command buffer and the submission signals its
 * fence, that no step of create or draw failed, and that the driver built the
 * objects the case needs -- the second colour image with its view, render pass
 * and framebuffer, the set the fill pass samples the uploaded texels through,
 * and the quad it draws. It also checks what the upload left in the image
 * memory the copy wrote, as vk_c4_texture_test.c does. The frame itself is the
 * console's to prove: the colour barrier is only observable where the render
 * runs, and src/diagnostics.cpp's c4-rtt draws this frame through the same
 * driver and reads the square back. PS5VK_PROBES names the probes directory.
 * The PS5 build only links; it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

/* The driver's own debug API, which a test may call: the console's
 * capture reads the same steps from it (src/diagnostics.cpp). */
#include "../ps5vk_debug.h"
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

/* The m3-texture canary's texture: 64x36 RGBA8 texels, every one distinct.
 * Red encodes the column, green the row and blue a two-level checker, so a
 * displaced, mis-strided or flipped upload -- or a fill pass that drew it into
 * the rendered image wrongly -- changes a byte of the readback.
 * src/diagnostics.cpp holds the same pattern as its texel_at/upload_texture,
 * and its readback check is what accepts the frame on the console. */
#define C4_RTT_TEXTURE_WIDTH 64
#define C4_RTT_TEXTURE_HEIGHT 36

static unsigned char kTexels[C4_RTT_TEXTURE_WIDTH * C4_RTT_TEXTURE_HEIGHT * 4];

static void
fill_texels(void)
{
   for (unsigned row = 0; row < C4_RTT_TEXTURE_HEIGHT; row++) {
      for (unsigned column = 0; column < C4_RTT_TEXTURE_WIDTH; column++) {
         /* R, G, B, A bytes, the order an R8G8B8A8_UNORM texel holds them. */
         const unsigned char texel[4] = {(unsigned char)(column * 4), (unsigned char)(row * 7),
                                         (unsigned char)(((column ^ row) & 1) != 0 ? 0xe0 : 0x20),
                                         0xff};
         memcpy(kTexels + ((size_t)row * C4_RTT_TEXTURE_WIDTH + column) * 4, texel, sizeof(texel));
      }
   }
}

/* The canary's square: position (x, y) then texture coordinate (u, v) per
 * 16-byte record, with texel row 0 at the geometry's +y edge, and two triangles
 * over six indices. Its vertex shader was compiled for exactly this layout
 * (tools/build-probe-shaders.sh m3-texture: location 0 R32G32_SFLOAT at offset
 * 0, location 1 R32G32_SFLOAT at offset 8, stride 16). The fill pass draws its
 * own full-target quad through the same pipeline and the same layout.
 * src/diagnostics.cpp holds the same records as kTextureVertices and kIndices,
 * and its readback check is what accepts the frame on the console. */
#define C4_RTT_VERTEX_STRIDE 16
#define C4_RTT_VERTEX_COUNT 4
#define C4_RTT_INDEX_COUNT 6

static const float kVertices[C4_RTT_VERTEX_COUNT * 4] = {
   -0.5f, -0.5f, 0.0f, 1.0f, /* 0 bottom left */
   0.5f,  -0.5f, 1.0f, 1.0f, /* 1 bottom right */
   0.5f,  0.5f,  1.0f, 0.0f, /* 2 top right */
   -0.5f, 0.5f,  0.0f, 0.0f, /* 3 top left */
};

static const uint16_t kIndices[C4_RTT_INDEX_COUNT] = {0, 1, 2, 2, 3, 0};

int
main(void)
{
   test_begin("C4 render to texture");
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
          * texture-coordinate attributes the m3-texture vertex shader declares.
          * The frame's own pass draws them, and the fill pass draws its own
          * full-target quad through the same pipeline. */
         kVertices, C4_RTT_VERTEX_COUNT, C4_RTT_VERTEX_STRIDE, kIndices, C4_RTT_INDEX_COUNT,
         2, {attributes[0], attributes[1]},
         /* Directly, not staged: the upload this test is about is the texture's
          * vkCmdCopyBufferToImage into the image the frame draws from. */
         false,
         /* No uniform buffer: this set's one descriptor is the sampler below. */
         NULL, 0, 0,
         /* The canary's texels, uploaded into a sampled image the fill pass then
          * draws into the image the frame samples, with the canary's first
          * frame nearest. */
         kTexels, C4_RTT_TEXTURE_WIDTH, C4_RTT_TEXTURE_HEIGHT, false,
         /* The image the frame samples is one it renders into first, both
          * passes in one command buffer: the case this test records. */
         true,
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
            "the frame renders into its texture, samples it, and signals its fence");
      check(steps.failed == NULL, "no step of the create or the draw failed before the fence");
      if (status == PS5VK_TRIANGLE_OK) {
         /* The objects the case adds: the second colour image with everything
          * the fill pass draws it through, and the set that pass samples the
          * uploaded texels through. */
         check(triangle.texture_is_rendered && triangle.rendered_image != VK_NULL_HANDLE &&
                  triangle.rendered_view != VK_NULL_HANDLE &&
                  triangle.rendered_pass != VK_NULL_HANDLE &&
                  triangle.rendered_framebuffer != VK_NULL_HANDLE,
               "the frame rendered into a second colour image, through a view, a pass and a "
               "framebuffer");
         check(triangle.texture_source_set != VK_NULL_HANDLE &&
                  triangle.fill_buffer != VK_NULL_HANDLE,
               "the fill pass drew the uploaded texels over it: a full-target quad and a set of "
               "its own");
         check(triangle.texture_set != VK_NULL_HANDLE &&
                  triangle.texture_samplers[0] != VK_NULL_HANDLE &&
                  triangle.texture_samplers[1] != VK_NULL_HANDLE,
               "the frame's set samples a combined image sampler at set 0, binding 0");

         /* vkCmdCopyBufferToImage is the only thing that wrote the uploaded
          * image's memory, and 64 RGBA8 texels are exactly the 256-byte row the
          * driver pads its rows to, so the readback must be the pattern byte
          * for byte (driver/ps5vk_image.c, PS5VK_IMAGE_STORAGE_ROWS). What the
          * fill pass drew from it is the console's readback to judge. */
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
      }
      check(frames == 1, "one frame recorded both passes and submitted");
#if defined(PS5VK_TEST_DIRECT)
      if (status == PS5VK_TRIANGLE_OK) {
         /* R70: the draw that samples the image no longer splits the
          * submission. Its words open with a GPU barrier -- RELEASE_MEM of
          * CACHE_FLUSH_AND_INV_TS_EVENT writing a fence value, then
          * WAIT_REG_MEM64 until the fence holds it -- so the frame reaches
          * the GPU as one step, and the queue gave the barrier a value the
          * release and the wait share (driver/ps5vk_draw.c,
          * ps5vk_cmd_buffer_gpu_barrier; driver/ps5vk_queue.c). The runner's
          * capture reads every step (ps5vk_debug_submission_steps), so this
          * checks what a golden of this frame holds. Only the direct build
          * calls it: the loader build sees the driver through its ICD, which
          * exports no such symbol (driver/ps5vk_icd.map). */
         ps5vk_debug_stage steps[4] = {{0}};
         const uint32_t count = ps5vk_debug_submission_steps(triangle.device, steps, 4);
         check(count == 1, "the submission reached the GPU as one step: the render, a GPU "
                           "barrier and the draw that samples it");
         if (count == 1) {
            const uint32_t *const frame = steps[0].address;
            const uint32_t frame_words = (uint32_t)(steps[0].bytes / sizeof(uint32_t));
            const uint32_t kReleaseMem = UINT32_C(0xc0064900);
            const uint32_t kBarrier = UINT32_C(0x0000c52d);
            const uint32_t kGpuBarrier = UINT32_C(0x0000c514);
            const uint32_t kWaitMem64 = UINT32_C(0xc0079300);
            uint32_t barriers = 0;
            uint32_t at = 0;
            for (uint32_t w = 0; w + 17 <= frame_words; w++) {
               if (frame[w] == kReleaseMem && frame[w + 1] == kGpuBarrier) {
                  barriers++;
                  at = w;
               }
            }
            check(barriers == 1, "one GPU barrier in the frame's words");
            check(barriers == 1 && at > 0 && frame[at + 8] == kWaitMem64 &&
                     frame[at + 9] == UINT32_C(0x06000113) && frame[at + 5] != 0 &&
                     frame[at + 12] == frame[at + 5] && frame[at + 3] == frame[at + 10] &&
                     frame[at + 4] == frame[at + 11],
                  "the barrier releases a fence value and waits in the prefetch parser until the "
                  "same address holds it");
            check(frame_words >= 16 && frame[frame_words - 16] == kReleaseMem &&
                     frame[frame_words - 15] == kBarrier && frame[frame_words - 8] == kReleaseMem &&
                     frame[frame_words - 3] != 0,
                  "the step's words end with its barrier and completion marker, whole");
         }
      }
#endif
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
