/*
 * PS5 Vulkan driver - B2: vkCreateBufferView, the last core command that had no
 * path.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B2 and D2 (docs/M5_REFERENCE.md). A buffer view is what a
 * uniform or storage texel buffer binds. What this command owns is the reporting
 * rule an application can check for itself: the format must carry the
 * texel-buffer feature the buffer's usage asks for. Blocker round 5 proved the
 * uniform half of it, so R8G8B8A8_UNORM now reports
 * VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT and a view of it is created; a
 * format the device reports no texel-buffer feature for (R32G32B32_SFLOAT, whose
 * buffer features are empty) still refuses by name with that reason -- the same
 * answer as "this device samples no texel buffer", reached through the format
 * table rather than by the command having no path.
 *
 * The checks are both sides of that rule and the tolerance of the handles a
 * caller with none passes (the B2 device test's style): a claimed format's view
 * exists and destroys, and a missing buffer, a format with no texel-buffer
 * feature, a range past the buffer and a destroy of no view are the refusals.
 * Built and run through the loader and directly by tools/check-driver.sh (see
 * ps5vk_test.h); the PS5 build only links, it is not run.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "ps5vk_test.h"

int
main(void)
{
   test_begin("B2 buffer view");
   VkInstance instance = VK_NULL_HANDLE;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   if (!test_create_device(&instance, &physical, &device))
      return test_finish();

   /* The reporting the rule rests on, both ways: the format the texel-buffer
    * probe binds reports the uniform bit, and a format whose entry carries no
    * buffer feature reports neither texel-buffer bit, so nothing may bind a view
    * of it. */
   const VkFormatFeatureFlags texel = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                                      VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
   VkFormatProperties properties;
   memset(&properties, 0, sizeof(properties));
   VK_FUNCTION(instance, GetPhysicalDeviceFormatProperties)
   (physical, VK_FORMAT_R8G8B8A8_UNORM, &properties);
   check((properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0,
         "R8G8B8A8_UNORM reports UNIFORM_TEXEL_BUFFER, which is what a view of it needs");
   memset(&properties, 0, sizeof(properties));
   /* R32_SFLOAT is the format the CTS requires these bits for
    * (dEQP-VK.api.info.format_properties.r32_sfloat), and the console proved both
    * texel-buffer halves of it (docs/M5_PHASE_C.md, CTS rounds 7 and 8): it is now
    * a positive case, and the negative one is R32G32B32_SFLOAT, which reports
    * VERTEX_BUFFER alone. */
   VK_FUNCTION(instance, GetPhysicalDeviceFormatProperties)
   (physical, VK_FORMAT_R32_SFLOAT, &properties);
   check((properties.bufferFeatures & texel) == texel,
         "R32_SFLOAT reports both texel-buffer features, which the console proved");
   memset(&properties, 0, sizeof(properties));
   VK_FUNCTION(instance, GetPhysicalDeviceFormatProperties)
   (physical, VK_FORMAT_R32G32B32_SFLOAT, &properties);
   check((properties.bufferFeatures & texel) == 0,
         "R32G32B32_SFLOAT reports no texel-buffer feature, which is what a view is "
         "refused for");

   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 256,
      .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer = VK_NULL_HANDLE;
   const VkResult created =
      VK_FUNCTION(instance, CreateBuffer)(device, &buffer_info, NULL, &buffer);
   check(created == VK_SUCCESS, "a buffer for the view's usage is created");

   if (created == VK_SUCCESS) {
      const VkBufferViewCreateInfo view_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
         .buffer = buffer,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .offset = 0,
         .range = 64,
      };
      VkBufferView view = VK_NULL_HANDLE;
      check(VK_FUNCTION(instance, CreateBufferView)(device, &view_info, NULL, &view) == VK_SUCCESS &&
               view != VK_NULL_HANDLE,
            "a view of the format whose uniform texel buffer the probe proved is created");
      VK_FUNCTION(instance, DestroyBufferView)(device, view, NULL);

      VkBufferViewCreateInfo refused = view_info;
      refused.format = VK_FORMAT_R32G32B32_SFLOAT;
      view = VK_NULL_HANDLE;
      check(VK_FUNCTION(instance, CreateBufferView)(device, &refused, NULL, &view) ==
               VK_ERROR_UNKNOWN &&
               view == VK_NULL_HANDLE,
            "a view of a format with no texel-buffer feature is refused and writes no handle");

      VkBufferViewCreateInfo missing = view_info;
      missing.buffer = VK_NULL_HANDLE;
      check(VK_FUNCTION(instance, CreateBufferView)(device, &missing, NULL, &view) ==
               VK_ERROR_UNKNOWN,
            "a view of no buffer is refused instead of crashing");

      VkBufferViewCreateInfo past = view_info;
      past.offset = 200;
      past.range = 128;
      check(VK_FUNCTION(instance, CreateBufferView)(device, &past, NULL, &view) ==
               VK_ERROR_UNKNOWN,
            "a view whose range leaves the buffer is refused");

      VK_FUNCTION(instance, DestroyBufferView)(device, VK_NULL_HANDLE, NULL);
      check(VK_TRUE, "destroying no view does nothing");
      VK_FUNCTION(instance, DestroyBuffer)(device, buffer, NULL);
   }

   VK_FUNCTION(instance, DestroyDevice)(device, NULL);
   VK_FUNCTION(instance, DestroyInstance)(instance, NULL);
   return test_finish();
}
