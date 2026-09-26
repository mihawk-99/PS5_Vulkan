/*
 * PS5 Vulkan driver - Phase B2 test: instance, physical device, device, queue.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B2 (docs/M5_PHASE_B.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 */

#include "ps5vk_test.h"

static const char kDeviceName[] = "PS5 AGC GPU (ps5vk)";
static const char kProperties2Extension[] = "VK_KHR_get_physical_device_properties2";

static void
check_instance_level(void)
{
   uint32_t version = 0;
   const __typeof__(&vkEnumerateInstanceVersion) enumerate_version =
      VK_FUNCTION(VK_NULL_HANDLE, EnumerateInstanceVersion);
   /* Vulkan 1.0, not the 1.3 this test used to require. The driver implements
    * 1.0's commands, features and formats, so an instance claiming more would
    * hand an application entry points whose objects it cannot honour -- and the
    * CTS reads the two versions together: dEQP-VK.api.info.extension_core_versions
    * failed while the instance claimed 1.3 and the device reported 1.0, because
    * its version graph does not treat a later version as supporting an earlier
    * one (docs/M5_PHASE_C.md, round 6). The loader is unaffected: every loader
    * test in this file still passes, and the ICD interface version it negotiates
    * is a separate number (driver/ps5vk_instance.c). */
   check(enumerate_version && enumerate_version(&version) == VK_SUCCESS &&
            version >= VK_API_VERSION_1_0,
         "the instance version is at least the 1.0 this driver implements");
#ifdef PS5VK_TEST_DIRECT
   /* Directly, this query is the driver's own answer and must be exactly what
    * the driver implements. Through the loader it is the *loader's* answer --
    * the loader reports its own version, which is why this check is direct-only
    * and why the loader's number says nothing about the driver. That distinction
    * matters to the CTS: its api.info.extension_core_versions case asks the
    * instance's version, so what it reads here is what it judges
    * (docs/M5_PHASE_C.md, round 6). */
   check(version == VK_API_VERSION_1_1, "the driver reports instance version 1.1 (R84)");
#endif

   const __typeof__(&vkEnumerateInstanceExtensionProperties) enumerate_extensions =
      VK_FUNCTION(VK_NULL_HANDLE, EnumerateInstanceExtensionProperties);
   VkExtensionProperties extensions[64];
   uint32_t count = sizeof(extensions) / sizeof(extensions[0]);
   const VkResult result = enumerate_extensions(NULL, &count, extensions);
   bool properties2 = false;
   for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++)
      properties2 = properties2 || strcmp(extensions[i].extensionName, kProperties2Extension) == 0;
   check(result == VK_SUCCESS && properties2, "instance extension VK_KHR_get_physical_device_properties2");
#ifdef PS5VK_TEST_DIRECT
   check(count == 5, "the driver has exactly five instance extensions: "
                     "properties2, surface, display, debug_report and debug_utils");
#endif
}

