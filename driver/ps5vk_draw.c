/*
 * PS5 Vulkan driver - rendering and draws.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B7 (docs/M5_PHASE_B.md). A draw records the frame of the
 * test runner's b4-headless test, which the console rendered exactly (runs
 * pid 107 and 108):
 * 1. one indirect context register table: the colour target's 16 CB_COLOR0
 *    registers, 15 viewport, guard-band, scissor and target-mask registers,
 *    the 34 linked context records and both shaders' context registers;
 * 2. the linked uniform table;
 * 3. one SH register table with both shaders' SH registers;
 * 4. DRAW_INDEX_AUTO.
 * Submission ends the stream with the colour-buffer barrier and the
 * completion marker (ps5vk_queue.c).
 *
 * The CB_COLOR0 registers start from AGC's context defaults and are adjusted
 * as append_target_registers adjusts them (src/diagnostics.cpp, after
 * ProsperoLight's render_frame and ps5-opengl's append_target_state), with
 * COMP_SWAP_STD, which stores R, G, B, A bytes. The viewport follows Vulkan's
 * transform as RADV programs it, y scale +height/2, so clip y = -1 lands on
 * the target's first row. ProsperoLight's registers use -height/2, the OpenGL
 * orientation the M3 vertex probe read back; the full-target triangle covers
 * every pixel either way, and runner test b7-corner reads the orientation
 * back.
 *
 * Render passes and framebuffers are Mesa's common implementation over
 * vkCmdBeginRendering. Image layouts carry no state here, so pipeline
 * barriers record nothing; the colour-buffer barrier ends every submission.
 *
 * What the hardware has not rendered through this path is refused: recording
 * fails with VK_ERROR_UNKNOWN at vkEndCommandBuffer, and the reason is logged.
 * Only renderings into one colour attachment are accepted, a single-level
 * 3840x2160 R8G8B8A8_UNORM or B8G8R8A8_UNORM image (swapchain images, Phase
 * C1) over its whole area that is loaded or not
 * cared about; only draws of one instance from vertex and instance 0, with a
 * viewport and scissor covering the target, and a pipeline whose state
 * ps5vk_pipeline.c accepts for drawing.
 *
 * Phase C4 adds the combined image-sampler descriptor: a set-0 binding whose
 * compiler metadata reads a 48-byte entry gets one, word for word as the
 * console's M3 texture canary and M4 render-to-texture test wrote it
 * (src/diagnostics.cpp, write_image_descriptor), built from the image its view
 * names and the sampler's word. Those canaries' images are the only ones a
 * descriptor accepts. A draw that samples a target an earlier draw in the same
 * command buffer rendered into carries the colour-buffer barrier before its
 * words (RELEASE_MEM event 45, the packet the M4 runner recorded) and splits
 * the submission there, with no copy at the split: the barrier flushes the
 * colour buffers, and the split's waited-for step boundary is what makes the
 * sample wait for the flush to drain (HARDWARE_FINDINGS.md; ps5vk_queue.c,
 * ps5vk_cmd_buffer_split).
 */

#include "ps5vk_private.h"

#include "vk_format.h"
#include "vk_framebuffer.h"
#include "vk_render_pass.h"

#include <assert.h>
#include <string.h>

/* CB_COLOR0_INFO fields (AMD's gfx103 register headers, which ps5-opengl's
 * third_party/opengnm carries): FORMAT bits 2-6 (V_028C70_COLOR_*), NUMBER_TYPE
 * bits 8-10 (V_028C70_NUMBER_*), COMP_SWAP bits 11-12 (V_028C70_SWAP_*),
 * SIMPLE_FLOAT bit 15, which CB_COLOR_INFO needs for a colour target's export
 * conversion and blending (Mesa sets it for every colour target). The format's
 * three values come from ps5vk_find_colour_format (driver/ps5vk_image.c). */
#define PS5VK_CB_FORMAT_MASK (UINT32_C(0x1f) << 2)
#define PS5VK_CB_FORMAT_SHIFT 2
#define PS5VK_CB_NUMBER_TYPE_MASK (UINT32_C(0x7) << 8)
#define PS5VK_CB_NUMBER_TYPE_SHIFT 8
#define PS5VK_CB_COMP_SWAP_MASK (UINT32_C(0x3) << 11)
#define PS5VK_CB_COMP_SWAP_SHIFT 11
/* The number-type-dependent bits, as Mesa's ac_build_cb_state sets them:
 * BLEND_CLAMP (bit 15) is set for the normalized and sRGB types and clear for
 * the integer ones, BLEND_BYPASS (bit 16) is set for the integer ones ("set
 * blend bypass according to docs if SINT/UINT"), and ROUND_MODE (bit 18) is set
 * for every type that is not normalized. SIMPLE_FLOAT (bit 17) stays whatever
 * AGC's default word carries, which is the state every frame before
 * V0-formats' colour targets ran with. */
#define PS5VK_CB_BLEND_CLAMP (UINT32_C(1) << 15)
#define PS5VK_CB_BLEND_BYPASS (UINT32_C(1) << 16)
#define PS5VK_CB_ROUND_MODE (UINT32_C(1) << 18)
#define PS5VK_CB_NUMBER_UNORM 0u
#define PS5VK_CB_NUMBER_SNORM 1u
#define PS5VK_CB_NUMBER_UINT 4u
#define PS5VK_CB_NUMBER_SINT 5u
#define PS5VK_CB_NUMBER_SRGB 6u
/* The target size the hardware rendered (M2-M4, B4, B5). */
#define PS5VK_TARGET_WIDTH 3840
#define PS5VK_TARGET_HEIGHT 2160
#define PS5VK_VIEWPORT_REGISTER_COUNT 15
/* CB_BLEND0_CONTROL (0x1e0) and CB_COLOR_CONTROL (0x202), the two registers a
 * blending draw adds to its colour target's: the word the pipeline's
 * colour-blend state becomes and colour control with RB+ off, which is what the
 * M4 blend canary and ps5-opengl's runtime write (docs/HARDWARE_FINDINGS.md). A
 * draw whose pipeline does not blend records neither, so every earlier draw's
 * words are unchanged. */
#define PS5VK_BLEND_REGISTER_COUNT 2
#define PS5VK_BLEND_CONTROL_REGISTER 0x1e0
/* CB_BLEND_RED/GREEN/BLUE/ALPHA (0x105 to 0x108): the four registers a
 * pipeline whose blend state reads the blend constants records, with the
 * constants' bits. A pipeline that does not leaves them at AGC's defaults. */
#define PS5VK_BLEND_CONSTANT_REGISTER 0x105
/* R1's rasterization registers, in the context table's own numbering (the
 * register database's MMIO address over four, minus 0xA000, which is what every
 * offset here is): PA_CL_CLIP_CNTL at 0x204, PA_SU_SC_MODE_CNTL at 0x205 and
 * polygon offset's block -- PA_SU_POLY_OFFSET_DB_FMT_CNTL at 0x2de through
 * PA_SU_POLY_OFFSET_BACK_OFFSET at 0x2e3, the six words ps5-opengl's runtime
 * writes in that order (src/platform/ps5_agc_native_runtime.c:3393) and the
 * ones a depth-biased pipeline's draw records (PS5_VULKAN_REQUESTS.md, R1). */
#define PS5VK_CLIP_CONTROL_REGISTER 0x204
#define PS5VK_RASTERIZER_REGISTER 0x205
#define PS5VK_CLIP_CONTROL_DISCARD (UINT32_C(1) << 22)
#define PS5VK_POLY_OFFSET_DB_FMT_REGISTER 0x2de
#define PS5VK_POLY_OFFSET_COUNT 6
/* The block's own two enables, in PA_SU_SC_MODE_CNTL (0x205): without them the
 * six words do nothing (ps5vk_pipeline.c, measured in Klog_Logs/r-depth-bias2.log
 * before they were set). */
#define PS5VK_RASTERIZER_POLY_OFFSET_FRONT (UINT32_C(1) << 11)
#define PS5VK_RASTERIZER_POLY_OFFSET_BACK (UINT32_C(1) << 12)
/* PA_SU_POLY_OFFSET_DB_FMT_CNTL for a 32-bit float depth attachment, which is
 * ps5-opengl's own word for GFX10 D32F: -23 in the low byte's signed
 * POLY_OFFSET_NEG_NUM_DB_BITS and POLY_OFFSET_DB_IS_FLOAT_FMT set
 * (src/gallium/ps5/ps5_screen.c:2151, "GFX10 D32F: -23 depth bits plus
 * floating-point format"). A UNORM depth attachment's word is not measured, so
 * a bias through one is refused by name rather than guessed at. */
#define PS5VK_POLY_OFFSET_DB_FMT_D32F 0x000001e9u
/* R8: the three words a line list's draw records, and no other draw does, in the
 * same numbering, sourced from the register database and RADV rather than
 * guessed:
 *   VGT_GS_OUT_PRIM_TYPE (0x29b) = LINESTRIP 1: what the NGG stage's primitives
 *     are rasterized as. The link writes this register among its context
 *     records (a triangle list's holds TRISTRIP 2, golden/c1-triangle); RADV
 *     derives it from the topology (radv_conv_prim_to_gs_out: a line list is
 *     LINESTRIP), and the draw writes it after the linked records, so the
 *     value holds whichever the link chose.
 *   PA_SU_LINE_CNTL (0x282) = 8: WIDTH is half the line's width in 12.4 fixed
 *     point, so 1.0 is 8 -- RADV's width * 8 and ps5-opengl's default
 *     point/line block (ps5_agc_native_runtime.c, runtime_point_line[2]).
 *   PA_SC_LINE_CNTL (0x2f7) = 0: RADV's word for Vulkan's default
 *     (non-rectangular) lines, no perpendicular end caps and no DX10 diamond
 *     test ("unnecessary with Vulkan", radv_cmd_buffer.c); strictLines is
 *     reported false, which is what allows it. */
#define PS5VK_GS_OUT_PRIM_TYPE_REGISTER 0x29b
#define PS5VK_GS_OUT_PRIM_LINESTRIP 1u
#define PS5VK_LINE_WIDTH_REGISTER 0x282
#define PS5VK_LINE_WIDTH_ONE 8u
#define PS5VK_LINE_CONTROL_REGISTER 0x2f7
#define PS5VK_LINE_CONTROL_NON_STRICT 0u
#define PS5VK_LINE_REGISTER_COUNT 3
#define PS5VK_BLEND_CONSTANT_COUNT 4
#define PS5VK_COLOR_CONTROL_REGISTER 0x202
#define PS5VK_COLOR_CONTROL_WORD 0x00cc0011u
/* DRAW_INDEX_AUTO's VGT_DRAW_INITIATOR: DI_SRC_SEL_AUTO_INDEX. */
#define PS5VK_DRAW_AUTO_INDEX 2
/* The SH offsets of the vertex and pixel user data (src/diagnostics.cpp and
 * the m3/m4 probes' recorded streams). */
#define PS5VK_VERTEX_USER_DATA_OFFSET 0x8c
#define PS5VK_PIXEL_USER_DATA_OFFSET 0x0c
/* One vertex-buffer table record, ps5-opengl's stride form: address low,
 * address high with the stride, the vertices a draw fetches, flags
 * (src/diagnostics.cpp, kVertexBufferFlags). */
#define PS5VK_VERTEX_RECORD_WORDS 4
#define PS5VK_VERTEX_BUFFER_FLAGS 0x5204u
/* A uniform-buffer descriptor in the recorded hardware-run form: address low,
 * address high with the element stride, element count, flags
 * (src/diagnostics.cpp, kUniformBufferFlags). */
#define PS5VK_UNIFORM_BUFFER_FLAGS (0xfacu | (77u << 12))
/* A texel buffer's word 3 is the view's own: the format entry's DST_SEL
 * selectors (the same field, and the same three-bit channel numbers, the image
 * descriptor's word 1 takes), the view's GFX10 format word in the FORMAT field
 * (S_008F0C_FORMAT_GFX10, the register database's combined numbering, which
 * this driver's fetch words already use) and RESOURCE_LEVEL, which is the bit
 * RADV's own texel-buffer descriptor sets on GFX10
 * (radv_make_texel_buffer_descriptor). ADD_TID_ENABLE is deliberately **not**
 * set: it makes the hardware add the thread's id to the element index -- it is
 * what a scratch buffer's descriptor uses -- and a fetch with it walks off the
 * end of a small view, which is what the first console run of the texel-buffer
 * probe measured (docs/M5_PHASE_C.md, blocker round 5). */
#define PS5VK_TEXEL_BUFFER_RESOURCE_LEVEL (UINT32_C(1) << 24)
#define PS5VK_TEXEL_BUFFER_FORMAT(word) (((word) & 0x7fu) << 12)
/* The 48-byte combined image-sampler descriptor, word for word as
 * src/diagnostics.cpp's write_image_descriptor writes it: the M3 texture
 * canary and the M4 render-to-texture test ran exactly these words. Word 0
 * holds the image address's high bits, word 1 the RGBA8 format, the low two
 * bits of its width and the address's top byte, word 2 the extent and the
 * resource level, word 3 the image kind (untiled texture storage, or a tiled
 * colour target), words 5, 8 and 9 the single level, clamp-to-edge addressing
 * and the LOD range, and word 10 the sampler's own word (ps5vk_image.c). The
 * words the canary leaves zero stay zero, word 4 included: it is the custom
 * row pitch of an untiled resource, which only an image whose width is not a
 * whole number of 256-byte rows needs (ps5vk_sampled_image). */
#define PS5VK_TEXTURE_RESOURCE_LEVEL UINT32_C(0x80000000)
/* Word 3's image kind: untiled texture storage, or a tiled colour target. Its
 * low twelve bits are the DST_SEL channel selectors, which come from the
 * format the view samples rather than from here (ps5vk_private.h,
 * PS5VK_FORMAT_SWIZZLE_RGBA). */
#define PS5VK_TEXTURE_2D_KIND UINT32_C(0x90000000)
#define PS5VK_TEXTURE_2D_TILED_KIND UINT32_C(0x91b00000)
/* SQ_IMG_RSRC_WORD3's TYPE (bits 28-31) for the layered views D1 adds: the
 * register database's SQ_RSRC_IMG_TYPE has 2D 9, CUBE 11 and 2D_ARRAY 13, and
 * SW_MODE 20-24 carries the same 64 KiB swizzle the tiled 2D kind does
 * (docs/HARDWARE_FINDINGS.md). */
#define PS5VK_TEXTURE_SWIZZLE UINT32_C(0x01b00000)
#define PS5VK_TEXTURE_2D_ARRAY_KIND (UINT32_C(0xd) << 28)
#define PS5VK_TEXTURE_CUBE_KIND (UINT32_C(0xb) << 28)
#define PS5VK_TEXTURE_SINGLE_LEVEL UINT32_C(0x00400000)
#define PS5VK_TEXTURE_CLAMP_TO_EDGE (2u | (2u << 3) | (2u << 6))
#define PS5VK_TEXTURE_LOD_RANGE UINT32_C(0x00fff000)
/* M4's render-to-texture barrier, as the runner recorded it and the queue
 * writes it: RELEASE_MEM event 45 with control 12, which flushes and
 * invalidates the colour buffers so a draw that samples one this command
 * buffer rendered into sees what the render wrote (HARDWARE_FINDINGS.md;
 * ps5vk_queue.c has the queue's copy of the same packet). */
#define PS5VK_COLOUR_BARRIER_EVENT 45
#define PS5VK_COLOUR_BARRIER_CONTROL 12
/* The most words one draw records: three 5-word register-table loads, both
 * stages' user data at the 16-dword maximum and a 14-word indexed draw (the
 * index size, base, count and DRAW_INDEX_2 packets), which is more than
 * DRAW_INDEX_AUTO's 3. */
#define PS5VK_DRAW_MAX_WORDS (8 + 5 + 5 + 5 + 2 * (2 + PS5VK_MAX_USER_DATA) + 14)
/* AGC's context defaults: a pointer to block pointers at offset 0, the first
 * block's record count at 0x20 (the layout the test runner reads). */
#define PS5VK_DEFAULTS_COUNT_OFFSET 0x20

/* gfx103 context register offsets: (address - 0x28000) / 4, one row per register
 * and one column per colour attachment, in attachment order. Derived from
 * amdgfxregs.h's gfx103 rows -- the header carries gfx9, gfx10, gfx103, gfx11 and
 * gfx115 under the same names, and its generation comments are a small language
 * ("<= gfx9, >= gfx10" covers everything, which two earlier readings of it got
 * wrong) -- and validated: row 0 reproduces, dword for dword, the sixteen
 * entries this table carried when it held one target (0x318 .. 0x3b8).
 *
 * Two per-target strides, and they are not the same one:
 *   - the ten registers from CB_COLORi_BASE to CB_COLORi_DCC_BASE stride by **15**
 *     dwords (0x318, 0x327, 0x336, 0x345);
 *   - the six from CB_COLORi_BASE_EXT to CB_COLORi_ATTRIB3 stride by **1** dword,
 *     with the fields eight dwords apart (0x390 .. 0x393, 0x398 .. 0x39b, ...).
 * A table built as "target 0 plus one stride" would be right for one family and
 * silently wrong for the other: writing target 1's INFO 15 dwords off lands in
 * another register and produces a plausible picture with corrupted state, not an
 * error (docs/REQUESTS_RESPONSE.md, step 1). */
