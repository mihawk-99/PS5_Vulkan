/*
 * PS5 Vulkan driver - queue submission.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B5 (docs/M5_PHASE_B.md). Submission is synchronous, in
 * Mesa's immediate mode (the only sync type has no timeline):
 * 1. wait for the submission's input syncs (semaphores);
 * 2. copy the command buffers' PM4 words into the queue's GPU-visible
 *    submission buffer;
 * 3. end the stream as ps5-opengl ends a frame it does not present: the
 *    colour-buffer barrier, then a completion marker that has a value unique
 *    to this submission written into the buffer's last word (Phase B4);
 * 4. submit with sceAgcDriverSubmitDcb, pass sceAgcSuspendPoint and poll the
 *    marker as the test runner does: flush its cache line, compare, sleep
 *    1 ms, for up to 2 s;
 * 5. signal the output syncs (fences and semaphores).
 * A command buffer's copies (vkCmdCopyBuffer, Phase C2, ps5vk_cmd_buffer.c) are
 * CPU memcpys: memory is shared, so the submission is split at each copy -- the
 * words before it run and complete, the CPU copies, and the words after it run
 * next -- which keeps Vulkan's command order. A draw that samples a target an
 * earlier draw in the same command buffer rendered into splits the submission
 * the same way, with no copy to run: the colour flush such a draw carries is
 * not a wait, so the step boundary and its marker are what let the sample see
 * the render (ps5vk_draw.c, ps5vk_cmd_buffer_split).
 * CPU and GPU share memory, and the colour targets the command buffers render
 * to stay mapped for the application, so their CPU cache lines are evicted
 * before submission, keeping a CPU write from reaching memory after the
 * GPU's, and again once the marker arrives, so the CPU reads what the GPU
 * wrote; the test runner flushes its targets the same way (flush_gpu_data).
 * A submission without command buffers has nothing for the GPU and only
 * signals. A marker that never arrives loses the device.
 *
 * Console runs (docs/M5_PHASE_B.md): a stream of only the barrier and marker
 * completes, so empty command buffers work (pid 107), and the marker arrives
 * after the last draw has rendered (pid 108, b5-order), so a fence signalled
 * here is safe for readback. The suspend point takes ~125 µs whatever the
 * stream holds; heavier work than seen so far may need later polls.
 *
 * Once vkQueueSubmit returns, the GPU no longer reads anything the
 * submission pointed at, so command buffers may reuse their register tables.
 *
 * Presentation (Phase C1, run pid 134): a submission rendering into a
 * swapchain image starts with that image's wait packet, and a present
 * submits the image's flip alone (ps5vk_queue_flip, called from
 * ps5vk_wsi.c).
 */

#include "ps5vk_private.h"
#include "ps5vk_debug.h"

#include "vk_format.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "util/os_time.h"

/* One 2 MiB GPU-visible buffer per queue; its last word is the marker. */
#define PS5VK_SUBMISSION_BYTES UINT64_C(0x200000)

/* ps5-opengl's colour-buffer barrier (event 45, control 12) and completion
 * marker (event 40, control 0x30c), as the test runner encodes them; each
 * packet is 8 words (golden/b4). */
#define PS5VK_BARRIER_EVENT 45
#define PS5VK_BARRIER_CONTROL 12
#define PS5VK_COMPLETION_EVENT 40
#define PS5VK_COMPLETION_CONTROL 0x30c
#define PS5VK_STREAM_END_WORDS 16

/* The test runner's marker wait: 1 ms sleeps for up to 2 s. */
#define PS5VK_MARKER_POLLS 2000
#define PS5VK_MARKER_POLL_MICROSECONDS 1000

uint8_t
ps5vk_agc_out_of_space(struct ps5vk_agc_command_buffer *buffer, uint32_t words, void *user_data)
{
   (void)buffer;
   (void)words;
   (void)user_data;
   return 0;
}

/* Evicts the CPU cache lines of every colour target of a submission. */
static void
ps5vk_queue_flush_targets(const struct vk_queue_submit *submit)
{
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      const struct ps5vk_cmd_buffer *const cmd_buffer =
         container_of(submit->command_buffers[i], struct ps5vk_cmd_buffer, vk);
      util_dynarray_foreach (&cmd_buffer->targets, struct ps5vk_render_target, target)
         ps5vk_flush_cpu_cache(target->address, target->bytes);
   }
}

/* One split of a submission and the stream offset its step ends at: the words
 * before that offset are submitted and waited for before the CPU copies, and
 * the words after it are submitted afterwards (ps5vk_queue_run). The copies a
 * command buffer recorded at one position share an offset, and they run in
 * record order; a record with no copy is a synchronization split, which the
 * wait alone does the work of (ps5vk_cmd_buffer_split). */
struct ps5vk_copy_split {
   uint32_t offset;
   const struct ps5vk_memory_copy *copy;
};

/* One source texel decoded to the RGBA8 bytes a UNORM destination takes: what
 * the sampler's fetch would return (Vulkan's fill-in rule for the channels a
 * format does not have, which V0-formats measured) converted to eight-bit
 * UNORM. An sRGB format's value is linearised, a SNORM one mapped from its
 * signed range, a float one clamped; every format is one ps5vk_image.c's table
 * reports a descriptor word for, and the recording refuses the rest by name
 * before a blit is ever recorded. */
static float
ps5vk_half_to_float(uint16_t half)
{
   const uint32_t sign = (uint32_t)(half >> 15) << 31;
   const uint32_t exponent = (half >> 10) & 0x1fu;
   const uint32_t mantissa = half & 0x3ffu;
   uint32_t bits;
   if (exponent == 0)
      bits = sign; /* A subnormal half is far below an eight-bit level. */
   else if (exponent == 31)
      bits = sign | 0x7f800000u; /* An infinity or a NaN clamps either way. */
   else
      bits = sign | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
   float value;
   memcpy(&value, &bits, sizeof(value));
   return value;
}

static uint8_t
ps5vk_unorm8(float value)
{
   return (uint8_t)CLAMP((int32_t)floorf(CLAMP(value, 0.0f, 1.0f) * 255.0f + 0.5f), 0, 255);
}

/* The little-endian word a packed texel's bytes hold. */
static uint32_t
ps5vk_packed_word(const uint8_t *texel, unsigned bytes)
{
   uint32_t word = 0;
   for (unsigned byte = 0; byte < bytes; byte++)
      word |= (uint32_t)texel[byte] << (8u * byte);
   return word;
}

/* An 11- or 10-bit float channel: five exponent bits over mantissa_bits, with
 * the implicit one, which is the specification's rule (V0-formats). */
static float
ps5vk_float_channel_to_float(uint32_t raw, unsigned mantissa_bits)
{
   const uint32_t exponent = raw >> mantissa_bits;
   const uint32_t mantissa = raw & ((1u << mantissa_bits) - 1u);
   if (exponent == 0)
      return 0.0f;
   return (1.0f + (float)mantissa / (float)(1u << mantissa_bits)) *
          ldexpf(1.0f, (int)exponent - 15);
}

static float
ps5vk_sfloat32(const uint8_t *bytes)
{
   float value;
   memcpy(&value, bytes, sizeof(value));
   return value;
}

/* One integer component as eight-bit UNORM: an unsigned value scaled by the
 * range its width has (0x40 of a byte stays 0x40, 0x4000 of a short and
 * 0x40000000 of a word become 0x40 too), and a signed one through the same
 * scale after its negative half is clamped to zero, which is the rule the
 * console's own SNORM and sRGB blits follow for a value below zero. */
static uint8_t
ps5vk_integer8(uint32_t raw, unsigned bits, bool is_signed)
{
   if (is_signed) {
      const int32_t value = (int32_t)(raw << (32u - bits)) >> (32u - bits);
      if (value <= 0)
         return 0;
      return ps5vk_unorm8((float)value / (float)((UINT32_C(1) << (bits - 1u)) - 1u));
   }
   const uint32_t max = bits == 32 ? UINT32_MAX : (UINT32_C(1) << bits) - 1u;
   return ps5vk_unorm8((float)raw / (float)max);
}

/* An integer family's texel: channels components of bits bits, little endian,
 * red first. A channel the format does not have reads zero and alpha reads
 * opaque, which is Vulkan's fill rule for a fetch and the rule the console's
 * vertex probe measured (docs/HARDWARE_FINDINGS.md). */
static bool
ps5vk_integer_to_rgba8(const uint8_t *texel, unsigned bits, unsigned channels, bool is_signed,
                       uint8_t rgba[4])
{
   const unsigned bytes = bits / 8u;
   rgba[0] = rgba[1] = rgba[2] = 0;
   rgba[3] = 255;
   for (unsigned channel = 0; channel < channels; channel++) {
      uint32_t raw = 0;
      memcpy(&raw, texel + channel * bytes, bytes);
      rgba[channel] = ps5vk_integer8(raw, bits, is_signed);
   }
   return true;
}