static void
check_properties(VkInstance instance, VkPhysicalDevice physical)
{
   VkPhysicalDeviceProperties p;
   memset(&p, 0, sizeof(p));
   VK_FUNCTION(instance, GetPhysicalDeviceProperties)(physical, &p);
   check(p.apiVersion == VK_API_VERSION_1_1, "device API version 1.1 (R84)");
   check(p.driverVersion == VK_MAKE_VERSION(0, 2, 0), "driver version 0.2.0");
   check(p.vendorID == 0x1002, "AMD vendor ID");
   check(p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, "integrated GPU");
   check(strcmp(p.deviceName, kDeviceName) == 0, "device name");

   const VkPhysicalDeviceLimits *const l = &p.limits;
   check(l->maxImageDimension2D == 16384 && l->maxFramebufferWidth == 16384 &&
            l->maxViewportDimensions[0] == 16384,
         "image, framebuffer and viewport limits 16384 (PPSSPP's 10x targets)");
   check(l->maxBoundDescriptorSets == 4 && l->maxPerStageResources == 44,
         "4 descriptor sets, 44 per-stage resources (footnote 2)");
   check(l->maxDescriptorSetSamplers == 48 && l->maxDescriptorSetUniformBuffers == 36 &&
            l->maxDescriptorSetStorageImages == 12,
         "per-set descriptors for 3 shader stages (footnote 8)");
   check(l->maxTessellationGenerationLevel == 0 && l->maxGeometryOutputVertices == 0 &&
            l->maxClipDistances == 0,
         "limits of unsupported features are 0");
   check(l->maxFragmentDualSrcAttachments == 1,
         "one dual-source attachment, as dualSrcBlend asks (R71)");
   check(l->maxTexelBufferElements == UINT32_MAX,
         "texel buffers of the 32-bit NUM_RECORDS count, as RADV reports (R72)");
   check(l->maxViewports == 1 && l->maxSamplerAnisotropy == 16.0f &&
            l->maxDrawIndexedIndexValue == 0xffffff && l->maxDrawIndirectCount == 1,
         "single viewport, 16x anisotropy, 24-bit indices, single indirect draw");
   check(l->pointSizeRange[0] == 1.0f && l->pointSizeRange[1] == 1.0f &&
            l->lineWidthRange[1] == 1.0f && l->pointSizeGranularity == 0.0f,
         "points and lines of size 1 only");
   /* R78: 1, 2, 4 and 8 samples render, sample and resolve. */
   check(l->framebufferColorSampleCounts == (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT |
                                             VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT) &&
            l->storageImageSampleCounts == VK_SAMPLE_COUNT_1_BIT,
         "required sample counts, and two and eight (R78)");
   check(l->minMemoryMapAlignment == 4096 && l->minUniformBufferOffsetAlignment == 256,
         "memory map and offset alignments");

   VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
   VK_FUNCTION(instance, GetPhysicalDeviceProperties2KHR)(physical, &p2);
   check(p2.properties.vendorID == 0x1002 && strcmp(p2.properties.deviceName, kDeviceName) == 0,
         "vkGetPhysicalDeviceProperties2KHR reports the same device");

   VkPhysicalDeviceFeatures features;
   memset(&features, 0xff, sizeof(features));
   VK_FUNCTION(instance, GetPhysicalDeviceFeatures)(physical, &features);
   /* Vulkan 1.0 requires exactly one feature (docs/M5_REFERENCE.md, "1.0: make
    * the current claim true"): robustBufferAccess, which V0-robust proved on
    * the console -- a draw asking for twice the indices its buffer holds draws
    * the same square (console run pid 140, golden/v0-robust). */
   check(features.robustBufferAccess == VK_TRUE, "robustBufferAccess is on, as 1.0 requires");
   const VkBool32 *const flags = (const VkBool32 *)&features;
   bool only_robust = true;
   for (size_t i = 0; i < sizeof(features) / sizeof(VkBool32); i++)
      only_robust = only_robust && (flags[i] == VK_FALSE || &flags[i] == &features.robustBufferAccess ||
                                    &flags[i] == &features.samplerAnisotropy ||
                                    &flags[i] == &features.dualSrcBlend);
   check(features.samplerAnisotropy == VK_TRUE, "samplerAnisotropy is on (PPSSPP's 16x filtering)");
   check(features.dualSrcBlend == VK_TRUE,
         "dualSrcBlend is on (R71: Dolphin's destination alpha, Resident Evil 4's haze)");
   check(only_robust, "every other Vulkan 1.0 feature is off");

   uint32_t families = 0;
   const __typeof__(&vkGetPhysicalDeviceQueueFamilyProperties) get_families =
      VK_FUNCTION(instance, GetPhysicalDeviceQueueFamilyProperties);
   get_families(physical, &families, NULL);
   VkQueueFamilyProperties family;
   memset(&family, 0, sizeof(family));
   uint32_t written = 1;
   get_families(physical, &written, &family);
   check(families == 1 && written == 1, "one queue family");
   /* The GPU clock V0-query measured makes all 64 bits of a timestamp valid
    * (pid 143, driver/ps5vk_query.c): the family that has the graphics and the
    * compute bit answers a timestamp write. */
   check(family.queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT) &&
            family.queueCount == 1 && family.timestampValidBits == 64,
         "graphics, compute and transfer; one queue; 64-bit timestamps");

   VkPhysicalDeviceMemoryProperties memory;
   memset(&memory, 0, sizeof(memory));
   VK_FUNCTION(instance, GetPhysicalDeviceMemoryProperties)(physical, &memory);
   check(memory.memoryTypeCount == 2 &&
            memory.memoryTypes[0].propertyFlags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT &&
            memory.memoryTypes[0].heapIndex == 0 &&
            memory.memoryTypes[PS5VK_TEST_HOST_MEMORY_TYPE].propertyFlags ==
               (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
            memory.memoryTypes[PS5VK_TEST_HOST_MEMORY_TYPE].heapIndex == 0,
         "a device-local memory type, then a device-local, host-visible, host-coherent one");
   check(memory.memoryHeapCount == 1 && memory.memoryHeaps[0].size > 0 &&
            memory.memoryHeaps[0].flags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
         "one device-local heap of the direct memory size");
   printf("  (heap: %llu bytes)\n", (unsigned long long)memory.memoryHeaps[0].size);
}