static const uint16_t ps5vk_target_offsets[PS5VK_TARGET_REGISTER_COUNT][PS5VK_MAX_COLOR_TARGETS] = {
   [0] = {0x318, 0x327, 0x336, 0x345},  /* CB_COLORi_BASE */
   [1] = {0x31b, 0x32a, 0x339, 0x348},  /* CB_COLORi_VIEW */
   [2] = {0x31c, 0x32b, 0x33a, 0x349},  /* CB_COLORi_INFO */
   [3] = {0x31d, 0x32c, 0x33b, 0x34a},  /* CB_COLORi_ATTRIB */
   [4] = {0x31e, 0x32d, 0x33c, 0x34b},  /* CB_COLORi_DCC_CONTROL */
   [5] = {0x31f, 0x32e, 0x33d, 0x34c},  /* CB_COLORi_CMASK */
   [6] = {0x321, 0x330, 0x33f, 0x34e},  /* CB_COLORi_FMASK */
   [7] = {0x323, 0x332, 0x341, 0x350},  /* CB_COLORi_CLEAR_WORD0 */
   [8] = {0x324, 0x333, 0x342, 0x351},  /* CB_COLORi_CLEAR_WORD1 */
   [9] = {0x325, 0x334, 0x343, 0x352},  /* CB_COLORi_DCC_BASE */
   [10] = {0x390, 0x391, 0x392, 0x393}, /* CB_COLORi_BASE_EXT */
   [11] = {0x398, 0x399, 0x39a, 0x39b}, /* CB_COLORi_CMASK_BASE_EXT */
   [12] = {0x3a0, 0x3a1, 0x3a2, 0x3a3}, /* CB_COLORi_FMASK_BASE_EXT */
   [13] = {0x3a8, 0x3a9, 0x3aa, 0x3ab}, /* CB_COLORi_DCC_BASE_EXT */
   [14] = {0x3b0, 0x3b1, 0x3b2, 0x3b3}, /* CB_COLORi_ATTRIB2 */
   [15] = {0x3b8, 0x3b9, 0x3ba, 0x3bb}, /* CB_COLORi_ATTRIB3 */
};
/* The AGC context default of a register, when AGC has one. */
static bool
ps5vk_default_register(uint16_t offset, uint32_t *value)
{
   const void *const defaults = sceAgcGetRegisterDefaults();
   if (!defaults)
      return false;
   const struct ps5vk_agc_register *const *blocks;
   uint32_t count;
   memcpy(&blocks, defaults, sizeof(blocks));
   memcpy(&count, (const uint8_t *)defaults + PS5VK_DEFAULTS_COUNT_OFFSET, sizeof(count));
   for (uint32_t index = 0; blocks && blocks[0] && index < count; index++) {
      if (blocks[0][index].offset == offset) {
         *value = blocks[0][index].value;
         return true;
      }
   }
   return false;
}

/* The 16 CB_COLOR0 registers as AGC's own context defaults hold them: what a
 * rendering with no colour attachment keeps, so a draw that writes no colour
 * still programs a target (vk_meta's depth clear is the pass that needs this).
 * False when a default is missing. */
static bool
ps5vk_default_target_registers(struct ps5vk_agc_register *records, uint32_t target)
{
   assert(target < PS5VK_MAX_COLOR_TARGETS);
   /* AGC's default set describes the **first** colour target: it does not carry
    * CB_COLOR1..3's copies of these registers, and it is not required to -- the
    * console's set happened to answer for them, the host model's does not, and
    * either could change. A field's default belongs to the *field*, not to the
    * target: CB_COLORi_INFO's bits that say "no compression" mean the same thing
    * for every i. So every row starts from target 0's defaults and takes only its
    * own offsets from the table, which is what makes a rendering into the second,
    * third or fourth attachment program the same register shapes as the first
    * (R7 step 1b; before this, target 1's uncomputed fields -- CB_COLOR1_BASE_EXT
    * among them -- kept values no target wrote, and the console's clear reached
    * attachment 0 while attachment 1 stayed at zero). */
   struct ps5vk_agc_register first[PS5VK_TARGET_REGISTER_COUNT];
   for (unsigned index = 0; index < PS5VK_TARGET_REGISTER_COUNT; index++) {
      first[index] = (struct ps5vk_agc_register){.offset = ps5vk_target_offsets[index][0]};
      if (!ps5vk_default_register(first[index].offset, &first[index].value))
         return false;
   }
   for (unsigned index = 0; index < PS5VK_TARGET_REGISTER_COUNT; index++) {
      records[index] = (struct ps5vk_agc_register){
         .offset = ps5vk_target_offsets[index][target], .value = first[index].value};
   }
   return true;
}

/* The 16 DB_* registers of a depth target, as ps5-opengl's hardware-run
 * append_depth_target_state writes them and the M4 step 1 canary ran them
 * (HARDWARE_FINDINGS.md): D32F in 64 KiB tiles (DB_Z_INFO 0x80000183), stencil
 * disabled (DB_STENCIL_INFO 0x20000180), the Z read and write bases low
 * (address >> 8) and high (address >> 40), depth view 0 (DB_DEPTH_VIEW) and the
 * target size (DB_DEPTH_SIZE_XY). The clear registers stay zero: a depth clear
 * is a draw, not a fast clear here.
 *
 * A stencil-bearing format turns the stencil half on (round 12): DB_STENCIL_INFO
 * FORMAT is STENCIL_8 (1) and its bases are the stencil plane's own, one byte a
 * texel in the same 64 KiB Z_X swizzle (ps5vk_image_stencil_plane). Its SW_MODE
 * is the depth surface's own 24 and TILE_STENCIL_DISABLE (bit 29) stays set, the
 * word ps5-opengl's runtime programs for the separate stencil buffer its
 * validated depth/stencil path binds; RADV's own combined-plane path leaves that
 * bit clear and is the candidate to fall back on if the console refuses this
 * one. D24_UNORM_S8_UINT's depth word is Z_24 (2) where D32F's is Z_32_FLOAT
 * (3); D32_SFLOAT_S8_UINT keeps D32F's measured word unchanged. */
static void
ps5vk_depth_registers(uint64_t address, uint64_t stencil_address, VkExtent2D extent,
                      VkSampleCountFlagBits samples, VkFormat format,
                      struct ps5vk_agc_register *records)
{
   const uint32_t low = (uint32_t)(address >> 8);
   const uint32_t high = (uint32_t)(address >> 40);
   const bool stencil = vk_format_has_stencil(format) && stencil_address != 0;
   /* Zero for every depth-only format, which is what the console's own recorded
    * depth frames carry (golden/c5-depth): a format without a stencil plane must
    * record exactly the words it recorded before the stencil half existed. */
   const uint32_t stencil_low = stencil ? (uint32_t)(stencil_address >> 8) : 0;
   const uint32_t stencil_high = stencil ? (uint32_t)(stencil_address >> 40) : 0;
   /* DB_Z_INFO: the word the console measured for D32_SFLOAT (0x80000183) with
    * its FORMAT field (bits 0-3) changed for another depth format -- the
    * register database's ZFormat has Z_16 = 1, Z_24 = 2 and Z_32_FLOAT = 3, so
    * a D16 target's word is 0x80000181 and a D24S8 target's 0x80000182 -- and
    * NUM_SAMPLES (bits 2-3, the sample count's log2) set for a four-sample
    * target (docs/HARDWARE_FINDINGS.md, C8's depth half; round 14's groundwork).
    */
   const uint32_t z_format = format == VK_FORMAT_D16_UNORM   ? 1u
                             : format == VK_FORMAT_D24_UNORM_S8_UINT ? 2u
                                                                     : 3u;
   const uint32_t z_info = (0x80000180u | z_format) |
                           (samples == VK_SAMPLE_COUNT_4_BIT ? (2u << 2) : 0u);
   const struct ps5vk_agc_register depth[PS5VK_DEPTH_REGISTER_COUNT] = {
      {0x010, 0, z_info}, /* DB_Z_INFO */
      {0x011, 0, stencil ? 0x20000181u : 0x20000180u}, /* DB_STENCIL_INFO */
      {0x012, 0, low},                                 /* DB_Z_READ_BASE */
      {0x013, 0, stencil_low},                         /* DB_STENCIL_READ_BASE */
      {0x014, 0, low},                                 /* DB_Z_WRITE_BASE */
      {0x015, 0, stencil_low},                         /* DB_STENCIL_WRITE_BASE */
      {0x01a, 0, high},                                /* DB_Z_READ_BASE_HI */
      {0x01b, 0, stencil_high},                        /* DB_STENCIL_READ_BASE_HI */
      {0x01c, 0, high},                                /* DB_Z_WRITE_BASE_HI */
      {0x01d, 0, stencil_high},                        /* DB_STENCIL_WRITE_BASE_HI */
      {0x01e, 0, 0},           /* DB_HTILE_DATA_BASE_HI */
      {0x002, 0, 0},           /* DB_DEPTH_VIEW */
      {0x005, 0, 0},           /* DB_HTILE_DATA_BASE */
      {0x007, 0, (extent.width - 1u) | ((extent.height - 1u) << 16)}, /* DB_DEPTH_SIZE_XY */
      {0x00b, 0, 0},           /* DB_DEPTH_CLEAR */
      {0x00a, 0, 0},           /* DB_STENCIL_CLEAR */
   };
   memcpy(records, depth, sizeof(depth));
}

/* The draw's DB_DEPTH_CONTROL word from the pipeline's depth and stencil state:
 * Z_ENABLE (bit 1), Z_WRITE_ENABLE (bit 2) and ZFUNC (bits 4-6), which is what
 * the M4 step 1 canary recorded at both ends (0x16 LESS, 0x76 ALWAYS;
 * HARDWARE_FINDINGS.md), plus -- round 12 -- STENCIL_ENABLE (bit 0),
 * BACKFACE_ENABLE (bit 7, which makes the back face read the register's own
 * back-face stencil state) and the two faces' STENCILFUNC (bits 8-10 and
 * 20-22). Vulkan's compare op order is the register's, the same way its depth
 * compare's is, so both functions are the enum's own value. RADV's emission is
 * the reference (radv_emit_depth_stencil_state). */
static uint32_t
ps5vk_depth_control(const struct vk_dynamic_graphics_state *dynamic, bool stencil_bound)
{
   const bool stencil = stencil_bound && dynamic->ds.stencil.test_enable;
   /* Not one stencil bit when the attachment has no stencil plane: Vulkan
    * ignores the stencil test there, and a word that kept the reference's
    * compare function would program a test nothing carries the plane for. */
   const uint32_t stencil_bits =
      stencil ? (UINT32_C(1) << 0) | (UINT32_C(1) << 7) |
                   ((uint32_t)dynamic->ds.stencil.front.op.compare << 8) |
                   ((uint32_t)dynamic->ds.stencil.back.op.compare << 20)
              : 0;
   return stencil_bits | (dynamic->ds.depth.test_enable ? UINT32_C(1) << 1 : 0) |
          (dynamic->ds.depth.write_enable ? UINT32_C(1) << 2 : 0) |
          ((uint32_t)dynamic->ds.depth.compare_op << 4);
}

/* A VkStencilOp as DB_STENCIL_CONTROL's four-bit operation field. Vulkan's
 * order is not the register's: REPLACE is the register's STENCIL_REPLACE_TEST
 * (3), the operations that clamp are 5 and 6 and the ones that wrap are 8 and
 * 9, which is RADV's own table (radv_translate_stencil_op). */
static uint32_t
ps5vk_stencil_op(VkStencilOp op)
{
   switch (op) {
   case VK_STENCIL_OP_KEEP: return 0;                 /* STENCIL_KEEP */
   case VK_STENCIL_OP_ZERO: return 1;                 /* STENCIL_ZERO */
   case VK_STENCIL_OP_REPLACE: return 3;              /* STENCIL_REPLACE_TEST */
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return 5;  /* STENCIL_ADD_CLAMP */
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return 6;  /* STENCIL_SUB_CLAMP */
   case VK_STENCIL_OP_INVERT: return 7;               /* STENCIL_INVERT */
   case VK_STENCIL_OP_INCREMENT_AND_WRAP: return 8;   /* STENCIL_ADD_WRAP */
   case VK_STENCIL_OP_DECREMENT_AND_WRAP: return 9;   /* STENCIL_SUB_WRAP */
   default: return 0;
   }
}

/* One face's DB_STENCILREFMASK word: the reference (bits 0-7), the compare mask
 * (8-15), the write mask (16-23) and the operation value (24-31). RADV writes
 * the op value 1, which only the STENCIL_REPLACE_OP operation reads and this
 * driver never programs, so every face's word is that constant. */
static uint32_t
ps5vk_stencil_refmask(const struct vk_stencil_test_face_state *face)
{
   return (uint32_t)face->reference | ((uint32_t)face->compare_mask << 8) |
          ((uint32_t)face->write_mask << 16) | (UINT32_C(1) << 24);
}

/* The three stencil registers a draw whose pipeline tests stencil adds beside
 * the depth target's: the six face operations in DB_STENCIL_CONTROL and the two
 * faces' reference words. A pipeline that does not test stencil programs none of
 * them, exactly as RADV's emission does not. */
static void
ps5vk_stencil_registers(const struct vk_dynamic_graphics_state *dynamic,
                        struct ps5vk_agc_register *records)
{
   const struct vk_stencil_test_face_state *const front = &dynamic->ds.stencil.front;
   const struct vk_stencil_test_face_state *const back = &dynamic->ds.stencil.back;
   const uint32_t control = ps5vk_stencil_op(front->op.fail) |
                            (ps5vk_stencil_op(front->op.pass) << 4) |
                            (ps5vk_stencil_op(front->op.depth_fail) << 8) |
                            (ps5vk_stencil_op(back->op.fail) << 12) |
                            (ps5vk_stencil_op(back->op.pass) << 16) |
                            (ps5vk_stencil_op(back->op.depth_fail) << 20);
   records[0] = (struct ps5vk_agc_register){.offset = PS5VK_STENCIL_CONTROL_REGISTER,
                                           .value = control};
   records[1] = (struct ps5vk_agc_register){.offset = PS5VK_STENCIL_REFMASK_REGISTER,
                                           .value = ps5vk_stencil_refmask(front)};
   records[2] = (struct ps5vk_agc_register){.offset = PS5VK_STENCIL_REFMASK_BF_REGISTER,
                                           .value = ps5vk_stencil_refmask(back)};
}

/* The CB_COLOR0 registers of a colour target at address, from AGC's defaults
 * adjusted as the test runner adjusts them; false when a default is missing. */
static bool
ps5vk_target_registers(uint64_t address, VkExtent2D extent,
                       const struct ps5vk_colour_format *colour, VkSampleCountFlagBits samples,
                       struct ps5vk_agc_register *records, uint32_t target)
{
   if (!ps5vk_default_target_registers(records, target))
      return false;
   records[0].value = (uint32_t)(address >> 8);
   records[1].value &= 0xfc001fffu;
   /* CB_COLOR0_INFO: the format's data format, number type and component order,
    * with the three bits the number type decides. AGC's default word carries
    * the rest (the endianness, the fast-clear, compression and SIMPLE_FLOAT
    * bits) and those bits are cleared rather than inherited where a probe
    * showed the default wrong. */
   const bool normalised = colour->cb_number_type == PS5VK_CB_NUMBER_UNORM ||
                           colour->cb_number_type == PS5VK_CB_NUMBER_SNORM ||
                           colour->cb_number_type == PS5VK_CB_NUMBER_SRGB;
   const bool integer = colour->cb_number_type == PS5VK_CB_NUMBER_UINT ||
                        colour->cb_number_type == PS5VK_CB_NUMBER_SINT;
   records[2].value =
      (records[2].value &
       ~(PS5VK_CB_FORMAT_MASK | PS5VK_CB_NUMBER_TYPE_MASK | PS5VK_CB_COMP_SWAP_MASK |
         PS5VK_CB_BLEND_CLAMP | PS5VK_CB_BLEND_BYPASS | PS5VK_CB_ROUND_MODE | 0x10000000u |
         0x10000u | 0x40000u | 0x4000u)) |
      (colour->cb_format << PS5VK_CB_FORMAT_SHIFT) |
      (colour->cb_number_type << PS5VK_CB_NUMBER_TYPE_SHIFT) |
      (colour->cb_comp_swap << PS5VK_CB_COMP_SWAP_SHIFT) |
      (normalised ? PS5VK_CB_BLEND_CLAMP : 0u) | (integer ? PS5VK_CB_BLEND_BYPASS : 0u) |
      (normalised ? 0u : PS5VK_CB_ROUND_MODE);
   /* CB_COLOR0_ATTRIB's sample count and fragments per pixel (bits 12-14 and
    * 15-16, the register database's NUM_SAMPLES and NUM_FRAGMENTS): a
    * four-sample target takes the 4x encoding, and a one-sample target the
    * AGC default the M2-M4 frames ran. */
   const uint32_t sample_bits = samples == VK_SAMPLE_COUNT_4_BIT ? (2u | (2u << 3)) : 0u;
   /* The fields are cleared before they are set: a second clear here emptied
    * them again, so every four-sample target was programmed as a one-sample one
    * and the console wrote one word a texel whatever the frame did
    * (docs/M5_PHASE_C.md, C8). */
   records[3].value = (records[3].value & ~(0x7000u | 0x38000u)) | (sample_bits << 12);
   records[4].value = (records[4].value & ~(0x60u | 0x0cu | 0x00100200u | 0x80000u)) | 0x48u;
   records[5].value = 0;
   records[6].value = 0;
   records[9].value = 0;
   /* BASE_EXT's BASE_256B is the address's bits 40-47, eight bits: the half
    * CB_COLORi_BASE's 32 bits cannot hold (the depth target programs the same
    * split as DB_Z_READ_BASE_HIGH). It is per target because it is per address,
    * and this write was already per target -- an earlier round mistook it for the
    * cause of attachment 1 staying zero and added a duplicate; the cause was the
    * write masks below (R7 step 1b, corrected). */
   records[10].value = (records[10].value & 0xffffff00u) | ((uint32_t)(address >> 40) & 0xffu);
   records[11].value &= 0xffffff00u;
   records[12].value &= 0xffffff00u;
   records[13].value &= 0xffffff00u;
   records[14].value = (extent.height - 1u) | ((extent.width - 1u) << 14);
   records[15].value =
      (records[15].value & ~(0x1fffu | 0x7c000u | 0x03000000u | 0x44000000u)) | 0x6c000u |
      0x01000000u | 0x44000000u;
   return true;
}

/* gfx103 context register offsets: (address - 0x28000) / 4, the same scheme as
 * ps5vk_target_offsets. */
