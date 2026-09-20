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
   check(enumerate_version && enumerate_version(&version) == VK_SUCCESS &&
            version >= VK_API_VERSION_1_1,
         "instance version is at least 1.1");
#ifdef PS5VK_TEST_DIRECT
   check(version == VK_API_VERSION_1_3, "the driver reports instance version 1.3");
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
   check(p.apiVersion == VK_API_VERSION_1_0, "device API version 1.0");
   check(p.driverVersion == VK_MAKE_VERSION(0, 2, 0), "driver version 0.2.0");
   check(p.vendorID == 0x1002, "AMD vendor ID");
   check(p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, "integrated GPU");
   check(strcmp(p.deviceName, kDeviceName) == 0, "device name");

   const VkPhysicalDeviceLimits *const l = &p.limits;
   check(l->maxImageDimension2D == 4096 && l->maxFramebufferWidth == 4096 &&
            l->maxViewportDimensions[0] == 4096,
         "image, framebuffer and viewport limits 4096");
   check(l->maxBoundDescriptorSets == 4 && l->maxPerStageResources == 44,
         "4 descriptor sets, 44 per-stage resources (footnote 2)");
   check(l->maxDescriptorSetSamplers == 48 && l->maxDescriptorSetUniformBuffers == 36 &&
            l->maxDescriptorSetStorageImages == 12,
         "per-set descriptors for 3 shader stages (footnote 8)");
   check(l->maxTessellationGenerationLevel == 0 && l->maxGeometryOutputVertices == 0 &&
            l->maxFragmentDualSrcAttachments == 0 && l->maxClipDistances == 0,
         "limits of unsupported features are 0");
   check(l->maxViewports == 1 && l->maxSamplerAnisotropy == 1.0f &&
            l->maxDrawIndexedIndexValue == 0xffffff && l->maxDrawIndirectCount == 1,
         "single viewport, no anisotropy, 24-bit indices, single indirect draw");
   check(l->pointSizeRange[0] == 1.0f && l->pointSizeRange[1] == 1.0f &&
            l->lineWidthRange[1] == 1.0f && l->pointSizeGranularity == 0.0f,
         "points and lines of size 1 only");
   check(l->framebufferColorSampleCounts == (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT) &&
            l->storageImageSampleCounts == VK_SAMPLE_COUNT_1_BIT,
         "required sample counts");
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
      only_robust = only_robust && (flags[i] == VK_FALSE || &flags[i] == &features.robustBufferAccess);
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
   check(memory.memoryTypeCount == 1 &&
            memory.memoryTypes[0].propertyFlags ==
               (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
            memory.memoryTypes[0].heapIndex == 0,
         "one device-local, host-visible, host-coherent memory type");
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
      .apiVersion = VK_API_VERSION_1_0,
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
   } else {
      check(false, "vkEnumeratePhysicalDevices returns the device");
   }

   VK_FUNCTION(instance, DestroyInstance)(instance, NULL);
   return test_finish();
}
