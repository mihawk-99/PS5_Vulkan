/*
 * PS5 Vulkan driver - Phase C7 test: a tiled mip chain through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase C7 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * Draws the runner's `c7-mip-tiled` frames through the driver: five horizontal
 * bands whose pixel shader takes its LOD from the fragment's own row, then one
 * frame per level with the sampler's LOD range pinned to it, sampling a
 * five-level 256x256 chain whose storage is a colour attachment's -- 64 KiB
 * tiles of 128x128 texels, the layout this console's sampler reads.
 *
 * Nothing renders on the PC, so the test checks that the six frames record,
 * submit and signal their fences, and fills the chain the way the console's
 * address map measured it (docs/HARDWARE_FINDINGS.md): each level at the base
 * the map named, its texels placed by the tile map the target readbacks decode
 * (tiled_level_offset). tools/check-driver.sh then compares the streams the
 * host layer records with the console's own run of the runner's test, whose
 * golden holds the six submissions.
 *
 * The console, not this test, is what proves the levels reach the screen:
 * src/diagnostics.cpp's `c7-mip-tiled` reads the frame back and every band must
 * hold its own level's grey. PS5VK_PROBES names the probes directory. The PS5
 * build only links; it is not run.
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

/* Phase C7's chain, as the runner builds it: five levels of 256, 128, 64, 32
 * and 16 texels, one solid grey each (src/diagnostics.cpp, kMipLevelGreys). */
#define C7_LEVELS 5
#define C7_EXTENT 256
static const uint8_t kLevelGreys[C7_LEVELS] = {10, 100, 200, 40, 60};

/* Where this console's sampler reads each level of that chain: measured, byte
 * for byte, by the address map (docs/HARDWARE_FINDINGS.md). The small levels
 * come first, which is not the layout the driver sizes its tiled images with.
 * Level 2's base is where its tail coordinate (64, 0) swizzles to -- 0x8400 --
 * not the lowest address its texels reach (0x8000): the raw map's LOD-2 address
 * 0xb4fc is 0x8400 plus the map of the texel that frame fetched, (31, 31), and
 * 0x8000 plus it is 0xb0fc. HARDWARE_FINDINGS.md has the arithmetic. */
static const uint64_t kLevelBases[C7_LEVELS] = {0x20000, 0x10000, 0x8400, 0x4800, 0x800};

/* The chain's measured length: the levels above fit in it, and the driver's own
 * sizing of the same chain is larger (each level's tiles after the last). */
#define C7_CHAIN_BYTES UINT64_C(0x60000)

/* What run 33's five pinned-LOD frames fetched, level by level
 * (Klog_Logs/c7-mip-run33.log, `agc_mip_addresses` `pinned_address`). A pinned
 * frame samples the level's centre, texel (side / 2 - 1, side / 2 - 1), so each
 * address is that level's base plus the map of that one texel: the raw numbers
 * the table above is read from, and the numbers that tell level 2's 0x8400 from
 * the 0x8000 its own texels also reach. */
static const uint32_t kPinnedAddresses[C7_LEVELS] = {0x2f0fc, 0x13cfc, 0xb4fc, 0x58fc, 0x8fc};

#define C7_TILE_TEXELS 128
#define C7_TILE_BYTES 0x10000

/* The byte offset of one texel of a level's own tiled image: the map the
 * target readbacks decode (driver/ps5vk_image.c sizes the storage the same
 * way, and src/diagnostics.cpp's tiled_rgba8_offset keeps the same XOR). */
static size_t
tiled_level_offset(uint32_t x, uint32_t y, uint32_t side)
{
   const size_t sx = x;
   const size_t sy = y;
   const size_t local = ((sy << 4) & 0x70u) ^ ((sy << 5) & 0xf00u) ^ ((sy << 9) & 0x1000u) ^
                        ((sy << 8) & 0x4000u) ^ ((sx << 2) & 0x0cu) ^ ((sx << 5) & 0x380u) ^
                        ((sx << 4) & 0x400u) ^ ((sx << 6) & 0x800u) ^ ((sx << 9) & 0xa000u);
   const size_t blocks_per_row = (side + C7_TILE_TEXELS - 1u) / C7_TILE_TEXELS;
   const size_t block = (sy / C7_TILE_TEXELS) * blocks_per_row + sx / C7_TILE_TEXELS;
   return block * C7_TILE_BYTES + local;
}