#define PS5VK_REG_DB_EQAA 0x201            /* 0x28804 */
#define PS5VK_REG_PA_SC_MODE_CNTL_0 0x292  /* 0x28a48 */
#define PS5VK_REG_PA_SC_CENTROID_PRIORITY_0 0x2f5 /* 0x28bd4 */
#define PS5VK_REG_PA_SC_CENTROID_PRIORITY_1 0x2f6
#define PS5VK_REG_PA_SC_AA_CONFIG 0x2f8     /* 0x28be0 */
#define PS5VK_REG_PA_SC_AA_SAMPLE_LOCS_X0Y0 0x2fe /* 0x28bf8 */
#define PS5VK_REG_PA_SC_AA_SAMPLE_LOCS_X1Y0 0x300 /* 0x28c08 */
#define PS5VK_REG_PA_SC_AA_SAMPLE_LOCS_X0Y1 0x302 /* 0x28c18 */
#define PS5VK_REG_PA_SC_AA_SAMPLE_LOCS_X1Y1 0x304 /* 0x28c28 */
#define PS5VK_REG_PA_SC_AA_MASK_X0Y0_X1Y0 0x30e /* 0x28c38 */
#define PS5VK_REG_PA_SC_AA_MASK_X0Y1_X1Y1 0x30f /* 0x28c3c */

/* The four standard four-sample locations, packed as the sample-location
 * registers hold them: the nibble pairs of (-2, -6), (6, -2), (-6, 2), (2, 6)
 * sixteenths of a pixel, which is Vulkan's standard 4x pattern (0.375, 0.125),
 * (0.875, 0.375), (0.125, 0.625), (0.625, 0.875) about the pixel centre. */
#define PS5VK_SAMPLE_LOCATIONS_4X UINT32_C(0x622ae6ae)

/* The rasterizer registers a four-sample draw adds: the sample count is what
 * decides whether the rasterizer covers one sample of a pixel or four, and the
 * CB_COLOR0_ATTRIB encoding alone leaves it at one -- which is what the console
 * measured before this (one word a texel in the four-sample target's storage,
 * docs/M5_PHASE_C.md, C8). The values are the ones radv programs for gfx10 with
 * the standard sample locations (radv_emit_default_sample_locations,
 * radv_emit_aa_state): MSAA_NUM_SAMPLES and MSAA_EXPOSED_SAMPLES are the sample
 * count's log2, MAX_SAMPLE_DIST is the pattern's own distance, the coverage
 * masks are full, PA_SC_MODE_CNTL_0 enables the rasterizer's MSAA, and DB_EQAA
 * carries the sample counts with no depth buffer (z_samples = 4) and one
 * fragment a pixel (PS_ITER_SAMPLES = log2(1) = 0). They are written out
 * rather than derived from AGC's defaults because the PC's replay only carries
 * the colour target's defaults, and both sides have to record the same table
 * (tools/golden.py, register_defaults). */
static void
ps5vk_multisample_registers(VkSampleCountFlagBits samples, uint32_t *count,
                            struct ps5vk_agc_register *records)
{
   *count = 0;
   if (samples != VK_SAMPLE_COUNT_4_BIT)
      return;
   const struct ps5vk_agc_register values[PS5VK_MULTISAMPLE_REGISTER_COUNT] = {
      /* PA_SC_AA_CONFIG: MSAA_NUM_SAMPLES (bits 0-2) and MSAA_EXPOSED_SAMPLES
       * (bits 20-22) are log2(4) = 2, MAX_SAMPLE_DIST (bits 13-16) is 6. */
      {PS5VK_REG_PA_SC_AA_CONFIG, 0, 2u | (6u << 13) | (2u << 20)},
      /* PA_SC_MODE_CNTL_0: MSAA_ENABLE (bit 0), the per-tile RB alternation and
       * the viewport scissor (bit 1), which every draw of this driver runs
       * with. */
      {PS5VK_REG_PA_SC_MODE_CNTL_0, 0, 0x23u},
      /* DB_EQAA: MAX_ANCHOR_SAMPLES (bits 0-2), MASK_EXPORT_NUM_SAMPLES (8-10)
       * and ALPHA_TO_MASK_NUM_SAMPLES (12-14) are log2(4), PS_ITER_SAMPLES
       * (4-6) is 0, and the quality bits are set. */
      {PS5VK_REG_DB_EQAA, 0,
       2u | (2u << 8) | (2u << 12) | (1u << 16) | (1u << 17) | (1u << 20)},
      {PS5VK_REG_PA_SC_AA_SAMPLE_LOCS_X0Y0, 0, PS5VK_SAMPLE_LOCATIONS_4X},
      {PS5VK_REG_PA_SC_AA_SAMPLE_LOCS_X1Y0, 0, PS5VK_SAMPLE_LOCATIONS_4X},
      {PS5VK_REG_PA_SC_AA_SAMPLE_LOCS_X0Y1, 0, PS5VK_SAMPLE_LOCATIONS_4X},
      {PS5VK_REG_PA_SC_AA_SAMPLE_LOCS_X1Y1, 0, PS5VK_SAMPLE_LOCATIONS_4X},
      /* The centroid priorities of the four-sample pattern: sample 0's
       * distance first, as radv's centroid_priority_4x is. */
      {PS5VK_REG_PA_SC_CENTROID_PRIORITY_0, 0, 0x32103210u},
      {PS5VK_REG_PA_SC_CENTROID_PRIORITY_1, 0, 0x32103210u},
      {PS5VK_REG_PA_SC_AA_MASK_X0Y0_X1Y0, 0, 0xffffffffu},
      {PS5VK_REG_PA_SC_AA_MASK_X0Y1_X1Y1, 0, 0xffffffffu},
   };
   memcpy(records, values, sizeof(values));
   *count = PS5VK_MULTISAMPLE_REGISTER_COUNT;
}

static uint32_t
ps5vk_float_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

/* Viewport, guard-band, scissor and target-mask registers: Vulkan's viewport
 * transform, ProsperoLight's guard band of 1, the scissor with the window
 * offset disabled, and every channel of target 0. */
static void
ps5vk_viewport_registers(const VkViewport *viewport, const VkRect2D *scissor,
                         struct ps5vk_agc_register *records)
{
   const uint32_t left = (uint32_t)scissor->offset.x;
   const uint32_t top = (uint32_t)scissor->offset.y;
   const struct ps5vk_agc_register values[PS5VK_VIEWPORT_REGISTER_COUNT] = {
      {0x10f, 0, ps5vk_float_bits(viewport->width * 0.5f)},                    /* PA_CL_VPORT_XSCALE */
      {0x110, 0, ps5vk_float_bits(viewport->x + viewport->width * 0.5f)},      /* PA_CL_VPORT_XOFFSET */
      {0x111, 0, ps5vk_float_bits(viewport->height * 0.5f)},                   /* PA_CL_VPORT_YSCALE */
      {0x112, 0, ps5vk_float_bits(viewport->y + viewport->height * 0.5f)},     /* PA_CL_VPORT_YOFFSET */
      {0x113, 0, ps5vk_float_bits(viewport->maxDepth - viewport->minDepth)},   /* PA_CL_VPORT_ZSCALE */
      {0x114, 0, ps5vk_float_bits(viewport->minDepth)},                        /* PA_CL_VPORT_ZOFFSET */
      {0x0b4, 0, ps5vk_float_bits(MIN2(viewport->minDepth, viewport->maxDepth))}, /* PA_SC_VPORT_ZMIN_0 */
      {0x0b5, 0, ps5vk_float_bits(MAX2(viewport->minDepth, viewport->maxDepth))}, /* PA_SC_VPORT_ZMAX_0 */
      {0x2fa, 0, ps5vk_float_bits(1.0f)},                                      /* PA_CL_GB_VERT_CLIP_ADJ */
      {0x2fb, 0, ps5vk_float_bits(1.0f)},                                      /* PA_CL_GB_VERT_DISC_ADJ */
      {0x2fc, 0, ps5vk_float_bits(1.0f)},                                      /* PA_CL_GB_HORZ_CLIP_ADJ */
      {0x2fd, 0, ps5vk_float_bits(1.0f)},                                      /* PA_CL_GB_HORZ_DISC_ADJ */
      {0x090, 0, 0x80000000u | left | (top << 16)},                            /* PA_SC_GENERIC_SCISSOR_TL */
      {0x091, 0, (left + scissor->extent.width) | ((top + scissor->extent.height) << 16)}, /* PA_SC_GENERIC_SCISSOR_BR */
      {0x08e, 0, 0xfu},                                                        /* CB_TARGET_MASK */
   };
   memcpy(records, values, sizeof(values));
}

/* Keeps the state a meta operation replaces: vk_meta binds its own pipeline,
 * vertex buffers and descriptor sets and pushes its own constants and sets its
 * own viewport and scissor, and the application's have to survive the
 * operation (vk_meta_save in Mesa's vk_meta.c). */
static void
ps5vk_meta_save(struct ps5vk_cmd_buffer *cmd_buffer, struct ps5vk_meta_saved_state *saved)
{
   saved->pipeline = cmd_buffer->pipeline;
   saved->dynamic = cmd_buffer->vk.dynamic_graphics_state;
   memcpy(saved->vertex_buffers, cmd_buffer->vertex_buffers, sizeof(saved->vertex_buffers));
   memcpy(saved->descriptor_sets, cmd_buffer->descriptor_sets, sizeof(saved->descriptor_sets));
   memcpy(saved->descriptor_set_offsets, cmd_buffer->descriptor_set_offsets,
          sizeof(saved->descriptor_set_offsets));
   memcpy(saved->push_constants, cmd_buffer->push_constants, sizeof(saved->push_constants));
}

static void
ps5vk_meta_restore(struct ps5vk_cmd_buffer *cmd_buffer, const struct ps5vk_meta_saved_state *saved)
{
   cmd_buffer->pipeline = saved->pipeline;
   cmd_buffer->vk.dynamic_graphics_state = saved->dynamic;
   memcpy(cmd_buffer->vertex_buffers, saved->vertex_buffers, sizeof(cmd_buffer->vertex_buffers));
   memcpy(cmd_buffer->descriptor_sets, saved->descriptor_sets, sizeof(cmd_buffer->descriptor_sets));
   memcpy(cmd_buffer->descriptor_set_offsets, saved->descriptor_set_offsets,
          sizeof(cmd_buffer->descriptor_set_offsets));
   memcpy(cmd_buffer->push_constants, saved->push_constants, sizeof(cmd_buffer->push_constants));
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   struct ps5vk_device *const device =
      container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk);
   const VkRenderingInfo *const info = pRenderingInfo;
   cmd_buffer->rendering = false;
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;

   /* Mesa's render passes set LOCAL_READ_CONCURRENT_ACCESS_CONTROL, which
    * describes concurrent input-attachment access and changes nothing
    * without input attachments. */
   const VkRenderingFlags flags =
      info->flags & ~(VkRenderingFlags)VK_RENDERING_LOCAL_READ_CONCURRENT_ACCESS_CONTROL_BIT_KHR;
   if (flags != 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "rendering flags 0x%x are not supported yet", (unsigned)flags);
      return;
   }
   if (info->layerCount != 1 || info->viewMask != 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "rendering %u layers with view mask 0x%x is not supported yet",
                              info->layerCount, info->viewMask);
      return;
   }
   /* The depth attachment, when the rendering has one: one D32_SFLOAT image the
    * size of the target, tiled like a colour attachment (Phase C5, the M4 step 1
    * canary's layout). */
   VkExtent2D extent = {0, 0};
   void *depth_address = NULL;
   /* A stencil-bearing depth attachment's stencil plane, or zero: the plane the
    * depth registers enable DB_STENCIL_INFO for (ps5vk_depth_registers). */
   uint64_t stencil_address = 0;
   const struct ps5vk_image *depth_attachment_image = NULL;
   VkFormat depth_format = VK_FORMAT_UNDEFINED;
   VkSampleCountFlagBits depth_samples = VK_SAMPLE_COUNT_1_BIT;
   if (info->pDepthAttachment && info->pDepthAttachment->imageView != VK_NULL_HANDLE) {
      const VkRenderingAttachmentInfo *const depth_attachment = info->pDepthAttachment;
      if (depth_attachment->resolveMode != VK_RESOLVE_MODE_NONE) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "resolving a depth attachment is not supported yet");
         return;
      }
      VK_FROM_HANDLE(vk_image_view, depth_view, depth_attachment->imageView);
      struct ps5vk_image *const depth_image =
         container_of(depth_view->image, struct ps5vk_image, vk);
      const VkExtent2D depth_extent = {depth_image->vk.extent.width,
                                       depth_image->vk.extent.height};
      /* A four-sample depth attachment is C8's depth half: DB_Z_INFO's
       * NUM_SAMPLES carries the count and the storage gains the sample planes
       * the map below places (docs/HARDWARE_FINDINGS.md). */
      if ((depth_view->format != VK_FORMAT_D32_SFLOAT &&
           depth_view->format != VK_FORMAT_D16_UNORM &&
           depth_view->format != VK_FORMAT_D24_UNORM_S8_UINT &&
           depth_view->format != VK_FORMAT_D32_SFLOAT_S8_UINT) ||
          depth_view->view_type != VK_IMAGE_VIEW_TYPE_2D ||
          depth_image->vk.mip_levels != 1 || depth_image->vk.array_layers != 1 ||
          (depth_image->vk.samples != VK_SAMPLE_COUNT_1_BIT &&
           depth_image->vk.samples != VK_SAMPLE_COUNT_4_BIT) ||
          depth_image->storage != PS5VK_IMAGE_STORAGE_TILES ||
          depth_extent.width != PS5VK_TARGET_WIDTH ||
          depth_extent.height != PS5VK_TARGET_HEIGHT ||
          /* A stencil-bearing attachment must have the plane the image placed
           * for it: a format that has one and a shape that did not derive one is
           * a refusal, not a draw into whatever follows the depth surface. */
          (vk_format_has_stencil(depth_view->format) &&
           (depth_image->stencil_offset == 0 || depth_image->vk.samples != VK_SAMPLE_COUNT_1_BIT))) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "depth attachments other than a single-level 3840x2160 "
                                 "D32_SFLOAT, D16_UNORM, D24_UNORM_S8_UINT or "
                                 "D32_SFLOAT_S8_UINT image, one or four samples, are not "
                                 "supported yet");
         return;
      }
      depth_samples = depth_image->vk.samples;
      assert(depth_image->address != 0);
      depth_address = (void *)(uintptr_t)depth_image->address;
      stencil_address = depth_image->address + depth_image->stencil_offset;
      depth_attachment_image = depth_image;
      depth_format = depth_view->format;
      extent = depth_extent;
   }
   /* The stencil attachment of a combined depth/stencil rendering: the same
    * image the depth attachment names, which is what Vulkan asks of an image
    * whose format carries both aspects. Any other stencil image is a refusal by
    * name -- a separate stencil-only image would need a second plane layout and
    * a second attachment binding, which nothing has measured. */
   if (info->pStencilAttachment != NULL &&
       info->pStencilAttachment->imageView != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(vk_image_view, stencil_view, info->pStencilAttachment->imageView);
      struct ps5vk_image *const stencil_image =
         container_of(stencil_view->image, struct ps5vk_image, vk);
      if (depth_attachment_image == NULL || stencil_image != depth_attachment_image ||
          stencil_view->view_type != VK_IMAGE_VIEW_TYPE_2D ||
          info->pStencilAttachment->resolveMode != VK_RESOLVE_MODE_NONE) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "a stencil attachment other than the depth attachment's own "
                                 "combined image is not supported yet");
         return;
      }
   }
   /* The attachments this device binds are the ones it advertises. The number is
    * read from the physical device that reports it rather than from the constant
    * that sizes the tables, so the refusal cannot drift from what the device
    * says (VkPhysicalDeviceLimits.maxColorAttachments). */
   const uint32_t advertised_targets = device->vk.physical->properties.maxColorAttachments;
   /* The per-attachment rows are programmed (ps5vk_target_offsets, the loop
    * below), but a rendering into more than one colour attachment does not land
    * its writes past the first yet: the console probe v0-mrt reads attachment 0's
    * value and zero in the others, with the clear colour unreached too, so the
    * registers -- not the export -- are where the second attachment stops. Two
    * hypotheses were tested and refuted on the way (the colour-export word, and
    * AGC's per-target defaults for the fields the driver does not compute); the
    * evidence and the next shape are in docs/M5_PHASE_C.md, step 1b. Until the
    * writes land, a rendering that asks for several attachments is refused by
    * name rather than drawn wrong: a picture with one attachment's data in all of
    * them is exactly the failure a probe must not accept silently. */
   if (info->colorAttachmentCount > 1) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a rendering into %u colour attachments: the per-attachment "
                              "registers are programmed but writes past the first do not land "
                              "yet, and this driver refuses them rather than draw a wrong "
                              "picture (the probe is v0-mrt; docs/M5_PHASE_C.md, R7 step 1b)",
                              info->colorAttachmentCount);
      return;
   }
   if (info->colorAttachmentCount > advertised_targets) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a rendering into %u colour attachments, more than the %u this "
                              "device advertises (VkPhysicalDeviceLimits.maxColorAttachments; "
                              "PS5VK_MAX_COLOR_TARGETS, ps5vk_private.h)",
                              info->colorAttachmentCount, advertised_targets);
      return;
   }
   /* A rendering may have no colour attachment at all when it has a depth one:
    * vk_meta's depth clear is exactly that pass (Phase C5). It keeps AGC's
    * default colour registers, which nothing writes through. */
   const bool has_colour = info->colorAttachmentCount > 0 &&
                           info->pColorAttachments[0].imageView != VK_NULL_HANDLE;
   if (!has_colour && depth_address == NULL) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a rendering needs a colour or a depth attachment");
      return;
   }
   /* Every colour attachment the rendering declares, in attachment order: each
    * one's own image, format and address, its own row of target registers, and
    * its own entry in the target list the submission flushes. The extent, the
    * sample count and the colour format are the ones attachment 0's image has --
    * a framebuffer's attachments share their dimensions, and the render pass's
    * sample count is the pipeline's -- and an attachment that disagrees is
    * refused by name rather than programmed into another's registers. */
   const uint32_t colour_count = has_colour ? info->colorAttachmentCount : 0u;
   VkSampleCountFlagBits target_samples = VK_SAMPLE_COUNT_1_BIT;
   if (colour_count == 0) {
      /* Nothing writes colour, but every draw's table still programs a target:
       * AGC's own defaults, which the M2-M4 frames ran with. */
      if (!ps5vk_default_target_registers(cmd_buffer->target_registers[0], 0)) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "AGC's context defaults lack a colour target register");
         return;
      }
      cmd_buffer->colour_attachment_count = 1;
      cmd_buffer->multisample_count = 0;
   }
   for (uint32_t at = 0; at < colour_count; at++) {
      const VkRenderingAttachmentInfo *const attachment = &info->pColorAttachments[at];
      VK_FROM_HANDLE(vk_image_view, view, attachment->imageView);
      struct ps5vk_image *const image =
         view != NULL ? container_of(view->image, struct ps5vk_image, vk) : NULL;
      const struct ps5vk_colour_format *const format =
         view != NULL ? ps5vk_find_colour_format(view->format) : NULL;
      if (view == NULL || image == NULL || format == NULL ||
          view->view_type != VK_IMAGE_VIEW_TYPE_2D || image->vk.mip_levels != 1 ||
          image->vk.array_layers != 1 ||
          (image->vk.samples != VK_SAMPLE_COUNT_1_BIT &&
           image->vk.samples != VK_SAMPLE_COUNT_4_BIT) ||
          image->storage != PS5VK_IMAGE_STORAGE_TILES ||
          image->vk.extent.width != PS5VK_TARGET_WIDTH ||
          image->vk.extent.height != PS5VK_TARGET_HEIGHT) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "colour attachment %u is not a 3840x2160 one-mip one-layer "
                                 "tiled 2D view this driver has a colour format word for",
                                 at);
         return;
      }
      if (at == 0) {
         extent = (VkExtent2D){image->vk.extent.width, image->vk.extent.height};
         target_samples = image->vk.samples;
         ps5vk_multisample_registers(image->vk.samples, &cmd_buffer->multisample_count,
                                     cmd_buffer->multisample_registers);
      } else if (image->vk.samples != target_samples) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "colour attachment %u has %u samples where attachment 0 has "
                                 "%u; a rendering's attachments share one sample count",
                                 at, (unsigned)image->vk.samples, (unsigned)target_samples);
         return;
      }
      /* Valid usage: the attachment's image is bound; swapchain images are bound
       * from their creation. */
      assert(image->address != 0);
      if (!ps5vk_target_registers(image->address, extent, format, image->vk.samples,
                                  cmd_buffer->target_registers[at], at)) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "AGC's context defaults lack a colour target register");
         return;
      }
      struct ps5vk_render_target *const target =
         util_dynarray_grow(&cmd_buffer->targets, struct ps5vk_render_target, 1);
      if (!target) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                                 "no memory to track colour targets");
         return;
      }
      *target = (struct ps5vk_render_target){(void *)(uintptr_t)image->address,
                                             (size_t)image->size, image->video,
                                             image->buffer_index};
   }
   if (colour_count != 0) {
      cmd_buffer->colour_attachment_count = colour_count;
      /* What the debug API reports to the probe's host half (ps5vk_debug.h):
       * each attachment's CB_COLORi_BASE word, which is what says the rows were
       * filled from the attachments rather than from one of them. */
      device->target_attachment_count = colour_count;
      for (uint32_t at = 0; at < colour_count; at++) {
         device->target_base_offsets[at] = cmd_buffer->target_registers[at][0].offset;
         device->target_base_values[at] = cmd_buffer->target_registers[at][0].value;
      }
   }
   cmd_buffer->depth_bound = depth_address != NULL;
   cmd_buffer->depth_format = cmd_buffer->depth_bound ? depth_format : VK_FORMAT_UNDEFINED;
   cmd_buffer->stencil_bound = cmd_buffer->depth_bound &&
                               vk_format_has_stencil(depth_format) && stencil_address != 0;
   if (cmd_buffer->depth_bound)
      ps5vk_depth_registers((uint64_t)(uintptr_t)depth_address, stencil_address, extent,
                            depth_samples, depth_format, cmd_buffer->depth_registers);
   cmd_buffer->target_extent = extent;
   cmd_buffer->rendering = true;

   /* The attachment descriptions a later vkCmdClearAttachments clears, and the
    * depth format vk_meta's clear draws into. */
   cmd_buffer->render = (struct vk_meta_rendering_info){
      .view_mask = info->viewMask,
      .samples = 1,
      .color_attachment_count = info->colorAttachmentCount,
      .depth_attachment_format = depth_format,
   };
   for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *const color = &info->pColorAttachments[i];
      if (color->imageView == VK_NULL_HANDLE)
         continue;
      VK_FROM_HANDLE(vk_image_view, color_view, color->imageView);
      cmd_buffer->render.color_attachment_formats[i] = color_view->format;
      cmd_buffer->render.color_attachment_write_masks[i] =
         VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
         VK_COLOR_COMPONENT_A_BIT;
   }

   /* A clear is a vk_meta draw of this rendering's attachment, recorded like
    * any other draw but with vk_meta's pipeline, vertex buffer and push
    * constants. The application's state around it is its own. */
   const VkRenderingAttachmentInfo *const first_colour =
      has_colour ? &info->pColorAttachments[0] : NULL;
   if ((first_colour != NULL && first_colour->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) ||
       (info->pDepthAttachment != NULL &&
        info->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)) {
      struct ps5vk_meta_saved_state saved;
      ps5vk_meta_save(cmd_buffer, &saved);
      /* WORKAROUND(R3): Mesa's vk_meta_clear unwraps a stencil attachment's
       * clear value with `.depthStencil.depth` where the value it wants is the
       * `.stencil` member (src/vulkan/runtime/vk_meta_clear.c, the
       * clear_att[clear_count].clearValue.depthStencil.stencil assignment), so
       * a render pass that clears a combined depth-stencil attachment with
       * depth 1.0 and stencil 0 leaves the stencil plane holding the depth
       * clear's own byte. Measured on the console: with depth 1.0 no byte of
       * the plane's first tile is 0, and with depth 0.5 every byte is
       * (PS5_VULKAN_REQUESTS.md R3, Klog_Logs/r3-stencil-clear-before.log). The
       * copy below carries the stencil's own value in the member vk_meta reads;
       * the depth attachment's copy is untouched, so the depth clear still
       * takes the depth. Retire this when that line takes `.stencil`. */
      const VkRenderingAttachmentInfo *corrected_stencil = NULL;
      VkRenderingAttachmentInfo stencil;
      VkRenderingInfo corrected;
      if (info->pStencilAttachment != NULL) {
         stencil = *info->pStencilAttachment;
         stencil.clearValue.depthStencil.depth = stencil.clearValue.depthStencil.stencil;
         corrected = *info;
         corrected.pStencilAttachment = &stencil;
         corrected_stencil = &stencil;
      }
      vk_meta_clear_rendering(&device->meta, &cmd_buffer->vk,
                              corrected_stencil != NULL ? &corrected : info);
      ps5vk_meta_restore(cmd_buffer, &saved);
   }
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdEndRendering(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   cmd_buffer->rendering = false;
}