bool
ps5vk_texel_to_rgba8(VkFormat format, const uint8_t *texel, uint8_t rgba[4])
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
      memcpy(rgba, texel, 4);
      return true;
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
      rgba[0] = texel[3];
      rgba[1] = texel[2];
      rgba[2] = texel[1];
      rgba[3] = texel[0];
      return true;
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_B8G8R8A8_SRGB: {
      /* The byte-reversed B8G8R8A8 form holds its channels R last; the packed
       * A8B8G8R8 form's image memory is the R, G, B, A order its twin has
       * (ps5vk_format.storage_reversed), so it decodes like the identity row. */
      const bool blue_first = format == VK_FORMAT_B8G8R8A8_SRGB;
      for (unsigned channel = 0; channel < 3; channel++) {
         const uint8_t stored = texel[blue_first ? 2u - channel : channel];
         const float value = (float)stored / 255.0f;
         const float linear =
            value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
         rgba[channel] = ps5vk_unorm8(linear);
      }
      rgba[3] = texel[3];
      return true;
   }
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_A8B8G8R8_SNORM_PACK32: {
      const bool packed = format == VK_FORMAT_A8B8G8R8_SNORM_PACK32;
      /* An SNORM texel's value is its signed byte over 127, and the UNORM
       * destination clamps the negative half to zero -- which is what the
       * console's own fetch showed: a 0x20 byte (0.252) lands at 0x40. */
      for (unsigned channel = 0; channel < 3; channel++) {
         const int8_t stored = (int8_t)texel[packed ? 3u - channel : channel];
         rgba[channel] = ps5vk_unorm8((float)stored / 127.0f);
      }
      rgba[3] = packed ? ps5vk_unorm8((float)(int8_t)texel[0] / 127.0f) : 255;
      return true;
   }
   case VK_FORMAT_R8_UNORM:
      rgba[0] = texel[0];
      rgba[1] = 0;
      rgba[2] = 0;
      rgba[3] = 255;
      return true;
   case VK_FORMAT_R8G8_UNORM:
      rgba[0] = texel[0];
      rgba[1] = texel[1];
      rgba[2] = 0;
      rgba[3] = 255;
      return true;
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16G16_UNORM: {
      const uint16_t *const values = (const uint16_t *)texel;
      const unsigned channels = format == VK_FORMAT_R16G16_UNORM ? 2u : 1u;
      for (unsigned channel = 0; channel < 4; channel++)
         rgba[channel] = channel < channels ? (uint8_t)(values[channel] / 257u)
                                            : (channel == 3 ? 255 : 0);
      return true;
   }
   case VK_FORMAT_R16G16B16A16_UNORM: {
      const uint16_t *const values = (const uint16_t *)texel;
      for (unsigned channel = 0; channel < 4; channel++)
         rgba[channel] = (uint8_t)(values[channel] / 257u);
      return true;
   }
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_SFLOAT: {
      const uint16_t *const values = (const uint16_t *)texel;
      const unsigned channels = format == VK_FORMAT_R16G16_SFLOAT ? 2u : 1u;
      for (unsigned channel = 0; channel < 4; channel++)
         rgba[channel] = channel < channels ? ps5vk_unorm8(ps5vk_half_to_float(values[channel]))
                                            : (channel == 3 ? 255 : 0);
      return true;
   }
   case VK_FORMAT_R16G16B16A16_SFLOAT: {
      const uint16_t *const values = (const uint16_t *)texel;
      for (unsigned channel = 0; channel < 4; channel++)
         rgba[channel] = ps5vk_unorm8(ps5vk_half_to_float(values[channel]));
      return true;
   }
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_SFLOAT: {
      const unsigned channels = format == VK_FORMAT_R32G32_SFLOAT ? 2u : 1u;
      for (unsigned channel = 0; channel < 4; channel++)
         rgba[channel] = channel < channels ? ps5vk_unorm8(ps5vk_sfloat32(texel + channel * 4u))
                                            : (channel == 3 ? 255 : 0);
      return true;
   }
   /* V0-formats' packed families: components that are not byte-aligned, so the
    * decode is per format rather than a copy. The rules are the specification's
    * and the sampler probe's console frames are what they are read against
    * (run_vulkan_format_sample_frames); tools/packed-format-check.py decodes the
    * same texels on the PC, and the blit case's readback is the colour both
    * name (run_vulkan_blit_format_frames). */
   case VK_FORMAT_R5G6B5_UNORM_PACK16: {
      const uint32_t word = ps5vk_packed_word(texel, 2);
      rgba[0] = ps5vk_unorm8((float)((word >> 11) & 0x1fu) / 31.0f);
      rgba[1] = ps5vk_unorm8((float)((word >> 5) & 0x3fu) / 63.0f);
      rgba[2] = ps5vk_unorm8((float)(word & 0x1fu) / 31.0f);
      rgba[3] = 255;
      return true;
   }
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16: {
      const uint32_t word = ps5vk_packed_word(texel, 2);
      rgba[0] = ps5vk_unorm8((float)((word >> 10) & 0x1fu) / 31.0f);
      rgba[1] = ps5vk_unorm8((float)((word >> 5) & 0x1fu) / 31.0f);
      rgba[2] = ps5vk_unorm8((float)(word & 0x1fu) / 31.0f);
      rgba[3] = (word & 0x8000u) != 0 ? 255 : 0;
      return true;
   }
   case VK_FORMAT_B4G4R4A4_UNORM_PACK16: {
      const uint32_t word = ps5vk_packed_word(texel, 2);
      rgba[0] = ps5vk_unorm8((float)((word >> 4) & 0xfu) / 15.0f);
      rgba[1] = ps5vk_unorm8((float)((word >> 8) & 0xfu) / 15.0f);
      rgba[2] = ps5vk_unorm8((float)((word >> 12) & 0xfu) / 15.0f);
      rgba[3] = ps5vk_unorm8((float)(word & 0xfu) / 15.0f);
      return true;
   }
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
      const uint32_t word = ps5vk_packed_word(texel, 4);
      rgba[0] = ps5vk_unorm8((float)(word & 0x3ffu) / 1023.0f);
      rgba[1] = ps5vk_unorm8((float)((word >> 10) & 0x3ffu) / 1023.0f);
      rgba[2] = ps5vk_unorm8((float)((word >> 20) & 0x3ffu) / 1023.0f);
      rgba[3] = ps5vk_unorm8((float)((word >> 30) & 0x3u) / 3.0f);
      return true;
   }
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: {
      const uint32_t word = ps5vk_packed_word(texel, 4);
      const float scale = ldexpf(1.0f, (int)((word >> 27) & 0x1fu) - 15) / 512.0f;
      rgba[0] = ps5vk_unorm8((float)(word & 0x1ffu) * scale);
      rgba[1] = ps5vk_unorm8((float)((word >> 9) & 0x1ffu) * scale);
      rgba[2] = ps5vk_unorm8((float)((word >> 18) & 0x1ffu) * scale);
      rgba[3] = 255;
      return true;
   }
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
      const uint32_t word = ps5vk_packed_word(texel, 4);
      rgba[0] = ps5vk_unorm8(ps5vk_float_channel_to_float(word & 0x7ffu, 6));
      rgba[1] = ps5vk_unorm8(ps5vk_float_channel_to_float((word >> 11) & 0x7ffu, 6));
      rgba[2] = ps5vk_unorm8(ps5vk_float_channel_to_float((word >> 22) & 0x3ffu, 5));
      rgba[3] = 255;
      return true;
   }
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      for (unsigned channel = 0; channel < 4; channel++)
         rgba[channel] = ps5vk_unorm8(ps5vk_sfloat32(texel + channel * 4u));
      return true;
   /* The integer families the audit lists for BLIT_SRC: the same rule the
    * sampled integer probes read, scaled to the eight-bit destination. */
   case VK_FORMAT_R8_UINT:
      return ps5vk_integer_to_rgba8(texel, 8, 1, false, rgba);
   case VK_FORMAT_R8_SINT:
      return ps5vk_integer_to_rgba8(texel, 8, 1, true, rgba);
   case VK_FORMAT_R8G8_UINT:
      return ps5vk_integer_to_rgba8(texel, 8, 2, false, rgba);
   case VK_FORMAT_R8G8_SINT:
      return ps5vk_integer_to_rgba8(texel, 8, 2, true, rgba);
   case VK_FORMAT_R8G8B8A8_UINT:
      return ps5vk_integer_to_rgba8(texel, 8, 4, false, rgba);
   case VK_FORMAT_R8G8B8A8_SINT:
      return ps5vk_integer_to_rgba8(texel, 8, 4, true, rgba);
   case VK_FORMAT_A8B8G8R8_UINT_PACK32:
      return ps5vk_integer_to_rgba8(texel, 8, 4, false, rgba);
   case VK_FORMAT_A8B8G8R8_SINT_PACK32:
      return ps5vk_integer_to_rgba8(texel, 8, 4, true, rgba);
   case VK_FORMAT_R16_UINT:
      return ps5vk_integer_to_rgba8(texel, 16, 1, false, rgba);
   case VK_FORMAT_R16_SINT:
      return ps5vk_integer_to_rgba8(texel, 16, 1, true, rgba);
   case VK_FORMAT_R16G16_UINT:
      return ps5vk_integer_to_rgba8(texel, 16, 2, false, rgba);
   case VK_FORMAT_R16G16_SINT:
      return ps5vk_integer_to_rgba8(texel, 16, 2, true, rgba);
   case VK_FORMAT_R16G16B16A16_UINT:
      return ps5vk_integer_to_rgba8(texel, 16, 4, false, rgba);
   case VK_FORMAT_R16G16B16A16_SINT:
      return ps5vk_integer_to_rgba8(texel, 16, 4, true, rgba);
   case VK_FORMAT_R32_UINT:
      return ps5vk_integer_to_rgba8(texel, 32, 1, false, rgba);
   case VK_FORMAT_R32_SINT:
      return ps5vk_integer_to_rgba8(texel, 32, 1, true, rgba);
   case VK_FORMAT_R32G32_UINT:
      return ps5vk_integer_to_rgba8(texel, 32, 2, false, rgba);
   case VK_FORMAT_R32G32_SINT:
      return ps5vk_integer_to_rgba8(texel, 32, 2, true, rgba);
   case VK_FORMAT_R32G32B32A32_UINT:
      return ps5vk_integer_to_rgba8(texel, 32, 4, false, rgba);
   case VK_FORMAT_R32G32B32A32_SINT:
      return ps5vk_integer_to_rgba8(texel, 32, 4, true, rgba);
   case VK_FORMAT_A2B10G10R10_UINT_PACK32: {
      uint32_t word = 0;
      memcpy(&word, texel, sizeof(word));
      rgba[0] = ps5vk_integer8(word & 0x3ffu, 10, false);
      rgba[1] = ps5vk_integer8((word >> 10) & 0x3ffu, 10, false);
      rgba[2] = ps5vk_integer8((word >> 20) & 0x3ffu, 10, false);
      rgba[3] = ps5vk_integer8(word >> 30, 2, false);
      return true;
   }
   /* The one- and two-channel signed-normalised forms, and the byte order
    * VideoOut scans out: B first, so red is the third byte. */
   case VK_FORMAT_R8_SNORM:
      rgba[0] = ps5vk_unorm8((float)(int8_t)texel[0] / 127.0f);
      rgba[1] = rgba[2] = 0;
      rgba[3] = 255;
      return true;
   case VK_FORMAT_R8G8_SNORM:
      rgba[0] = ps5vk_unorm8((float)(int8_t)texel[0] / 127.0f);
      rgba[1] = ps5vk_unorm8((float)(int8_t)texel[1] / 127.0f);
      rgba[2] = 0;
      rgba[3] = 255;
      return true;
   case VK_FORMAT_B8G8R8A8_UNORM:
      rgba[0] = texel[2];
      rgba[1] = texel[1];
      rgba[2] = texel[0];
      rgba[3] = texel[3];
      return true;
   default:
      return false;
   }
}