static void
check_devices(VkInstance instance, VkPhysicalDevice physical)
{
   const __typeof__(&vkCreateDevice) create_device = VK_FUNCTION(instance, CreateDevice);
   const __typeof__(&vkDestroyDevice) destroy_device = VK_FUNCTION(instance, DestroyDevice);
   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };

   VkDevice device = VK_NULL_HANDLE;
   VkResult result = create_device(physical, &device_info, NULL, &device);
   check(result == VK_SUCCESS && device != VK_NULL_HANDLE, "vkCreateDevice with one queue");
   if (result == VK_SUCCESS) {
      VkQueue queue = VK_NULL_HANDLE;
      VK_FUNCTION(instance, GetDeviceQueue)(device, 0, 0, &queue);
      check(queue != VK_NULL_HANDLE, "vkGetDeviceQueue returns the queue");
      destroy_device(device, NULL);
   }

   const VkPhysicalDeviceFeatures geometry = {.geometryShader = VK_TRUE};
   device_info.pEnabledFeatures = &geometry;
   device = VK_NULL_HANDLE;
   result = create_device(physical, &device_info, NULL, &device);
   check(result == VK_ERROR_FEATURE_NOT_PRESENT, "an unsupported feature is rejected");
   if (result == VK_SUCCESS)
      destroy_device(device, NULL);
   device_info.pEnabledFeatures = NULL;

   const char *const swapchain[] = {"VK_KHR_swapchain"};
   device_info.enabledExtensionCount = 1;
   device_info.ppEnabledExtensionNames = swapchain;
   device = VK_NULL_HANDLE;
   result = create_device(physical, &device_info, NULL, &device);
   check(result == VK_SUCCESS, "VK_KHR_swapchain is supported (Phase C1)");
   if (result == VK_SUCCESS)
      destroy_device(device, NULL);

   const char *const unknown[] = {"VK_KHR_ps5vk_no_such_extension"};
   device_info.ppEnabledExtensionNames = unknown;
   device = VK_NULL_HANDLE;
   result = create_device(physical, &device_info, NULL, &device);
   check(result == VK_ERROR_EXTENSION_NOT_PRESENT, "an unsupported device extension is rejected");
   if (result == VK_SUCCESS)
      destroy_device(device, NULL);
}

/* R84: Vulkan 1.1. Its core device commands resolve by their core names on a
 * device the application created for 1.1, the 1.1 feature it requires
 * (multiview) is on and the optional ones off, and its properties say what the
 * driver implements: compute subgroups of 32 with the basic operations, eight
 * views, and no external memory. */