/* Image layouts carry no state, and the colour-buffer barrier ends every
 * submission (ps5vk_queue.c), so barriers record nothing. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)
{
   (void)commandBuffer;
   (void)pDependencyInfo;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                      VkPipeline _pipeline)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_pipeline, pipeline, _pipeline);
   /* A compute pipeline has no graphics state to bind: vkCmdDispatch reads it
    * from the command buffer (Phase D2, ps5vk_compute.c). */
   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
      cmd_buffer->compute_pipeline = pipeline;
      return;
   }
   assert(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS);
   cmd_buffer->pipeline = pipeline;
   /* The viewport and scissor the pipeline declares statically; what it
    * declares dynamic stays as vkCmdSetViewport and vkCmdSetScissor left it. */
   vk_cmd_set_dynamic_graphics_state(&cmd_buffer->vk, &pipeline->dynamic);
}

/* An indexed draw's arguments, or NULL for a non-indexed one. */
struct ps5vk_indexed_draw {
   uint32_t index_count;
   uint32_t first_index;
   int32_t vertex_offset;
};

/* What the 48 bytes of a combined image-sampler descriptor need from the image
 * its view names: the address, the extent and whether the storage is the tiled
 * kind of an attachment, plus the sampler's word. */
struct ps5vk_sampled_image {
   uint64_t address;
   VkExtent2D extent;
   /* The image's IMG_DATA_FORMAT, which the descriptor's FORMAT field carries
    * (V0-formats: one per format, ps5vk_image.c's table). */
   uint32_t format_word;
   uint32_t pitch_texels;
   /* The format's DST_SEL channel selectors, which word 3's low twelve bits
    * carry: they are what fills a format's missing channels with zero and one
    * the way Vulkan's fetch rule says, since the hardware aliases them
    * (ps5vk_private.h, PS5VK_FORMAT_SWIZZLE_R001). */
   uint32_t dst_sel;
   uint32_t sampler_word;
   /* Word 8 of the combined image-sampler descriptor: the sampler's three
    * address modes (R2, ps5vk_image.c). */
   uint32_t address_word;
   /* The levels the view names (Phase C7): the first and the last, which the
    * descriptor carries so the hardware reads a mip chain. A single-level view
    * is 0 and 0, the words every earlier descriptor held. */
   uint32_t base_mip_level;
   uint32_t last_mip_level;
   /* The image's own last level, which word 5's MAX_MIP field carries: it is
    * the length of the chain the hardware walks, and ps5-opengl writes the
    * image's last level there rather than the view's (Phase C7). A single-level
    * image leaves MAX_MIP zero, which is every descriptor before C7. */
   uint32_t image_last_mip_level;
   /* The view's layers (D1): base_layer is word 4's BASE_ARRAY field and
    * layer_count - 1 its DEPTH field, and the kind the view asks for is word
    * 3's TYPE -- a 2D array (13) or a cube (11) rather than the 2D (9) every
    * earlier descriptor wrote. All three are the register database's own field
    * positions (docs/HARDWARE_FINDINGS.md); a single-layer view leaves them at
    * zero and its descriptor is the one before this step, word for word. */
   uint32_t base_layer;
   uint32_t layer_count;
   bool array;
   bool cube;
   /* Word 9: the sampler's LOD range, which only a multi-level view writes;
    * the single-level path keeps the canary's own word. */
   uint32_t lod_word;
   bool tiled;
   /* Whether the image is one this command buffer rendered into, and so needs
    * the colour barrier in the words before the draw that samples it (the
    * recorded packet is ps5vk_queue.c's, M4's render-to-texture barrier). */
   bool barrier;
};

/* The image and sampler a combined image sampler binding samples, or false
 * with the recording refused by name: the 2D views of the formats
 * ps5vk_image.c's table carries a descriptor format word and swizzle for, from
 * the M3 texture canary's R8G8B8A8_UNORM on (docs/M5_REFERENCE.md, C4 and
 * V0-formats). */
static bool
ps5vk_sampled_image(struct ps5vk_cmd_buffer *cmd_buffer, uint32_t set, uint32_t binding,
                    const struct ps5vk_descriptor_buffer *written,
                    struct ps5vk_sampled_image *sampled)
{
   /* A storage image's write names a view and no sampler: its descriptor is the
    * image's words alone, so a missing sampler is only the combined image
    * sampler's refusal (driver/ps5vk_descriptor_set.c records both types). */
   const bool needs_sampler = written->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   if (written->view == VK_NULL_HANDLE || (needs_sampler && written->sampler == VK_NULL_HANDLE)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u: a %s write names no %s", (unsigned)set, binding,
                              needs_sampler ? "combined image sampler" : "storage image",
                              written->view == VK_NULL_HANDLE ? "image view" : "sampler");
      return false;
   }
   VK_FROM_HANDLE(vk_image_view, view, written->view);
   VK_FROM_HANDLE(ps5vk_sampler, sampler, written->sampler);
   if (view == NULL || (needs_sampler && sampler == NULL)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u names a %s this driver did not create", (unsigned)set, binding,
                              view == NULL ? "image view" : "sampler");
      return false;
   }
   /* The view's format must be one the table carries a descriptor format word
    * for, which V0-formats grew from R8G8B8A8_UNORM to the sampled families
    * below it (ps5vk_image.c); the check further down names the refusal. The
    * kind a view may name is the 2D one every earlier canary used, a 2D array
    * (D1's layers) and a cube (six layers of a cube-compatible image), which
    * word 3's TYPE field carries (docs/HARDWARE_FINDINGS.md). */
   if (view->view_type != VK_IMAGE_VIEW_TYPE_2D &&
       view->view_type != VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
       view->view_type != VK_IMAGE_VIEW_TYPE_CUBE) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u samples a view that is not 2D, a 2D array or a "
                              "cube; those are the kinds the console's canaries and D1's arrays "
                              "sampled (docs/M5_REFERENCE.md)", (unsigned)set, binding);
      return false;
   }
   struct ps5vk_image *const image = container_of(view->image, struct ps5vk_image, vk);
   /* Valid usage: the view names an image, of the format it is created with. */
   assert(image != NULL && image->vk.format == view->format);
   if (image->address == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u samples an image with no GPU address: it has to "
                              "be bound to memory", (unsigned)set, binding);
      return false;
   }
   /* Valid usage: the view names levels the image has. The descriptor carries
    * the first and last of them (Phase C7), and the hardware walks the chain
    * from the image's base address, whose per-level layout ps5vk_image.c sizes
    * by ps5-opengl's rules. */
   assert(view->level_count != 0 && view->base_mip_level + view->level_count <= image->vk.mip_levels);
   /* The view's layers (D1): the descriptor names the first and the count, and
    * the image's slices are consecutive chains of the shape the oracle table
    * covers (ps5vk_image_layer_bytes), so no new storage rule is involved. */
   if (view->base_array_layer + view->layer_count > image->vk.array_layers) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u samples %u layers from layer %u of a %u-layer "
                              "image", (unsigned)set, binding, view->layer_count, view->base_array_layer,
                              image->vk.array_layers);
      return false;
   }
   if (view->layer_count > 1 && ps5vk_image_layer_bytes(image) == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u samples %u array layers of an image whose slice "
                              "placement no measurement covers; the oracle's table holds the "
                              "shapes D1 is written for (tools/check-mip-layout.sh)",
                              (unsigned)set, binding, view->layer_count);
      return false;
   }
   if (image->vk.samples != VK_SAMPLE_COUNT_1_BIT) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u samples a %u-sample image; 4x sampling is C8 "
                              "(docs/M5_REFERENCE.md)", (unsigned)set, binding, (unsigned)image->vk.samples);
      return false;
   }
   /* Vulkan applies a view's component mapping to what a shader reads through
    * a combined image sampler, and the descriptor carries only the format's own
    * DST_SEL selectors (ps5vk_image.c): a mapping that is not the identity
    * would return the format's own channels where the application asked for
    * others, so it is refused rather than sampled wrongly. The runtime resolves
    * VK_COMPONENT_SWIZZLE_IDENTITY to R, G, B and A whatever the format is, and
    * Vulkan requires the identity mapping for every other use this driver has of
    * a view -- framebuffer attachments, storage images and input attachments --
    * so this is the only place that has to check it. Composing the mapping into
    * the selectors is a runner probe's step (docs/M5_REFERENCE.md, V0-formats). */
   if (view->swizzle.r != VK_COMPONENT_SWIZZLE_R || view->swizzle.g != VK_COMPONENT_SWIZZLE_G ||
       view->swizzle.b != VK_COMPONENT_SWIZZLE_B || view->swizzle.a != VK_COMPONENT_SWIZZLE_A) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u samples a view whose component mapping is not the "
                              "identity; composing it into the descriptor's selectors needs a "
                              "runner probe (docs/M5_REFERENCE.md, V0-formats)", (unsigned)set, binding);
      return false;
   }
   const bool tiled = image->storage == PS5VK_IMAGE_STORAGE_TILES;
   /* Row storage aligns bytes, not texels. Word 4 can encode a custom
    * pitch for a single-level 2D image (ps5-opengl ps5_screen.c); arrays
    * use that field for layers, and mip chains have their own layout. */
   const uint32_t texel_bytes = vk_format_get_blocksize(image->vk.format);
   const uint32_t pitch_texels = align(image->vk.extent.width * texel_bytes, 256) / texel_bytes;
   const bool padded = !tiled && pitch_texels != image->vk.extent.width;
   if (padded && (view->view_type != VK_IMAGE_VIEW_TYPE_2D || image->vk.array_layers != 1 ||
                  image->vk.mip_levels != 1 || pitch_texels > 0x4000)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u needs a padded texture pitch of %u texels; "
                              "image %ux%ux%u format %u mips %u layers %u view %u name %s; "
                              "only single-level, single-layer 2D images up to 16384 texels "
                              "have a custom-pitch descriptor probe (C4)",
                              (unsigned)set, binding, pitch_texels,
                              image->vk.extent.width, image->vk.extent.height, image->vk.extent.depth,
                              (unsigned)image->vk.format, image->vk.mip_levels, image->vk.array_layers,
                              (unsigned)view->view_type,
                              image->vk.base.object_name ? image->vk.base.object_name : "(unnamed)");
      return false;
   }
   /* Sampling a target this command buffer rendered into needs both the
    * colour barrier between the render and the sample (HARDWARE_FINDINGS.md,
    * event 45) and a wait for that barrier's flush: the packet alone leaves the
    * image's most recently written rows unflushed when the sample's first
    * fetches run (docs/M5_PHASE_C.md, C4), so the draw that samples it carries
    * the packet and splits the submission (ps5vk_cmd_draw,
    * ps5vk_cmd_buffer_split). What the queue writes ends a submission, which
    * orders a render in an earlier submission and nothing else. */
   bool rendered_here = false;
   util_dynarray_foreach (&cmd_buffer->targets, struct ps5vk_render_target, target) {
      if ((uint64_t)(uintptr_t)target->address == image->address)
         rendered_here = true;
   }
   /* The descriptor drops the address's low byte, which images keep zero:
    * ps5vk_image.c aligns them to it. */
   assert((image->address & 0xffu) == 0);
   const struct ps5vk_format *const entry = ps5vk_find_format(image->vk.format);
   if (entry == NULL || entry->image_format == 0 || entry->dst_sel == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u samples a format %u image, whose descriptor "
                              "format word and channel selectors no probe has recorded "
                              "(docs/M5_REFERENCE.md, V0-formats)",
                              (unsigned)set, binding, (unsigned)image->vk.format);
      return false;
   }
   sampled->address = image->address;
   sampled->extent = (VkExtent2D){image->vk.extent.width, image->vk.extent.height};
   sampled->format_word = entry->image_format << 20;
   sampled->pitch_texels = padded ? pitch_texels : 0;
   sampled->dst_sel = entry->dst_sel;
   /* Only a combined image sampler carries a sampler's words; a storage image's
    * 32 bytes leave them out (ps5vk_write_image_descriptor). */
   sampled->sampler_word = needs_sampler ? sampler->word : 0;
   sampled->lod_word = needs_sampler ? sampler->lod_word : 0;
   sampled->address_word = needs_sampler ? sampler->address_word : PS5VK_TEXTURE_CLAMP_TO_EDGE;
   sampled->base_layer = view->base_array_layer;
   sampled->layer_count = view->layer_count;
   sampled->array = view->layer_count > 1;
   sampled->cube = view->layer_count == 6 &&
                   (image->vk.create_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0;
   sampled->base_mip_level = view->base_mip_level;
   sampled->last_mip_level = view->base_mip_level + view->level_count - 1;
   sampled->image_last_mip_level = image->vk.mip_levels - 1;
   sampled->tiled = tiled;
   sampled->barrier = rendered_here;
   return true;
}