/* One image clear, written on the CPU at the split point that recorded it: the
 * encoded texel over every texel of the region, through the destination side's
 * own map (the padded rows of a linear image, the measured tile map of a colour
 * attachment). The touched bytes are flushed once, over the range they span,
 * because the GPU reads what this wrote. */
void
ps5vk_clear_execute(const struct ps5vk_memory_copy *copy)
{
   const uint32_t texel_bytes = copy->destination_texel_bytes;
   /* A four-sample destination clears every sample: its texel is four samples in
    * four planes, each one a copy of the clear colour's four bytes (Phase C8). */
   const uint32_t samples = copy->destination_side.samples == 4 ? 4u : 1u;
   const uint32_t sample_bytes = texel_bytes / samples;
   uint64_t low = UINT64_MAX;
   uint64_t high = 0;
   for (uint32_t row = 0; row < copy->height; row++) {
      for (uint32_t column = 0; column < copy->width; column++) {
         for (uint32_t sample = 0; sample < samples; sample++) {
            const uint64_t at = ps5vk_image_copy_address(
               &copy->destination_side, (int32_t)(copy->destination_x + column),
               (int32_t)(copy->destination_y + row), sample_bytes, sample);
            memcpy((void *)(uintptr_t)at, copy->clear_texel, sample_bytes);
            low = MIN2(low, at);
            high = MAX2(high, at + sample_bytes);
         }
      }
   }
   if (low < high)
      ps5vk_flush_cpu_cache((const void *)(uintptr_t)low, (size_t)(high - low));
}

/* One resolve region: each destination texel takes the average of the four
 * sample words of its source texel. A four-sample texel's four samples are four
 * four-byte words in four 0x4000-byte planes, each at the texel's own swizzled
 * position (ps5vk_image.c, ps5vk_image_copy_address), which is what the console
 * measured; averaging is commutative, so which plane is which sample does not
 * matter here, only that the four are the four of that texel. */
static void
ps5vk_resolve_execute(const struct ps5vk_memory_copy *copy)
{
   const uint32_t sample_bytes = copy->source_texel_bytes / 4u;
   uint64_t source_low = UINT64_MAX;
   uint64_t source_high = 0;
   uint64_t destination_low = UINT64_MAX;
   uint64_t destination_high = 0;
   for (uint32_t row = 0; row < copy->height; row++) {
      for (uint32_t column = 0; column < copy->width; column++) {
         uint8_t resolved[4] = {0, 0, 0, 0};
         for (uint32_t byte = 0; byte < copy->destination_texel_bytes; byte++) {
            uint32_t total = 0;
            for (uint32_t sample = 0; sample < 4; sample++) {
               const uint64_t source_at =
                  ps5vk_image_copy_address(&copy->source_side, (int32_t)column, (int32_t)row,
                                           sample_bytes, sample);
               uint8_t value = 0;
               memcpy(&value, (const void *)(uintptr_t)(source_at + byte), sizeof(value));
               total += value;
               source_low = MIN2(source_low, source_at);
               source_high = MAX2(source_high, source_at + sample_bytes);
            }
            resolved[byte] = (uint8_t)((total + 2u) / 4u);
         }
         const uint64_t destination_at = ps5vk_image_copy_address(
            &copy->destination_side, (int32_t)(copy->destination_x + column),
            (int32_t)(copy->destination_y + row), copy->destination_texel_bytes, 0);
         memcpy((void *)(uintptr_t)destination_at, resolved, copy->destination_texel_bytes);
         destination_low = MIN2(destination_low, destination_at);
         destination_high = MAX2(destination_high, destination_at + copy->destination_texel_bytes);
      }
   }
   if (source_low < source_high)
      ps5vk_flush_cpu_cache((const void *)(uintptr_t)source_low, (size_t)(source_high - source_low));
   if (destination_low < destination_high)
      ps5vk_flush_cpu_cache((const void *)(uintptr_t)destination_low,
                            (size_t)(destination_high - destination_low));
}

/* One scaled blit region, resampled on the CPU at the split point that recorded
 * it: the destination's texels take the source texels their sample points name,
 * nearest or bilinear. Vulkan maps a destination texel's centre onto the source
 * rectangle's edges -- so a 2x upscale lands each source texel's centre on a
 * 2x2 block exactly -- and a sample outside the rectangle clamps to its edge,
 * which is the clamp-to-edge sampling the driver's descriptors use.
 *
 * The blend is on the texel bytes, which the recording has already restricted
 * to four-byte texels (and R8G8B8A8_UNORM for the filtered case,
 * ps5vk_image.c). The touched ranges are flushed around the write: the GPU
 * wrote the source and reads the destination. */