/* Every texel of every level: level L holds its grey at the base the map named,
 * which is what makes the console read the right level's bytes. The image is
 * the one the library created and bound, and its memory is host visible and
 * coherent, so the test writes the texels through it (the runner uses the
 * driver's debug hook for the same address, src/diagnostics.cpp). */
static bool
fill_chain(VkInstance instance, VkDevice device, VkDeviceMemory memory, size_t bytes)
{
   /* The device functions come from the device, as the driver's own tests take
    * them: a NULL instance answers only for the instance-level ones. */
   const PFN_vkGetDeviceProcAddr get_device_proc =
      (PFN_vkGetDeviceProcAddr)VK_FUNCTION(instance, GetDeviceProcAddr);
   const PFN_vkMapMemory map_memory =
      get_device_proc != NULL ? (PFN_vkMapMemory)get_device_proc(device, "vkMapMemory") : NULL;
   const PFN_vkUnmapMemory unmap_memory =
      get_device_proc != NULL ? (PFN_vkUnmapMemory)get_device_proc(device, "vkUnmapMemory") : NULL;
   void *mapped = NULL;
   if (map_memory == NULL || unmap_memory == NULL ||
       map_memory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == NULL)
      return false;
   uint8_t *const storage = mapped;
   for (uint32_t level = 0; level < C7_LEVELS; level++) {
      const uint32_t side = C7_EXTENT >> level;
      const uint32_t grey = kLevelGreys[level];
      const uint32_t texel = grey * 0x00010101u | 0xff000000u;
      if (kLevelBases[level] + (uint64_t)side * side * 4u > bytes)
         return false;
      for (uint32_t y = 0; y < side; y++)
         for (uint32_t x = 0; x < side; x++)
            *(uint32_t *)(storage + kLevelBases[level] + tiled_level_offset(x, y, side)) = texel;
   }
   unmap_memory(device, memory);
   return true;
}

/* Five bands of 16-byte records: a position and a texture coordinate, one band
 * per level, drawn through an index buffer (probes/c7-mip/compile.txt). */
static const float kBandVertices[C7_LEVELS * 4 * 4] = {
   /* band 0 */ -1.0f, 0.6f, 0.0f, 0.0f, 1.0f, 0.6f, 1.0f, 0.0f,
   1.0f, 1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
   /* band 1 */ -1.0f, 0.2f, 0.0f, 0.0f, 1.0f, 0.2f, 1.0f, 0.0f,
   1.0f, 0.6f, 1.0f, 1.0f, -1.0f, 0.6f, 0.0f, 1.0f,
   /* band 2 */ -1.0f, -0.2f, 0.0f, 0.0f, 1.0f, -0.2f, 1.0f, 0.0f,
   1.0f, 0.2f, 1.0f, 1.0f, -1.0f, 0.2f, 0.0f, 1.0f,
   /* band 3 */ -1.0f, -0.6f, 0.0f, 0.0f, 1.0f, -0.6f, 1.0f, 0.0f,
   1.0f, -0.2f, 1.0f, 1.0f, -1.0f, -0.2f, 0.0f, 1.0f,
   /* band 4 */ -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
   1.0f, -0.6f, 1.0f, 1.0f, -1.0f, -0.6f, 0.0f, 1.0f,
};

/* The table against the console's raw numbers: each level's base plus the map
 * of the texel its pinned frame sampled has to be the address that frame
 * fetched. A base that is any texel's address rather than the level's start
 * fails here (level 2 at 0x8000 lands 0x400 short of 0xb4fc). */
static bool
bases_match_the_measured_addresses(void)
{
   for (uint32_t level = 0; level < C7_LEVELS; level++) {
      const uint32_t side = C7_EXTENT >> level;
      const uint32_t centre = side / 2u - 1u;
      if (kLevelBases[level] + tiled_level_offset(centre, centre, side) != kPinnedAddresses[level])
         return false;
   }
   return true;
}

/* Every texel of every level at its own address inside the chain's measured
 * length: the bases have to separate the levels, and a tail level's texels are
 * interleaved into one block, so the levels cannot be told apart by their
 * extents alone. */
static bool
bases_keep_every_texel_inside_the_chain(void)
{
   uint8_t *const seen = calloc((size_t)C7_CHAIN_BYTES / 4u, 1);
   if (!seen)
      return false;
   bool ok = true;
   for (uint32_t level = 0; level < C7_LEVELS && ok; level++) {
      const uint32_t side = C7_EXTENT >> level;
      for (uint32_t y = 0; y < side && ok; y++) {
         for (uint32_t x = 0; x < side && ok; x++) {
            const uint64_t at = kLevelBases[level] + tiled_level_offset(x, y, side);
            if (at + 4u > C7_CHAIN_BYTES || seen[at / 4u] != 0)
               ok = false;
            else
               seen[at / 4u] = 1;
         }
      }
   }
   free(seen);
   return ok;
}