static void
check_vulkan11(VkInstance instance, VkPhysicalDevice physical)
{
   VkPhysicalDeviceVulkan11Features features11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
   VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &features11};
   VK_FUNCTION(instance, GetPhysicalDeviceFeatures2)(physical, &features2);
   check(features11.multiview && !features11.multiviewGeometryShader &&
            !features11.multiviewTessellationShader && !features11.storageBuffer16BitAccess &&
            !features11.variablePointers && !features11.protectedMemory &&
            !features11.samplerYcbcrConversion && !features11.shaderDrawParameters,
         "Vulkan 1.1 features: multiview, which 1.1 requires, and none of the optional ones");
   VkPhysicalDeviceVulkan11Properties properties11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
   VkPhysicalDeviceProperties2 properties2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &properties11};
   VK_FUNCTION(instance, GetPhysicalDeviceProperties2)(physical, &properties2);
   check(properties11.subgroupSize == 32 &&
            (properties11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
            (properties11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) &&
            properties11.maxMultiviewViewCount >= 6 &&
            properties11.maxMultiviewInstanceIndex >= (1u << 27) - 1 &&
            properties11.maxPerSetDescriptors >= 1024 &&
            properties11.maxMemoryAllocationSize >= (UINT64_C(1) << 30),
         "Vulkan 1.1 properties at or above the required values");
   VkExternalBufferProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
   const VkPhysicalDeviceExternalBufferInfo external_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   external.externalMemoryProperties.externalMemoryFeatures = 0xffffffffu;
   VK_FUNCTION(instance, GetPhysicalDeviceExternalBufferProperties)(physical, &external_info,
                                                                     &external);
   check(external.externalMemoryProperties.externalMemoryFeatures == 0,
         "no external memory handle type is supported");

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const VkPhysicalDeviceVulkan11Features enable11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .multiview = VK_TRUE};
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &enable11,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device = VK_NULL_HANDLE;
   const VkResult result =
      VK_FUNCTION(instance, CreateDevice)(physical, &device_info, NULL, &device);
   check(result == VK_SUCCESS, "vkCreateDevice with Vulkan 1.1's multiview enabled");
   if (result != VK_SUCCESS)
      return;
   const __typeof__(&vkGetDeviceProcAddr) get_device_proc =
      (__typeof__(&vkGetDeviceProcAddr))VK_FUNCTION(instance, GetDeviceProcAddr);
   static const char *const commands[] = {
      "vkBindBufferMemory2", "vkBindImageMemory2", "vkGetBufferMemoryRequirements2",
      "vkGetImageMemoryRequirements2", "vkGetImageSparseMemoryRequirements2",
      "vkTrimCommandPool", "vkGetDeviceQueue2", "vkCreateDescriptorUpdateTemplate",
      "vkDestroyDescriptorUpdateTemplate", "vkUpdateDescriptorSetWithTemplate",
      "vkGetDescriptorSetLayoutSupport", "vkCmdDispatchBase", "vkCmdSetDeviceMask",
      "vkGetDeviceGroupPeerMemoryFeatures", "vkCreateSamplerYcbcrConversion",
      "vkDestroySamplerYcbcrConversion",
   };
   unsigned resolved = 0;
   for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
      const bool found = get_device_proc(device, commands[i]) != NULL;
      if (!found)
         printf("  (%s does not resolve)\n", commands[i]);
      resolved += found;
   }
   check(resolved == sizeof(commands) / sizeof(commands[0]),
         "every Vulkan 1.1 core device command resolves by its core name");
   VK_FUNCTION(instance, DestroyDevice)(device, NULL);
}

int
main(void)
{
   test_begin("B2");
   check_instance_level();

   const __typeof__(&vkCreateInstance) create_instance = VK_FUNCTION(VK_NULL_HANDLE, CreateInstance);
   const char *const unknown[] = {"VK_EXT_ps5vk_no_such_extension"};
   const VkInstanceCreateInfo unknown_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = unknown,
   };
   VkInstance instance = VK_NULL_HANDLE;
   check(create_instance(&unknown_info, NULL, &instance) == VK_ERROR_EXTENSION_NOT_PRESENT,
         "an unknown instance extension is rejected");

   const VkApplicationInfo application = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "ps5vk-b2-test",
      /* R84: an application of the driver's own version, so the 1.1 core
       * commands are the ones it may call (check_vulkan11). */
      .apiVersion = VK_API_VERSION_1_1,
   };
   const char *const enabled[] = {kProperties2Extension};
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = enabled,
   };
   instance = VK_NULL_HANDLE;
   VkResult result = create_instance(&instance_info, NULL, &instance);
   check(result == VK_SUCCESS, "vkCreateInstance");
   if (result != VK_SUCCESS)
      return test_finish();

   const __typeof__(&vkEnumeratePhysicalDevices) enumerate_devices =
      VK_FUNCTION(instance, EnumeratePhysicalDevices);
   uint32_t device_count = 0;
   result = enumerate_devices(instance, &device_count, NULL);
   check(result == VK_SUCCESS && device_count == 1, "one physical device");
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   uint32_t written = 1;
   result = enumerate_devices(instance, &written, &physical);
   if (result == VK_SUCCESS && physical != VK_NULL_HANDLE) {
      check_properties(instance, physical);
      check_devices(instance, physical);
      check_vulkan11(instance, physical);
   } else {
      check(false, "vkEnumeratePhysicalDevices returns the device");
   }

   VK_FUNCTION(instance, DestroyInstance)(instance, NULL);
   return test_finish();
}