void
ps5vk_blit_execute(const struct ps5vk_memory_copy *copy)
{
   const uint32_t source_texel_bytes = copy->source_texel_bytes;
   const uint32_t destination_texel_bytes = copy->destination_texel_bytes;
   const float left = (float)copy->source_x;
   const float top = (float)copy->source_y;
   const float right = left + copy->source_step_x * (float)copy->width;
   const float bottom = top + copy->source_step_y * (float)copy->height;
   /* The source rectangle's texel bounds, for the clamp. A mirrored step makes
    * the right and bottom below the left and top, so both ends sort. */
   const int32_t low_x = (int32_t)floorf(MIN2(left, right));
   const int32_t low_y = (int32_t)floorf(MIN2(top, bottom));
   const int32_t high_x = (int32_t)ceilf(MAX2(left, right)) - 1;
   const int32_t high_y = (int32_t)ceilf(MAX2(top, bottom)) - 1;
   uint64_t source_low = UINT64_MAX;
   uint64_t source_high = 0;
   uint64_t destination_low = UINT64_MAX;
   uint64_t destination_high = 0;

   for (uint32_t row = 0; row < copy->height; row++) {
      const float sample_y = top + ((float)row + 0.5f) * copy->source_step_y - 0.5f;
      for (uint32_t column = 0; column < copy->width; column++) {
         const float sample_x = left + ((float)column + 0.5f) * copy->source_step_x - 0.5f;
         uint8_t texel[16] = {0};
         uint8_t rgba[4] = {0, 0, 0, 0};
         if (!copy->linear) {
            const int32_t x = CLAMP((int32_t)floorf(sample_x + 0.5f), low_x, high_x);
            const int32_t y = CLAMP((int32_t)floorf(sample_y + 0.5f), low_y, high_y);
            const uint64_t at =
               ps5vk_image_copy_address(&copy->source_side, x, y, source_texel_bytes, 0);
            memcpy(texel, (const uint8_t *)(uintptr_t)at, source_texel_bytes);
            source_low = MIN2(source_low, at);
            source_high = MAX2(source_high, at + source_texel_bytes);
            /* The sampler's fetch, converted to the destination's four bytes:
             * the same format copies its words, and any other decode the
             * recording has already accepted (ps5vk_texel_to_rgba8). */
            if (!ps5vk_texel_to_rgba8(copy->source_format, texel, rgba))
               return;
         } else {
            /* Four texels around the sample point, each weighted by how close
             * the point is to it. A sample outside the rectangle clamps, so a
             * clamped pair is the same texel twice and the weights still add up
             * to one. */
            const float x0 = floorf(sample_x);
            const float y0 = floorf(sample_y);
            const float fx = sample_x - x0;
            const float fy = sample_y - y0;
            const int32_t ix0 = CLAMP((int32_t)x0, low_x, high_x);
            const int32_t iy0 = CLAMP((int32_t)y0, low_y, high_y);
            const int32_t ix1 = CLAMP((int32_t)x0 + 1, low_x, high_x);
            const int32_t iy1 = CLAMP((int32_t)y0 + 1, low_y, high_y);
            const uint64_t at[4] = {
               ps5vk_image_copy_address(&copy->source_side, ix0, iy0, source_texel_bytes, 0),
               ps5vk_image_copy_address(&copy->source_side, ix1, iy0, source_texel_bytes, 0),
               ps5vk_image_copy_address(&copy->source_side, ix0, iy1, source_texel_bytes, 0),
               ps5vk_image_copy_address(&copy->source_side, ix1, iy1, source_texel_bytes, 0),
            };
            const float weight[4] = {
               (1.0f - fx) * (1.0f - fy),
               fx * (1.0f - fy),
               (1.0f - fx) * fy,
               fx * fy,
            };
            for (unsigned tap = 0; tap < 4; tap++) {
               source_low = MIN2(source_low, at[tap]);
               source_high = MAX2(source_high, at[tap] + source_texel_bytes);
            }
            /* A filtered blit is between four-byte UNORM images, so the blend
             * is on their bytes (ps5vk_image.c). */
            for (uint32_t byte = 0; byte < 4; byte++) {
               float value = 0.0f;
               for (unsigned tap = 0; tap < 4; tap++)
                  value += (float)((const uint8_t *)(uintptr_t)at[tap])[byte] * weight[tap];
               rgba[byte] = (uint8_t)CLAMP((int32_t)floorf(value + 0.5f), 0, 255);
            }
         }
         const uint64_t destination_at = ps5vk_image_copy_address(
            &copy->destination_side, (int32_t)(copy->destination_x + column),
            (int32_t)(copy->destination_y + row), destination_texel_bytes, 0);
         const uint32_t encoded = ps5vk_rgba8_to_texel(copy->destination_format, rgba, texel);
         if (encoded == 0)
            return;
         memcpy((void *)(uintptr_t)destination_at, texel, encoded);
         destination_low = MIN2(destination_low, destination_at);
         destination_high = MAX2(destination_high, destination_at + destination_texel_bytes);
      }
   }
   if (source_low < source_high)
      ps5vk_flush_cpu_cache((const void *)(uintptr_t)source_low, (size_t)(source_high - source_low));
   if (destination_low < destination_high)
      ps5vk_flush_cpu_cache((const void *)(uintptr_t)destination_low,
                            (size_t)(destination_high - destination_low));
}

/* Submits the words [start, end) of the stream with the colour-buffer barrier
 * and a completion marker, and waits for the marker: the whole submission when
 * it has no copies, or one step of a submission the copies split. Everything
 * the step holds has run once this returns, so the caller may copy on the CPU
 * (ps5vk_queue_run). */
static VkResult
ps5vk_queue_run_step(struct ps5vk_queue *queue, const struct vk_queue_submit *submit,
                     uint32_t *const stream, size_t capacity, uint32_t *const marker,
                     uint32_t start, uint32_t end, uint32_t *capture)
{
   struct ps5vk_agc_command_buffer command = {
      .bottom = stream + start,
      .top = stream + capacity,
      .up = stream + end,
      .down = stream + capacity,
      .callback = (uintptr_t)ps5vk_agc_out_of_space,
   };
   /* Zero is the marker word's cleared value, so no submission carries it. */
   queue->marker_value = queue->marker_value == UINT32_MAX ? 1 : queue->marker_value + 1;
   const uint32_t value = queue->marker_value;
   *marker = 0;
   const uint32_t *const barrier =
      sceAgcCbReleaseMem(&command, PS5VK_BARRIER_EVENT, PS5VK_BARRIER_CONTROL, 1, 0, NULL, 0, 0,
                         0, 1, 0, 0);
   const uint32_t *const completion =
      sceAgcCbReleaseMem(&command, PS5VK_COMPLETION_EVENT, PS5VK_COMPLETION_CONTROL, 0, 0, marker,
                         1, value, 0, 0, 0, 0);
   /* Mesa's immediate submission does not mark the queue lost when
    * driver_submit fails, so every failure after this point does. */
   if (!barrier || !completion || command.up > command.top)
      return vk_queue_set_lost(&queue->vk, "the AGC helpers did not write the completion packets");
   const uint32_t word_count = (uint32_t)(command.up - command.bottom);
   /* The words stay in the stream until the next submission replaces them, and
    * the runner's capture reads them there (ps5vk_debug_last_submission) --
    * unless the submission is split, where the step after this one lands its
    * own words on this step's end packets and the caller hands over a capture
    * buffer for them (ps5vk_queue_run_copy_steps, ps5vk_debug_submission_steps).
    */
   if (capture != NULL) {
      memcpy(capture, stream + start, (size_t)word_count * sizeof(uint32_t));
      ps5vk_flush_cpu_cache(capture, (size_t)word_count * sizeof(uint32_t));
   }
   const uint32_t *const words = capture != NULL ? capture : stream + start;
   queue->last_words = word_count;
   queue->last_stream = words;
   if (queue->step_count < PS5VK_MAX_SUBMISSION_STEPS) {
      queue->steps[queue->step_count].stream = words;
      queue->steps[queue->step_count].words = word_count;
      queue->step_count++;
   }
   ps5vk_queue_flush_targets(submit);
   ps5vk_flush_cpu_cache(stream + start, word_count * sizeof(uint32_t));
   ps5vk_flush_cpu_cache(marker, sizeof(*marker));

   struct ps5vk_agc_submit_description description = {
      .words = stream + start,
      .word_count = word_count,
   };
   int32_t result = sceAgcDriverSubmitDcb(&description);
   if (result != 0)
      return vk_queue_set_lost(&queue->vk, "sceAgcDriverSubmitDcb failed: 0x%08x",
                               (unsigned)result);
   result = sceAgcSuspendPoint();
   if (result != 0)
      return vk_queue_set_lost(&queue->vk, "sceAgcSuspendPoint failed: 0x%08x", (unsigned)result);

   for (unsigned poll = 0; poll < PS5VK_MARKER_POLLS; poll++) {
      ps5vk_flush_cpu_cache(marker, sizeof(*marker));
      if (*marker == value) {
         ps5vk_queue_flush_targets(submit);
         return VK_SUCCESS;
      }
      sceKernelUsleep(PS5VK_MARKER_POLL_MICROSECONDS);
   }
   return vk_queue_set_lost(&queue->vk, "completion marker 0x%08x not written within 2 s", value);
}

/* An event action recorded by vkCmdSetEvent and its neighbours (ps5vk_sync.c):
 * the flag is in this process, so a set or a reset is a store under the event's
 * lock, and a wait reads the flag under that lock, sleeps a millisecond and
 * reads it again, which is the shape the completion marker poll has. The poll
 * is what a host vkSetEvent on another thread is seen by, and its two-second
 * bound is what makes an event nothing ever sets a lost device with its own
 * reason instead of a hang.
 *
 * The sleep is os_time_sleep and not the sceKernelUsleep the marker poll uses:
 * the PC runner's sceKernelUsleep does not wait at all (nothing on the PC
 * completes asynchronously, host/ps5/ps5_host.cpp), while a host thread really
 * can set an event on the PC, so this wait has to keep time on both. A
 * condition variable would need fewer wakeups, but the runtime's c11 wrapper
 * for a *timed* wait returns without waiting on this build: the first version
 * of this wait polled two thousand times in 67 microseconds and lost the device
 * before the host's set arrived, which driver/tests/vk_b5_events_test.c
 * caught. */