/* R10: the entry an input-attachment binding reads, built from the subpass the
 * command buffer is inside rather than from an application's write -- Vulkan
 * forbids a descriptor write of VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, so the
 * application never has one (driver/ps5vk_pipeline.c records which bindings
 * each stage reads that way, from the shader's own InputAttachmentIndex,
 * DescriptorSet and Binding decorations).
 *
 * Nothing here is a new read path: the entry is the same 32-byte image
 * descriptor a storage image gets, and ps5vk_sampled_image builds it from the
 * attachment's view. What the entry then does differently follows from the
 * machinery that was already there -- subpass 0 rendered into that image in
 * this command buffer, so the draw that reads it carries the colour-buffer
 * barrier and splits the submission, which is what makes subpass 1's fetch see
 * subpass 0's writes (ps5vk_sampled_image's rendered_here, HARDWARE_FINDINGS.md
 * event 45).
 *
 * Returns the entry, NULL when this binding is not an input attachment (the
 * application's own write is then what fills it), or NULL with *refused set
 * when it is one and the subpass cannot supply it: a sentence rather than a
 * wrong picture. */
static const struct ps5vk_descriptor_buffer *
ps5vk_input_attachment_descriptor(struct ps5vk_cmd_buffer *cmd_buffer,
                                  const struct ps5vk_pipeline *pipeline, uint32_t stage,
                                  const PsbcDescriptorBinding *binding, bool *refused,
                                  struct ps5vk_descriptor_buffer *out)
{
   const struct ps5vk_input_attachment *declared = NULL;
   for (uint32_t at = 0; at < pipeline->input_attachment_count; at++) {
      if (pipeline->input_attachments[at].stage == stage &&
          pipeline->input_attachments[at].set == binding->set &&
          pipeline->input_attachments[at].binding == binding->binding) {
         declared = &pipeline->input_attachments[at];
         break;
      }
   }
   if (declared == NULL)
      return NULL;
   if (binding->array_size != 1) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "input attachment arrays need a subpass-index probe (R10)");
      *refused = true;
      return NULL;
   }
   const struct vk_render_pass *const pass = cmd_buffer->vk.render_pass;
   const struct vk_framebuffer *const framebuffer = cmd_buffer->vk.framebuffer;
   if (pass == NULL || framebuffer == NULL ||
       cmd_buffer->vk.subpass_idx >= pass->subpass_count) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u is an input attachment the shader reads and the "
                              "draw is not inside a render pass subpass; an input attachment is "
                              "read from the subpass that names it (docs/M5_PHASE_C.md, R10)",
                              (unsigned)binding->set, (unsigned)binding->binding);
      *refused = true;
      return NULL;
   }
   const struct vk_subpass *const subpass = &pass->subpasses[cmd_buffer->vk.subpass_idx];
   if (declared->index >= subpass->input_count) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u reads input attachment %u and the subpass the "
                              "draw is in declares %u of them; the shader's own "
                              "InputAttachmentIndex is what has to name one of its subpass's input "
                              "attachments (docs/M5_PHASE_C.md, R10)",
                              (unsigned)binding->set, (unsigned)binding->binding, declared->index,
                              subpass->input_count);
      *refused = true;
      return NULL;
   }
   const uint32_t attachment = subpass->input_attachments[declared->index].attachment;
   if (attachment == VK_ATTACHMENT_UNUSED || attachment >= framebuffer->attachment_count) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "set %u binding %u reads input attachment %u, which is "
                              "VK_ATTACHMENT_UNUSED or past the framebuffer's %u attachments "
                              "(docs/M5_PHASE_C.md, R10)",
                              (unsigned)binding->set, (unsigned)binding->binding, declared->index,
                              framebuffer->attachment_count);
      *refused = true;
      return NULL;
   }
   *out = (struct ps5vk_descriptor_buffer){
      .type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
      .view = framebuffer->attachments[attachment],
   };
   return out;
}

/* The 48 bytes of a combined image sampler, as write_image_descriptor writes
 * them (src/diagnostics.cpp), with word 4 also carrying a custom row pitch. */
static void
ps5vk_write_image_descriptor(uint32_t *descriptor, const struct ps5vk_sampled_image *sampled)
{
   memset(descriptor, 0, PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES);
   descriptor[0] = (uint32_t)(sampled->address >> 8);
   descriptor[1] = sampled->format_word | (((sampled->extent.width - 1u) & 3u) << 30) |
                   (uint32_t)(sampled->address >> 40);
   descriptor[2] = ((sampled->extent.width - 1u) >> 2) |
                   ((sampled->extent.height - 1u) << 14) | PS5VK_TEXTURE_RESOURCE_LEVEL;
   /* Word 3's bits 12-15 are the view's first level and bits 16-19 its last
    * (ps5-opengl's texture descriptor), and word 5's bits 4-7 the image's own
    * last level: that field is the chain the hardware walks, so a view of one
    * level of a five-level image still writes 4 there (Phase C7). A single-level
    * image leaves every level field zero, which is every descriptor before C7
    * word for word. The low twelve bits are the format's own DST_SEL, so a
    * one-, two- or three-channel format fetches with Vulkan's fill-in rule
    * (V0-formats; ps5vk_private.h, PS5VK_FORMAT_SWIZZLE_R001). */
   /* Word 3's TYPE (bits 28-31): a view of more than one layer is a 2D array,
    * one of six cube-compatible layers is a cube (the register database's
    * SQ_RSRC_IMG_TYPE, docs/HARDWARE_FINDINGS.md). */
   const uint32_t kind = sampled->cube      ? (PS5VK_TEXTURE_CUBE_KIND |
                                               (sampled->tiled ? PS5VK_TEXTURE_SWIZZLE : 0))
                         : sampled->array ? (PS5VK_TEXTURE_2D_ARRAY_KIND |
                                             (sampled->tiled ? PS5VK_TEXTURE_SWIZZLE : 0))
                                          : (sampled->tiled ? PS5VK_TEXTURE_2D_TILED_KIND
                                                            : PS5VK_TEXTURE_2D_KIND);
   descriptor[3] = kind | sampled->dst_sel | (sampled->base_mip_level << 12) |
                   (sampled->last_mip_level << 16);
   /* Word 4 carries the layers (D1): DEPTH, the count minus one, at bits 0-12
    * and BASE_ARRAY, the first layer, at bits 16-28 -- the register database's
    * SQ_IMG_RSRC_WORD4 fields. A single-layer view writes word 4 as zero, which
    * is every descriptor before this step. */
   descriptor[4] = (sampled->layer_count > 0 ? (sampled->layer_count - 1u) & 0x1fffu : 0u) |
                   ((sampled->base_layer & 0x1fffu) << 16);
   /* GFX10.3 custom linear pitch: DEPTH and PITCH_MSB form bits 0-13.
    * Only the guarded non-array 2D case uses this instead of layer fields. */
   if (sampled->pitch_texels != 0)
      descriptor[4] = sampled->pitch_texels - 1;
   descriptor[5] = PS5VK_TEXTURE_SINGLE_LEVEL | (sampled->image_last_mip_level << 4);
   descriptor[8] = sampled->address_word;
   descriptor[9] = sampled->image_last_mip_level == 0 ? PS5VK_TEXTURE_LOD_RANGE : sampled->lod_word;
   descriptor[10] = sampled->sampler_word;
}

/* Shared by draws and dispatches: the compiler's per-set table ABI and push
 * constant pointers have the same layout in every shader stage. */