static const uint16_t kBandIndices[C7_LEVELS * 6] = {
   0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10,
   10, 11, 8,  12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16,
};

int
main(void)
{
   test_begin("C7 tiled mip chain");
   check(bases_match_the_measured_addresses(),
         "the bases are the ones the console's five pinned LODs fetched");
   check(bases_keep_every_texel_inside_the_chain(),
         "every texel of every level has its own address inside the measured chain");
   size_t vertex_bytes = 0;
   size_t pixel_bytes = 0;
#if defined(__linux__)
   const char *const probes = getenv("PS5VK_PROBES");
   uint32_t *const vertex = probes ? read_spirv(probes, "c7-mip", "vertex", &vertex_bytes) : NULL;
   uint32_t *const pixel = probes ? read_spirv(probes, "c7-mip", "pixel", &pixel_bytes) : NULL;
   check(vertex && pixel, "PS5VK_PROBES holds the c7-mip SPIR-V");
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
      struct ps5vk_triangle_input input = {
         GET_PROC, 1,
         {{vertex, vertex_bytes, pixel, pixel_bytes}, {NULL, 0, NULL, 0}},
         VK_ATTACHMENT_LOAD_OP_CLEAR, &report, PS5VK_TRIANGLE_OUTPUT_IMAGE,
         kBandVertices, C7_LEVELS * 4, 16, kBandIndices, C7_LEVELS * 6,
         2, {attributes[0], attributes[1]}, false, NULL, 0, 0,
      };
      /* The library fills a staging buffer from texture_data, which a tiled
       * chain never copies from; the level colours are what a frame's own
       * expectation needs, and the console probe passes the same pointer
       * (src/diagnostics.cpp, run_vulkan_mip_frames). */
      input.texture_data = kLevelGreys;
      input.texture_width = C7_EXTENT;
      input.texture_height = C7_EXTENT;
      input.texture_bilinear = false;
      input.texture_levels = C7_LEVELS;
      input.texture_level_colours = kLevelGreys;
      input.texture_mip_linear = false;
      input.texture_max_lod = (float)(C7_LEVELS - 1);
      /* The attachment storage a tiled chain uses, filled by this test. */
      input.texture_tiled = true;
      struct ps5vk_triangle triangle;

      enum ps5vk_triangle_status status = ps5vk_triangle_create(&triangle, &input);
      if (status == PS5VK_TRIANGLE_OK) {
         VkMemoryRequirements requirements = {0};
         const PFN_vkGetDeviceProcAddr get_device_proc =
            (PFN_vkGetDeviceProcAddr)VK_FUNCTION(triangle.instance, GetDeviceProcAddr);
         const PFN_vkGetImageMemoryRequirements get_requirements =
            get_device_proc != NULL
               ? (PFN_vkGetImageMemoryRequirements)get_device_proc(
                    triangle.device, "vkGetImageMemoryRequirements")
               : NULL;
         if (get_requirements != NULL)
            get_requirements(triangle.device, triangle.texture_image, &requirements);
         check(fill_chain(triangle.instance, triangle.device, triangle.texture_memory,
                          (size_t)requirements.size),
               "the chain's texels land at the bases the address map measured");
      }
      for (uint32_t frame = 0; frame < C7_LEVELS + 1 && status == PS5VK_TRIANGLE_OK; frame++) {
         if (frame > 0 && !ps5vk_triangle_set_texture_lod(&triangle, (float)(frame - 1),
                                                          (float)(frame - 1)))
            status = PS5VK_TRIANGLE_FAILED;
         else
            status = ps5vk_triangle_draw(&triangle, PS5VK_TRIANGLE_ONE_DRAW);
      }
      check(status == PS5VK_TRIANGLE_OK,
            "the six frames record, submit and signal their fences");
      check(steps.failed == NULL, "no step of any frame failed before its fence");
      if (status != PS5VK_TRIANGLE_IN_FLIGHT)
         ps5vk_triangle_finish(&triangle);
   }

   free(vertex);
   free(pixel);
   (void)vertex_bytes;
   (void)pixel_bytes;
   return test_finish();
}