static VkResult
ps5vk_event_execute(struct ps5vk_queue *queue, const struct ps5vk_memory_copy *record)
{
   struct ps5vk_event *const event = record->event;
   const bool wait = record->event_action == PS5VK_EVENT_ACTION_WAIT;
   for (unsigned poll = 0; poll < PS5VK_MARKER_POLLS; poll++) {
      mtx_lock(&event->lock);
      if (!wait) {
         event->signaled = record->event_action == PS5VK_EVENT_ACTION_SET;
         mtx_unlock(&event->lock);
         return VK_SUCCESS;
      }
      const bool signaled = event->signaled;
      mtx_unlock(&event->lock);
      if (signaled)
         return VK_SUCCESS;
      os_time_sleep(PS5VK_MARKER_POLL_MICROSECONDS);
   }
   return vk_queue_set_lost(&queue->vk, "an event waited for was not set within %u ms",
                            PS5VK_MARKER_POLLS * PS5VK_MARKER_POLL_MICROSECONDS / 1000);
}

/* Runs a submission's steps, split where its copies and synchronization splits
 * fall (ps5vk_cmd_buffer_split): submit the words up to a split, wait for them,
 * run the copies recorded there on the CPU, then submit the words after it, so
 * a copy observes everything recorded before it and everything recorded after
 * it observes the copy (docs/M5_REFERENCE.md, C2), and a draw after a split
 * observes everything recorded before it (C4). The end packets of a step land
 * on the first words of the step after it, so the words as they were built are
 * kept aside and put back before each step runs. */
static VkResult
ps5vk_queue_run_copy_steps(struct ps5vk_queue *queue, const struct vk_queue_submit *submit,
                           uint32_t *const stream, size_t capacity, uint32_t *const marker,
                           uint32_t words, struct util_dynarray *splits)
{
   const struct ps5vk_copy_split *const split = util_dynarray_begin(splits);
   const uint32_t split_count = util_dynarray_num_elements(splits, struct ps5vk_copy_split);
   uint32_t *const saved = words != 0 ? malloc((size_t)words * sizeof(uint32_t)) : NULL;
   if (words != 0 && saved == NULL)
      return vk_errorf(queue, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "no memory for the words of a split submission");
   if (saved != NULL)
      memcpy(saved, stream, (size_t)words * sizeof(uint32_t));

   /* One step's words stay whole in the stream only until the next one lands
    * its end packets on them, so a split submission's capture keeps its own
    * copy of every step, end packets included (ps5vk_queue_run_step,
    * ps5vk_debug_submission_steps). It outlives this call: the runner's
    * capture reads it until the next submission replaces it.
    *
    * The steps are the words before the first split, one step per split, and
    * the words after the last one: at most split_count + 1 of them carry words,
    * and each writes its own end packets -- so the buffer holds one step's end
    * packets more than the split count alone suggests. Sizing it for the splits
    * alone is what wrote PS5VK_STREAM_END_WORDS words past this allocation on
    * every frame of a recording whose last split was followed by more words
    * (R6: two render passes with the resolve recorded between them; the check
    * below names that arithmetic instead of repeating it). */
   const size_t capture_steps = (size_t)split_count + 1;
   const size_t capture_words = (size_t)words + capture_steps * PS5VK_STREAM_END_WORDS;
   if (queue->step_capture_words < capture_words) {
      free(queue->step_capture);
      queue->step_capture = malloc(capture_words * sizeof(uint32_t));
      queue->step_capture_words = queue->step_capture == NULL ? 0 : capture_words;
      if (queue->step_capture == NULL) {
         free(saved);
         return vk_errorf(queue, VK_ERROR_OUT_OF_HOST_MEMORY,
                          "no memory for the words of a split submission's capture");
      }
   }
   size_t capture_at = 0;
   uint32_t start = 0;
   uint32_t at = 0;
   VkResult result = VK_SUCCESS;
   while (at < split_count || start < words) {
      /* This step ends where the next copy falls, or at the stream's end. */
      const uint32_t end = at < split_count ? split[at].offset : words;
      /* A step with no words is a copy and nothing for the GPU to do -- a
       * command buffer that only records copies, which is the staging upload's
       * first one. There is nothing to submit and nothing to wait for: the
       * step before it has already completed, so the copy runs now. Submitting
       * it anyway would put an empty stream and its marker in front of the
       * frame the capture reads. */
      if (end != start) {
         /* R6: the capture buffer holds one step's end packets per *split*, but
          * a recording whose last split is followed by more words runs
          * split_count + 1 steps, and the last step's end packets land past the
          * buffer: a heap write of PS5VK_STREAM_END_WORDS words, which is what
          * corrupted the heap of an application that records two render passes
          * a frame (docs/M5_PHASE_C.md, R6). The check is here rather than only
          * in the size below so that a future change to how steps are counted
          * names itself instead of writing past the buffer. */
         const size_t step_words = (size_t)(end - start) + PS5VK_STREAM_END_WORDS;
         if (capture_at + step_words > queue->step_capture_words) {
            fprintf(stderr, "[ps5vk] step capture overflow: %zu words at %zu, buffer %zu\n",
                    step_words, capture_at, queue->step_capture_words);
            result = vk_errorf(queue, VK_ERROR_UNKNOWN,
                               "a split submission's step capture needs %zu words where the "
                               "buffer holds %zu (%u words, %u splits)",
                               capture_at + step_words, queue->step_capture_words, words,
                               split_count);
            break;
         }
         memcpy(stream + start, saved + start, (size_t)(end - start) * sizeof(uint32_t));
         result = ps5vk_queue_run_step(queue, submit, stream, capacity, marker, start, end,
                                       queue->step_capture + capture_at);
         if (result != VK_SUCCESS)
            break;
         /* The end packets of this step take the reserve the stream keeps for
          * them, which is where the next step's copy starts. */
         capture_at += (size_t)(end - start) + PS5VK_STREAM_END_WORDS;
      }
      /* Every copy recorded at this offset runs now, in record order: the GPU
       * has finished the words before it and has not seen the words after. A
       * record with no bytes is a split with no copy, which the step boundary
       * and the marker waited for above have already done all the work of
       * (a draw that samples a target this submission rendered into,
       * ps5vk_draw.c). */
      for (; at < split_count && split[at].offset == end; at++) {
         const struct ps5vk_memory_copy *const copy = split[at].copy;
         if (copy->resolve) {
            ps5vk_resolve_execute(copy);
            continue;
         }
         if (copy->blit) {
            ps5vk_blit_execute(copy);
            continue;
         }
         /* An upload into tiled storage is a region of texels, not a range of
          * bytes: every texel lives at the map's own place, so it too runs
          * before the empty-copy check (C7). */
         if (copy->image_write) {
            ps5vk_image_write_execute(copy);
            continue;
         }
         /* An image copy is the same shape: a region of one map's runs written
          * at another's, with no bytes of its own. */
         if (copy->image_copy) {
            ps5vk_image_copy_execute(copy);
            continue;
         }
         /* A clear is a region of an image, not a range of bytes: it has no
          * bytes of its own, so it is dispatched before the empty-copy check. */
         if (copy->clear) {
            ps5vk_clear_execute(copy);
            continue;
         }
         /* A query-result copy computes its bytes from the pool's counters
          * rather than naming them, so it too runs before the empty-copy
          * check (Phase V0-query). */
         if (copy->query) {
            ps5vk_query_execute(copy);
            continue;
         }
         /* An event action carries no bytes either: it is the flag the command
          * named, set, reset or waited for at this point in the order. */
         if (copy->event_action != PS5VK_EVENT_ACTION_NONE) {
            result = ps5vk_event_execute(queue, copy);
            if (result != VK_SUCCESS)
               break;
            continue;
         }
         if (copy->bytes == 0)
            continue;
         if (copy->fill) {
            /* A fill writes the same word over the range, four bytes at a time
             * (valid usage aligns both ends). */
            uint32_t *const words = (uint32_t *)(uintptr_t)copy->destination;
            const uint64_t count = copy->bytes / 4;
            for (uint64_t word = 0; word < count; word++)
               words[word] = copy->fill_value;
            ps5vk_flush_cpu_cache((const void *)(uintptr_t)copy->destination,
                                  (size_t)copy->bytes);
            continue;
         }
         ps5vk_flush_cpu_cache((const void *)(uintptr_t)copy->source, (size_t)copy->bytes);
         if (copy->reverse_texel_bytes == 0) {
            memcpy((void *)(uintptr_t)copy->destination, (const void *)(uintptr_t)copy->source,
                   (size_t)copy->bytes);
         } else {
            /* An application's bytes meeting a reversed image's: the four bytes of
             * every texel swap as they move (ps5vk_format.storage_reversed). */
            const uint8_t *const from = (const uint8_t *)(uintptr_t)copy->source;
            uint8_t *const to = (uint8_t *)(uintptr_t)copy->destination;
            const uint32_t texel_bytes = copy->reverse_texel_bytes;
            for (uint64_t texel = 0; texel + texel_bytes <= copy->bytes; texel += texel_bytes) {
               for (uint32_t byte = 0; byte < texel_bytes; byte++)
                  to[texel + byte] = from[texel + texel_bytes - 1u - byte];
            }
         }
         ps5vk_flush_cpu_cache((const void *)(uintptr_t)copy->destination, (size_t)copy->bytes);
      }
      if (result != VK_SUCCESS)
         break;
      start = end;
   }
   free(saved);
   return result;
}