bool
ps5vk_cmd_buffer_shader_resources(struct ps5vk_cmd_buffer *cmd_buffer,
                                  const struct ps5vk_pipeline *pipeline,
                                  const PsbcShaderMetadata *const *metadata,
                                  const VkShaderStageFlags *stage_bits, uint32_t stage_count,
                                  uint32_t user_data[][PS5VK_MAX_USER_DATA], bool *colour_barrier)
{
   struct ps5vk_device *const device =
      container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk);
   for (uint32_t s = 0; s < stage_count; s++) {
      if (metadata[s]->user_sgpr_count > PS5VK_MAX_USER_DATA ||
          metadata[s]->descriptor_binding_count > PSBC_MAX_DESCRIPTOR_BINDINGS) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "shader resource metadata exceeds the driver budget");
         return false;
      }
   }
   /* The set-0 descriptor table of a stage holds every binding its metadata
    * names, each at the offset the compiler reads it from: push constants, as
    * a driver-owned uniform buffer at the reserved binding (docs/M5_PHASE_C.md,
    * C1b question 1), and the application's uniform buffers, from the set
    * vkCmdBindDescriptorSets bound (C3). A stage that reads both gets one
    * table holding both, and the table is per stage and per draw. */
   uint8_t *push_constant_block = NULL;
   uint32_t push_constant_bytes = 0;
   if (pipeline->push_constant_bytes != 0) {
      push_constant_bytes =
         (uint32_t)ALIGN_POT(pipeline->push_constant_bytes, PS5VK_UNIFORM_BUFFER_DESCRIPTOR_BYTES);
      push_constant_block =
         ps5vk_cmd_buffer_table(cmd_buffer, push_constant_bytes, PS5VK_BUFFER_ALIGNMENT);
      if (!push_constant_block)
         return false;
      memcpy(push_constant_block, cmd_buffer->push_constants, pipeline->push_constant_bytes);
      ps5vk_flush_cpu_cache(push_constant_block, push_constant_bytes);
      /* What the debug API hands a probe (ps5vk_debug.h): the last draw's
       * upload, so a test can assert the bytes a stage reads instead of
       * inferring them from pixels (R9). */
      device->push_constant_block = push_constant_block;
      device->push_constant_bytes = pipeline->push_constant_bytes;
      device->push_constant_user_data_dword = UINT32_MAX;
   }

   /* R9: an application's push constants reach the stage through a user-data
    * dword the *compiler* names -- a pointer to the data, because the standalone
    * path passes no inline mask (tooling/psbc/patch-push-constant-location.py).
    * Until that field existed the driver wrote nothing there and the shader read
    * an unwritten SGPR: the silent zero v0-push-constant measured. The words are
    * the block built above, and the two forms that cannot be programmed are
    * refused by name rather than left to read garbage. */
   if (getenv("PS5VK_PUSH_TRACE") != NULL)
      for (uint32_t s = 0; s < stage_count; s++)
         fprintf(stderr,
                 "[ps5vk] stage %u push: valid=%d dword=%u count=%u inline=%d(%u) sgprs=%u\n", s,
                 (int)metadata[s]->push_constant_valid,
                 metadata[s]->push_constant_user_data_dword,
                 metadata[s]->push_constant_dword_count,
                 (int)metadata[s]->push_constant_inline,
                 metadata[s]->push_constant_inline_count, metadata[s]->user_sgpr_count);
   for (uint32_t s = 0; s < stage_count; s++) {
      const PsbcShaderMetadata *const stage_metadata = metadata[s];
      if (!stage_metadata->push_constant_valid ||
          (pipeline->push_constant_stages & stage_bits[s]) == 0)
         continue;
      if (stage_metadata->push_constant_inline) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "stage %u reads %u push-constant dwords the compiler inlined into "
                                 "user SGPRs; this driver programs the pointer form only "
                                 "(tooling/psbc/patch-push-constant-location.py, R9)",
                                 s, stage_metadata->push_constant_inline_count);
         return false;
      }
      const uint32_t count = stage_metadata->push_constant_dword_count;
      const uint32_t at = stage_metadata->push_constant_user_data_dword;
      if (push_constant_block == NULL || (count != 1 && count != 2) ||
          at + count > PS5VK_MAX_USER_DATA) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "stage %u reads its push constants from %u user-data dwords at "
                                 "dword %u, and this driver writes a one- or two-dword address "
                                 "inside the %u it programs "
                                 "(tooling/psbc/patch-push-constant-location.py, R9)",
                                 s, count, at, PS5VK_MAX_USER_DATA);
         return false;
      }
      /* The pointer, in the form the compiler declared it: one dword in the
       * 32-bit-pointer ABI this driver compiles for (PS5VK_ADDRESS_HIGH_WORD is
       * the half ACO already knows), two when the ABI asks for a full 64-bit
       * address. Same convention as the vertex-buffer table's dword. */
      user_data[s][at] = (uint32_t)(uintptr_t)push_constant_block;
      if (count == 2)
         user_data[s][at + 1] = (uint32_t)((uintptr_t)push_constant_block >> 32);
      device->push_constant_user_data_dword = at;
      device->push_constant_user_data_stage = s;
      device->push_constant_user_data_low = user_data[s][at];
      device->push_constant_user_data_high = count == 2 ? user_data[s][at + 1] : 0;
   }
   /* R7: the tables this draw builds replace the last draw's in the debug API
    * (ps5vk_debug_descriptor_tables), so a probe reads the frame's last one. */
   device->descriptor_table_count = 0;
   /* Whether this draw samples an image the command buffer rendered into
    * earlier: the colour barrier then has to sit in the words before it
    * (HARDWARE_FINDINGS.md, event 45; ps5vk_sampled_image). */
   *colour_barrier = false;
   for (uint32_t s = 0; s < stage_count; s++) {
      const PsbcShaderMetadata *const stage_metadata = metadata[s];
      if (stage_metadata->descriptor_binding_count == 0)
         continue;
      /* One table per set the stage reads, each sized from that set's own
       * bindings: the compiler builds one layout per set index and the offsets
       * inside a table start at zero in every set
       * (tooling/psbc/patch-descriptor-sets.py, docs/M5_PHASE_C.md R7). A
       * binding outside the sets this driver advertises would be dropped by
       * this loop rather than refused, so it is refused here instead. */
      for (uint32_t b = 0; b < stage_metadata->descriptor_binding_count; b++) {
         if (stage_metadata->descriptor_bindings[b].set >= PS5VK_DESCRIPTOR_SET_COUNT) {
            ps5vk_cmd_buffer_refuse(
               cmd_buffer, VK_ERROR_UNKNOWN,
               "the stage reads set %u binding %u; more than the %u sets this driver advertises "
               "(VkPhysicalDeviceLimits.maxBoundDescriptorSets; PS5VK_DESCRIPTOR_SET_COUNT, "
               "ps5vk_private.h)",
               (unsigned)stage_metadata->descriptor_bindings[b].set,
               (unsigned)stage_metadata->descriptor_bindings[b].binding,
               (unsigned)PS5VK_DESCRIPTOR_SET_COUNT);
            return false;
         }
      }
      for (uint32_t set = 0; set < PS5VK_DESCRIPTOR_SET_COUNT; set++) {
         if (!stage_metadata->descriptor_sets_valid[set])
            continue;
         if (stage_metadata->descriptor_sets_user_data_dword[set] >= stage_metadata->user_sgpr_count) {
            ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                    "the stage reads set %u and the metadata the shader was "
                                    "compiled with names no user-data dword for its pointer",
                                    (unsigned)set);
            return false;
         }
         /* Every binding is checked before its table exists: what the compiler
          * reads, where it reads it, and what the application put there. The
          * table reaches past the last binding's end, its offset plus one entry
          * for each descriptor the binding holds. */
         size_t table_bytes = 0;
         for (uint32_t b = 0; b < stage_metadata->descriptor_binding_count; b++) {
            const PsbcDescriptorBinding *const binding = &stage_metadata->descriptor_bindings[b];
            if (binding->set != set)
               continue;
            /* Every binding's entry counts towards the table before any of the
             * checks below can skip it: the table's size and the pointer the
             * stage's user data gets are what the shader's own reads are sized
             * against. */
            table_bytes =
               MAX2(table_bytes, (size_t)binding->offset +
                                    (size_t)binding->array_size * binding->stride);
            if (binding->stride == 0) {
               ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                       "set %u binding %u: descriptor type %d has no proven table "
                                       "entry, and a runner probe names the step that proves one "
                                       "(docs/M5_REFERENCE.md)",
                                       (unsigned)binding->set, (unsigned)binding->binding,
                                       (unsigned)binding->type);
               return false;
            }
            /* Two bindings at one table entry would overwrite each other: the
             * compiler places the reserved push-constant binding at the start of
             * the table, where an application's first binding also begins. */
            for (uint32_t p = 0; p < b; p++) {
               if (stage_metadata->descriptor_bindings[p].set != binding->set)
                  continue;
               if (stage_metadata->descriptor_bindings[p].offset == binding->offset) {
                  ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                          "the stage reads binding %u and binding %u from table "
                                          "entry %u; the reserved push-constant binding and an "
                                          "application binding cannot share one "
                                          "(docs/M5_REFERENCE.md, C3)",
                                          (unsigned)stage_metadata->descriptor_bindings[p].binding,
                                          (unsigned)binding->binding, binding->offset);
                  return false;
               }
            }
            if (binding->binding == PS5VK_PUSH_CONSTANT_BINDING &&
                (pipeline->push_constant_stages & stage_bits[s]))
               continue;
            for (uint32_t element = 0; element < binding->array_size; element++) {
               /* R10: an input attachment comes from the subpass, everything
                * else from the application's write. */
               struct ps5vk_descriptor_buffer attachment = {0};
               bool refused_attachment = false;
               const struct ps5vk_descriptor_buffer *written = ps5vk_input_attachment_descriptor(
                  cmd_buffer, pipeline, s, binding, &refused_attachment, &attachment);
               if (refused_attachment)
                  return false;
               if (written == NULL)
                  written = ps5vk_cmd_buffer_descriptor(cmd_buffer, binding->set, binding->binding, element);
               if (written == NULL) {
                  ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                          "set %u binding %u element %u is not bound or holds no write; the "
                                          "application has to bind and update it "
                                          "(docs/M5_REFERENCE.md, C3)",
                                          (unsigned)set, (unsigned)binding->binding, element);
                  return false;
               }
               if (binding->stride == PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES ||
                   binding->stride == PS5VK_STORAGE_IMAGE_DESCRIPTOR_BYTES) {
                  /* The compiler reads one 48-byte entry per combined image
                   * sampler and one 32-byte entry per storage image: the same
                   * image descriptor, with a sampler after it for the first. */
                  /* The entry's shape comes from the stride and the types that
                   * fill it are the ones with that shape: a 48-byte entry is a
                   * combined image sampler, a bare sampled image or a bare sampler
                   * -- the last two carry one half each, the image's entry and the
                   * sampler's entry of the same texture instruction (R2) -- and a
                   * 32-byte one a storage or an input image. */
                  const bool entry_48 =
                     binding->stride == PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES;
                  const VkDescriptorType wanted =
                     entry_48 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                  const bool shape_matches =
                     entry_48 ? (written->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                                 written->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                                 written->type == VK_DESCRIPTOR_TYPE_SAMPLER)
                              : (written->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                                 written->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
                  if (!shape_matches) {
                     ps5vk_cmd_buffer_refuse(
                        cmd_buffer, VK_ERROR_UNKNOWN,
                        "set %u binding %u: the compiler reads a %u-byte %s entry and the write is "
                        "descriptor type %d",
                        (unsigned)set, (unsigned)binding->binding, (unsigned)binding->stride,
                        wanted == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "storage image" : "combined image "
                                                                                    "sampler",
                        (unsigned)written->type);
                     return false;
                  }
                  /* A bare sampler names no view: its entry is the sampler's
                   * three words and nothing else, written below. */
                  if (written->type != VK_DESCRIPTOR_TYPE_SAMPLER) {
                     struct ps5vk_sampled_image sampled;
                     if (!ps5vk_sampled_image(cmd_buffer, set, binding->binding, written,
                                             &sampled))
                        return false;
                     *colour_barrier = *colour_barrier || sampled.barrier;
                  }
               } else if (binding->stride == PS5VK_UNIFORM_BUFFER_DESCRIPTOR_BYTES ||
                          binding->stride == PS5VK_TEXEL_BUFFER_DESCRIPTOR_BYTES) {
                  const bool texel_buffer =
                     written->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                     written->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                  if (written->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                      written->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
                      written->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && !texel_buffer) {
                     ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                             "set %u binding %u: the compiler reads a 16-byte buffer "
                                             "entry and the write is descriptor type %d",
                                             (unsigned)set, (unsigned)binding->binding, (unsigned)written->type);
                     return false;
                  }
                  /* A texel buffer's entry is the same 16 bytes, and its own words
                   * are the writer's below: the checks it skips are the uniform
                   * buffer's address and range, not this loop's accounting, which
                   * has already counted the entry (a `continue` here would leave
                   * the table's size at zero, and the entry would be written past
                   * the end of what the draw reserved for it). */
                  if (texel_buffer)
                     continue;
                  if (written->address == 0) {
                     ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                             "set %u binding %u names a buffer with no GPU address: it "
                                             "has to be bound to memory",
                                             (unsigned)set, (unsigned)binding->binding);
                     return false;
                  }
                  if (written->size == 0 ||
                      ((written->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                        written->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) &&
                       written->size % binding->stride != 0)) {
                     ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                             "set %u binding %u covers %" PRIu64 " bytes, not a whole "
                                             "number of %u-byte elements", (unsigned)set, (unsigned)binding->binding,
                                             written->size, binding->stride);
                     return false;
                  }
               } else {
                  ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                          "set %u binding %u: the compiler reads %u-byte entries, which "
                                          "no descriptor type this driver records fills "
                                          "(docs/M5_REFERENCE.md)",
                                          (unsigned)set, (unsigned)binding->binding, binding->stride);
                  return false;
               }
            }
         }
         if (table_bytes > PS5VK_TABLE_CHUNK_BYTES) {
            ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                    "set %u: the table reaches %zu bytes, past the %" PRIu64
                                    " one chunk holds", (unsigned)set, table_bytes,
                                    (uint64_t)PS5VK_TABLE_CHUNK_BYTES);
            return false;
         }
         const size_t allocated = ALIGN_POT(table_bytes, PS5VK_BUFFER_ALIGNMENT);
         uint32_t *const table = ps5vk_cmd_buffer_table(cmd_buffer, allocated, PS5VK_BUFFER_ALIGNMENT);
         if (!table)
            return false;
         memset(table, 0, allocated);
         for (uint32_t b = 0; b < stage_metadata->descriptor_binding_count; b++) {
            const PsbcDescriptorBinding *const binding = &stage_metadata->descriptor_bindings[b];
            if (binding->set != set)
               continue;
            uint32_t *descriptor = table + binding->offset / sizeof(uint32_t);
            if (binding->binding == PS5VK_PUSH_CONSTANT_BINDING &&
                (pipeline->push_constant_stages & stage_bits[s])) {
               /* The reserved binding's descriptor, exactly as the C1b path
                * wrote it: the draw's push-constant bytes, one 16-byte entry. */
               descriptor[0] = (uint32_t)(uintptr_t)push_constant_block;
               descriptor[1] = (uint32_t)((uintptr_t)push_constant_block >> 32) |
                               (PS5VK_UNIFORM_BUFFER_DESCRIPTOR_BYTES << 16);
               descriptor[2] = push_constant_bytes / PS5VK_UNIFORM_BUFFER_DESCRIPTOR_BYTES;
               descriptor[3] = PS5VK_UNIFORM_BUFFER_FLAGS;
               device->push_constant_descriptor = descriptor;
               continue;
            }
            for (uint32_t element = 0; element < binding->array_size; element++) {
               descriptor = table + (binding->offset + element * binding->stride) / sizeof(uint32_t);
               struct ps5vk_descriptor_buffer attachment = {0};
               bool refused_attachment = false;
               const struct ps5vk_descriptor_buffer *written = ps5vk_input_attachment_descriptor(
                  cmd_buffer, pipeline, s, binding, &refused_attachment, &attachment);
               if (refused_attachment)
                  return false;
               if (written == NULL)
                  written = ps5vk_cmd_buffer_descriptor(cmd_buffer, binding->set, binding->binding, element);
               assert(written != NULL); /* Validated above. */
               struct ps5vk_sampled_image sampled;
               if ((binding->stride == PS5VK_STORAGE_IMAGE_DESCRIPTOR_BYTES ||
                    binding->stride == PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES) &&
                   written->type != VK_DESCRIPTOR_TYPE_SAMPLER &&
                   !ps5vk_sampled_image(cmd_buffer, set, binding->binding, written, &sampled))
                  return false;
               /* The application's binding: a combined image sampler's 48 bytes, or
                * the uniform buffer's entry in the stride form the hardware ran: the
                * address, its high word with the element stride, the elements the
                * range covers and the flags (src/diagnostics.cpp,
                * kUniformBufferFlags). */
               if (binding->stride == PS5VK_STORAGE_IMAGE_DESCRIPTOR_BYTES) {
                  /* A storage image's 32 bytes are the combined sampler's first
                   * eight: the image descriptor without the sampler's three words.
                   * The kind, levels and layer fields are the sampled path's, and the
                   * format's DST_SEL is what supplies Vulkan's fill-in rule for the
                   * channels a format does not have (ps5vk_write_image_descriptor). */
                  uint32_t image_descriptor[PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES / 4];
                  ps5vk_write_image_descriptor(image_descriptor, &sampled);
                  memcpy(descriptor, image_descriptor, PS5VK_STORAGE_IMAGE_DESCRIPTOR_BYTES);
                  continue;
               }
               if (binding->stride == PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES) {
                  /* A bare sampler's entry is the sampler half alone: the words the
                   * image descriptor leaves at 8, 9 and 10, with the image half zero
                   * because the paired SAMPLED_IMAGE binding's entry carries it. The
                   * instruction reads each half from its own binding, which is what
                   * makes the separated form fetch what the combined form fetches
                   * (R2 of the port's requests). */
                  if (written->type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                     VK_FROM_HANDLE(ps5vk_sampler, sampler, written->sampler);
                     memset(descriptor, 0, PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES);
                     descriptor[8] = sampler->address_word;
                     descriptor[9] = sampler->lod_word;
                     descriptor[10] = sampler->word;
                     continue;
                  }
                  ps5vk_write_image_descriptor(descriptor, &sampled);
                  continue;
               }
               if (written->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                   written->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                  /* A texel buffer's V#: the view's buffer as elements of the view's
                   * format, the format entry's channel selectors and format word in
                   * word 3 (docs/BLOCKERS.md, the descriptor types). */
                  VK_FROM_HANDLE(ps5vk_buffer_view, view, written->buffer_view);
                  const struct ps5vk_format *const texel = view ? ps5vk_find_format(view->format) : NULL;
                  if (view == NULL || texel == NULL || texel->image_format == 0 ||
                      view->buffer->vk.device_address == 0) {
                     ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                             "set %u binding %u names a texel buffer view with no buffer, "
                                             "address or recorded format word",
                                             (unsigned)set, (unsigned)binding->binding);
                     return false;
                  }
                  const uint32_t texel_bytes = vk_format_get_blocksize(view->format);
                  const uint64_t address = view->buffer->vk.device_address + view->offset;
                  descriptor[0] = (uint32_t)address;
                  descriptor[1] = (uint32_t)(address >> 32) | (texel_bytes << 16);
                  descriptor[2] = texel_bytes != 0 ? (uint32_t)(view->range / texel_bytes) : 0u;
                  descriptor[3] = texel->dst_sel | PS5VK_TEXEL_BUFFER_FORMAT(texel->image_format) |
                                  PS5VK_TEXEL_BUFFER_RESOURCE_LEVEL;
                  continue;
               }
               if (written->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                  descriptor[0] = (uint32_t)written->address;
                  descriptor[1] = (uint32_t)(written->address >> 32);
                  descriptor[2] = (uint32_t)written->size;
                  /* The byte-addressed storage-buffer form proven by c0/D2. */
                  descriptor[3] = UINT32_C(0x31016fac);
                  continue;
               }
               /* A dynamic uniform buffer's address is the application's offset into
                * the bound range (VkBindDescriptorSetsInfo.pDynamicOffsets, D1). The
                * bound range itself does not change, so the record count does not
                * either. */
               uint64_t address = written->address;
               if (written->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                  const struct ps5vk_descriptor_set_layout *layout =
                     cmd_buffer->descriptor_sets[binding->set]->layout;
                  const uint32_t dynamic_index = layout->bindings[binding->binding].dynamic_index + element;
                  assert(dynamic_index < PS5VK_DYNAMIC_UNIFORM_COUNT);
                  address += cmd_buffer->descriptor_set_offsets[binding->set][dynamic_index];
               }
               descriptor[0] = (uint32_t)address;
               descriptor[1] = (uint32_t)(address >> 32) | (binding->stride << 16);
               descriptor[2] = (uint32_t)(written->size / binding->stride);
               descriptor[3] = PS5VK_UNIFORM_BUFFER_FLAGS;
            }
         }
         ps5vk_flush_cpu_cache(table, allocated);
         const uint32_t dword = stage_metadata->descriptor_sets_user_data_dword[set];
         user_data[s][dword] = (uint32_t)(uintptr_t)table;
         /* What the debug API hands a probe: this set's table, the dword its
          * pointer went to and the pointer itself, so a caller asserts both
          * sets' tables instead of inferring them from pixels (R7). */
         if (device->descriptor_table_count < ARRAY_SIZE(device->descriptor_tables)) {
            device->descriptor_tables[device->descriptor_table_count++] = (ps5vk_debug_table){
               .stage = s,
               .set = set,
               .user_data_dword = dword,
               .address_low = user_data[s][dword],
               .address_high = 0,
               .words = table,
               .bytes = allocated,
            };
         }
      }
   }

   return true;
}

/* Records one draw, indexed or not: the three register tables, both stages'
 * user data and the draw packet. An indexed draw's index state is the index
 * buffer the application bound (ps5vk_CmdBindIndexBuffer3KHR); a non-indexed
 * one is the recorded frames' DRAW_INDEX_AUTO. */
