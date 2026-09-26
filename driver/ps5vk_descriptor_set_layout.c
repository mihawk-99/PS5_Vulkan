/*
 * PS5 Vulkan driver - descriptor set layouts.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B6 (docs/M5_PHASE_B.md). A set's descriptors live in a
 * GPU-visible table whose address the shaders read from user data. Bindings
 * take consecutive table entries in binding order, each descriptor the size
 * the shader compiler expects for its type: 16 bytes for a uniform buffer and
 * 48 for a combined image sampler, the layouts the M3 probes compiled and the
 * hardware ran (binding 0 at offset 0). Other types get stride 0, and
 * pipelines refuse them until a probe proves their entries.
 *
 * Pipeline layouts and destruction are Mesa's common implementation.
 */

#include "ps5vk_private.h"

#include <assert.h>

#include "util/macros.h"

static uint32_t
ps5vk_descriptor_stride(VkDescriptorType type)
{
   switch (type) {
   /* A dynamic uniform buffer is the same 16-byte descriptor: the offset the
    * application passes to vkCmdBindDescriptorSets is added to the address the
    * descriptor names when the draw writes it (ps5vk_draw.c, D1). */
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      return PS5VK_UNIFORM_BUFFER_DESCRIPTOR_BYTES;
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      return PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES;
   /* A compute dispatch's operand (Phase D2): the same 16 bytes, whose entry
    * the dispatch writes in the storage-buffer form (ps5vk_compute.c). */
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      return PS5VK_STORAGE_BUFFER_DESCRIPTOR_BYTES;
   /* A texel buffer is a buffer descriptor with the view's format in it, and
    * the compiler reads it as one 16-byte entry (docs/BLOCKERS.md). */
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      return PS5VK_TEXEL_BUFFER_DESCRIPTOR_BYTES;
   /* A storage image is the image descriptor alone: the compiler reads its
    * 32-byte entry where a combined image sampler's is 48 (docs/BLOCKERS.md). */
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
   /* An input attachment is a fetch of an image, so it is the same 32 bytes:
    * the image words alone (R2 of the port's requests). */
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      return PS5VK_STORAGE_IMAGE_DESCRIPTOR_BYTES;
   /* The separated form of a combined image sampler: the compiler validates a
    * texture instruction's *two* halves by looking up a
    * PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER entry at each half's own index
    * (psbc_compile.c, legacy_texture_bindings_valid), so a bare SAMPLER and a
    * bare SAMPLED_IMAGE are each the 48-byte combined entry and the instruction
    * reads the half it needs from each: the image words from the SAMPLED_IMAGE
    * binding's entry, the sampler words from the SAMPLER binding's. One mechanism
    * for the pair, which is why its acceptance is that a frame the separated form
    * draws is texel-for-texel the frame one COMBINED_IMAGE_SAMPLER draws (R2). */
   case VK_DESCRIPTOR_TYPE_SAMPLER:
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      return PS5VK_COMBINED_IMAGE_SAMPLER_DESCRIPTOR_BYTES;
   default:
      return 0;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateDescriptorSetLayout(VkDevice _device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator,
                                VkDescriptorSetLayout *pSetLayout)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* Mesa allocates reference-counted set layouts from the device. */
   (void)pAllocator;

   uint32_t binding_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++)
      binding_count = MAX2(binding_count, pCreateInfo->pBindings[i].binding + 1);

   struct ps5vk_descriptor_set_layout *const layout = vk_descriptor_set_layout_zalloc(
      &device->vk, sizeof(*layout) + binding_count * sizeof(layout->bindings[0]), pCreateInfo);
   if (!layout)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   layout->binding_count = binding_count;

   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *const source = &pCreateInfo->pBindings[i];
      struct ps5vk_descriptor_binding *const binding = &layout->bindings[source->binding];
      /* Valid usage: binding numbers are unique. */
      assert(binding->count == 0);
      binding->type = source->descriptorType;
      binding->count = source->descriptorCount;
      binding->stages = source->stageFlags;
      binding->stride = ps5vk_descriptor_stride(source->descriptorType);
   }

   uint32_t offset = 0;
   uint32_t dynamic_index = 0;
   for (uint32_t index = 0; index < binding_count; index++) {
      struct ps5vk_descriptor_binding *const binding = &layout->bindings[index];
      binding->offset = offset;
      binding->record_index = layout->descriptor_count;
      layout->descriptor_count += binding->count;
      if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
         binding->dynamic_index = dynamic_index;
         dynamic_index += binding->count;
      }
      offset += binding->count * binding->stride;
   }
   layout->table_bytes = offset;

   *pSetLayout = ps5vk_descriptor_set_layout_to_handle(layout);
   return VK_SUCCESS;
}

/* R84: vkGetDescriptorSetLayoutSupport (Vulkan 1.1's maintenance3). A layout
 * is supported when every binding's type has a descriptor this driver writes
 * (ps5vk_descriptor_stride) and the set's descriptors stay within the device's
 * per-set limits, which vkCreateDescriptorSetLayout's layouts are held to. */
VKAPI_ATTR void VKAPI_CALL
ps5vk_GetDescriptorSetLayoutSupport(VkDevice _device,
                                    const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                    VkDescriptorSetLayoutSupport *pSupport)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   const struct vk_properties *const limits = &device->vk.physical->properties;
   uint64_t samplers = 0, uniform = 0, uniform_dynamic = 0, storage = 0, storage_dynamic = 0;
   uint64_t sampled = 0, storage_images = 0, input = 0, total = 0;
   bool known = true;
   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *const binding = &pCreateInfo->pBindings[i];
      const uint64_t count = binding->descriptorCount;
      known = known && ps5vk_descriptor_stride(binding->descriptorType) != 0;
      total += count;
      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         samplers += count;
         break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         samplers += count;
         sampled += count;
         break;
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         sampled += count;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         storage_images += count;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         uniform += count;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         uniform_dynamic += count;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         storage += count;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         storage_dynamic += count;
         break;
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         input += count;
         break;
      default:
         known = false;
         break;
      }
   }
   pSupport->supported =
      known && total <= limits->maxPerSetDescriptors &&
      samplers <= limits->maxDescriptorSetSamplers &&
      uniform + uniform_dynamic <= limits->maxDescriptorSetUniformBuffers &&
      uniform_dynamic <= limits->maxDescriptorSetUniformBuffersDynamic &&
      storage + storage_dynamic <= limits->maxDescriptorSetStorageBuffers &&
      storage_dynamic <= limits->maxDescriptorSetStorageBuffersDynamic &&
      sampled <= limits->maxDescriptorSetSampledImages &&
      storage_images <= limits->maxDescriptorSetStorageImages &&
      input <= limits->maxDescriptorSetInputAttachments;
}