/* Steps 2 to 4 for a submission with command buffers. */
static VkResult
ps5vk_queue_run(struct ps5vk_queue *queue, const struct vk_queue_submit *submit)
{
   uint32_t *const stream = queue->submission.address;
   const size_t capacity = queue->submission.bytes / sizeof(uint32_t) - 1;
   uint32_t *const marker = stream + capacity;
   size_t words = 0;

   /* The steps this submission runs as, which the runner's capture reads whole
    * (ps5vk_debug_last_submission). */
   queue->step_count = 0;

   /* A submission that renders into a swapchain image starts with the image's
    * wait packet: the GPU waits until VideoOut no longer scans that buffer out
    * (M2, C1). There is one VideoOut, so each buffer waits once. */
   const uint32_t wait_words = sceAgcDriverGetWaitRenderingPacketSizeInDwords();
   uint32_t waited = 0;
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      const struct ps5vk_cmd_buffer *const cmd_buffer =
         container_of(submit->command_buffers[i], struct ps5vk_cmd_buffer, vk);
      util_dynarray_foreach (&cmd_buffer->targets, struct ps5vk_render_target, target) {
         if (target->video < 0 || (waited & (UINT32_C(1) << target->buffer_index)))
            continue;
         if (wait_words > capacity - PS5VK_STREAM_END_WORDS - words)
            return vk_errorf(queue, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                             "no room for a wait packet in the submission buffer");
         uint32_t *up = stream + words;
         const uint32_t result = sceAgcDriverWaitUntilSafeForRendering(
            &up, wait_words, 0, (uint32_t)target->video, (int)target->buffer_index);
         if (result != 0 || up != stream + words + wait_words)
            return vk_queue_set_lost(&queue->vk,
                                     "the AGC helper did not write the wait packet of buffer %u: "
                                     "0x%08x", target->buffer_index, (unsigned)result);
         words += wait_words;
         waited |= UINT32_C(1) << target->buffer_index;
      }
   }

   /* The copies and synchronization splits the command buffers recorded, and
    * the stream offset each of them falls at: the submission is split into
    * steps there (Phase C2, C4). */
   struct util_dynarray splits;
   util_dynarray_init(&splits, NULL);
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      const struct ps5vk_cmd_buffer *const cmd_buffer =
         container_of(submit->command_buffers[i], struct ps5vk_cmd_buffer, vk);
      const size_t count = util_dynarray_num_elements(&cmd_buffer->words, uint32_t);
      if (count > capacity - PS5VK_STREAM_END_WORDS - words) {
         util_dynarray_fini(&splits);
         return vk_errorf(queue, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "a submission of more than %zu words", capacity - PS5VK_STREAM_END_WORDS);
      }
      /* A copy or split falls after the words its command buffer had recorded
       * when the application recorded it, which is that command buffer's
       * words' start in the stream plus that offset. */
      util_dynarray_foreach (&cmd_buffer->copies, struct ps5vk_memory_copy, copy) {
         struct ps5vk_copy_split *const split =
            util_dynarray_grow(&splits, struct ps5vk_copy_split, 1);
         if (!split) {
            util_dynarray_fini(&splits);
            return vk_errorf(queue, VK_ERROR_OUT_OF_HOST_MEMORY,
                             "no memory to track the copies of a submission");
         }
         *split = (struct ps5vk_copy_split){
            .offset = (uint32_t)words + MIN2(copy->after_words, (uint32_t)count),
            .copy = copy,
         };
      }
      if (count != 0)
         memcpy(stream + words, util_dynarray_begin(&cmd_buffer->words), count * sizeof(uint32_t));
      words += count;
   }

   VkResult result;
   if (util_dynarray_num_elements(&splits, struct ps5vk_copy_split) == 0) {
      /* Without copies or splits the submission is one step of every word,
       * ended by the barrier and the marker, as every frame recorded so far
       * has been. */
      result =
         ps5vk_queue_run_step(queue, submit, stream, capacity, marker, 0, (uint32_t)words, NULL);
   } else {
      result = ps5vk_queue_run_copy_steps(queue, submit, stream, capacity, marker, (uint32_t)words,
                                          &splits);
   }
   util_dynarray_fini(&splits);
   return result;
}