static void
ps5vk_cmd_draw(struct ps5vk_cmd_buffer *cmd_buffer, uint32_t vertex_count, uint32_t instance_count,
               uint32_t first_vertex, uint32_t first_instance,
               const struct ps5vk_indexed_draw *indexed)
{
   struct ps5vk_device *const device =
      container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk);
   struct ps5vk_pipeline *const pipeline = cmd_buffer->pipeline;
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || instance_count == 0 ||
       (indexed != NULL ? indexed->index_count == 0 : vertex_count == 0))
      return;
   /* Valid usage: inside a rendering, with a graphics pipeline bound. */
   assert(cmd_buffer->rendering && pipeline);

   if (pipeline->draw_refusal) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN, "%s", pipeline->draw_refusal);
      return;
   }
   if (first_instance != 0 || (first_vertex != 0 && indexed == NULL)) {
      /* An indexed draw's first_vertex is its base vertex, which the vertex
       * stage's base-vertex user data carries (Phase C2's probe:
       * src/diagnostics.cpp, run_vulkan_base_vertex_frames). A non-indexed
       * first vertex and a first instance still need their own probe: nothing
       * recorded sets either. */
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a first vertex on a non-indexed draw, or a first instance, needs a "
                              "runner probe (docs/M5_REFERENCE.md, C2)");
      return;
   }
   /* The viewport and scissor of the draw, which vkCmdBindPipeline took from
    * the pipeline and vkCmdSetViewport and vkCmdSetScissor may have replaced.
    * One of each fills the viewport registers; vk_meta's clears use a
    * 4096x4096 one over a 3840x2160 target, which clips nothing. */
   const struct vk_dynamic_graphics_state *const dynamic = &cmd_buffer->vk.dynamic_graphics_state;
   if (dynamic->vp.viewport_count != 1 || dynamic->vp.scissor_count != 1) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "draws need exactly one viewport and scissor; %u viewports and %u "
                              "scissors are set",
                              dynamic->vp.viewport_count, dynamic->vp.scissor_count);
      return;
   }
   /* R1's depth bias through R8's dynamic state: the pipeline's three factors
    * reach the command buffer through vkCmdBindPipeline unless it declares
    * VK_DYNAMIC_STATE_DEPTH_BIAS, where vkCmdSetDepthBias is the only source
    * (ps5vk_pipeline_dynamic_state fills the state the bind copies). Two things
    * are refused by name rather than guessed at, and the static and the dynamic
    * form refuse exactly the same state:
    *
    *   - a non-zero depthBiasClamp. Vulkan clamps the bias o to
    *     +-depthBiasClamp, this hardware's PA_SU_POLY_OFFSET_CLAMP (0x2df)
    *     measured inert on the D32 float path -- a 2e-5 clamp left a 0.00049
    *     pull intact and changed no pixel of a ramp
    *     (Klog_Logs/r-depth-bias5.log) -- and capping the bias in the driver
    *     instead would be a wrong depth reported as a success, which is the
    *     failure mode both request documents exist to remove. The clamp needs a
    *     console run that shows the register moving a biased depth.
    *   - a depth attachment whose format has no measured DB_FMT_CNTL word: the
    *     block's first word is the format's, and only the D32 float word is
    *     measured (ps5-opengl's 0x1e9).
    *
    * A rendering with no depth attachment makes the bias inert, which is what
    * Vulkan says, and records no word at all. */
   const bool depth_bias = dynamic->rs.depth_bias.enable;
   if (depth_bias && dynamic->rs.depth_bias.clamp != 0.0f) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "drawing with a depth bias clamped to %g: the clamp needs a runner "
                              "probe, because this hardware's PA_SU_POLY_OFFSET_CLAMP (0x2df) "
                              "measured inert on the D32 float path and capping the bias in the "
                              "driver would report a wrong depth as a success "
                              "(PS5_VULKAN_REQUESTSv2.md, the clamp decision)",
                              (double)dynamic->rs.depth_bias.clamp);
      return;
   }
   if (depth_bias && cmd_buffer->depth_bound && cmd_buffer->depth_format != VK_FORMAT_D32_SFLOAT &&
       cmd_buffer->depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "drawing with a depth bias through a depth attachment of format %d: "
                              "the six PA_SU_POLY_OFFSET_* words are ps5-opengl's D32 float block "
                              "(PA_SU_POLY_OFFSET_DB_FMT_CNTL 0x1e9), and another depth format's "
                              "word is not measured and needs its own probe "
                              "(PS5_VULKAN_REQUESTS.md, R1)",
                              (int)cmd_buffer->depth_format);
      return;
   }
   /* The three words the block records, from the factors the dynamic state
    * holds, in ps5-opengl's own encoding (its src/gallium/ps5/ps5_screen.c:2149
    * block: the clamp and the constant factor as float bits, the slope factor
    * times sixteen). The back pair mirrors the front's, and the clamp word is
    * always zero here because a non-zero clamp was refused above. */
   float bias_clamp = dynamic->rs.depth_bias.clamp;
   const float bias_scale = dynamic->rs.depth_bias.slope_factor * 16.0f;
   const float bias_offset = dynamic->rs.depth_bias.constant_factor;
   uint32_t bias_clamp_word = 0;
   uint32_t bias_scale_word = 0;
   uint32_t bias_offset_word = 0;
   memcpy(&bias_clamp_word, &bias_clamp, sizeof(bias_clamp));
   memcpy(&bias_scale_word, &bias_scale, sizeof(bias_scale));
   memcpy(&bias_offset_word, &bias_offset, sizeof(bias_offset));
   /* The block's own two enables in the rasterizer word: without
    * PA_SU_SC_MODE_CNTL's POLY_OFFSET_FRONT_ENABLE (bit 11) and
    * POLY_OFFSET_BACK_ENABLE (bit 12) the six words do nothing at all, which is
    * measured (Klog_Logs/r-depth-bias2.log). They are the draw's because the
    * enable can arrive through vkCmdSetDepthBias as well as the pipeline. */
   const uint32_t rasterizer_word =
      pipeline->rasterizer_word |
      (depth_bias ? (PS5VK_RASTERIZER_POLY_OFFSET_FRONT | PS5VK_RASTERIZER_POLY_OFFSET_BACK) : 0u);
   const VkResult prepared = ps5vk_pipeline_prepare_shaders(device, pipeline);
   if (prepared != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, prepared);
      return;
   }

   const struct ps5vk_pipeline_shaders *const shaders = &pipeline->shaders;
   const struct ps5vk_shader_tables *const vertex = &shaders->tables[PS5VK_PIPELINE_STAGE_VERTEX];
   const struct ps5vk_shader_tables *const pixel = &shaders->tables[PS5VK_PIPELINE_STAGE_PIXEL];
   const uint8_t *const stage = shaders->stage.address;
   const PsbcShaderMetadata *const metadata[PS5VK_PIPELINE_STAGE_COUNT] = {
      &pipeline->stages[PS5VK_PIPELINE_STAGE_VERTEX].metadata,
      &pipeline->stages[PS5VK_PIPELINE_STAGE_PIXEL].metadata,
   };

   /* The user data each stage is programmed with before this draw. A shader
    * object's register tables hold no user-data registers, so a stage that
    * left them alone would read the previous draw's (HARDWARE_FINDINGS.md,
    * pid 117): the meta clear's vertex-buffer table would become the next
    * triangle's base vertex. */
   uint32_t user_data[PS5VK_PIPELINE_STAGE_COUNT][PS5VK_MAX_USER_DATA] = {{0}};
   uint32_t user_data_count[PS5VK_PIPELINE_STAGE_COUNT] = {0};

   /* The vertex-buffer table: one 16-byte record per binding the pipeline's
    * vertex input uses, naming the buffer vkCmdBindVertexBuffers bound and
    * the vertices this draw fetches. */
   uint32_t binding_count = 0;
   for (uint32_t binding = 0; binding < PS5VK_MAX_VERTEX_BINDINGS; binding++) {
      if (pipeline->vertex_bindings[binding].used)
         binding_count = binding + 1;
   }
   if (binding_count != 0) {
      const size_t table_bytes = binding_count * PS5VK_VERTEX_RECORD_WORDS * sizeof(uint32_t);
      uint32_t *const table =
         ps5vk_cmd_buffer_table(cmd_buffer, table_bytes, PS5VK_BUFFER_ALIGNMENT);
      if (!table)
         return;
      memset(table, 0, table_bytes);
      for (uint32_t binding = 0; binding < binding_count; binding++) {
         if (!pipeline->vertex_bindings[binding].used)
            continue;
         const struct ps5vk_vertex_buffer *const bound = &cmd_buffer->vertex_buffers[binding];
         if (bound->address == 0) {
            ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN, "vertex binding %u is not bound",
                                    binding);
            return;
         }
         uint32_t *const record = table + binding * PS5VK_VERTEX_RECORD_WORDS;
         record[0] = (uint32_t)bound->address;
         record[1] =
            (uint32_t)(bound->address >> 32) | (pipeline->vertex_bindings[binding].stride << 16);
         /* An indexed draw's table holds the records the bound buffer has,
          * not its index count; without a stride there is none to count, and
          * the draw's own count stays. The non-indexed value is compared
          * against golden frames and must not change. */
         const uint32_t stride = pipeline->vertex_bindings[binding].stride;
         record[2] = indexed == NULL || stride == 0
                        ? vertex_count
                        : (uint32_t)MIN2(bound->size / stride, UINT32_MAX);
         record[3] = PS5VK_VERTEX_BUFFER_FLAGS;
      }
      ps5vk_flush_cpu_cache(table, table_bytes);
      if (!metadata[PS5VK_PIPELINE_STAGE_VERTEX]->vertex_buffer_table_valid ||
          metadata[PS5VK_PIPELINE_STAGE_VERTEX]->vertex_buffer_table_user_data_dword >=
             PS5VK_MAX_USER_DATA) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "the vertex stage does not name its vertex-buffer table");
         return;
      }
      user_data[PS5VK_PIPELINE_STAGE_VERTEX]
               [metadata[PS5VK_PIPELINE_STAGE_VERTEX]->vertex_buffer_table_user_data_dword] =
         (uint32_t)(uintptr_t)table;
   }
   /* The base vertex and the LDS layout the vertex stage's ABI reads even when
    * it reads no buffer: the corner draws write them with no table at all. */
   if (metadata[PS5VK_PIPELINE_STAGE_VERTEX]->base_vertex_valid)
      user_data[PS5VK_PIPELINE_STAGE_VERTEX]
               [metadata[PS5VK_PIPELINE_STAGE_VERTEX]->base_vertex_user_data_dword] =
         indexed != NULL ? (uint32_t)indexed->vertex_offset : first_vertex;
   if (metadata[PS5VK_PIPELINE_STAGE_VERTEX]->ngg_lds_layout_valid)
      user_data[PS5VK_PIPELINE_STAGE_VERTEX]
               [metadata[PS5VK_PIPELINE_STAGE_VERTEX]->ngg_lds_layout_user_data_dword] =
         metadata[PS5VK_PIPELINE_STAGE_VERTEX]->ngg_lds_layout;

   const VkShaderStageFlags stage_bits[PS5VK_PIPELINE_STAGE_COUNT] = {
      VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
   bool colour_barrier = false;
   if (!ps5vk_cmd_buffer_shader_resources(cmd_buffer, pipeline, metadata, stage_bits,
                                          PS5VK_PIPELINE_STAGE_COUNT, user_data, &colour_barrier))
      return;

   /* How many dwords each stage is programmed with: the compiler's count. */
   for (uint32_t s = 0; s < PS5VK_PIPELINE_STAGE_COUNT; s++) {
      if (metadata[s]->user_sgpr_count > PS5VK_MAX_USER_DATA) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "the stage reads %u user-data dwords, more than the %u this driver "
                                 "programs",
                                 metadata[s]->user_sgpr_count, PS5VK_MAX_USER_DATA);
         return;
      }
      user_data_count[s] = metadata[s]->user_sgpr_count;
   }

   /* The depth registers and the depth-control word join the table only when the
    * rendering has a depth attachment, so every draw without one records exactly
    * the words it recorded before Phase C5 (Phase C5). */
   const uint32_t depth_count =
      cmd_buffer->depth_bound ? PS5VK_DEPTH_REGISTER_COUNT + 1u : 0u;
   /* A draw whose pipeline tests stencil adds the three stencil state registers
    * right after the depth control word. A pipeline that does not test stencil
    * records exactly the words it recorded before round 12, and the words are
    * the bound attachment's own: a stencil test with no stencil plane bound is
    * what the pipeline's validation refuses before a draw is recorded. */
   const uint32_t stencil_count = cmd_buffer->stencil_bound && dynamic->ds.stencil.test_enable
                                     ? PS5VK_STENCIL_REGISTER_COUNT
                                     : 0u;
   /* A pipeline that writes no colour zeroes the masks after every other record
    * has set them: the linked context and the viewport registers carry RGBA
    * (Phase C5; vk_meta's depth clear is the pipeline that asks for none). */
   /* CB_TARGET_MASK (0x08e) and CB_SHADER_MASK (0x08f) carry one *per-target*
    * nibble each: TARGETn_ENABLE and OUTPUTn_ENABLE, four bits at 4n
    * (R_028238/R_02823C). The viewport block below programs 0xf, which enables
    * target 0's RGBA and nothing for the other targets -- the single-attachment
    * default -- so a rendering into more than one attachment has to override both
    * words with every attachment's own mask. Programming attachment 0's mask for a
    * two-attachment rendering masks the second target's writes off in hardware
    * whatever its address is, which is what the console read as attachment 1
    * holding zero through both the draw and vk_meta's clear (that clear is a draw
    * through these registers too). The vkQuake port project named the register.
    * zero mask_count means the default word already says it. */
   const uint32_t mask_count = pipeline->colour_write_mask == 0xfu ? 0u : 2u;
   /* A four-sample rendering's rasterizer registers come right after the colour
    * target's, so a one-sample draw records exactly the words it recorded before
    * Phase C8. */
   const uint32_t msaa_count = cmd_buffer->multisample_count;
   /* A blending pipeline's CB_BLEND0_CONTROL and CB_COLOR_CONTROL go at the end
    * of the table, after the write masks, and CB_BLEND_RED/GREEN/BLUE/ALPHA
    * follow when its state reads the blend constants: every draw that does not
    * blend, and every blending draw whose factors are not constants, records
    * exactly the words it recorded before. */
   const uint32_t blend_constant_count =
      pipeline->blend_uses_constants ? PS5VK_BLEND_CONSTANT_COUNT : 0u;
   const uint32_t blend_count = (pipeline->blend_control != 0 ? PS5VK_BLEND_REGISTER_COUNT : 0u) +
                                blend_constant_count;
   /* R1's rasterization words go behind everything else, and only the state a
    * pipeline asks for: a draw that culls nothing, discards nothing and biases
    * nothing records exactly the words it recorded before R1. A depth-biased
    * pipeline's six PA_SU_POLY_OFFSET_* words follow, in the block's own order
    * (the format word, then the clamp, the front scale and offset and the back
    * pair), and only when the rendering bound a depth attachment: with none the
    * bias is inert and the words would be the only thing asking for a depth
    * format the draw does not have. */
   const uint32_t depth_bias_count =
      depth_bias && cmd_buffer->depth_bound ? PS5VK_POLY_OFFSET_COUNT : 0u;
   const uint32_t line_count = pipeline->line_rasterizer ? PS5VK_LINE_REGISTER_COUNT : 0u;
   /* A line's rasterizer word is recorded even when it is 0: the pipeline
    * cleared its cull bits (ps5vk_pipeline.c), and a table without the word
    * would leave an earlier culling draw's in the register. */
   const bool rasterizer_recorded = rasterizer_word != 0 || pipeline->line_rasterizer;
   const uint32_t raster_count = (rasterizer_recorded ? 1u : 0u) +
                                 (pipeline->discard_rasterizer ? 1u : 0u) + depth_bias_count +
                                 line_count;
   /* One row of target registers per colour attachment the rendering declared:
    * together with the copy below, this is the arithmetic R6's heap corruption
    * lived in, so the reservation and the copy read the same number
    * (cmd_buffer->colour_attachment_count, set by begin-rendering). */
   const uint32_t target_words =
      cmd_buffer->colour_attachment_count * PS5VK_TARGET_REGISTER_COUNT;
   const uint32_t fixed = target_words + msaa_count + depth_count + stencil_count +
                          PS5VK_VIEWPORT_REGISTER_COUNT;
   const uint32_t cx_count = fixed + PS5VK_STAGE_CONTEXT_RECORDS + vertex->cx_count +
                             pixel->cx_count + mask_count + blend_count + raster_count;
   const uint32_t sh_count = vertex->sh_count + pixel->sh_count;
   struct ps5vk_agc_register *const cx =
      ps5vk_cmd_buffer_table(cmd_buffer, cx_count * sizeof(*cx), 8);
   struct ps5vk_agc_register *const sh =
      cx ? ps5vk_cmd_buffer_table(cmd_buffer, sh_count * sizeof(*sh), 8) : NULL;
   if (!cx || !sh)
      return;

   /* One attachment's row: `fixed` reserves PS5VK_TARGET_REGISTER_COUNT records
    * above, and the memcpy's size has to be the row's, not the table's -- with a
    * row per target, sizeof(cmd_buffer->target_registers) is four rows and would
    * write three of them past what this stream reserved (the arithmetic R6's heap
    * corruption lived in). The draw's loop over the rendering's attachments is
    * what multiplies the reservation and this copy together. */
   memcpy(cx, cmd_buffer->target_registers, target_words * sizeof(*cx));
   if (msaa_count != 0)
      memcpy(cx + target_words, cmd_buffer->multisample_registers, msaa_count * sizeof(*cx));
   if (cmd_buffer->depth_bound) {
      memcpy(cx + target_words + msaa_count, cmd_buffer->depth_registers,
             sizeof(cmd_buffer->depth_registers));
      cx[target_words + msaa_count + PS5VK_DEPTH_REGISTER_COUNT] =
         (struct ps5vk_agc_register){.offset = PS5VK_DEPTH_CONTROL_REGISTER,
                                     .value = ps5vk_depth_control(dynamic,
                                                                  cmd_buffer->stencil_bound)};
      if (stencil_count != 0)
         ps5vk_stencil_registers(dynamic, cx + target_words + msaa_count + depth_count);
   }
   ps5vk_viewport_registers(&dynamic->vp.viewports[0], &dynamic->vp.scissors[0],
                            cx + target_words + msaa_count + depth_count + stencil_count);
   memcpy(cx + fixed, stage + PS5VK_STAGE_CONTEXT_OFFSET,
          PS5VK_STAGE_CONTEXT_RECORDS * sizeof(*cx));
   memcpy(cx + fixed + PS5VK_STAGE_CONTEXT_RECORDS, vertex->cx, vertex->cx_count * sizeof(*cx));
   memcpy(cx + fixed + PS5VK_STAGE_CONTEXT_RECORDS + vertex->cx_count, pixel->cx,
          pixel->cx_count * sizeof(*cx));
   if (mask_count != 0) {
      struct ps5vk_agc_register *const masks =
         cx + fixed + PS5VK_STAGE_CONTEXT_RECORDS + vertex->cx_count + pixel->cx_count;
      masks[0] = (struct ps5vk_agc_register){.offset = 0x08e,
                                             .value = pipeline->colour_write_mask};
      masks[1] = (struct ps5vk_agc_register){.offset = 0x08f,
                                             .value = pipeline->colour_write_mask};
   }
   if (blend_count != 0) {
      struct ps5vk_agc_register *const blend =
         cx + fixed + PS5VK_STAGE_CONTEXT_RECORDS + vertex->cx_count + pixel->cx_count + mask_count;
      blend[0] = (struct ps5vk_agc_register){.offset = PS5VK_BLEND_CONTROL_REGISTER,
                                             .value = pipeline->blend_control};
      blend[1] = (struct ps5vk_agc_register){.offset = PS5VK_COLOR_CONTROL_REGISTER,
                                             .value = PS5VK_COLOR_CONTROL_WORD};
      for (uint32_t index = 0; index < blend_constant_count; index++)
         blend[PS5VK_BLEND_REGISTER_COUNT + index] =
            (struct ps5vk_agc_register){.offset = (uint16_t)(PS5VK_BLEND_CONSTANT_REGISTER + index),
                                        .value = pipeline->blend_constants[index]};
   }
   if (raster_count != 0) {
      struct ps5vk_agc_register *raster =
         cx + fixed + PS5VK_STAGE_CONTEXT_RECORDS + vertex->cx_count + pixel->cx_count +
         mask_count + blend_count;
      if (rasterizer_recorded)
         *raster++ = (struct ps5vk_agc_register){.offset = PS5VK_RASTERIZER_REGISTER,
                                                 .value = rasterizer_word};
      if (pipeline->discard_rasterizer)
         *raster++ = (struct ps5vk_agc_register){.offset = PS5VK_CLIP_CONTROL_REGISTER,
                                                 .value = PS5VK_CLIP_CONTROL_DISCARD};
      if (depth_bias_count != 0) {
         /* Vulkan's one bias is the front face's and the back face's alike, as
          * ps5-opengl's block writes it: the back pair mirrors the front's. */
         const uint32_t words[PS5VK_POLY_OFFSET_COUNT] = {
            PS5VK_POLY_OFFSET_DB_FMT_D32F,
            bias_clamp_word,
            bias_scale_word,
            bias_offset_word,
            bias_scale_word,
            bias_offset_word,
         };
         for (uint32_t index = 0; index < PS5VK_POLY_OFFSET_COUNT; index++)
            *raster++ = (struct ps5vk_agc_register){
               .offset = (uint16_t)(PS5VK_POLY_OFFSET_DB_FMT_REGISTER + index),
               .value = words[index]};
      }
      if (line_count != 0) {
         *raster++ = (struct ps5vk_agc_register){.offset = PS5VK_GS_OUT_PRIM_TYPE_REGISTER,
                                                 .value = PS5VK_GS_OUT_PRIM_LINESTRIP};
         *raster++ = (struct ps5vk_agc_register){.offset = PS5VK_LINE_WIDTH_REGISTER,
                                                 .value = PS5VK_LINE_WIDTH_ONE};
         *raster++ = (struct ps5vk_agc_register){.offset = PS5VK_LINE_CONTROL_REGISTER,
                                                 .value = PS5VK_LINE_CONTROL_NON_STRICT};
      }
   }
   memcpy(sh, vertex->sh, vertex->sh_count * sizeof(*sh));
   memcpy(sh + vertex->sh_count, pixel->sh, pixel->sh_count * sizeof(*sh));
   ps5vk_flush_cpu_cache(cx, cx_count * sizeof(*cx));
   ps5vk_flush_cpu_cache(sh, sh_count * sizeof(*sh));

   /* Vulkan 1.0's robustBufferAccess (V0-robust): a draw whose count reaches
    * past the bound index buffer fetches no index outside it, so the count is
    * clamped to the indices the binding covers. Every frame before that probe
    * draws inside its bound, where the clamp changes nothing. */
   uint32_t draw_count = indexed != NULL ? indexed->index_count : vertex_count;
   if (indexed != NULL && cmd_buffer->index_buffer.size != 0) {
      const uint64_t bound = cmd_buffer->index_buffer.size / sizeof(uint16_t);
      const uint64_t covers =
         bound > indexed->first_index ? bound - indexed->first_index : 0;
      if (draw_count > covers)
         draw_count = (uint32_t)covers;
   }
   /* A count the bound covers none of draws nothing. */
   if (draw_count == 0)
      return;
   /* An indexed draw names the index buffer the application bound; a binding
    * that never happened leaves address 0. Only 16-bit indices have a
    * recorded stream to match (docs/M5_REFERENCE.md, C6). */
   if (indexed != NULL) {
      if (cmd_buffer->index_buffer.address == 0) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "an indexed draw needs an index buffer");
         return;
      }
      if (cmd_buffer->index_buffer.type != VK_INDEX_TYPE_UINT16) {
         ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                                 "32-bit indices need a runner probe (docs/M5_REFERENCE.md, C6)");
         return;
      }
   }

   uint32_t words[PS5VK_DRAW_MAX_WORDS];
   struct ps5vk_agc_command_buffer command = {
      .bottom = words,
      .top = words + PS5VK_DRAW_MAX_WORDS,
      .up = words,
      .down = words + PS5VK_DRAW_MAX_WORDS,
      .callback = (uintptr_t)ps5vk_agc_out_of_space,
   };
   /* A draw that samples a target an earlier draw in this command buffer
    * rendered into splits the submission where it starts, so the queue submits
    * the words before it, waits for the step's colour flush to complete, and
    * only then runs this draw (ps5vk_cmd_buffer_split, ps5vk_queue.c): the
    * barrier below flushes the colour buffers, but a flush is not a wait, and
    * without the wait the draws behind it fetch the image's most recently
    * written rows before they have drained (docs/M5_PHASE_C.md, C4). */
   if (colour_barrier && !ps5vk_cmd_buffer_split(cmd_buffer))
      return;
   /* The order the recorded streams use: the three register tables, both
    * stages' user data, then the draw. */
   bool encoded =
      (!colour_barrier ||
       sceAgcCbReleaseMem(&command, PS5VK_COLOUR_BARRIER_EVENT, PS5VK_COLOUR_BARRIER_CONTROL, 1,
                          0, NULL, 0, 0, 0, 1, 0, 0) != NULL) &&
      sceAgcDcbSetCxRegistersIndirect(&command, cx, cx_count) &&
      sceAgcDcbSetUcRegistersIndirect(&command, stage + PS5VK_STAGE_UNIFORM_OFFSET,
                                      PS5VK_STAGE_UNIFORM_RECORDS) &&
      sceAgcDcbSetShRegistersIndirect(&command, sh, sh_count);
   for (uint32_t s = 0; encoded && s < PS5VK_PIPELINE_STAGE_COUNT; s++) {
      if (user_data_count[s] == 0)
         continue;
      const uint32_t offset = s == PS5VK_PIPELINE_STAGE_VERTEX ? PS5VK_VERTEX_USER_DATA_OFFSET
                                                               : PS5VK_PIXEL_USER_DATA_OFFSET;
      encoded = sceAgcCbSetShRegisterRangeDirect(&command, offset, user_data[s],
                                                 user_data_count[s]) != NULL;
   }
   /* An instanced draw asks the hardware for the count with the AGC library's
    * own packet and puts it back to one after the draw, which is what
    * ps5-opengl's runtime does around one (ps5_agc_set_instances, Phase C2's
    * instancing probe). The count has to be the packet immediately before the
    * draw, because it is the draw that reads it: a count programmed earlier is
    * overwritten by the reset the draw before it left behind, which is what
    * console run 17 did -- three instances asked for, one drawn
    * (docs/M5_PHASE_C.md). */
   if (instance_count != 1 && encoded &&
       sceAgcDcbSetNumInstances(&command, instance_count) == NULL)
      encoded = false;
   if (indexed == NULL) {
      encoded = encoded && sceAgcDcbDrawIndexAuto(&command, draw_count, PS5VK_DRAW_AUTO_INDEX);
   } else {
      /* The address of the draw's first index: 16-bit indices are two bytes
       * each, and the runner records the byte address the same way
       * (src/diagnostics.cpp). */
      const uint64_t index_address =
         cmd_buffer->index_buffer.address + (uint64_t)indexed->first_index * 2;
      /* The index packets of the recorded stream, in its order: the index size
       * once, then the draw's INDEX_BASE, INDEX_BUFFER_SIZE and DRAW_INDEX_2
       * (golden/runner/m3-vertex-1.json). */
      encoded = encoded &&
                (!cmd_buffer->index_buffer.size_written
                    ? (cmd_buffer->index_buffer.size_written = true,
                       sceAgcDcbSetIndexSize(&command, 0, 0) != NULL)
                    : true) &&
                sceAgcDcbSetIndexBuffer(&command, (void *)(uintptr_t)index_address) != NULL &&
                sceAgcDcbSetIndexCount(&command, draw_count) != NULL &&
                sceAgcDcbDrawIndex(&command, draw_count,
                                   (void *)(uintptr_t)index_address, 0) != NULL;
   }
   /* The draw has read the count; the next draw runs one instance unless it
    * asks for its own. */
   if (instance_count != 1 && encoded && sceAgcDcbSetNumInstances(&command, 1) == NULL)
      encoded = false;
   const uint32_t draw_words =
      encoded ? (uint32_t)(command.up - command.bottom) : 0;
   if (!encoded || draw_words > PS5VK_DRAW_MAX_WORDS) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "the AGC helpers did not encode the draw in %u words",
                              PS5VK_DRAW_MAX_WORDS);
      return;
   }
   uint32_t *const recorded = util_dynarray_grow(&cmd_buffer->words, uint32_t, draw_words);
   if (!recorded) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY, "no memory for a draw");
      return;
   }
   memcpy(recorded, words, draw_words * sizeof(*words));
}