static VkResult
ps5vk_queue_submit(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   struct ps5vk_queue *const queue = container_of(vk_queue, struct ps5vk_queue, vk);
   struct vk_device *const device = vk_queue->base.device;

   VkResult result = vk_sync_wait_many(device, submit->wait_count, submit->waits,
                                       VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;
   if (submit->command_buffer_count != 0) {
      result = ps5vk_queue_run(queue, submit);
      if (result != VK_SUCCESS)
         return result;
   }
   return vk_sync_signal_many(device, submit->signal_count, submit->signals);
}

/* The flip mode the runner flips with. */
#define PS5VK_FLIP_MODE 1
/* The runner's flip wait: up to 200 vblanks. */
#define PS5VK_FLIP_WAITS 200
/* sceVideoOutGetFlipStatus fills 16 64-bit words; the fourth is the marker of
 * the latest flip shown. */
#define PS5VK_FLIP_STATUS_WORDS 16
#define PS5VK_FLIP_STATUS_MARKER 3

/* A flip in a stream of its own, as run pid 134 presented (c1-present):
 * submitted, through the suspend point, then confirmed when VideoOut's flip
 * status reaches marker, waiting a vblank at a time as the runner waits. */
VkResult
ps5vk_queue_flip(struct ps5vk_queue *queue, int video, uint32_t buffer_index, int64_t marker)
{
   uint32_t *const stream = queue->submission.address;
   const size_t capacity = queue->submission.bytes / sizeof(uint32_t) - 1;
   struct ps5vk_agc_command_buffer command = {
      .bottom = stream,
      .top = stream + capacity,
      .up = stream,
      .down = stream + capacity,
      .callback = (uintptr_t)ps5vk_agc_out_of_space,
   };
   if (!sceAgcDcbSetFlip(&command, (uint32_t)video, (int)buffer_index, PS5VK_FLIP_MODE, marker) ||
       command.up > command.top)
      return vk_queue_set_lost(&queue->vk, "the AGC helper did not write the flip of buffer %u",
                               buffer_index);
   const uint32_t word_count = (uint32_t)(command.up - command.bottom);
   /* A flip is a stream of its own in the same buffer: the capture reads this
    * count with the words (ps5vk_debug_last_submission) -- and it is the
    * submission the step list has to report, or a capture of a flip logs the
    * previous draw's step: the draw's length over the flip's words, which is
    * 104 words over 64 for c1-triangle and 40 words of the draw's own buffer
    * behind them (2026-09-20). */
   queue->last_words = word_count;
   queue->last_stream = stream;
   queue->step_count = 1;
   queue->steps[0].stream = stream;
   queue->steps[0].words = word_count;
   ps5vk_flush_cpu_cache(stream, word_count * sizeof(uint32_t));

   struct ps5vk_agc_submit_description description = {
      .words = stream,
      .word_count = word_count,
   };
   int32_t result = sceAgcDriverSubmitDcb(&description);
   if (result != 0)
      return vk_queue_set_lost(&queue->vk, "sceAgcDriverSubmitDcb failed for a flip: 0x%08x",
                               (unsigned)result);
   result = sceAgcSuspendPoint();
   if (result != 0)
      return vk_queue_set_lost(&queue->vk, "sceAgcSuspendPoint failed after a flip: 0x%08x",
                               (unsigned)result);

   uint64_t status[PS5VK_FLIP_STATUS_WORDS];
   for (unsigned wait = 0; wait < PS5VK_FLIP_WAITS; wait++) {
      if (sceVideoOutGetFlipStatus(video, status) == 0 &&
          (int64_t)status[PS5VK_FLIP_STATUS_MARKER] >= marker)
         return VK_SUCCESS;
      sceVideoOutWaitVblank(video);
   }
   return vk_queue_set_lost(&queue->vk,
                            "flip %" PRId64 " of buffer %u did not reach VideoOut within %d "
                            "vblanks", marker, buffer_index, PS5VK_FLIP_WAITS);
}

/* An upload into tiled storage (C7): the source is linear, the destination a
 * tiled side, so the region's texels are read row by row from the source's pitch
 * and written one at a time at the place the image's own map gives them. */
void
ps5vk_image_write_execute(const struct ps5vk_memory_copy *copy)
{
   const uint8_t *const source = (const uint8_t *)(uintptr_t)copy->source;
   const uint32_t texel_bytes = copy->destination_texel_bytes;
   ps5vk_flush_cpu_cache(source, (size_t)(copy->source_pitch * (copy->height - 1u) +
                                          (uint64_t)copy->width * texel_bytes));
   for (uint32_t row = 0; row < copy->height; row++) {
      const uint8_t *const row_source = source + (uint64_t)row * copy->source_pitch;
      for (uint32_t x = 0; x < copy->width; x++) {
         uint8_t *const destination =
            (uint8_t *)(uintptr_t)ps5vk_image_copy_address(
               &copy->destination_side, (int32_t)(copy->destination_x + x),
               (int32_t)(copy->destination_y + row), texel_bytes, 0);
         const uint8_t *const texel = row_source + (uint64_t)x * texel_bytes;
         if (copy->reverse_texel_bytes == 0) {
            memcpy(destination, texel, texel_bytes);
         } else {
            for (uint32_t byte = 0; byte < texel_bytes; byte++)
               destination[byte] = texel[texel_bytes - 1u - byte];
         }
      }
   }
   /* The sampler reads these texels once the words after the split have run. */
   ps5vk_flush_cpu_cache((const void *)(uintptr_t)copy->destination_side.address,
                         (size_t)copy->destination_side.level_width *
                            (size_t)copy->destination_side.level_width * texel_bytes);
}

/* The inverse of ps5vk_texel_to_rgba8 for the formats the driver can write a
 * blit *into*: the eight-bit UNORM the resampler produced, encoded as the
 * destination format's texel. Zero means no encode, which the recorded blit
 * refuses by name (ps5vk_image.c); the packed float forms and the two depth
 * formats are that case. The scaling rules are the decode's own read backwards,
 * so a round trip through the two functions returns the byte the frame held. */
static uint32_t
ps5vk_scale8(uint8_t value, uint32_t max)
{
   /* 64 bits wide: a 32-bit channel's maximum overflows a 32-bit product, and
    * the wrap made every 32-bit integer encode zero (round 5's host run,
    * docs/M5_PHASE_C.md). */
   return (uint32_t)(((uint64_t)value * max + 127u) / 255u);
}

/* IEEE binary16, round to nearest even: the decode's ps5vk_half_to_float read
 * backwards for the values a blit carries. */
static uint16_t
ps5vk_float_to_half(float value)
{
   uint32_t bits = 0;
   memcpy(&bits, &value, sizeof(bits));
   const uint32_t sign = (bits >> 16) & 0x8000u;
   const int32_t exponent = (int32_t)((bits >> 23) & 0xffu) - 127 + 15;
   uint32_t mantissa = bits & 0x7fffffu;
   if (((bits >> 23) & 0xffu) == 0xffu)
      return (uint16_t)(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
   if (exponent >= 0x1f)
      return (uint16_t)(sign | 0x7c00u);
   if (exponent <= 0) {
      if (exponent < -10)
         return (uint16_t)sign;
      mantissa |= 0x800000u;
      const uint32_t shift = (uint32_t)(14 - exponent);
      const uint32_t half = mantissa >> shift;
      const uint32_t round = (mantissa >> (shift - 1u)) & 1u;
      return (uint16_t)(sign | ((half + round) >> 1));
   }
   const uint32_t half = (uint32_t)exponent << 10 | (mantissa >> 13);
   const uint32_t round = (mantissa >> 12) & 1u;
   return (uint16_t)(sign | (half + round));
}

/* channels components of bits bits, little endian, red first: the integer and
 * normalised families' encode, which is ps5vk_integer_to_rgba8 read backwards.
 * Returns the bytes it wrote into texel. */
static uint32_t
ps5vk_rgba8_from_integer(const uint8_t rgba[4], unsigned bits, unsigned channels, bool is_signed,
                         uint8_t texel[16])
{
   const unsigned bytes = bits / 8u;
   memset(texel, 0, 16);
   for (unsigned channel = 0; channel < channels; channel++) {
      uint32_t value = 0;
      if (is_signed)
         value = (uint32_t)(int32_t)ps5vk_scale8(rgba[channel], (1u << (bits - 1u)) - 1u);
      else
         value = ps5vk_scale8(rgba[channel], bits == 32 ? UINT32_MAX : (1u << bits) - 1u);
      memcpy(texel + channel * bytes, &value, bytes);
   }
   return channels * bytes;
}

uint32_t
ps5vk_rgba8_to_texel(VkFormat format, const uint8_t rgba[4], uint8_t texel[16])
{
   memset(texel, 0, 16);
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
      memcpy(texel, rgba, 4);
      return 4;
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
      texel[0] = rgba[3];
      texel[1] = rgba[2];
      texel[2] = rgba[1];
      texel[3] = rgba[0];
      return 4;
   case VK_FORMAT_B8G8R8A8_UNORM:
      texel[0] = rgba[2];
      texel[1] = rgba[1];
      texel[2] = rgba[0];
      texel[3] = rgba[3];
      return 4;
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_B8G8R8A8_SRGB: {
      /* The byte-reversed B8G8R8A8 form writes the three encoded channels in
       * B, G, R order; the packed A8B8G8R8 one writes its storage's own
       * R, G, B, A order (ps5vk_format.storage_reversed). */
      const bool blue_first = format == VK_FORMAT_B8G8R8A8_SRGB;
      for (unsigned channel = 0; channel < 3; channel++) {
         const float linear = (float)rgba[channel] / 255.0f;
         const float encoded = linear <= 0.0031308f
                                  ? linear * 12.92f
                                  : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
         texel[blue_first ? 2u - channel : channel] =
            (uint8_t)CLAMP((int32_t)floorf(encoded * 255.0f + 0.5f), 0, 255);
      }
      texel[3] = rgba[3];
      return 4;
   }
   case VK_FORMAT_R8_UNORM:
      texel[0] = rgba[0];
      return 1;
   case VK_FORMAT_R8G8_UNORM:
      texel[0] = rgba[0];
      texel[1] = rgba[1];
      return 2;
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16B16A16_UNORM: {
      const unsigned channels = format == VK_FORMAT_R16_UNORM ? 1u
                               : format == VK_FORMAT_R16G16_UNORM ? 2u
                                                                  : 4u;
      return ps5vk_rgba8_from_integer(rgba, 16, channels, false, texel);
   }
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_SFLOAT: {
      const unsigned channels = format == VK_FORMAT_R16_SFLOAT ? 1u
                               : format == VK_FORMAT_R16G16_SFLOAT ? 2u
                                                                   : 4u;
      for (unsigned channel = 0; channel < channels; channel++) {
         const uint16_t half = ps5vk_float_to_half((float)rgba[channel] / 255.0f);
         memcpy(texel + channel * 2u, &half, sizeof(half));
      }
      return channels * 2u;
   }
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_SFLOAT: {
      const unsigned channels = format == VK_FORMAT_R32_SFLOAT ? 1u
                               : format == VK_FORMAT_R32G32_SFLOAT ? 2u
                                                                   : 4u;
      for (unsigned channel = 0; channel < channels; channel++) {
         const float value = (float)rgba[channel] / 255.0f;
         memcpy(texel + channel * 4u, &value, sizeof(value));
      }
      return channels * 4u;
   }
   case VK_FORMAT_R5G6B5_UNORM_PACK16: {
      const uint32_t word = ps5vk_scale8(rgba[0], 31u) << 11 | ps5vk_scale8(rgba[1], 63u) << 5 |
                            ps5vk_scale8(rgba[2], 31u);
      memcpy(texel, &word, 2);
      return 2;
   }
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16: {
      const uint32_t word = (rgba[3] >= 128 ? 1u : 0u) << 15 | ps5vk_scale8(rgba[0], 31u) << 10 |
                            ps5vk_scale8(rgba[1], 31u) << 5 | ps5vk_scale8(rgba[2], 31u);
      memcpy(texel, &word, 2);
      return 2;
   }
   case VK_FORMAT_B4G4R4A4_UNORM_PACK16: {
      const uint32_t word = ps5vk_scale8(rgba[3], 15u) << 12 | ps5vk_scale8(rgba[0], 15u) << 8 |
                            ps5vk_scale8(rgba[1], 15u) << 4 | ps5vk_scale8(rgba[2], 15u);
      memcpy(texel, &word, 2);
      return 2;
   }
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
      const uint32_t word = ps5vk_scale8(rgba[3], 3u) << 30 | ps5vk_scale8(rgba[2], 1023u) << 20 |
                            ps5vk_scale8(rgba[1], 1023u) << 10 | ps5vk_scale8(rgba[0], 1023u);
      memcpy(texel, &word, 4);
      return 4;
   }
   case VK_FORMAT_A2B10G10R10_UINT_PACK32: {
      const uint32_t word = ps5vk_scale8(rgba[3], 3u) << 30 | ps5vk_scale8(rgba[2], 1023u) << 20 |
                            ps5vk_scale8(rgba[1], 1023u) << 10 | ps5vk_scale8(rgba[0], 1023u);
      memcpy(texel, &word, 4);
      return 4;
   }
   /* The integer families, one case a width: the block size alone does not name
    * the component width -- an R8G8 texel is two bytes of *eight* bits each, not
    * one of sixteen -- so each format carries the (bits, channels, signedness)
    * ps5vk_integer_to_rgba8's decode names for it, and the encode is that
    * function read backwards. */
   case VK_FORMAT_R8_UINT:
      return ps5vk_rgba8_from_integer(rgba, 8, 1, false, texel);
   case VK_FORMAT_R8_SINT:
      return ps5vk_rgba8_from_integer(rgba, 8, 1, true, texel);
   case VK_FORMAT_R8G8_UINT:
      return ps5vk_rgba8_from_integer(rgba, 8, 2, false, texel);
   case VK_FORMAT_R8G8_SINT:
      return ps5vk_rgba8_from_integer(rgba, 8, 2, true, texel);
   case VK_FORMAT_R8G8B8A8_UINT:
      return ps5vk_rgba8_from_integer(rgba, 8, 4, false, texel);
   case VK_FORMAT_R8G8B8A8_SINT:
      return ps5vk_rgba8_from_integer(rgba, 8, 4, true, texel);
   case VK_FORMAT_A8B8G8R8_UINT_PACK32:
      return ps5vk_rgba8_from_integer(rgba, 8, 4, false, texel);
   case VK_FORMAT_A8B8G8R8_SINT_PACK32:
      return ps5vk_rgba8_from_integer(rgba, 8, 4, true, texel);
   case VK_FORMAT_R16_UINT:
      return ps5vk_rgba8_from_integer(rgba, 16, 1, false, texel);
   case VK_FORMAT_R16_SINT:
      return ps5vk_rgba8_from_integer(rgba, 16, 1, true, texel);
   case VK_FORMAT_R16G16_UINT:
      return ps5vk_rgba8_from_integer(rgba, 16, 2, false, texel);
   case VK_FORMAT_R16G16_SINT:
      return ps5vk_rgba8_from_integer(rgba, 16, 2, true, texel);
   case VK_FORMAT_R16G16B16A16_UINT:
      return ps5vk_rgba8_from_integer(rgba, 16, 4, false, texel);
   case VK_FORMAT_R16G16B16A16_SINT:
      return ps5vk_rgba8_from_integer(rgba, 16, 4, true, texel);
   case VK_FORMAT_R32_UINT:
      return ps5vk_rgba8_from_integer(rgba, 32, 1, false, texel);
   case VK_FORMAT_R32_SINT:
      return ps5vk_rgba8_from_integer(rgba, 32, 1, true, texel);
   case VK_FORMAT_R32G32_UINT:
      return ps5vk_rgba8_from_integer(rgba, 32, 2, false, texel);
   case VK_FORMAT_R32G32_SINT:
      return ps5vk_rgba8_from_integer(rgba, 32, 2, true, texel);
   case VK_FORMAT_R32G32B32A32_UINT:
      return ps5vk_rgba8_from_integer(rgba, 32, 4, false, texel);
   case VK_FORMAT_R32G32B32A32_SINT:
      return ps5vk_rgba8_from_integer(rgba, 32, 4, true, texel);
   default:
      return 0;
   }
}

uint32_t
ps5vk_rgba8_texel_bytes(VkFormat format)
{
   uint8_t texel[16];
   const uint8_t rgba[4] = {0, 0, 0, 255};
   return ps5vk_rgba8_to_texel(format, rgba, texel);
}

/* One recorded image copy: the runs of the region, each side placed by its own
 * map, moved at the split point. The walk is the one the recording used to do
 * itself, run here instead so that one record holds a whole region -- a
 * four-sample depth image's sixteen-byte texel is one run each, and expanding a
 * 4K copy's 8.3 million runs into records needs more memory than an application
 * has (the console run that found this is in docs/HARDWARE_FINDINGS.md). The
 * run length is the largest span the map keeps contiguous (PS5VK_TILED_RUN_BYTES,
 * ps5vk_private.h). */
void
ps5vk_image_copy_execute(const struct ps5vk_memory_copy *copy)
{
   const uint32_t texel_bytes = copy->source_texel_bytes;
   const uint64_t row_bytes = (uint64_t)copy->width * texel_bytes;
   const uint64_t run_texels = PS5VK_TILED_RUN_BYTES / texel_bytes > 0
                                  ? PS5VK_TILED_RUN_BYTES / texel_bytes
                                  : 1u;
   for (uint32_t row = 0; row < copy->height; row++) {
      for (uint64_t at = 0; at < row_bytes; at += run_texels * texel_bytes) {
         const uint64_t run_bytes = MIN2(row_bytes - at, run_texels * texel_bytes);
         const uint64_t source_at = ps5vk_image_copy_address(
            &copy->source_side, copy->source_x + (int32_t)(at / texel_bytes),
            copy->source_y + (int32_t)row, texel_bytes, 0);
         const uint64_t destination_at = ps5vk_image_copy_address(
            &copy->destination_side, copy->destination_x + (uint32_t)(at / texel_bytes),
            copy->destination_y + row, texel_bytes, 0);
         ps5vk_flush_cpu_cache((const void *)(uintptr_t)source_at, (size_t)run_bytes);
         if (copy->reverse_texel_bytes == 0) {
            memcpy((void *)(uintptr_t)destination_at, (const void *)(uintptr_t)source_at,
                   (size_t)run_bytes);
         } else {
            /* A readback out of a reversed image: the four bytes of every texel
             * swap as the run moves (ps5vk_format.storage_reversed). */
            const uint8_t *const from = (const uint8_t *)(uintptr_t)source_at;
            uint8_t *const to = (uint8_t *)(uintptr_t)destination_at;
            for (uint64_t texel = 0; texel + texel_bytes <= run_bytes; texel += texel_bytes) {
               for (uint32_t byte = 0; byte < texel_bytes; byte++)
                  to[texel + byte] = from[texel + texel_bytes - 1u - byte];
            }
         }
         ps5vk_flush_cpu_cache((const void *)(uintptr_t)destination_at, (size_t)run_bytes);
      }
   }
}

VkResult
ps5vk_queue_init(struct ps5vk_device *device, struct ps5vk_queue *queue,
                 const VkDeviceQueueCreateInfo *info)
{
   VkResult result = vk_queue_init(&queue->vk, &device->vk, info, 0);
   if (result != VK_SUCCESS)
      return result;
   queue->marker_value = 0;
   queue->last_words = 0;
   queue->last_stream = NULL;

   const int32_t mapped = ps5vk_direct_mapping_create(&queue->submission, PS5VK_SUBMISSION_BYTES,
                                                      PS5VK_SUBMISSION_BYTES);
   if (mapped != 0) {
      vk_queue_finish(&queue->vk);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "the queue's submission buffer could not be mapped in the address "
                       "window: 0x%08x", (unsigned)mapped);
   }
   /* The AGC helpers reserve more words than they write -- the wait packet 32
    * against its 16 and the flip 64 against its 19 -- and the console leaves
    * whatever the buffer held in the rest: a frame's wait carries the previous
    * submission's words at that offset and a flip carries the frame's tail
    * (runner pid 109, run pid 113; docs/M5_PHASE_C.md). Direct memory is
    * recycled without being cleared, so the first submission's reserved words
    * would otherwise hold another process's leftovers and the streams would
    * not be reproducible. Zeroing once, here, leaves everything the helpers do
    * not write a function of this queue's own submissions. */
   memset(queue->submission.address, 0, (size_t)queue->submission.bytes);
   /* Until the first submission the capture reads from the buffer's start,
    * which is where the words of the first one will lie. */
   queue->last_stream = queue->submission.address;

   queue->vk.driver_submit = ps5vk_queue_submit;
   return VK_SUCCESS;
}

void
ps5vk_queue_finish(struct ps5vk_queue *queue)
{
   vk_queue_finish(&queue->vk);
   ps5vk_direct_mapping_destroy(&queue->submission);
   free(queue->step_capture);
}

const uint32_t *
ps5vk_debug_last_submission(VkDevice _device, uint32_t *dwords)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   const bool queued = device != NULL && device->queue_initialized;
   if (dwords != NULL)
      *dwords = queued ? device->queue.last_words : 0;
   return queued ? device->queue.last_stream : NULL;
}

uint32_t
ps5vk_debug_submission_steps(VkDevice _device, ps5vk_debug_stage *steps, uint32_t capacity)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   if (device == NULL || !device->queue_initialized || steps == NULL)
      return 0;
   const uint32_t count = device->queue.step_count;
   for (uint32_t index = 0; index < count && index < capacity; index++) {
      steps[index].address = (void *)(uintptr_t)device->queue.steps[index].stream;
      steps[index].bytes = device->queue.steps[index].words * sizeof(uint32_t);
   }
   return count;
}