/* A non-indexed draw: the DRAW_INDEX_AUTO the recorded frames use. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
              uint32_t firstVertex, uint32_t firstInstance)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   ps5vk_cmd_draw(cmd_buffer, vertexCount, instanceCount, firstVertex, firstInstance, NULL);
}

/* The base vertex is what the vertex stage's ABI reads (ps5vk_cmd_draw), so a
 * vertex offset other than 0 is refused there until a runner probe records
 * one (docs/M5_REFERENCE.md, C2). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
                     uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   const struct ps5vk_indexed_draw indexed = {indexCount, firstIndex, vertexOffset};
   ps5vk_cmd_draw(cmd_buffer, indexCount, instanceCount, (uint32_t)vertexOffset, firstInstance,
                  &indexed);
}

/* vkCmdBindIndexBuffer and vkCmdBindIndexBuffer2KHR reach this through the
 * runtime's common implementations, which turn the buffer, offset and size
 * into the address range the draw's INDEX_BASE packet needs; a NULL buffer
 * gives address 0, which ps5vk_cmd_draw refuses. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdBindIndexBuffer3KHR(VkCommandBuffer commandBuffer, const VkBindIndexBuffer3InfoKHR *pInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   cmd_buffer->index_buffer.address = pInfo->addressRange.address;
   cmd_buffer->index_buffer.size = pInfo->addressRange.size;
   cmd_buffer->index_buffer.type = pInfo->indexType;
}

/* Mesa's vk_meta draws the colour clears this driver cannot encode itself
 * (docs/M5_PHASE_C.md, C1b). It builds its rectangle vertex buffer through the
 * driver: the buffer is created by vk_meta_create_buffer, and this gives it
 * the address and the mapping it writes the vertices into. The command
 * buffer's GPU-visible chunks already are CPU- and GPU-visible memory that
 * lives until the command buffer is reset, which is what a meta draw needs. */
static VkResult
ps5vk_meta_bind_map_buffer(struct vk_command_buffer *cmd, struct vk_meta_device *meta,
                           VkBuffer _buffer, void **map_out)
{
   struct ps5vk_cmd_buffer *const cmd_buffer =
      container_of(cmd, struct ps5vk_cmd_buffer, vk);
   VK_FROM_HANDLE(ps5vk_buffer, buffer, _buffer);
   (void)meta;

   void *const map =
      ps5vk_cmd_buffer_table(cmd_buffer, buffer->vk.size, PS5VK_BUFFER_ALIGNMENT);
   if (!map)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   buffer->vk.device_address = (uint64_t)(uintptr_t)map;
   assert(ps5vk_address_range_valid(buffer->vk.device_address, buffer->vk.size));
   *map_out = map;
   return VK_SUCCESS;
}

VkResult
ps5vk_meta_init(struct ps5vk_device *device)
{
   VkResult result = vk_meta_device_init(&device->vk, &device->meta);
   if (result != VK_SUCCESS)
      return result;
   /* The flags C1b question 2 settled: vk_meta supplies the rectangle vertex
    * shader and its vertex input, and this driver has no geometry stage, no
    * stencil export and no layered rendering to give it. */
   device->meta.use_rect_list_pipeline = true;
   device->meta.use_gs_for_layer = false;
   device->meta.use_stencil_export = false;
   device->meta.use_layered_rendering = false;
   device->meta.cmd_bind_map_buffer = ps5vk_meta_bind_map_buffer;
   /* Three quarters of a table chunk: vk_meta_draw_rects divides this by the
    * 72 bytes a rectangle's vertices need but writes 96, so a smaller limit
    * would let one batch outgrow the chunk it suballocates from. */
   device->meta.max_bind_map_buffer_size_B = PS5VK_TABLE_CHUNK_BYTES / 4 * 3;
   device->meta_initialized = true;
   return VK_SUCCESS;
}

void
ps5vk_meta_finish(struct ps5vk_device *device)
{
   if (!device->meta_initialized)
      return;
   vk_meta_device_finish(&device->vk, &device->meta);
   device->meta_initialized = false;
}

/* Mesa's common viewport and scissor commands write into the command buffer's
 * dynamic state, which the draw reads, so the driver adds nothing to them. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
                     const VkViewport *pViewports)
{
   vk_common_CmdSetViewport(commandBuffer, firstViewport, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount,
                    const VkRect2D *pScissors)
{
   vk_common_CmdSetScissor(commandBuffer, firstScissor, scissorCount, pScissors);
}

/* Records one binding of a vertex-buffer bind: the address a draw's
 * vertex-buffer table names, and the bytes of it that exist. */
static void
ps5vk_cmd_bind_vertex_buffer(struct ps5vk_cmd_buffer *cmd_buffer, uint32_t binding,
                             VkBuffer _buffer, VkDeviceSize offset, VkDeviceSize size)
{
   VK_FROM_HANDLE(ps5vk_buffer, buffer, _buffer);
   if (binding >= PS5VK_MAX_VERTEX_BINDINGS) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "vertex binding %u is beyond the %u this driver supports", binding,
                              PS5VK_MAX_VERTEX_BINDINGS);
      return;
   }
   if (!buffer) {
      cmd_buffer->vertex_buffers[binding] = (struct ps5vk_vertex_buffer){0};
      return;
   }
   assert(offset <= buffer->vk.size);
   const VkDeviceSize bytes =
      size == VK_WHOLE_SIZE ? buffer->vk.size - offset : MIN2(size, buffer->vk.size - offset);
   cmd_buffer->vertex_buffers[binding] =
      (struct ps5vk_vertex_buffer){buffer->vk.device_address + offset, bytes};
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                           uint32_t bindingCount, const VkBuffer *pBuffers,
                           const VkDeviceSize *pOffsets)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   for (uint32_t i = 0; i < bindingCount; i++)
      ps5vk_cmd_bind_vertex_buffer(cmd_buffer, firstBinding + i, pBuffers[i],
                                   pOffsets ? pOffsets[i] : 0, VK_WHOLE_SIZE);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                            uint32_t bindingCount, const VkBuffer *pBuffers,
                            const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes,
                            const VkDeviceSize *pStrides)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   (void)pStrides;
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   for (uint32_t i = 0; i < bindingCount; i++)
      ps5vk_cmd_bind_vertex_buffer(cmd_buffer, firstBinding + i, pBuffers[i],
                                   pOffsets ? pOffsets[i] : 0,
                                   pSizes ? pSizes[i] : VK_WHOLE_SIZE);
}

/* Push constants are not a register write here: the draw copies these bytes
 * into a uniform buffer whose descriptor it puts in the set-0 table
 * (docs/M5_PHASE_C.md, C1b question 1). Recording them is therefore a memcpy,
 * and a stage that reads them is one the NIR rewrite gave the reserved
 * binding to. */
static void
ps5vk_cmd_push_constants(struct ps5vk_cmd_buffer *cmd_buffer, uint32_t offset, uint32_t size,
                         const void *values)
{
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || size == 0)
      return;
   if (offset + size > PS5VK_MAX_PUSH_CONSTANT_BYTES) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "push constants at offset %u for %u bytes exceed the %u this driver "
                              "supports",
                              offset, size, PS5VK_MAX_PUSH_CONSTANT_BYTES);
      return;
   }
   memcpy(cmd_buffer->push_constants + offset, values, size);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                       VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size,
                       const void *pValues)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   (void)layout;
   (void)stageFlags;
   ps5vk_cmd_push_constants(cmd_buffer, offset, size, pValues);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdPushConstants2(VkCommandBuffer commandBuffer,
                        const VkPushConstantsInfo *pPushConstantsInfo)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   ps5vk_cmd_push_constants(cmd_buffer, pPushConstantsInfo->offset, pPushConstantsInfo->size,
                            pPushConstantsInfo->pValues);
}

/* vkCmdClearAttachments clears arbitrary rectangles of the attachment a
 * rendering bound, which are the same vk_meta draw the load-op clear is
 * (vk_meta_clear_rendering). Only the application's own state has to survive
 * it. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
                          const VkClearAttachment *pAttachments, uint32_t rectCount,
                          const VkClearRect *pRects)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   struct ps5vk_device *const device =
      container_of(cmd_buffer->vk.base.device, struct ps5vk_device, vk);
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || rectCount == 0)
      return;
   /* Valid usage: inside a rendering. */
   assert(cmd_buffer->rendering);

   struct ps5vk_meta_saved_state saved;
   ps5vk_meta_save(cmd_buffer, &saved);
   vk_meta_clear_attachments(&cmd_buffer->vk, &device->meta, &cmd_buffer->render, attachmentCount,
                             pAttachments, rectCount, pRects);
   ps5vk_meta_restore(cmd_buffer, &saved);
}

/* ---------------- indirect draws (1.0's vkCmdDrawIndirect) ---------------- */

/* Whether this command buffer's own recorded work writes the bytes an indirect
 * draw reads its parameters from. The parameters are read when the draw is
 * recorded, which is what a CPU-side driver can do, and Vulkan says they are
 * read at execution time: a write this command buffer records would land after
 * the read, so such a draw is refused by name rather than recorded with stale
 * parameters. A write in an earlier submission has already run -- this driver's
 * submissions are synchronous -- and one recorded after the draw does not
 * affect it, so only the records made so far matter. */
bool
ps5vk_cmd_buffer_writes_range(const struct ps5vk_cmd_buffer *cmd_buffer, uint64_t address,
                              uint64_t bytes)
{
   util_dynarray_foreach (&cmd_buffer->copies, struct ps5vk_memory_copy, copy) {
      /* A blit or a clear writes an image, never a buffer's bytes. */
      if (copy->blit || copy->clear || copy->bytes == 0)
         continue;
      if (address < copy->destination + copy->bytes && copy->destination < address + bytes)
         return true;
   }
   return false;
}

/* The indirect draw's parameters, read from the buffer the application bound.
 * A buffer is host memory here, so the read is a memcpy; the two structures are
 * Vulkan's own (VkDrawIndirectCommand and VkDrawIndexedIndirectCommand). */
static void
ps5vk_cmd_draw_indirect(struct ps5vk_cmd_buffer *cmd_buffer, VkBuffer _buffer, VkDeviceSize offset,
                        uint32_t draw_count, uint32_t stride, bool indexed)
{
   VK_FROM_HANDLE(ps5vk_buffer, buffer, _buffer);
   const uint32_t command_bytes = indexed ? 20u : 16u;
   /* Vulkan ignores stride for a single draw (vkQuake passes zero).
    * Only a multi-draw array needs aligned, non-overlapping records. */
   assert(buffer != NULL);
   assert(draw_count <= 1 || ((stride % 4) == 0 && stride >= command_bytes));
   if (buffer->vk.device_address == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "an indirect draw names a buffer with no GPU address: it has to be "
                              "bound to memory");
      return;
   }
   const VkDeviceSize last_offset = offset + (VkDeviceSize)(draw_count - 1u) * stride;
   assert(last_offset + command_bytes <= buffer->vk.size);
   const uint64_t first_address = buffer->vk.device_address + offset;
   if (ps5vk_cmd_buffer_writes_range(cmd_buffer, first_address,
                                     (uint64_t)(draw_count - 1u) * stride + command_bytes)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "an indirect draw whose parameters this command buffer writes; the "
                              "parameters are read when the draw is recorded, and a split-point "
                              "read is a later step (docs/M5_REFERENCE.md)");
      return;
   }
   ps5vk_flush_cpu_cache((const void *)(uintptr_t)first_address,
                         (size_t)((uint64_t)(draw_count - 1u) * stride + command_bytes));
   for (uint32_t draw = 0; draw < draw_count; draw++) {
      const uint8_t *const command =
         (const uint8_t *)(uintptr_t)(first_address + (uint64_t)draw * stride);
      if (indexed) {
         uint32_t fields[5] = {0};
         memcpy(fields, command, sizeof(fields));
         const struct ps5vk_indexed_draw indexed_draw = {fields[0], fields[2], (int32_t)fields[3]};
         ps5vk_cmd_draw(cmd_buffer, fields[0], fields[1], 0, fields[4], &indexed_draw);
      } else {
         uint32_t fields[4] = {0};
         memcpy(fields, command, sizeof(fields));
         ps5vk_cmd_draw(cmd_buffer, fields[0], fields[1], fields[2], fields[3], NULL);
      }
      /* A refusal ends the recording: the rest of the draws would be recorded
       * into a command buffer that has already failed. */
      if (vk_command_buffer_has_error(&cmd_buffer->vk))
         return;
   }
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                      uint32_t drawCount, uint32_t stride)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || drawCount == 0)
      return;
   ps5vk_cmd_draw_indirect(cmd_buffer, buffer, offset, drawCount, stride, false);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                             uint32_t drawCount, uint32_t stride)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || drawCount == 0)
      return;
   ps5vk_cmd_draw_indirect(cmd_buffer, buffer, offset, drawCount, stride, true);
}
