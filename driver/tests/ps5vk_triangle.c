/*
 * PS5 Vulkan driver - triangles through the Vulkan API.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ps5vk_triangle.h.
 */

#include "ps5vk_triangle.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* Longer than the driver's 2 s completion wait (driver/ps5vk_queue.c). */
#define PS5VK_TRIANGLE_FENCE_TIMEOUT_NS UINT64_C(4000000000)
#define PS5VK_TRIANGLE_MAX_EXTENSIONS 64
#define PS5VK_TRIANGLE_MAX_FORMATS 16
#define PS5VK_TRIANGLE_MAX_DISPLAYS 4
/* The uniform buffer the m3 pixel shader was compiled for: one 16-byte
 * fragment-visible descriptor at set 0, binding 0 (Phase C3). */
#define PS5VK_TRIANGLE_UNIFORM_BYTES 16
/* The texture's two samplers (Phase C4): the nearest one the frames start
 * with, then the linear one ps5vk_triangle_set_texture_filter selects. */
#define PS5VK_TRIANGLE_TEXTURE_SAMPLERS 2
#define PS5VK_TRIANGLE_SAMPLER_NEAREST 0
#define PS5VK_TRIANGLE_SAMPLER_LINEAR 1
/* The m3-texture vertex layout a render-to-texture frame's two passes need: a
 * 16-byte record of a position and a texture coordinate, drawn with the two
 * attributes the canary's shaders declare (probes/m3-texture/compile.txt). The
 * fill pass is recorded with the caller's pipeline, so the caller's geometry
 * has to be this one. */
#define PS5VK_TRIANGLE_TEXTURE_VERTEX_STRIDE 16
#define PS5VK_TRIANGLE_TEXTURE_ATTRIBUTES 2
/* The larger of two unsigned extents; this file includes no util header. */
#define PS5VK_MAX2(a, b) ((a) > (b) ? (a) : (b))
/* The full-target quad the fill pass draws: two triangles over the whole target
 * (Phase C4's render-to-texture case), as six records of the layout above. */
#define PS5VK_TRIANGLE_FILL_VERTICES 6

/* A Vulkan function as a pointer of its prototype's type. */
#define CALL(triangle, name)                                                                     \
   ((__typeof__(&vk##name))(triangle)->get_instance_proc_addr((triangle)->instance, "vk" #name))

static bool
step(const struct ps5vk_triangle *triangle, const char *name, VkResult result, const char *detail)
{
   const bool passed = result == VK_SUCCESS;
   if (triangle->report && triangle->report->step)
      triangle->report->step(triangle->report->context, name, passed, (int)result,
                             detail ? detail : "");
   return passed;
}

/* A query's result, with VK_INCOMPLETE accepted: the program asks for the
 * first entries only. */
static VkResult
first_entries(VkResult result)
{
   return result == VK_INCOMPLETE ? VK_SUCCESS : result;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
forward_message(VkDebugUtilsMessageSeverityFlagsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                const VkDebugUtilsMessengerCallbackDataEXT *data, void *user_data)
{
   (void)types;
   const struct ps5vk_triangle_report *const report = user_data;
   /* Mesa reports the reasons for driver errors as warnings (vk_log.c). */
   const bool notable = (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0;
   if (report && report->step)
      report->step(report->context, "vk_message", !notable, 0,
                   data && data->pMessage ? data->pMessage : "");
   return VK_FALSE;
}

const char *
ps5vk_triangle_grouping_name(enum ps5vk_triangle_grouping grouping)
{
   switch (grouping) {
   case PS5VK_TRIANGLE_ONE_DRAW:
      return "one draw";
   case PS5VK_TRIANGLE_ONE_COMMAND_BUFFER:
      return "two draws in one command buffer";
   case PS5VK_TRIANGLE_TWO_COMMAND_BUFFERS:
      return "two command buffers in one VkSubmitInfo";
   case PS5VK_TRIANGLE_TWO_SUBMIT_INFOS:
      return "two VkSubmitInfos in one vkQueueSubmit";
   case PS5VK_TRIANGLE_TWO_SUBMISSIONS:
      return "two vkQueueSubmit calls";
   }
   return "unknown grouping";
}

/* The display, its 3840x2160 mode and a surface on plane 0, supported by
 * queue family 0. */
static bool
create_display_surface(struct ps5vk_triangle *triangle, VkPhysicalDevice physical)
{
   VkDisplayPropertiesKHR display;
   uint32_t count = 1;
   VkResult result = first_entries(
      CALL(triangle, GetPhysicalDeviceDisplayPropertiesKHR)(physical, &count, &display));
   if (result == VK_SUCCESS && count == 0)
      result = VK_ERROR_INITIALIZATION_FAILED;
   if (!step(triangle, "get_display", result,
             result == VK_SUCCESS && display.displayName ? display.displayName : "no display"))
      return false;

   VkDisplayKHR displays[PS5VK_TRIANGLE_MAX_DISPLAYS];
   count = PS5VK_TRIANGLE_MAX_DISPLAYS;
   result = first_entries(
      CALL(triangle, GetDisplayPlaneSupportedDisplaysKHR)(physical, 0, &count, displays));
   bool on_plane = false;
   for (uint32_t index = 0; result == VK_SUCCESS && index < count; index++)
      on_plane = on_plane || displays[index] == display.display;
   if (result == VK_SUCCESS && !on_plane)
      result = VK_ERROR_INITIALIZATION_FAILED;
   if (!step(triangle, "get_plane_displays", result, "plane 0 shows the display"))
      return false;

   VkDisplayModePropertiesKHR mode;
   count = 1;
   result = first_entries(
      CALL(triangle, GetDisplayModePropertiesKHR)(physical, display.display, &count, &mode));
   if (result == VK_SUCCESS && count == 0)
      result = VK_ERROR_INITIALIZATION_FAILED;
   const VkExtent2D region = result == VK_SUCCESS ? mode.parameters.visibleRegion : (VkExtent2D){0};
   if (result == VK_SUCCESS &&
       (region.width != PS5VK_TRIANGLE_WIDTH || region.height != PS5VK_TRIANGLE_HEIGHT))
      result = VK_ERROR_INITIALIZATION_FAILED;
   char detail[64];
   snprintf(detail, sizeof(detail), "%ux%u at %u mHz", (unsigned)region.width,
            (unsigned)region.height, result == VK_SUCCESS ? (unsigned)mode.parameters.refreshRate : 0u);
   if (!step(triangle, "get_display_mode", result, detail))
      return false;

   const VkDisplaySurfaceCreateInfoKHR surface_info = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = mode.displayMode,
      .planeIndex = 0,
      .planeStackIndex = 0,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .globalAlpha = 1.0f,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = region,
   };
   if (!step(triangle, "create_display_surface",
             CALL(triangle, CreateDisplayPlaneSurfaceKHR)(triangle->instance, &surface_info, NULL,
                                                          &triangle->surface),
             NULL))
      return false;
   VkBool32 supported = VK_FALSE;
   result = CALL(triangle, GetPhysicalDeviceSurfaceSupportKHR)(physical, 0, triangle->surface,
                                                               &supported);
   if (result == VK_SUCCESS && !supported)
      result = VK_ERROR_INITIALIZATION_FAILED;
   return step(triangle, "get_surface_support", result, "queue family 0 presents to the surface");
}

/* A FIFO swapchain of B8G8R8A8_UNORM images over the surface. */
static bool
create_swapchain(struct ps5vk_triangle *triangle, VkPhysicalDevice physical)
{
   VkSurfaceCapabilitiesKHR capabilities;
   if (!step(triangle, "get_surface_capabilities",
             CALL(triangle, GetPhysicalDeviceSurfaceCapabilitiesKHR)(physical, triangle->surface,
                                                                     &capabilities),
             NULL))
      return false;
   VkSurfaceFormatKHR formats[PS5VK_TRIANGLE_MAX_FORMATS];
   uint32_t count = PS5VK_TRIANGLE_MAX_FORMATS;
   VkResult result = first_entries(CALL(triangle, GetPhysicalDeviceSurfaceFormatsKHR)(
      physical, triangle->surface, &count, formats));
   triangle->format = VK_FORMAT_UNDEFINED;
   for (uint32_t index = 0; result == VK_SUCCESS && index < count; index++) {
      if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM &&
          formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
         triangle->format = VK_FORMAT_B8G8R8A8_UNORM;
   }
   if (result == VK_SUCCESS && triangle->format == VK_FORMAT_UNDEFINED)
      result = VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (!step(triangle, "get_surface_formats", result, "B8G8R8A8_UNORM in SRGB_NONLINEAR"))
      return false;

   /* One image more than the minimum, as the Vulkan Tutorial asks. */
   uint32_t image_count = capabilities.minImageCount + 1;
   if (capabilities.maxImageCount != 0 && image_count > capabilities.maxImageCount)
      image_count = capabilities.maxImageCount;
   const VkExtent2D extent = capabilities.currentExtent;
   char detail[96];
   snprintf(detail, sizeof(detail), "%ux%u, %u images", (unsigned)extent.width,
            (unsigned)extent.height, (unsigned)image_count);
   if (!step(triangle, "check_surface_capabilities",
             extent.width == PS5VK_TRIANGLE_WIDTH && extent.height == PS5VK_TRIANGLE_HEIGHT &&
                   image_count <= PS5VK_TRIANGLE_MAX_IMAGES &&
                   (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                ? VK_SUCCESS
                : VK_ERROR_INITIALIZATION_FAILED,
             detail))
      return false;

   triangle->swapchain_info = (VkSwapchainCreateInfoKHR){
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = triangle->surface,
      .minImageCount = image_count,
      .imageFormat = triangle->format,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    (triangle->display_readback ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
   };
   if (!step(triangle, "create_swapchain",
             CALL(triangle, CreateSwapchainKHR)(triangle->device, &triangle->swapchain_info, NULL,
                                                &triangle->swapchain),
             "FIFO"))
      return false;
   count = PS5VK_TRIANGLE_MAX_IMAGES;
   result = CALL(triangle, GetSwapchainImagesKHR)(triangle->device, triangle->swapchain, &count,
                                                  triangle->images);
   triangle->image_count = result == VK_SUCCESS ? count : 0;
   snprintf(detail, sizeof(detail), "%u images", (unsigned)triangle->image_count);
   return step(triangle, "get_swapchain_images", result, detail);
}

/* Whether a texture format's fetch reads a depth value (round 21's depth pair):
 * a depth image is a row layout and never a colour attachment. */
static bool
texture_is_depth(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_X8_D24_UNORM_PACK32:
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_D16_UNORM_S8_UINT:
   case VK_FORMAT_D24_UNORM_S8_UINT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
   default:
      return false;
   }
}

/* The program's own colour image on host-visible memory, mapped: R8G8B8A8_UNORM
 * unless the caller asked for another format the driver has a CB_COLOR0_INFO
 * word for (input->target_format). */
static bool
create_image(struct ps5vk_triangle *triangle, VkPhysicalDevice physical)
{
   /* A resolve's destination is the one-sample image a caller reads back; the
    * samples below belong to the image the frame renders into, which
    * create_resolve_target makes (Phase C8). */
   const VkSampleCountFlagBits samples =
      triangle->resolve_output ? VK_SAMPLE_COUNT_1_BIT : triangle->samples;
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = triangle->format,
      .extent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      /* A colour attachment and nothing else: a case that wants the frame's
       * bytes reads the mapped storage, which is what the target image is for.
       * R5's frame asks for the resolve destination's specification usage
       * instead -- TRANSFER_DST and SAMPLED, no colour attachment -- which this
       * driver stores in rows and refuses to resolve into (a resolve only walks
       * tiles, ps5vk_image.c). */
      .usage = triangle->resolve_destination_transfer_only
                  ? (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
                  : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (!step(triangle, "create_image",
             CALL(triangle, CreateImage)(triangle->device, &image_info, NULL, &triangle->images[0]),
             "3840x2160 colour attachment"))
      return false;
   triangle->image_count = 1;
   VkMemoryRequirements requirements;
   CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->images[0], &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         memory_type = index;
   }
   char detail[128];
   snprintf(detail, sizeof(detail), "%llu bytes, alignment %llu, memory type %u",
            (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
            (unsigned)memory_type);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   if (!step(triangle, "allocate_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                                 &triangle->memory),
             detail) ||
       !step(triangle, "bind_image_memory",
             CALL(triangle, BindImageMemory)(triangle->device, triangle->images[0],
                                             triangle->memory, 0),
             NULL) ||
       !step(triangle, "map_memory",
             CALL(triangle, MapMemory)(triangle->device, triangle->memory, 0, VK_WHOLE_SIZE, 0,
                                       &triangle->mapped),
             NULL))
      return false;
   triangle->memory_bytes = (size_t)requirements.size;
   triangle->colour_attachment_count = 1;
   triangle->target_mappings[0] = triangle->mapped;
   triangle->target_memory_bytes[0] = triangle->memory_bytes;
   return true;
}

/* A frame that declares more than one colour attachment: one image with its own
 * host-visible mapped memory per attachment, all in the frame's colour format and
 * at its sample count, with a view each. The first attachment's mapping is also
 * target/target_bytes, so a case reads attachment 0 the way it reads a
 * single-target frame and the others through target_mappings.
 *
 * The images are kept in images[] and views[] like the single target's, but
 * image_count stays 1: image_count is how many *frames* the output double-buffers
 * (the display's swapchain images), and each of those framebuffers names every
 * attachment at once. */
static bool
create_color_attachments(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
                         const struct ps5vk_triangle_input *input)
{
   const uint32_t count = input->color_attachment_count;
   if (count == 0 || count > PS5VK_TRIANGLE_MAX_TARGETS)
      return step(triangle, "colour attachments", VK_ERROR_INITIALIZATION_FAILED,
                  "a colour attachment count inside the advertised maximum");
   const VkSampleCountFlagBits samples =
      triangle->resolve_output ? VK_SAMPLE_COUNT_1_BIT : triangle->samples;
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   for (uint32_t at = 0; at < count; at++) {
      const VkImageCreateInfo image_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = triangle->format,
         .extent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = samples,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      char detail[96];
      snprintf(detail, sizeof(detail), "colour attachment %u of %u", at + 1, count);
      if (!step(triangle, "create_image",
                CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                            &triangle->images[at]),
                detail))
         return false;
      VkMemoryRequirements requirements;
      CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->images[at],
                                                 &requirements);
      uint32_t memory_type = UINT32_MAX;
      for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
           index++) {
         if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
             (memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            memory_type = index;
      }
      const VkMemoryAllocateInfo allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type,
      };
      if (memory_type == UINT32_MAX ||
          !step(triangle, "allocate_memory",
                CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                               &triangle->target_memories[at]),
                NULL) ||
          !step(triangle, "bind_image_memory",
                CALL(triangle, BindImageMemory)(triangle->device, triangle->images[at],
                                                triangle->target_memories[at], 0),
                NULL) ||
          !step(triangle, "map_memory",
                CALL(triangle, MapMemory)(triangle->device, triangle->target_memories[at], 0,
                                          VK_WHOLE_SIZE, 0, &triangle->target_mappings[at]),
                NULL))
         return false;
      triangle->target_memory_bytes[at] = (size_t)requirements.size;
      const VkImageViewCreateInfo view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = triangle->images[at],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = triangle->format,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      if (!step(triangle, "create_image_view",
                CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                                &triangle->views[at]),
                NULL))
         return false;
   }
   triangle->colour_attachment_count = count;
   triangle->image_count = 1;
   triangle->target = triangle->target_mappings[0];
   triangle->target_bytes = triangle->target_memory_bytes[0];
   triangle->mapped = triangle->target_mappings[0];
   triangle->memory_bytes = triangle->target_memory_bytes[0];
   return true;
}

/* One of the caller's buffers on host-visible memory, mapped and holding a
 * copy of data, which the vertex shader fetches (Phase C2). The memory is
 * host-coherent as well, so the copy needs no flush to reach the GPU. A NULL
 * mapped asks for a buffer that is left unmapped until a caller maps it to
 * read what a copy wrote; a NULL data leaves the mapping unwritten, for a
 * caller that fills several ranges of it itself. */
static bool
create_buffer(struct ps5vk_triangle *triangle, VkPhysicalDevice physical, VkDeviceSize size,
              VkBufferUsageFlags usage, const char *name, const void *data, VkBuffer *buffer,
              VkDeviceMemory *memory, void **mapped)
{
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (!step(triangle, "create_buffer",
             CALL(triangle, CreateBuffer)(triangle->device, &buffer_info, NULL, buffer), name))
      return false;
   VkMemoryRequirements requirements;
   CALL(triangle, GetBufferMemoryRequirements)(triangle->device, *buffer, &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (memory_properties.memoryTypes[index].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
         memory_type = index;
   }
   char detail[96];
   snprintf(detail, sizeof(detail), "%s: %llu bytes, memory type %u", name,
            (unsigned long long)size, (unsigned)memory_type);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   if (!step(triangle, "allocate_buffer_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL, memory),
             detail) ||
       !step(triangle, "bind_buffer_memory",
             CALL(triangle, BindBufferMemory)(triangle->device, *buffer, *memory, 0), name) ||
       (mapped != NULL &&
        !step(triangle, "map_buffer_memory",
              CALL(triangle, MapMemory)(triangle->device, *memory, 0, VK_WHOLE_SIZE, 0, mapped),
              name)))
      return false;
   if (mapped != NULL && data != NULL)
      memcpy(*mapped, data, (size_t)size);
   return true;
}

/* The caller's indexed geometry, uploaded to host-visible memory (Phase C2);
 * nothing at all when the caller draws the sets' unbound three-vertex
 * triangles. A caller that stages its geometry writes it into a staging
 * buffer instead, and a frame's copies move it into the buffers the draw
 * binds. */
static bool
create_geometry(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
                const struct ps5vk_triangle_input *input)
{
   /* The pipeline declares one binding of at most two attributes: the probe
    * sets' shaders take a position and one other, and a third would be
    * dropped. */
   if (input->attribute_count > PS5VK_TRIANGLE_MAX_ATTRIBUTES)
      return step(triangle, "vertex geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "vertex attribute count exceeds the harness limit");
   /* Vertex-less shaders generate their own triangle from gl_VertexIndex. */
   if (input->index_count == 0 && input->vertex_data == NULL)
      return true;
   /* A frame with vertices and no indices is a non-indexed draw -- a triangle
    * strip is one -- and its vertex buffer is as necessary as an indexed frame's. */
   if (input->vertex_data == NULL || input->vertex_count == 0 || input->vertex_stride == 0)
      return step(triangle, "vertex geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "indexed draws need vertex data, a vertex count and a stride");
   /* Indices are optional: a frame with vertices and none draws them with
    * VkCmdDraw, which is the shape a triangle strip is (input.primitive_topology),
    * and then it has no index buffer to create -- a VkBuffer of size 0 is invalid
    * usage, and Mesa's vk_buffer_init asserts on it. R6's first strip frame
    * created one anyway, and that assertion is the console's "abort is called"
    * after its vertex buffer was mapped (Klog_Logs/r6-strip.log): the title
    * aborted before the frame recorded anything, which is the "wedge" the
    * battery had to be killed for. */
   if ((input->index_count == 0) != (input->index_data == NULL))
      return step(triangle, "vertex geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "indices need both data and a count, or neither");
   triangle->vertex_count = input->vertex_count;
   triangle->index_count = input->index_count;
   const VkDeviceSize vertex_bytes = (VkDeviceSize)input->vertex_count * input->vertex_stride;
   if (input->index_type != VK_INDEX_TYPE_UINT16 && input->index_type != VK_INDEX_TYPE_UINT32)
      return step(triangle, "index type", VK_ERROR_INITIALIZATION_FAILED,
                  "the harness supports UINT16 and UINT32");
   triangle->index_type = input->index_type;
   const VkDeviceSize index_bytes = (VkDeviceSize)triangle->index_count *
                                    (input->index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2);
   if (input->stage_geometry) {
      /* Phase C2's upload: the geometry goes into one mapped staging buffer,
       * the records first and the indices right after them, and the copies a
       * frame records move it into the buffers the draw binds. Those two are
       * left unmapped -- the copy is the only thing that writes them -- and a
       * test maps them after the submission to read what the copy left. */
      if (!create_buffer(triangle, physical, vertex_bytes + index_bytes,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "staging", NULL,
                         &triangle->staging_buffer, &triangle->staging_memory,
                         &triangle->staging_mapped))
         return false;
      memcpy(triangle->staging_mapped, input->vertex_data, (size_t)vertex_bytes);
      if (index_bytes != 0)
         memcpy((char *)triangle->staging_mapped + (size_t)vertex_bytes, input->index_data,
                (size_t)index_bytes);
      if (!create_buffer(triangle, physical, vertex_bytes,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         "vertex", NULL, &triangle->vertex_buffer, &triangle->vertex_memory,
                         NULL) ||
          (index_bytes != 0 &&
           !create_buffer(triangle, physical, index_bytes,
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          "index", NULL, &triangle->index_buffer, &triangle->index_memory, NULL)))
         return false;
      triangle->staged = true;
      triangle->vertex_bytes = vertex_bytes;
      triangle->index_bytes = index_bytes;
   } else if (!create_buffer(triangle, physical, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             "vertex", input->vertex_data, &triangle->vertex_buffer,
                             &triangle->vertex_memory, &triangle->vertex_mapped) ||
              (index_bytes != 0 &&
               !create_buffer(triangle, physical, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              "index", input->index_data, &triangle->index_buffer,
                              &triangle->index_memory, &triangle->index_mapped))) {
      return false;
   }
   return true;
}

/* Points the set at the uniform buffer's range, which ps5vk_triangle_set_uniform
 * repeats when a caller replaces the bytes and ps5vk_triangle_set_uniform_range
 * replaces alone. The range is the buffer's unless the caller asked for a
 * shorter one (V0-robust: a bound a shader's read can fall outside of, which is
 * what makes a frame's descriptor different from its neighbour's). */
static void
update_uniform_descriptor(const struct ps5vk_triangle *triangle)
{
   const VkDescriptorBufferInfo buffer_info = {
      .buffer = triangle->uniform_buffer,
      .offset = 0,
      .range = triangle->uniform_range_bytes != 0 ? triangle->uniform_range_bytes
                                                 : triangle->uniform_bytes,
   };
   VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = triangle->descriptor_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = triangle->uniform_dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo = &buffer_info,
   };
   CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
   if (triangle->uniform_dynamic_pair) {
      write.dstBinding = 5;
      CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
      write.dstBinding = 2;
      write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
   }
}

/* The caller's uniform buffer and the descriptor set that names it (Phase C3),
 * or nothing at all when the caller passed no uniform data. The set layout is
 * created here so that ps5vk_triangle_create's pipeline layout can hold it: a
 * set layout must be alive when every pipeline layout that names it is
 * created, and ps5vk_triangle_finish destroys it only after that pipeline
 * layout is gone. */
static bool
create_uniform(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
               const struct ps5vk_triangle_input *input)
{
   if (input->uniform_data == NULL) {
      /* A caller that reads no descriptors must record and submit exactly what
       * it recorded before Phase C3: no set layout, no layout references, no
       * recording calls. A byte count without bytes is a mistake, not a
       * program that wants nothing. */
      if (input->uniform_bytes != 0)
         return step(triangle, "uniform geometry", VK_ERROR_INITIALIZATION_FAILED,
                     "a uniform buffer needs data");
      return true;
   }
   if (input->uniform_bytes == 0)
      return step(triangle, "uniform geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "a uniform buffer needs bytes");
   /* The m3 probes are what the console's hardware runs compiled against:
    * --descriptor-binding 0:0:uniform_buffer:1:0:16, a 16-byte fragment-stage
    * buffer (tools/build-probe-shaders.sh, probes/m3/PROVENANCE.txt). A larger
    * range would name bytes the shader was never told about. */
   /* One 16-byte colour, or the 32 bytes a dynamic binding moves its 16-byte
    * range through (Phase D1): the shader's declaration stays one colour. */
   /* R62: a larger block of whole 16-byte rows, up to 64 KiB, for a shader that
    * declares one (a uniform array); its compile options declare the binding
    * the same way, 16-byte rows. */
   if (input->uniform_bytes % PS5VK_TRIANGLE_UNIFORM_BYTES != 0 || input->uniform_bytes > 65536)
      return step(triangle, "uniform geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "the uniform buffer holds whole 16-byte rows, at most 64 KiB");
   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      /* A dynamic offset is the application's choice of where in the bound
       * range the descriptor starts (Phase D1's dynamic uniform buffer). */
      .descriptorType = input->uniform_dynamic_offset != 0
                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                           : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      /* The stage the caller's shader reads the buffer from: the m3 pixel
       * shader is a fragment-stage read, the c3-quad vertex shader a
       * vertex-stage one, and a binding counts for a stage only if its layout
       * declares it for that stage (driver/ps5vk_pipeline.c,
       * ps5vk_descriptor_options). */
      .stageFlags = input->uniform_stages,
   };
   VkDescriptorSetLayoutBinding pair_bindings[3] = {binding, binding, binding};
   pair_bindings[0].binding = 5;
   pair_bindings[1].binding = 2;
   pair_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   /* Deliberately unsorted: dynamic offsets follow binding number, not input order. */
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = input->uniform_dynamic_pair ? 3u : 1u,
      .pBindings = input->uniform_dynamic_pair ? pair_bindings : &binding,
   };
   if (!step(triangle, "create_descriptor_set_layout",
             CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &set_info, NULL,
                                                       &triangle->set_layout),
             "one uniform buffer at set 0, binding 0"))
      return false;
   /* One uniform-buffer descriptor, for the one set a frame binds, of the type
    * the set's layout declares: a dynamic binding draws its pool entry from
    * UNIFORM_BUFFER_DYNAMIC (Phase D1). */
   const VkDescriptorPoolSize pool_sizes[2] = {{
      .type = input->uniform_dynamic_offset != 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = input->uniform_dynamic_pair ? 2u : 1u,
   }, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = input->uniform_dynamic_pair ? 2u : 1u,
      .pPoolSizes = pool_sizes,
   };
   if (!step(triangle, "create_descriptor_pool",
             CALL(triangle, CreateDescriptorPool)(triangle->device, &pool_info, NULL,
                                                  &triangle->descriptor_pool),
             NULL))
      return false;
   if (!create_buffer(triangle, physical, input->uniform_bytes,
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "uniform", input->uniform_data,
                      &triangle->uniform_buffer, &triangle->uniform_memory,
                      &triangle->uniform_mapped))
      return false;
   triangle->uniform_bytes = input->uniform_bytes;
   if (input->uniform_range_bytes > input->uniform_bytes)
      return step(triangle, "uniform geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "the bound range is longer than the uniform buffer");
   triangle->uniform_range_bytes = input->uniform_range_bytes;
   triangle->uniform_dynamic = input->uniform_dynamic_offset != 0;
   triangle->uniform_dynamic_pair = input->uniform_dynamic_pair;
   const VkDescriptorSetAllocateInfo set_allocate = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = triangle->descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &triangle->set_layout,
   };
   if (!step(triangle, "allocate_descriptor_set",
             CALL(triangle, AllocateDescriptorSets)(triangle->device, &set_allocate,
                                                    &triangle->descriptor_set),
             NULL))
      return false;
   update_uniform_descriptor(triangle);
   return true;
}

bool
ps5vk_triangle_set_uniform(struct ps5vk_triangle *triangle, const void *data, uint32_t bytes)
{
   if (triangle->uniform_buffer == VK_NULL_HANDLE || triangle->descriptor_set == VK_NULL_HANDLE)
      return false;
   if (data == NULL || bytes == 0 || (VkDeviceSize)bytes > triangle->uniform_bytes)
      return false;
   /* The buffer's memory is host-coherent, as create_buffer asks, so the copy
    * is what the GPU reads and no flush is needed -- and a caller's bytes are
    * at most the range the shader reads, so no shorter range is written below.
    * The update is not needed for the copy to reach the shader, but a caller
    * that replaces the buffer is the application changing the descriptor's
    * contents, which is what the set names. */
   if (triangle->uniform_mapped == NULL)
      return false;
   memcpy(triangle->uniform_mapped, data, bytes);
   update_uniform_descriptor(triangle);
   return true;
}

/* Raises the index count a frame's draws ask for, past what the buffer holds
 * (V0-robust): the driver clamps it to the bound, so the frame draws the same
 * geometry. */
bool
ps5vk_triangle_set_draw_index_count(struct ps5vk_triangle *triangle, uint32_t count)
{
   if (triangle->index_buffer == VK_NULL_HANDLE || count == 0)
      return false;
   triangle->draw_index_count = count;
   return true;
}

/* Moves the dynamic uniform buffer's offset, which the next frames' draws bind
 * the set with (Phase D1). Only a frame whose layout declared the dynamic type
 * may do this. */
bool
ps5vk_triangle_set_uniform_offset(struct ps5vk_triangle *triangle, uint32_t offset)
{
   if (triangle->uniform_buffer == VK_NULL_HANDLE || triangle->descriptor_set == VK_NULL_HANDLE)
      return false;
   if ((VkDeviceSize)offset + (triangle->uniform_range_bytes != 0
                                  ? triangle->uniform_range_bytes
                                  : triangle->uniform_bytes) >
       triangle->uniform_bytes)
      return false;
   triangle->uniform_dynamic_offset = offset;
   return true;
}

/* Binds a shorter range of the uniform buffer than the whole of it, or the
 * whole buffer again with 0 (V0-robust). The descriptor is rewritten, so the
 * next frame's draws record the new bound. */
bool
ps5vk_triangle_set_uniform_range(struct ps5vk_triangle *triangle, uint32_t bytes)
{
   if (triangle->uniform_buffer == VK_NULL_HANDLE || triangle->descriptor_set == VK_NULL_HANDLE)
      return false;
   if ((VkDeviceSize)bytes > triangle->uniform_bytes)
      return false;
   triangle->uniform_range_bytes = bytes;
   update_uniform_descriptor(triangle);
   return true;
}

/* The two samplers the texture's frames filter with (Phase C4), nearest first
 * and linear second. Clamp-to-edge, one level, no mip filtering: exactly the
 * state the M3 texture canary ran and the only state ps5vk_CreateSampler
 * accepts (driver/ps5vk_image.c). A frame changes which one the descriptor set
 * names, as an application would, rather than recreating a sampler. */
static bool
create_texture_samplers(struct ps5vk_triangle *triangle)
{
   /* A frame that samples a mip chain (Phase C7) asks for the chain's own mip
    * filter and its levels; every earlier frame keeps the single-level samplers
    * and their recorded words. */
   const bool mips = triangle->texture_levels > 1;
   /* R2: the address mode a caller asked for, or the clamp-to-edge the M3
    * texture canary ran. */
   const VkSamplerAddressMode address = triangle->texture_address_mode_set
                                           ? triangle->texture_address_mode
                                           : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   const VkSamplerCreateInfo nearest = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      /* R7's anisotropy probe: a frame may ask for the flag, which at this
       * device's maxSamplerAnisotropy of 1.0 cannot change a fetch. */
      .anisotropyEnable = triangle->sampler_anisotropy,
      .maxAnisotropy = triangle->sampler_max_anisotropy,
      .mipmapMode = mips && triangle->texture_mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                         : VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = address,
      .addressModeV = address,
      .addressModeW = address,
      .maxLod = mips ? triangle->texture_max_lod : 0.0f,
   };
   for (unsigned index = 0; index < PS5VK_TRIANGLE_TEXTURE_SAMPLERS; index++) {
      VkSamplerCreateInfo info = nearest;
      if (index == PS5VK_TRIANGLE_SAMPLER_LINEAR) {
         info.magFilter = VK_FILTER_LINEAR;
         info.minFilter = VK_FILTER_LINEAR;
      }
      char detail[96];
      snprintf(detail, sizeof(detail), "%s, address mode %d, %s",
               index == PS5VK_TRIANGLE_SAMPLER_LINEAR ? "linear" : "nearest", (int)address,
               mips ? (triangle->texture_mip_linear ? "a linear mip chain" : "a nearest mip chain")
                    : "one level");
      if (!step(triangle, "create_sampler",
                CALL(triangle, CreateSampler)(triangle->device, &info, NULL,
                                              &triangle->texture_samplers[index]),
                detail))
         return false;
   }
   return true;
}

/* Points one of the texture's sets at a view and one of its two samplers, which
 * ps5vk_triangle_set_texture_filter repeats when a caller changes the filter:
 * the application replacing the sampler a descriptor names. */
static void
update_texture_set(const struct ps5vk_triangle *triangle, VkDescriptorSet set, VkImageView view,
                   bool bilinear)
{
   const VkDescriptorImageInfo image_info = {
      .sampler = triangle->texture_samplers[bilinear ? PS5VK_TRIANGLE_SAMPLER_LINEAR
                                                     : PS5VK_TRIANGLE_SAMPLER_NEAREST],
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &image_info,
   };
   CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
}

/* Points the set a frame's pixel shader samples through at the image it
 * samples, in the filter the frame runs: the caller's uploaded texels, or --
 * when the frame renders into the texture first -- the image it rendered into,
 * which is the view this set has to name for the sample to read it back. */
/* R2's separated form: the same view and sampler a combined write puts in one
 * entry, in two sets with their own types -- VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
 * names the view and no sampler, VK_DESCRIPTOR_TYPE_SAMPLER names the sampler and
 * no view, which is what Vulkan's valid usage for each asks. */
static void
update_separated_texture_sets(const struct ps5vk_triangle *triangle)
{
   const VkSampler sampler =
      triangle->texture_samplers[triangle->texture_bilinear ? PS5VK_TRIANGLE_SAMPLER_LINEAR
                                                            : PS5VK_TRIANGLE_SAMPLER_NEAREST];
   const VkDescriptorImageInfo image_info = {
      .sampler = VK_NULL_HANDLE,
      .imageView = triangle->texture_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   const VkDescriptorImageInfo sampler_info = {
      .sampler = sampler,
      .imageView = VK_NULL_HANDLE,
      .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   const VkWriteDescriptorSet writes[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstSet = triangle->texture_set,
       .dstBinding = 0,
       .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
       .pImageInfo = &image_info},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstSet = triangle->sampler_set,
       .dstBinding = 0,
       .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
       .pImageInfo = &sampler_info},
   };
   CALL(triangle, UpdateDescriptorSets)(triangle->device, 2, writes, 0, NULL);
}

static void
update_texture_descriptor(const struct ps5vk_triangle *triangle)
{
   if (triangle->separated_texture_pair) {
      update_separated_texture_sets(triangle);
      return;
   }

   /* The image the frame samples: the caller's uploaded texels, or -- when the
    * frame renders into the texture first, or copies the texels into a tiled
    * one (Phase C7) -- that image, whose view is the one this set has to name
    * for the sample to read back what the render or the copy left there. */
   const VkImageView sampled =
       triangle->texture_is_rendered
           ? triangle->rendered_view
           : (triangle->texture_copied || triangle->texture_blitted || triangle->texture_blit_scaled)
                ? triangle->copied_view
                : triangle->texture_view;
   update_texture_set(triangle, triangle->texture_set, sampled, triangle->texture_bilinear);
}

/* Points the fill pass's set at the caller's uploaded texels and the nearest
 * sampler: that pass copies the pattern into the image the frame samples with
 * one image pixel per source texel block, which only an exact nearest sample
 * reproduces. ps5vk_triangle_set_texture_filter never touches this set, so the
 * frame's bilinear run leaves the copy exact. */
static void
update_texture_source_descriptor(const struct ps5vk_triangle *triangle)
{
   update_texture_set(triangle, triangle->texture_source_set, triangle->texture_view, false);
}

/* The caller's texture and the objects that sample it (Phase C4): an
 * R8G8B8A8_UNORM sampled image the texels are copied into, the view and the two
 * samplers a combined image sampler names, and the set the caller's pixel
 * shader reads at set 0, binding 0. The image is not an attachment: the driver
 * stores it row-major with rows padded to 256 bytes, which is the pitch its
 * descriptor encodes and what its copy from the staging buffer accounts for
 * (driver/ps5vk_image.c, PS5VK_IMAGE_STORAGE_ROWS), so a row that is whole
 * 256-byte rows -- the canary's 64 texels -- copies without a padded source
 * pitch. Nothing at all when the caller passed no texture. */
/* The bytes per texel of the formats the sampled image may be created with
 * (Phase V0-formats), which are the ones driver/ps5vk_image.c's table carries a
 * descriptor word for; anything else is refused rather than sampled wrongly. */
static uint32_t
texture_texel_bytes(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   /* The one- and two-channel signed-normalised forms, whose entries the fetch
    * rows gained (round 6). */
   case VK_FORMAT_R8_SNORM:
   /* V0-formats' integer families, which the sampled probes upload too. */
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
      return 1;
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R16_UNORM:
   /* The depth formats a sampled frame uploads (round 13). */
   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8_UINT:
   /* V0-formats' packed two-byte families (run_vulkan_format_sample_frames). */
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
   case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
      return 2;
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
   case VK_FORMAT_R8G8B8A8_SRGB:
   /* Round 13's byte-reversed sRGB form, four bytes a texel like its twin. */
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_A8B8G8R8_UINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SINT_PACK32:
   /* Its packed four-byte families: the two shared-exponent forms and the
    * ten-bit one. */
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return 4;
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
      return 8;
   case VK_FORMAT_R32G32B32A32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
      return 16;
   default:
      return 0;
   }
}

/* R7's vkQuake shape: three sampled images at set 0's bindings 0, 1 and 2, one
 * descriptor set that names all three, and the samplers the frame's fragment
 * stage samples them through. The uniform block's set is not built here: it is
 * create_uniform's, which every other frame binds at set 0 and this shape places
 * at set 1 (ps5vk_triangle_create).
 *
 * The texels are the caller's: each image's memory is mapped and reported
 * through multiset_mappings, because a 64-texel RGBA8 row is exactly the
 * 256-byte row this driver pads to and the caller -- the console runner -- can
 * write and clflush it itself. That keeps this shape's uploads out of the
 * copy-and-split machinery create_texture uses, which is what a frame that is
 * about descriptor sets and not about uploads wants. */
static bool
create_multiset_textures(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
                         const struct ps5vk_triangle_input *input)
{
   if (!input->textures_in_first_set)
      return true;
   const void *const texels[PS5VK_TRIANGLE_MULTISET_TEXTURES] = {
      input->texture_data, input->texture_data_second, input->texture_data_third};
   for (unsigned index = 0; index < PS5VK_TRIANGLE_MULTISET_TEXTURES; index++) {
      if (texels[index] == NULL)
         return step(triangle, "multiset texture", VK_ERROR_INITIALIZATION_FAILED,
                     "the vkQuake shape needs three textures");
   }
   if (input->texture_width == 0 || input->texture_height == 0 ||
       input->texture_format != VK_FORMAT_R8G8B8A8_UNORM)
      return step(triangle, "multiset texture", VK_ERROR_FORMAT_NOT_SUPPORTED,
                  "three 64-texel-wide RGBA8 images, the row this driver records");
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   for (unsigned index = 0; index < PS5VK_TRIANGLE_MULTISET_TEXTURES; index++) {
      const VkImageCreateInfo image_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = input->texture_format,
         .extent = {input->texture_width, input->texture_height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      char detail[96];
      snprintf(detail, sizeof(detail), "image %u of %u, %ux%u RGBA8", index + 1,
               (unsigned)PS5VK_TRIANGLE_MULTISET_TEXTURES, input->texture_width,
               input->texture_height);
      if (!step(triangle, "create_multiset_image",
                CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                            &triangle->multiset_images[index]),
                detail))
         return false;
      VkMemoryRequirements requirements;
      CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->multiset_images[index],
                                                 &requirements);
      uint32_t memory_type = UINT32_MAX;
      for (uint32_t type = 0; type < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
           type++) {
         if ((requirements.memoryTypeBits & (UINT32_C(1) << type)) &&
             (memory_properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            memory_type = type;
      }
      const VkMemoryAllocateInfo allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type,
      };
      if (memory_type == UINT32_MAX ||
          !step(triangle, "allocate_multiset_memory",
                CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                               &triangle->multiset_memories[index]),
                NULL) ||
          !step(triangle, "bind_multiset_memory",
                CALL(triangle, BindImageMemory)(triangle->device, triangle->multiset_images[index],
                                                triangle->multiset_memories[index], 0),
                NULL) ||
          !step(triangle, "map_multiset_memory",
                CALL(triangle, MapMemory)(triangle->device, triangle->multiset_memories[index], 0,
                                          VK_WHOLE_SIZE, 0, &triangle->multiset_mappings[index]),
                NULL))
         return false;
      triangle->multiset_bytes[index] = (size_t)requirements.size;
      triangle->texture_extent = (VkExtent2D){input->texture_width, input->texture_height};
      const VkImageViewCreateInfo view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = triangle->multiset_images[index],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = input->texture_format,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      if (!step(triangle, "create_multiset_view",
                CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                                &triangle->multiset_views[index]),
                NULL))
         return false;
   }
   /* One set layout with three image-sampler bindings and one set that names
    * them: the table the driver has to size from this set's own bindings. */
   VkDescriptorSetLayoutBinding bindings[PS5VK_TRIANGLE_MULTISET_TEXTURES];
   for (unsigned index = 0; index < PS5VK_TRIANGLE_MULTISET_TEXTURES; index++) {
      bindings[index] = (VkDescriptorSetLayoutBinding){
         .binding = index,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };
   }
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = PS5VK_TRIANGLE_MULTISET_TEXTURES,
      .pBindings = bindings,
   };
   if (!step(triangle, "create_multiset_set_layout",
             CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &set_info, NULL,
                                                       &triangle->multiset_set_layout),
             "three combined image samplers at set 0, bindings 0, 1 and 2") ||
       !create_texture_samplers(triangle))
      return false;
   const VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = PS5VK_TRIANGLE_MULTISET_TEXTURES,
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   if (!step(triangle, "create_multiset_pool",
             CALL(triangle, CreateDescriptorPool)(triangle->device, &pool_info, NULL,
                                                  &triangle->multiset_pool),
             NULL))
      return false;
   const VkDescriptorSetAllocateInfo allocate_set = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = triangle->multiset_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &triangle->multiset_set_layout,
   };
   if (!step(triangle, "allocate_multiset_set",
             CALL(triangle, AllocateDescriptorSets)(triangle->device, &allocate_set,
                                                    &triangle->multiset_set),
             NULL))
      return false;
   VkDescriptorImageInfo images[PS5VK_TRIANGLE_MULTISET_TEXTURES];
   VkWriteDescriptorSet writes[PS5VK_TRIANGLE_MULTISET_TEXTURES];
   for (unsigned index = 0; index < PS5VK_TRIANGLE_MULTISET_TEXTURES; index++) {
      images[index] = (VkDescriptorImageInfo){
         .sampler = triangle->texture_samplers[PS5VK_TRIANGLE_SAMPLER_NEAREST],
         .imageView = triangle->multiset_views[index],
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      writes[index] = (VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = triangle->multiset_set,
         .dstBinding = index,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &images[index],
      };
   }
   CALL(triangle, UpdateDescriptorSets)(triangle->device, PS5VK_TRIANGLE_MULTISET_TEXTURES, writes,
                                        0, NULL);
   return true;
}

static bool
create_texture(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
               const struct ps5vk_triangle_input *input)
{
   /* R7's vkQuake shape builds its own three images and their set
    * (create_multiset_textures); this path's one texture and one set would be a
    * fourth binding and a second set layout nothing asked for. */
   if (input->textures_in_first_set)
      return true;
   if (input->texture_data == NULL) {
      /* A caller that samples nothing must record and submit exactly what it
       * recorded before Phase C4: no image, no set, no copy and no descriptor
       * binding. A size without bytes is a mistake, not a program that wants
       * nothing. */
      if (input->texture_width != 0 || input->texture_height != 0)
         return step(triangle, "texture geometry", VK_ERROR_INITIALIZATION_FAILED,
                     "a texture needs data");
      return true;
   }
   if (input->texture_width == 0 || input->texture_height == 0)
      return step(triangle, "texture geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "a texture needs a width and a height");

   /* Phase C7: a chain of levels, or the single level every earlier frame
    * sampled. Phase V0-formats: the format the frame samples, whose texels
    * input->texture_data holds, tightly packed. */
   const uint32_t levels = input->texture_levels > 1 ? input->texture_levels : 1u;
   const VkFormat format = input->texture_format != VK_FORMAT_UNDEFINED
                              ? input->texture_format
                              : VK_FORMAT_R8G8B8A8_UNORM;
   const uint32_t texel_bytes = texture_texel_bytes(format);
   if (texel_bytes == 0)
      return step(triangle, "create_texture_image", VK_ERROR_FORMAT_NOT_SUPPORTED,
                  "a texture format this driver reports a descriptor word for");
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {input->texture_width, input->texture_height, 1},
      .mipLevels = levels,
      .arrayLayers = input->texture_layers > 1 ? input->texture_layers : 1u,
      /* A cube is six layers of a cube-compatible image (D1): the flag is what
       * makes the view a cube and the descriptor's TYPE 11. */
      .flags = input->texture_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      /* A tiled chain is stored the way the driver stores an attachment, which
       * its colour-attachment usage is what asks for; the caller fills its
       * texels, so no copy follows (input->texture_tiled). */
      /* A tiled chain is an attachment: the caller fills it, or -- when it asked
       * for the upload -- the driver does, through the copies a transfer
       * destination allows. A linear image is nobody's attachment and keeps the
       * usage it always had, or the driver would store its texels in tiles. */
      /* A depth format is nobody's colour attachment: its image stays the row
       * layout the sampler reads through that format's own fetch word, which is
       * what round 21's depth pair samples and blits from. */
      .usage = input->texture_upload && !texture_is_depth(input->texture_format)
                  ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       (input->texture_blit_mips ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0)
              : input->texture_tiled && !texture_is_depth(input->texture_format)
                  ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                  : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   const char *const name = input->texture_tiled && !input->texture_upload
                               ? "the caller's texels, tiled and sampled"
                               : "the caller's texels, sampled and copied into";
   if (!step(triangle, "create_texture_image",
             CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                         &triangle->texture_image),
             name))
      return false;
   triangle->texture_format = format;
   VkMemoryRequirements requirements;
   CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->texture_image,
                                              &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[index].propertyFlags;
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         memory_type = index;
   }
   char detail[128];
   snprintf(detail, sizeof(detail), "%llu bytes, alignment %llu, memory type %u",
            (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
            (unsigned)memory_type);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   /* The image memory is mapped as the colour target's is: the copy's
    * destination is the image storage itself, so a test reads what the upload
    * left there once the fence has signalled. */
   if (!step(triangle, "allocate_texture_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                                 &triangle->texture_memory),
             detail) ||
       !step(triangle, "bind_texture_memory",
             CALL(triangle, BindImageMemory)(triangle->device, triangle->texture_image,
                                             triangle->texture_memory, 0),
             NULL) ||
       !step(triangle, "map_texture_memory",
             CALL(triangle, MapMemory)(triangle->device, triangle->texture_memory, 0,
                                       VK_WHOLE_SIZE, 0, &triangle->texture_mapped),
             NULL))
      return false;
   triangle->texture_bytes = (size_t)requirements.size;
   triangle->texture_extent = (VkExtent2D){input->texture_width, input->texture_height};

   /* The texels reach the image through this mapped staging buffer: the
    * caller's rows, tightly packed, which is what a copy's rowLength of 0
    * means. The driver runs the copy on the CPU at a submission split point. */
   uint8_t *level_texels = NULL;
   const void *staging_texels = input->texture_data;
   const uint32_t layers = input->texture_layers > 1 ? input->texture_layers : 1u;
   VkDeviceSize texels =
     (VkDeviceSize)input->texture_width * input->texture_height * texel_bytes;
   if (input->texture_level_colours != NULL) {
      /* Every texel of level L of layer Y is texture_level_colours[Y * levels +
       * L], four bytes each: a layer repeats the level stack, which is the order
       * the staging offsets use (triangle->texture_level_offsets). One layer is
       * the level-indexed table every frame before D1 passed. */
      VkDeviceSize total = 0;
      for (uint32_t layer = 0; layer < layers; layer++)
         for (uint32_t level = 0; level < levels; level++)
            total += (VkDeviceSize)PS5VK_MAX2(input->texture_width >> level, 1u) *
                     PS5VK_MAX2(input->texture_height >> level, 1u) * 4;
      level_texels = malloc((size_t)total);
      if (!level_texels)
         return step(triangle, "texture level texels", VK_ERROR_OUT_OF_HOST_MEMORY, NULL);
      VkDeviceSize at = 0;
      for (uint32_t layer = 0; layer < layers; layer++) {
         for (uint32_t level = 0; level < levels; level++) {
            const uint32_t width = PS5VK_MAX2(input->texture_width >> level, 1u);
            const uint32_t height = PS5VK_MAX2(input->texture_height >> level, 1u);
            const uint8_t *const colour =
               input->texture_level_colours + ((size_t)layer * levels + level) * 4;
            for (VkDeviceSize texel = 0; texel < (VkDeviceSize)width * height; texel++)
               memcpy(level_texels + at + texel * 4, colour, 4);
            at += (VkDeviceSize)width * height * 4;
         }
      }
      staging_texels = level_texels;
      texels = total;
   }
   const bool staged_texels =
      create_buffer(triangle, physical, texels, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "texture",
                    staging_texels, &triangle->texture_staging_buffer,
                    &triangle->texture_staging_memory, &triangle->texture_staging_mapped);
   free(level_texels);
   if (!staged_texels)
      return false;

   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = triangle->texture_image,
      .viewType = input->texture_cube                             ? VK_IMAGE_VIEW_TYPE_CUBE
                  : layers > 1 || input->texture_array_view       ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                  : VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      /* The caller's component mapping, which is the identity for every frame
       * that leaves the field out (VK_COMPONENT_SWIZZLE_IDENTITY is zero). */
      .components = input->texture_components,
      /* The view names the levels the caller asked for: the whole chain, or
       * level 0 alone for the control (Phase C7). */
      .subresourceRange = {texture_is_depth(input->texture_format) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                 : VK_IMAGE_ASPECT_COLOR_BIT,
                           input->texture_view_base_level,
                           input->texture_view_levels != 0 && input->texture_view_levels < levels
                              ? input->texture_view_levels
                              : levels - input->texture_view_base_level,
                           0, layers},
   };
   if (!step(triangle, "create_texture_view",
             CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                             &triangle->texture_view),
             NULL) ||
       !create_texture_samplers(triangle))
      return false;

   /* See input->separated_texture_pair: the same view in set 0 and the sampler in
    * set 1, each with its own type. */
   const bool separated = input->separated_texture_pair;
   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = separated ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      /* The pixel stage is what reads it: the m3-texture canary's fragment
       * shader, and only for that stage does the driver's compiler option
       * name the binding (driver/ps5vk_pipeline.c, ps5vk_descriptor_options). */
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   if (!step(triangle, "create_texture_set_layout",
             CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &set_info, NULL,
                                                       &triangle->texture_set_layout),
             separated ? "one sampled image at set 0, binding 0"
                       : "one combined image sampler at set 0, binding 0"))
      return false;
   const VkDescriptorSetLayoutBinding sampler_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo sampler_set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &sampler_binding,
   };
   if (separated &&
       !step(triangle, "create_sampler_set_layout",
             CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &sampler_set_info, NULL,
                                                       &triangle->sampler_set_layout),
             "one sampler at set 1, binding 0"))
      return false;
   /* One combined image sampler descriptor, for the one set a frame binds --
    * two when the frame renders into the image it samples, whose fill pass
    * needs a set of its own that names the uploaded texels. */
   const uint32_t sets = input->texture_is_rendered ? 2u : 1u;
   const VkDescriptorPoolSize pool_sizes[2] = {
      {.type = separated ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = sets},
      {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1},
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = sets + (separated ? 1u : 0u),
      .poolSizeCount = separated ? 2u : 1u,
      .pPoolSizes = pool_sizes,
   };
   if (!step(triangle, "create_texture_pool",
             CALL(triangle, CreateDescriptorPool)(triangle->device, &pool_info, NULL,
                                                  &triangle->texture_pool),
             NULL))
      return false;
   const VkDescriptorSetLayout layouts[2] = {triangle->texture_set_layout,
                                             triangle->texture_set_layout};
   VkDescriptorSet allocated[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   const VkDescriptorSetAllocateInfo set_allocate = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = triangle->texture_pool,
      .descriptorSetCount = sets,
      .pSetLayouts = layouts,
   };
   if (!step(triangle, "allocate_texture_set",
             CALL(triangle, AllocateDescriptorSets)(triangle->device, &set_allocate, allocated),
             NULL))
      return false;
   triangle->texture_set = allocated[0];
   triangle->texture_source_set = allocated[1];
   if (separated) {
      const VkDescriptorSetAllocateInfo sampler_allocate = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = triangle->texture_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &triangle->sampler_set_layout,
      };
      if (!step(triangle, "allocate_sampler_set",
                CALL(triangle, AllocateDescriptorSets)(triangle->device, &sampler_allocate,
                                                       &triangle->sampler_set),
                "the sampler's own set"))
         return false;
   }
   triangle->texture_bilinear = input->texture_bilinear;
   update_texture_descriptor(triangle);
   if (triangle->texture_source_set != VK_NULL_HANDLE)
      update_texture_source_descriptor(triangle);
   return true;
}

/* The caller's uniform texel buffer and the objects that fetch it (V0-formats'
 * descriptor-type rows): a buffer holding the caller's texels, the view that
 * names them as one element of one format each, and the set 0 binding 0
 * VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER descriptor the caller's pixel shader
 * reads -- the binding the compiler is told about
 * (tooling/psbc/patch-descriptor-types.py) and the driver writes as a hardware
 * V# (driver/ps5vk_draw.c). The set is the program's one set at 0, so it takes
 * texture_set/texture_set_layout, and a caller cannot ask for both a texture
 * and a texel buffer. Nothing at all when the caller passed neither. */
static bool
create_texel_buffer(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
                    const struct ps5vk_triangle_input *input)
{
   if (input->texel_buffer_data == NULL) {
      /* A caller that fetches nothing records and submits exactly what it
       * recorded before this path existed: no buffer, no view, no set. A size
       * or a format without bytes is a mistake, not a program that wants
       * nothing. */
      if (input->texel_buffer_bytes != 0 || input->texel_buffer_format != VK_FORMAT_UNDEFINED)
         return step(triangle, "texel buffer geometry", VK_ERROR_INITIALIZATION_FAILED,
                     "a texel buffer needs data");
      return true;
   }
   if (input->texel_buffer_bytes == 0 || input->texel_buffer_format == VK_FORMAT_UNDEFINED)
      return step(triangle, "texel buffer geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "a texel buffer needs bytes and a format");
   if (triangle->texture_set_layout != VK_NULL_HANDLE)
      return step(triangle, "create_texel_buffer_set_layout", VK_ERROR_INITIALIZATION_FAILED,
                  "one set at 0: a texture and a texel buffer cannot share it");

   /* A storage texel buffer is the same buffer, view and 16-byte entry under
    * the usage and descriptor type the pixel stage stores through (V0-formats'
    * second descriptor-type row); a uniform one is what the fetch reads. */
   const VkBufferUsageFlags usage = input->texel_buffer_storage
                                       ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
                                       : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
   const VkDescriptorType descriptor_type =
      input->texel_buffer_storage ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                  : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
   if (!create_buffer(triangle, physical, input->texel_buffer_bytes, usage, "texel buffer",
                      input->texel_buffer_data, &triangle->texel_buffer,
                      &triangle->texel_buffer_memory, &triangle->texel_buffer_mapped))
      return false;
   const VkBufferViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = triangle->texel_buffer,
      .format = input->texel_buffer_format,
      .offset = 0,
      /* The whole buffer: the driver's V# takes its element count from this
       * range, so a view that named less would fetch fewer texels. */
      .range = input->texel_buffer_bytes,
   };
   if (!step(triangle, "create_texel_buffer_view",
             CALL(triangle, CreateBufferView)(triangle->device, &view_info, NULL,
                                              &triangle->texel_buffer_view),
             input->texel_buffer_storage ? "the caller's texels as one storage texel buffer"
                                         : "the caller's texels as one uniform texel buffer"))
      return false;

   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = descriptor_type,
      .descriptorCount = 1,
      /* The pixel stage is what fetches it, and only for that stage does the
       * driver's compiler option name the binding (driver/ps5vk_pipeline.c,
       * ps5vk_descriptor_options). */
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   if (!step(triangle, "create_texel_buffer_set_layout",
             CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &set_info, NULL,
                                                       &triangle->texture_set_layout),
             input->texel_buffer_storage ? "one storage texel buffer at set 0, binding 0"
                                         : "one uniform texel buffer at set 0, binding 0"))
      return false;
   const VkDescriptorPoolSize pool_size = {
      .type = descriptor_type,
      .descriptorCount = 1,
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   if (!step(triangle, "create_texel_buffer_pool",
             CALL(triangle, CreateDescriptorPool)(triangle->device, &pool_info, NULL,
                                                  &triangle->texture_pool),
             NULL))
      return false;
   const VkDescriptorSetAllocateInfo set_allocate = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = triangle->texture_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &triangle->texture_set_layout,
   };
   if (!step(triangle, "allocate_texel_buffer_set",
             CALL(triangle, AllocateDescriptorSets)(triangle->device, &set_allocate,
                                                    &triangle->texture_set),
             NULL))
      return false;
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = triangle->texture_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = descriptor_type,
      .pTexelBufferView = &triangle->texel_buffer_view,
   };
   CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
   return true;
}

/* The storage image the caller's pixel shader stores into, and the objects that
 * bind it (V0-formats' descriptor-type rows): a row-layout 2D image with
 * VK_IMAGE_USAGE_STORAGE_BIT, mapped so the caller reads the stores out of it,
 * its view, and the set 0 binding 0 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE descriptor
 * whose 32 bytes the driver writes as the image descriptor the sampled path's
 * combined image sampler starts with (driver/ps5vk_draw.c). The set is the
 * program's one set at 0, so it takes texture_set/texture_set_layout and a
 * caller cannot ask for a texture, a texel buffer and a storage image at once.
 * Nothing at all when the caller passed no storage image. */
static bool
create_storage_image(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
                     const struct ps5vk_triangle_input *input)
{
   if (input->storage_image_format == VK_FORMAT_UNDEFINED) {
      /* A caller that stores nothing records and submits exactly what it
       * recorded before this path existed: no image, no view, no set. A size
       * without a format is a mistake, not a program that wants nothing. */
      if (input->storage_image_width != 0 || input->storage_image_height != 0)
         return step(triangle, "storage image geometry", VK_ERROR_INITIALIZATION_FAILED,
                     "a storage image needs a format");
      return true;
   }
   if (input->storage_image_width == 0 || input->storage_image_height == 0)
      return step(triangle, "storage image geometry", VK_ERROR_INITIALIZATION_FAILED,
                  "a storage image needs a width and a height");
   if (triangle->texture_set_layout != VK_NULL_HANDLE)
      return step(triangle, "create_storage_image_set_layout", VK_ERROR_INITIALIZATION_FAILED,
                  "one set at 0: a texture or texel buffer and a storage image cannot share it");

   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = input->storage_image_format,
      .extent = {input->storage_image_width, input->storage_image_height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      /* Storage and nothing else: the frame's stores are the only writer, and
       * the caller reads them out of the mapping. */
      .usage = VK_IMAGE_USAGE_STORAGE_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (!step(triangle, "create_storage_image",
             CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                         &triangle->storage_image),
             "the caller's stores, a row-layout storage image"))
      return false;
   VkMemoryRequirements requirements;
   CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->storage_image,
                                              &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[index].propertyFlags;
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         memory_type = index;
   }
   char detail[128];
   snprintf(detail, sizeof(detail), "%llu bytes, alignment %llu, memory type %u",
            (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
            (unsigned)memory_type);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   if (!step(triangle, "allocate_storage_image_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                                 &triangle->storage_image_memory),
             detail) ||
       !step(triangle, "bind_storage_image_memory",
             CALL(triangle, BindImageMemory)(triangle->device, triangle->storage_image,
                                             triangle->storage_image_memory, 0),
             NULL) ||
       !step(triangle, "map_storage_image_memory",
             CALL(triangle, MapMemory)(triangle->device, triangle->storage_image_memory, 0,
                                       VK_WHOLE_SIZE, 0, &triangle->storage_image_mapped),
             NULL))
      return false;
   triangle->storage_image_bytes = (size_t)requirements.size;
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = triangle->storage_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = input->storage_image_format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   if (!step(triangle, "create_storage_image_view",
             CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                             &triangle->storage_image_view),
             "the caller's stores as one storage image"))
      return false;

   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   if (!step(triangle, "create_storage_image_set_layout",
             CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &set_info, NULL,
                                                       &triangle->texture_set_layout),
             "one storage image at set 0, binding 0"))
      return false;
   const VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .descriptorCount = 1,
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   if (!step(triangle, "create_storage_image_pool",
             CALL(triangle, CreateDescriptorPool)(triangle->device, &pool_info, NULL,
                                                  &triangle->texture_pool),
             NULL))
      return false;
   const VkDescriptorSetAllocateInfo set_allocate = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = triangle->texture_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &triangle->texture_set_layout,
   };
   if (!step(triangle, "allocate_storage_image_set",
             CALL(triangle, AllocateDescriptorSets)(triangle->device, &set_allocate,
                                                    &triangle->texture_set),
             NULL))
      return false;
   const VkDescriptorImageInfo image_descriptor = {
      .imageView = triangle->storage_image_view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = triangle->texture_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .pImageInfo = &image_descriptor,
   };
   CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
   return true;
}

bool
ps5vk_triangle_set_texture_filter(struct ps5vk_triangle *triangle, bool bilinear)
{
   if (triangle->texture_set == VK_NULL_HANDLE || triangle->texture_view == VK_NULL_HANDLE)
      return false;
   triangle->texture_bilinear = bilinear;
   update_texture_descriptor(triangle);
   return true;
}

/* Phase V0-query: the frames' occlusion queries. The program owns nothing of
 * them beyond the query each frame records inside (triangle->query_pool and
 * triangle->query), so a caller creates the pool on the program's device,
 * points the frames at one of its queries and reads the result back in
 * samples. */
void
ps5vk_triangle_set_base_vertex(struct ps5vk_triangle *triangle, int32_t offset)
{
   triangle->base_vertex = offset;
}

void
ps5vk_triangle_set_instance_count(struct ps5vk_triangle *triangle, uint32_t count)
{
   triangle->instance_count = count;
}

bool
ps5vk_triangle_set_texture_view(struct ps5vk_triangle *triangle, uint32_t level)
{
   if (triangle->texture_image == VK_NULL_HANDLE || triangle->texture_set == VK_NULL_HANDLE)
      return false;
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = triangle->texture_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1},
   };
   VkImageView view = VK_NULL_HANDLE;
   char detail[48];
   snprintf(detail, sizeof(detail), "level %u alone", level);
   if (!step(triangle, "create_level_view",
             CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL, &view), detail))
      return false;
   if (triangle->texture_level_view != VK_NULL_HANDLE)
      CALL(triangle, DestroyImageView)(triangle->device, triangle->texture_level_view, NULL);
   triangle->texture_level_view = view;
   const VkDescriptorImageInfo image_info = {
      .sampler = triangle->texture_lod_sampler != VK_NULL_HANDLE
                    ? triangle->texture_lod_sampler
                    : triangle->texture_samplers[PS5VK_TRIANGLE_SAMPLER_NEAREST],
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = triangle->texture_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &image_info,
   };
   CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
   return true;
}

bool
ps5vk_triangle_set_texture_lod(struct ps5vk_triangle *triangle, float min_lod, float max_lod)
{
   return ps5vk_triangle_set_texture_lod_bias(triangle, min_lod, max_lod, 0.0f);
}

bool
ps5vk_triangle_set_texture_lod_bias(struct ps5vk_triangle *triangle, float min_lod, float max_lod,
                                  float bias)
{
   if (triangle->texture_view == VK_NULL_HANDLE || triangle->texture_set == VK_NULL_HANDLE)
      return false;
   const VkSamplerCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = triangle->texture_bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
      .minFilter = triangle->texture_bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
      .mipmapMode = triangle->texture_mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .mipLodBias = bias,
      .minLod = min_lod,
      .maxLod = max_lod,
   };
   VkSampler sampler = VK_NULL_HANDLE;
   char detail[64];
   snprintf(detail, sizeof(detail), "LOD %g to %g", (double)min_lod, (double)max_lod);
   if (!step(triangle, "create_lod_sampler",
             CALL(triangle, CreateSampler)(triangle->device, &info, NULL, &sampler), detail))
      return false;
   if (triangle->texture_lod_sampler != VK_NULL_HANDLE)
      CALL(triangle, DestroySampler)(triangle->device, triangle->texture_lod_sampler, NULL);
   triangle->texture_lod_sampler = sampler;
   const VkDescriptorImageInfo image_info = {
      .sampler = sampler,
      .imageView = triangle->texture_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = triangle->texture_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &image_info,
   };
   CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
   return true;
}

bool
ps5vk_triangle_set_texture_border(struct ps5vk_triangle *triangle, VkSamplerAddressMode mode,
                                  VkBorderColor border)
{
   if (triangle->texture_view == VK_NULL_HANDLE || triangle->texture_set == VK_NULL_HANDLE)
      return false;
   const VkSamplerCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = mode,
      .addressModeV = mode,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .borderColor = border,
   };
   VkSampler sampler = VK_NULL_HANDLE;
   char detail[64];
   snprintf(detail, sizeof(detail), "address mode %d border %d", (int)mode, (int)border);
   if (!step(triangle, "create_border_sampler",
             CALL(triangle, CreateSampler)(triangle->device, &info, NULL, &sampler), detail))
      return false;
   if (triangle->texture_lod_sampler != VK_NULL_HANDLE)
      CALL(triangle, DestroySampler)(triangle->device, triangle->texture_lod_sampler, NULL);
   triangle->texture_lod_sampler = sampler;
   const VkDescriptorImageInfo image_info = {
      .sampler = sampler,
      .imageView = triangle->texture_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = triangle->texture_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &image_info,
   };
   CALL(triangle, UpdateDescriptorSets)(triangle->device, 1, &write, 0, NULL);
   return true;
}

VkQueryPool
ps5vk_triangle_create_timestamp_pool(struct ps5vk_triangle *triangle, uint32_t count)
{
   const VkQueryPoolCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = count,
   };
   VkQueryPool pool = VK_NULL_HANDLE;
   const VkResult created = CALL(triangle, CreateQueryPool)(triangle->device, &info, NULL, &pool);
   if (!step(triangle, "create_timestamp_pool", created, "a timestamp query pool"))
      return VK_NULL_HANDLE;
   return pool;
}

void
ps5vk_triangle_set_timestamp_pool(struct ps5vk_triangle *triangle, VkQueryPool pool)
{
   triangle->timestamp_pool = pool;
}

bool
ps5vk_triangle_query_timestamp(struct ps5vk_triangle *triangle, VkQueryPool pool, uint32_t query,
                               uint64_t *ticks)
{
   uint64_t result = 0;
   const VkResult read = CALL(triangle, GetQueryPoolResults)(
      triangle->device, pool, query, 1, sizeof(result), &result, sizeof(result),
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
   if (!step(triangle, "get_query_pool_results", read, "a timestamp query's ticks"))
      return false;
   *ticks = result;
   return true;
}

VkQueryPool
ps5vk_triangle_create_query_pool(struct ps5vk_triangle *triangle, uint32_t count)
{
   const VkQueryPoolCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_OCCLUSION,
      .queryCount = count,
   };
   VkQueryPool pool = VK_NULL_HANDLE;
   const VkResult created = CALL(triangle, CreateQueryPool)(triangle->device, &info, NULL, &pool);
   if (!step(triangle, "create_query_pool", created, "an occlusion query pool"))
      return VK_NULL_HANDLE;
   return pool;
}

void
ps5vk_triangle_destroy_query_pool(struct ps5vk_triangle *triangle, VkQueryPool pool)
{
   if (pool != VK_NULL_HANDLE)
      CALL(triangle, DestroyQueryPool)(triangle->device, pool, NULL);
}

bool
ps5vk_triangle_query_copy(const struct ps5vk_triangle *triangle, uint64_t *value,
                          uint64_t *available)
{
   if (triangle == NULL || triangle->query_copy_mapped == NULL || value == NULL)
      return false;
   const uint64_t *const words = triangle->query_copy_mapped;
   *value = words[0];
   if (available != NULL)
      *available = words[1];
   return true;
}

void
ps5vk_triangle_set_query(struct ps5vk_triangle *triangle, VkQueryPool pool, uint32_t query)
{
   triangle->query_pool = pool;
   triangle->query = query;
}

bool
ps5vk_triangle_query_samples(struct ps5vk_triangle *triangle, VkQueryPool pool, uint32_t query,
                             uint64_t *samples)
{
   uint64_t result = 0;
   const VkResult read = CALL(triangle, GetQueryPoolResults)(
      triangle->device, pool, query, 1, sizeof(result), &result, sizeof(result),
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
   if (!step(triangle, "get_query_pool_results", read, "an occlusion query's samples"))
      return false;
   *samples = result;
   return true;
}

/* A render pass over one colour attachment of format: the first of a frame
 * loads it with load_op from an undefined layout, a later one loads its
 * contents. A swapchain image leaves both ready to present; the image a
 * render-to-texture frame samples leaves its fill pass ready to be sampled. */
static VkResult
create_render_pass(struct ps5vk_triangle *triangle, VkFormat format, VkAttachmentLoadOp load_op,
                   VkImageLayout final_layout, uint32_t colour_count, VkRenderPass *pass)
{
   assert(colour_count >= 1 && colour_count <= PS5VK_TRIANGLE_MAX_TARGETS);
   const bool load = load_op == VK_ATTACHMENT_LOAD_OP_LOAD;
   const VkAttachmentDescription attachment = {
      .format = format,
      .samples = triangle->samples,
      .loadOp = load_op,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = load ? final_layout : VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = final_layout,
   };
   /* Phase C5: a frame with a depth attachment gives every pass one, at the
    * layout a depth attachment is sampled and written in. The first pass does
    * what the caller asked with it (load or clear); the pass a later draw uses
    * loads it, so the depth the first pass wrote stays. */
   /* A combined depth/stencil format's plane is part of the same attachment, so
    * its load and store are the attachment's and have to be legal for the
    * format: a format with no stencil aspect keeps DONT_CARE, which Vulkan
    * requires of it (round 12). */
   const bool stencil_attached =
      triangle->depth_format == VK_FORMAT_D24_UNORM_S8_UINT ||
      triangle->depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
      triangle->depth_format == VK_FORMAT_D16_UNORM_S8_UINT ||
      triangle->depth_format == VK_FORMAT_S8_UINT;
   const VkAttachmentDescription depth_attachment = {
      .format = triangle->depth_format,
      /* The pass's sample count is the frame's (C8's depth half): a four-sample
       * frame renders through a four-sample depth attachment, and the runtime
       * checks the two agree. */
      .samples = triangle->samples,
      .loadOp = triangle->depth_load_op,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = stencil_attached
                          ? (load ? VK_ATTACHMENT_LOAD_OP_LOAD : triangle->stencil_load_op)
                          : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = stencil_attached ? VK_ATTACHMENT_STORE_OP_STORE
                                         : VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = triangle->depth_load_op == VK_ATTACHMENT_LOAD_OP_LOAD
                          ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                          : VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   };
   /* One attachment description and one reference per colour attachment, in
    * attachment order, with the depth attachment after them: the order the
    * framebuffer's views and the draw's register rows follow too. */
   VkAttachmentDescription attachments[PS5VK_TRIANGLE_MAX_TARGETS + 1];
   VkAttachmentReference references[PS5VK_TRIANGLE_MAX_TARGETS];
   for (uint32_t at = 0; at < colour_count; at++) {
      attachments[at] = attachment;
      references[at] = (VkAttachmentReference){at, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   }
   const VkAttachmentReference depth_reference = {
      colour_count, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   if (triangle->depth)
      attachments[colour_count] = depth_attachment;
   const VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = colour_count,
      .pColorAttachments = references,
      .pDepthStencilAttachment = triangle->depth ? &depth_reference : NULL,
   };
   const VkRenderPassCreateInfo pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = colour_count + (triangle->depth ? 1u : 0u),
      .pAttachments = attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   return CALL(triangle, CreateRenderPass)(triangle->device, &pass_info, NULL, pass);
}

/* R10's subpass-input probe: the second colour image the frame's render pass
 * declares as attachment 0, which subpass 0 draws into and subpass 1 reads as
 * its input attachment. Completely absent unless the caller asked for the
 * probe, so every other frame's allocations and recording are exactly what
 * they were. */
static bool
create_subpass_target(struct ps5vk_triangle *triangle, VkPhysicalDevice physical)
{
   if (!triangle->subpass_input)
      return true;
   if (triangle->output != PS5VK_TRIANGLE_OUTPUT_IMAGE)
      return step(triangle, "subpass target", VK_ERROR_INITIALIZATION_FAILED,
                  "the subpass-input probe reads back the program's image: attachment 1 is that "
                  "image, which the display output has not got");
   if (triangle->pipeline_count < 2)
      return step(triangle, "subpass target", VK_ERROR_INITIALIZATION_FAILED,
                  "the subpass-input probe draws subpass 0 with the first pipeline and subpass 1 "
                  "with the second");
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = triangle->format,
      .extent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      /* The colour attachment subpass 1 reads as an input attachment: the same
       * two usages the driver's own image table answers for this format. */
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (!step(triangle, "create_subpass_image",
             CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                         &triangle->subpass_image),
             NULL))
      return false;
   VkMemoryRequirements requirements = {0};
   CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->subpass_image,
                                              &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (memory_properties.memoryTypes[index].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) /* read back through vkMapMemory below */
         memory_type = index;
   }
   const VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   if (!step(triangle, "allocate_subpass_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocation, NULL,
                                                 &triangle->subpass_memory),
             NULL) ||
       !step(triangle, "bind_subpass_memory",
             CALL(triangle, BindImageMemory)(triangle->device, triangle->subpass_image,
                                        triangle->subpass_memory, 0),
             NULL) ||
       !step(triangle, "map_subpass_memory",
             CALL(triangle, MapMemory)(triangle->device, triangle->subpass_memory, 0,
                                       VK_WHOLE_SIZE, 0, &triangle->subpass_mapped), NULL))
      return false;
   triangle->subpass_bytes = (size_t)requirements.size;
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = triangle->subpass_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = triangle->format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   return step(triangle, "create_subpass_image_view",
               CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                               &triangle->subpass_view),
               NULL);
}

/* R10's render pass: two colour attachments and two subpasses -- subpass 0
 * writes attachment 0, subpass 1 reads it as its input attachment and writes
 * attachment 1. The pass the probe's frames use in place of the one-subpass
 * pass every other frame records; nothing else about a frame changes. */
static VkResult
create_subpass_pass(struct ps5vk_triangle *triangle, VkRenderPass *pass)
{
   const VkAttachmentDescription attachments[2] = {
      {.format = triangle->format,
       .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {.format = triangle->format,
       .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
   };
   const VkAttachmentReference written = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   const VkAttachmentReference read = {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   const VkAttachmentReference output = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   const VkSubpassDescription subpasses[2] = {
      {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
       .colorAttachmentCount = 1,
       .pColorAttachments = &written},
      {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
       .inputAttachmentCount = 1,
       .pInputAttachments = &read,
       .colorAttachmentCount = 1,
       .pColorAttachments = &output},
   };
   /* Subpass 1 reads what subpass 0 wrote: the dependency an application
    * declares for that, which the driver's own flush is what satisfies it --
    * the subpass boundary is where the colour buffer is flushed and the
    * submission splits for the draw that reads it (driver/ps5vk_draw.c). */
   const VkSubpassDependency dependency = {
      .srcSubpass = 0,
      .dstSubpass = 1,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
      .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
   };
   const VkRenderPassCreateInfo pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = attachments,
      .subpassCount = 2,
      .pSubpasses = subpasses,
      .dependencyCount = 1,
      .pDependencies = &dependency,
   };
   return CALL(triangle, CreateRenderPass)(triangle->device, &pass_info, NULL, pass);
}

/* Whether the caller's geometry is the m3-texture one a render-to-texture
 * frame needs: the two attributes the canary's vertex shader declares, at the
 * offsets of the 16-byte record, which is the layout the fill quad is written
 * in (probes/m3-texture/compile.txt). */
static bool
texture_geometry(const struct ps5vk_triangle_input *input)
{
   return input->vertex_stride == PS5VK_TRIANGLE_TEXTURE_VERTEX_STRIDE &&
          input->attribute_count == PS5VK_TRIANGLE_TEXTURE_ATTRIBUTES &&
          input->attributes[0].location == 0 &&
          input->attributes[0].format == VK_FORMAT_R32G32_SFLOAT &&
          input->attributes[0].offset == 0 && input->attributes[1].location == 1 &&
          input->attributes[1].format == VK_FORMAT_R32G32_SFLOAT &&
          input->attributes[1].offset == 8;
}

/* Phase C4's render-to-texture case (input->texture_is_rendered): the second
 * colour image the frame draws the caller's texels over and then samples in the
 * same command buffer, its view, the pass and framebuffer the fill pass draws
 * over it, and the full-target quad that pass draws. The image is the program's
 * own target size because that is the one colour attachment size the driver
 * records (driver/ps5vk_draw.c refuses any other), so the fill pass draws the
 * caller's pattern over the whole of it, and the frame's own pass -- the
 * canary's square, sampling this image -- reads the pattern back. Nothing at
 * all when the caller did not ask for the case, which leaves every allocation
 * and recording a plain textured frame makes exactly as it was. */
static bool
create_rendered_texture(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
                        const struct ps5vk_triangle_input *input)
{
   if (!input->texture_is_rendered)
      return true;
   /* The fill pass draws the caller's own texels, so the case needs them, and
    * it draws them with the caller's pipeline: a geometry of another layout
    * would put the quad's position and texture coordinate in the wrong record
    * fields, and nothing would fill the target. */
   if (input->texture_data == NULL)
      return step(triangle, "rendered texture", VK_ERROR_INITIALIZATION_FAILED,
                  "a rendered texture needs the caller's texels to draw into it");
   if (!texture_geometry(input))
      return step(triangle, "rendered texture", VK_ERROR_INITIALIZATION_FAILED,
                  "a rendered texture draws with the m3-texture geometry: 16-byte records whose "
                  "attributes are location 0 and 1 R32G32_SFLOAT at offsets 0 and 8");
   if (triangle->output != PS5VK_TRIANGLE_OUTPUT_IMAGE)
      return step(triangle, "rendered texture", VK_ERROR_INITIALIZATION_FAILED,
                  "a rendered texture is a colour attachment the frame samples, which the "
                  "display output is not");

   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      /* A colour attachment the frame also samples: the driver stores it tiled
       * and flags the colour barrier for the draw that samples it after this
       * command buffer rendered into it (driver/ps5vk_image.c,
       * driver/ps5vk_draw.c, ps5vk_sampled_image). */
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   const char *const name = "the image the frame renders into and samples";
   if (!step(triangle, "create_rendered_image",
             CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                         &triangle->rendered_image),
             name))
      return false;
   VkMemoryRequirements requirements;
   CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->rendered_image,
                                              &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (memory_properties.memoryTypes[index].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         memory_type = index;
   }
   char detail[128];
   snprintf(detail, sizeof(detail), "%llu bytes, alignment %llu, memory type %u",
            (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
            (unsigned)memory_type);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   /* The image memory is mapped too: the GPU is the only thing that writes
    * this image, and a test reads the mapping after the fence to see what the
    * render left there, beside the frame's own readback of its sample. */
   if (!step(triangle, "allocate_rendered_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                                 &triangle->rendered_memory),
             detail) ||
       !step(triangle, "bind_rendered_memory",
             CALL(triangle, BindImageMemory)(triangle->device, triangle->rendered_image,
                                             triangle->rendered_memory, 0),
             NULL) ||
       !step(triangle, "map_rendered_memory",
             CALL(triangle, MapMemory)(triangle->device, triangle->rendered_memory, 0,
                                       VK_WHOLE_SIZE, 0, &triangle->rendered),
             NULL))
      return false;
   triangle->rendered_bytes = requirements.size;

   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = triangle->rendered_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   /* The pass draws over the whole target, so it needs no clear, and loading
    * the image's contents is not even defined. It leaves the image in the
    * layout the frame's descriptor names it in. */
   if (!step(triangle, "create_rendered_view",
             CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                             &triangle->rendered_view),
             NULL) ||
       !step(triangle, "create_rendered_pass",
             create_render_pass(triangle, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1,
                                &triangle->rendered_pass),
             "the image the frame samples"))
      return false;
   const VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = triangle->rendered_pass,
      .attachmentCount = 1,
      .pAttachments = &triangle->rendered_view,
      .width = PS5VK_TRIANGLE_WIDTH,
      .height = PS5VK_TRIANGLE_HEIGHT,
      .layers = 1,
   };
   if (!step(triangle, "create_rendered_framebuffer",
             CALL(triangle, CreateFramebuffer)(triangle->device, &framebuffer_info, NULL,
                                               &triangle->rendered_framebuffer),
             NULL))
      return false;

   /* The quad both triangles of the fill pass draw: the whole target in clip
    * space, with the texture coordinate the canary's square carries -- except
    * that its first row, which Vulkan puts at clip y = -1 (shaders/b7/corner.vert),
    * is where v = 0 goes. The sampler reads an image's first row as the
    * texture's first row, and the upload laid the caller's pattern out with
    * texel row 0 first, so v = 0 belongs there for the two images to hold the
    * same pattern and the frame's readback to be the canary's. */
   const float quad[PS5VK_TRIANGLE_FILL_VERTICES * 4] = {
      -1.0f, -1.0f, 0.0f, 0.0f, /* first row, left edge */
      1.0f,  -1.0f, 1.0f, 0.0f, /* first row, right edge */
      1.0f,  1.0f,  1.0f, 1.0f, /* last row, right edge */
      -1.0f, -1.0f, 0.0f, 0.0f, /* the second triangle, the same corner */
      -1.0f, 1.0f,  0.0f, 1.0f, /* last row, left edge */
      1.0f,  1.0f,  1.0f, 1.0f, /* the second triangle, the same corner */
   };
   return create_buffer(triangle, physical, sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        "fill quad", quad, &triangle->fill_buffer, &triangle->fill_memory,
                        &triangle->fill_mapped);
}

/* Phase C7's copy case (input->texture_copied or texture_blitted): the second,
 * tiled image the frame copies -- or one-to-one blits -- the texels it uploaded
 * into, and then samples. Tiled means the colour-attachment usage below, which
 * is what makes the driver store it in the measured tile layout; the copy is
 * vkCmdCopyImage and the blit vkCmdBlitImage, both recorded in the frame's
 * upload command buffer, before the render pass. The image is the caller's
 * texture extent and format, single-level, and its memory is mapped so the
 * caller reads what the copy left there. Nothing at all when the caller did not
 * ask for the case. */
static bool
create_copied_texture(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
                      const struct ps5vk_triangle_input *input)
{
   if (!input->texture_copied && !input->texture_blitted && !input->texture_blit_scaled)
      return true;
   if (input->texture_data == NULL || input->texture_tiled) {
      return step(triangle, "copied texture", VK_ERROR_INITIALIZATION_FAILED,
                  "a copied texture is the caller's uploaded texels, copied into a tiled image "
                  "the library creates: pass texture_data and leave texture_tiled unset");
   }
   const unsigned transfers = (input->texture_copied ? 1u : 0u) +
                              (input->texture_blitted ? 1u : 0u) +
                              (input->texture_blit_scaled ? 1u : 0u);
   if (transfers != 1)
      return step(triangle, "copied texture", VK_ERROR_INITIALIZATION_FAILED,
                  "a copied texture is one of vkCmdCopyImage, the one-to-one vkCmdBlitImage or the "
                  "scaled one");
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      /* The copy and the one-to-one blit move texels of one format, so the
       * destination takes the caller's; a scaled blit writes the four-byte
       * destination its resampler converts into, which is R8G8B8A8_UNORM. */
      .format = (input->texture_copied || input->texture_blitted) &&
                     input->texture_format != VK_FORMAT_UNDEFINED
                   ? input->texture_format
                   : VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {input->texture_width, input->texture_height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      /* The colour-attachment usage is what makes the driver store this image
       * in tiles, sampled and the destination of the frame's copy. */
      .usage = texture_is_depth(image_info.format)
                  ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                  : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (!step(triangle, "create_copied_image",
             CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                         &triangle->copied_image),
             "the tiled image the frame copies the texels into and samples"))
      return false;
   VkMemoryRequirements requirements;
   CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->copied_image,
                                              &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (memory_properties.memoryTypes[index].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         memory_type = index;
   }
   char detail[128];
   snprintf(detail, sizeof(detail), "%llu bytes, alignment %llu, memory type %u",
            (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
            (unsigned)memory_type);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   if (!step(triangle, "allocate_copied_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                                 &triangle->copied_memory),
             detail) ||
       !step(triangle, "bind_copied_memory",
             CALL(triangle, BindImageMemory)(triangle->device, triangle->copied_image,
                                             triangle->copied_memory, 0),
             NULL) ||
       !step(triangle, "map_copied_memory",
             CALL(triangle, MapMemory)(triangle->device, triangle->copied_memory, 0, VK_WHOLE_SIZE,
                                       0, &triangle->copied),
             NULL))
      return false;
   triangle->copied_bytes = requirements.size;

   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = triangle->copied_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = image_info.format,
      .subresourceRange = {texture_is_depth(image_info.format) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                              : VK_IMAGE_ASPECT_COLOR_BIT,
                           0, 1, 0, 1},
   };
   if (!step(triangle, "create_copied_view",
             CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                             &triangle->copied_view),
             NULL))
      return false;
   triangle->texture_copied = input->texture_copied;
   triangle->texture_blitted = input->texture_blitted;
   triangle->texture_blit_scaled = input->texture_blit_scaled;
   triangle->texture_blit_linear = input->texture_blit_linear;
   return true;
}

/* Phase C8's resolve target (input->resolve_output): the four-sample image the
 * frame renders into, which the frame then resolves into the program's own
 * one-sample image (ps5vk_triangle_draw). Its view is what the render passes'
 * framebuffers name, and it is not mapped: nothing on the CPU reads four
 * samples, the resolve writes what a caller reads. Nothing at all when the
 * caller did not ask for the case. */
static bool
create_resolve_target(struct ps5vk_triangle *triangle, VkPhysicalDevice physical,
                      const struct ps5vk_triangle_input *input)
{
   if (!input->resolve_output)
      return true;
   if (input->samples != VK_SAMPLE_COUNT_4_BIT)
      return step(triangle, "resolve target", VK_ERROR_INITIALIZATION_FAILED,
                  "a resolve renders into a four-sample image: pass samples = "
                  "VK_SAMPLE_COUNT_4_BIT with resolve_output");
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_4_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (!step(triangle, "create_resolve_image",
             CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                         &triangle->resolve_image),
             "the four-sample image the frame renders into and resolves"))
      return false;
   VkMemoryRequirements requirements;
   CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->resolve_image,
                                              &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (memory_properties.memoryTypes[index].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         memory_type = index;
   }
   char detail[128];
   snprintf(detail, sizeof(detail), "%llu bytes, alignment %llu, memory type %u",
            (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
            (unsigned)memory_type);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   if (!step(triangle, "allocate_resolve_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                                 &triangle->resolve_memory),
             detail) ||
       !step(triangle, "bind_resolve_memory",
             CALL(triangle, BindImageMemory)(triangle->device, triangle->resolve_image,
                                             triangle->resolve_memory, 0),
             NULL))
      return false;
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = triangle->resolve_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   return step(triangle, "create_resolve_view",
               CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                               &triangle->resolve_view),
               NULL);
}

/* Phase C5's depth attachment: a D32_SFLOAT image the size of the program's
 * target, tiled like a colour attachment, on its own mapped host-visible memory
 * so a caller can read what the frame's draws left in it (the M4 canary's
 * layout, src/diagnostics.cpp's tiled_depth_offset). */
static bool
create_depth_attachment(struct ps5vk_triangle *triangle, VkPhysicalDevice physical)
{
   if (!triangle->depth)
      return true;
   if (triangle->texture_is_rendered) {
      step(triangle, "create_depth_image", VK_ERROR_INITIALIZATION_FAILED,
           "the render-to-texture case has no depth attachment yet (Phase C5)");
      return false;
   }
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = triangle->depth_format,
      .extent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      /* A depth attachment follows the frame's sample count (C8's depth 4x):
       * the render pass, the pipelines and the rasterizer's MSAA state all take
       * it from the same input, so a four-sample frame renders into a
       * four-sample attachment. */
      .samples = triangle->samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (!step(triangle, "create_depth_image",
             CALL(triangle, CreateImage)(triangle->device, &image_info, NULL,
                                         &triangle->depth_image),
             NULL))
      return false;
   VkMemoryRequirements requirements;
   CALL(triangle, GetImageMemoryRequirements)(triangle->device, triangle->depth_image,
                                              &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   CALL(triangle, GetPhysicalDeviceMemoryProperties)(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t index = 0; index < memory_properties.memoryTypeCount && memory_type == UINT32_MAX;
        index++) {
      if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
          (memory_properties.memoryTypes[index].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         memory_type = index;
   }
   char detail[128];
   snprintf(detail, sizeof(detail), "%llu bytes, alignment %llu, memory type %u",
            (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
            (unsigned)memory_type);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   if (!step(triangle, "allocate_depth_memory",
             memory_type == UINT32_MAX
                ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                : CALL(triangle, AllocateMemory)(triangle->device, &allocate_info, NULL,
                                                 &triangle->depth_memory),
             detail) ||
       !step(triangle, "bind_depth_memory",
             CALL(triangle, BindImageMemory)(triangle->device, triangle->depth_image,
                                             triangle->depth_memory, 0),
             NULL) ||
       !step(triangle, "map_depth_memory",
             CALL(triangle, MapMemory)(triangle->device, triangle->depth_memory, 0,
                                       VK_WHOLE_SIZE, 0, &triangle->depth_mapped),
             NULL))
      return false;
   triangle->depth_bytes = requirements.size;
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = triangle->depth_image,
      .viewType = triangle->depth_array_view ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
      .format = triangle->depth_format,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
   };
   return step(triangle, "create_depth_view",
               CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                               &triangle->depth_view),
               NULL);
}

/* Pipeline index: the shaders over the whole target, compatible with both
 * render passes. */
static bool
create_pipeline(struct ps5vk_triangle *triangle, uint32_t index,
                const struct ps5vk_triangle_shaders *shaders,
                const struct ps5vk_triangle_input *input)
{
   const VkShaderModuleCreateInfo vertex_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shaders->vertex_bytes,
      .pCode = shaders->vertex_spirv,
   };
   const VkShaderModuleCreateInfo pixel_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shaders->pixel_bytes,
      .pCode = shaders->pixel_spirv,
   };
   if (!step(triangle, "create_shader_modules",
             CALL(triangle, CreateShaderModule)(triangle->device, &vertex_info, NULL,
                                                &triangle->vertex[index]),
             "vertex") ||
       (shaders->pixel_spirv &&
        !step(triangle, "create_shader_modules",
             CALL(triangle, CreateShaderModule)(triangle->device, &pixel_info, NULL,
                                                &triangle->pixel[index]),
             "fragment")))
      return false;
   const VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = triangle->vertex[index], .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = triangle->pixel[index],
       .pName = "main",
       .pSpecializationInfo = input->pixel_specialization},
   };
   /* The caller's shaders fetch their vertices through one binding, which the
    * driver builds its vertex-buffer table from (driver/ps5vk_pipeline.c);
    * the triangle sets declare none and keep the empty state below. */
   const VkVertexInputBindingDescription binding = {
      .binding = 0,
      .stride = input->vertex_stride,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   const VkPipelineVertexInputStateCreateInfo geometry_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding,
      .vertexAttributeDescriptionCount = input->attribute_count,
      .pVertexAttributeDescriptions = input->attributes,
   };
   const VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   const VkPipelineInputAssemblyStateCreateInfo assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = input->primitive_topology != 0 ? input->primitive_topology
                                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .primitiveRestartEnable =
         input->primitive_restart && !(input->primitive_restart_first_only && index != 0),
   };
   const VkViewport viewport = {0.0f,
                                0.0f,
                                PS5VK_TRIANGLE_WIDTH,
                                PS5VK_TRIANGLE_HEIGHT,
                                input->viewport_depth_inverted ? 1.0f : 0.0f,
                                input->viewport_depth_inverted ? 0.0f : 1.0f};
   /* The pipeline's scissor: the caller's rect when it declares one (V0-query's
    * known regions), and the whole target -- which is what every frame before
    * that phase drew with -- when it does not. The caller's rect is used
    * verbatim, a zero extent included: that is the region no fragment reaches. */
   const VkRect2D scissor = input->use_scissor
                               ? input->scissor
                               : (VkRect2D){{0, 0}, {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT}};
   const VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };
   /* R1: the frame's cull mode and rasterizer discard, which are core Vulkan
    * 1.0 state and the driver programs (driver/ps5vk_pipeline.c), and its depth
    * bias, whose three factors the driver keeps for the draw to program beside
    * the depth attachment's own format word. */
   const VkPipelineRasterizationStateCreateInfo rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = triangle->rasterization_cull_mode,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .rasterizerDiscardEnable = triangle->rasterization_discard ? VK_TRUE : VK_FALSE,
      .depthBiasEnable = triangle->depth_bias_enable ? VK_TRUE : VK_FALSE,
      .depthBiasConstantFactor = triangle->depth_bias_constant,
      .depthBiasSlopeFactor = triangle->depth_bias_slope,
      .depthBiasClamp = triangle->depth_bias_clamp,
      .lineWidth = 1.0f,
   };
   const VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = triangle->samples,
   };
   /* V0-formats' colour targets: every pipeline of the frame blends when the
    * caller asked it to, with the factors and equations it passed. A blending
    * frame clears its attachment to the destination colour and draws the source
    * over it in one blended draw, so one pipeline is enough. */
   const bool blends = input->blend;
   const VkPipelineColorBlendAttachmentState blend_attachment = {
      .blendEnable = blends ? VK_TRUE : VK_FALSE,
      .srcColorBlendFactor = input->blend_src_colour,
      .dstColorBlendFactor = input->blend_dst_colour,
      .colorBlendOp = input->blend_op_colour,
      .srcAlphaBlendFactor = input->blend_src_alpha,
      .dstAlphaBlendFactor = input->blend_dst_alpha,
      .alphaBlendOp = input->blend_op_alpha,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   /* One blend attachment per colour attachment: Vulkan requires the pipeline's
    * count to equal the render pass's, and every attachment takes the caller's
    * settings. */
   VkPipelineColorBlendAttachmentState blend_attachments[PS5VK_TRIANGLE_MAX_TARGETS];
   for (uint32_t at = 0; at < triangle->colour_attachment_count; at++)
      blend_attachments[at] = blend_attachment;
   VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = triangle->colour_attachment_count,
      .pAttachments = blend_attachments,
   };
   /* The constant factors' four values, which the driver records as
    * CB_BLEND_RED/GREEN/BLUE/ALPHA (input->blend_constants). */
   for (unsigned index = 0; index < 4; index++)
      blend.blendConstants[index] = input->blend_constants[index];
   /* Phase C5's depth state, which the driver turns into DB_DEPTH_CONTROL
    * (driver/ps5vk_draw.c). A frame without a depth attachment passes none. */
   const struct ps5vk_stencil_state *const stencil = &input->stencil[index];
   const VkStencilOpState stencil_face = {
      .failOp = stencil->fail_op,
      .passOp = stencil->pass_op,
      .depthFailOp = stencil->depth_fail_op,
      .compareOp = stencil->compare_op,
      .compareMask = stencil->compare_mask,
      .writeMask = stencil->write_mask,
      .reference = stencil->reference,
   };
   const VkPipelineDepthStencilStateCreateInfo depth_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = input->depth_test ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = input->depth_write ? VK_TRUE : VK_FALSE,
      .depthCompareOp = input->depth_compare_op,
      .depthBoundsTestEnable = VK_FALSE,
      .stencilTestEnable = stencil->test ? VK_TRUE : VK_FALSE,
      .front = stencil_face,
      .back = stencil_face,
   };
   /* R8: a frame whose draws set the depth bias themselves declares it dynamic,
    * so the pipeline's rasterization state does not bake it (Vulkan ignores the
    * three factors then, Valid Usage). No other frame declares any dynamic
    * state, which is what every pipeline before this did. */
   const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_DEPTH_BIAS};
   const VkPipelineDynamicStateCreateInfo dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 1,
      .pDynamicStates = dynamic_states,
   };
   const VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = shaders->pixel_spirv ? 2 : 1,
      .pStages = stages,
      .pDynamicState = input->dynamic_depth_bias ? &dynamic_state : NULL,
      .pVertexInputState = input->attribute_count != 0 ? &geometry_input : &vertex_input,
      .pInputAssemblyState = &assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .pDepthStencilState = input->depth ? &depth_state : NULL,
      .layout = triangle->layout,
      .renderPass = triangle->first_pass,
   };
   return step(triangle, "create_graphics_pipeline",
               CALL(triangle, CreateGraphicsPipelines)(triangle->device, VK_NULL_HANDLE, 1,
                                                       &pipeline_info, NULL,
                                                       &triangle->pipelines[index]),
               NULL);
}

enum ps5vk_triangle_status
ps5vk_triangle_create(struct ps5vk_triangle *triangle, const struct ps5vk_triangle_input *input)
{
   memset(triangle, 0, sizeof(*triangle));
   triangle->get_instance_proc_addr = input->get_instance_proc_addr;
   triangle->report = input->report;
   triangle->pipeline_count = input->pipeline_count;
   triangle->output = input->output;
   /* Phase C4's render-to-texture case changes what a frame records and what
    * its sets name, so the program keeps it (input->texture_is_rendered). */
   /* R10's subpass-input probe, which changes the frame's render pass and the
    * attachments its framebuffers name (input->subpass_input). */
   triangle->subpass_input = input->subpass_input;
   triangle->texture_is_rendered = input->texture_is_rendered;
   triangle->texture_tiled = input->texture_tiled;
   triangle->texture_upload = input->texture_upload;
   triangle->texture_blit_mips = input->texture_blit_mips;
   /* Phase C5's depth attachment, and what a frame's passes do with it. */
   triangle->depth = input->depth;
   triangle->depth_clear_image = triangle->depth && input->depth_clear_image;
   triangle->depth_array_view = triangle->depth && input->depth_array_view;
   triangle->depth_load_op = triangle->depth && input->depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR
                                ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   triangle->stencil_load_op = input->stencil_load_op;
   triangle->depth_clear_value = input->depth_clear_value;
   triangle->depth_format = input->depth_format != VK_FORMAT_UNDEFINED ? input->depth_format
                                                                      : VK_FORMAT_D32_SFLOAT;
   /* Phase V0-query: the scissor the pipelines declare, and the occlusion query
    * each frame's draws are recorded inside. A caller that leaves both unset
    * records what every frame before that phase recorded. */
   triangle->use_scissor = input->use_scissor;
   triangle->scissor = input->scissor;
   triangle->query_pool = input->query_pool;
   triangle->timestamp_pool = VK_NULL_HANDLE;
   triangle->query = input->query;
   /* Phase C7: the chain the sampled image is created with, the mip filter the
    * samplers declare, the levels they reach, and where each level's texels sit
    * in the staging buffer. */
   triangle->base_vertex = input->base_vertex;
   triangle->indirect = input->indirect;
   triangle->secondary = input->secondary;
   /* The frame's clear colour: PS5VK_TRIANGLE_CLEAR_WORD's own unless the
    * caller passed one (V0-formats' colour targets blend over theirs). */
   const bool clear_set = input->clear_colour[0] != 0.0f || input->clear_colour[1] != 0.0f ||
                          input->clear_colour[2] != 0.0f || input->clear_colour[3] != 0.0f;
   triangle->clear_colour[0] = clear_set ? input->clear_colour[0] : 64.0f / 255.0f;
   triangle->clear_colour[1] = clear_set ? input->clear_colour[1] : 128.0f / 255.0f;
   triangle->clear_colour[2] = clear_set ? input->clear_colour[2] : 1.0f;
   triangle->clear_colour[3] = clear_set ? input->clear_colour[3] : 1.0f;
   triangle->instance_count = input->instance_count != 0 ? input->instance_count : 1u;
   triangle->explicit_viewport = input->explicit_viewport;
   triangle->texture_levels = input->texture_levels > 1 ? input->texture_levels : 1u;
   triangle->texture_layers = input->texture_layers > 1 ? input->texture_layers : 1u;
   triangle->texture_mip_linear = input->texture_mip_linear;
   triangle->texture_max_lod = input->texture_max_lod;
   {
      VkDeviceSize at = 0;
      for (uint32_t layer = 0; layer < triangle->texture_layers; layer++) {
         for (uint32_t level = 0; level < triangle->texture_levels; level++) {
            triangle->texture_level_offsets[layer * PS5VK_TRIANGLE_MAX_MIP_LEVELS + level] = at;
            at += (VkDeviceSize)PS5VK_MAX2(input->texture_width >> level, 1u) *
                  PS5VK_MAX2(input->texture_height >> level, 1u) * 4;
         }
      }
   }
   const bool display = input->output == PS5VK_TRIANGLE_OUTPUT_DISPLAY;
   if (input->pipeline_count < 1 || input->pipeline_count > PS5VK_TRIANGLE_MAX_PIPELINES ||
       (input->output != PS5VK_TRIANGLE_OUTPUT_IMAGE && !display)) {
      step(triangle, "check_input", VK_ERROR_INITIALIZATION_FAILED,
           "one or two pipelines, and an image or the display");
      return PS5VK_TRIANGLE_FAILED;
   }

   /* The instance, with the driver's messages when VK_EXT_debug_utils exists,
    * and the display extensions to draw to the display. */
   VkExtensionProperties extensions[PS5VK_TRIANGLE_MAX_EXTENSIONS];
   uint32_t extension_count = PS5VK_TRIANGLE_MAX_EXTENSIONS;
   bool debug_utils = false;
   if (CALL(triangle, EnumerateInstanceExtensionProperties)(NULL, &extension_count, extensions) >= 0) {
      for (uint32_t index = 0; index < extension_count; index++)
         debug_utils = debug_utils || strcmp(extensions[index].extensionName,
                                             VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
   }
   const char *enabled[3];
   uint32_t enabled_count = 0;
   if (debug_utils)
      enabled[enabled_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
   if (display) {
      enabled[enabled_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
      enabled[enabled_count++] = VK_KHR_DISPLAY_EXTENSION_NAME;
   }
   const VkApplicationInfo application = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "ps5vk triangles",
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
      .enabledExtensionCount = enabled_count,
      .ppEnabledExtensionNames = enabled,
   };
   if (!step(triangle, "create_instance",
             CALL(triangle, CreateInstance)(&instance_info, NULL, &triangle->instance),
             debug_utils ? "with VK_EXT_debug_utils" : "without VK_EXT_debug_utils"))
      return PS5VK_TRIANGLE_FAILED;
   if (debug_utils) {
      const VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
         .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
         .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
         .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
         .pfnUserCallback = forward_message,
         .pUserData = (void *)input->report,
      };
      step(triangle, "create_debug_messenger",
           CALL(triangle, CreateDebugUtilsMessengerEXT)(triangle->instance, &messenger_info, NULL,
                                                        &triangle->messenger),
           "driver messages are reported as vk_message");
   }

   /* The physical device, the display surface, the device and its queue. */
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   uint32_t physical_count = 1;
   if (!step(triangle, "enumerate_physical_devices",
             first_entries(CALL(triangle, EnumeratePhysicalDevices)(triangle->instance,
                                                                     &physical_count, &physical)),
             NULL) ||
       physical == VK_NULL_HANDLE)
      return PS5VK_TRIANGLE_FAILED;
   if (display && !create_display_surface(triangle, physical))
      return PS5VK_TRIANGLE_FAILED;
   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const char *const device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = display ? 1 : 0,
      .ppEnabledExtensionNames = device_extensions,
   };
   if (!step(triangle, "create_device",
             CALL(triangle, CreateDevice)(physical, &device_info, NULL, &triangle->device), NULL))
      return PS5VK_TRIANGLE_FAILED;
   CALL(triangle, GetDeviceQueue)(triangle->device, 0, 0, &triangle->queue);
   /* Phase V0-query: the buffer a frame records vkCmdCopyQueryPoolResults into,
    * host-visible so the caller reads the copied result back. */
   if (input->query_copy) {
      triangle->query_pool = input->query_pool;
      triangle->query = input->query;
      if (!create_buffer(triangle, physical, sizeof(uint64_t) * 4u,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, "query result", NULL,
                         &triangle->query_copy_buffer, &triangle->query_copy_memory,
                         &triangle->query_copy_mapped))
         return PS5VK_TRIANGLE_FAILED;
      memset(triangle->query_copy_mapped, 0, sizeof(uint64_t) * 4u);
   }
   /* An indirect frame's draws read their parameters from a buffer of the
    * harness's own, which the recording fills (Phase C2). */
   if (input->indirect &&
       !create_buffer(triangle, physical, 64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "indirect", NULL,
                      &triangle->indirect_buffer, &triangle->indirect_memory,
                      &triangle->indirect_mapped))
      return PS5VK_TRIANGLE_FAILED;

   /* The colour targets. */
   /* Phase C8's sample count reaches the target's image, its passes and the
    * pipelines through the program, and nothing else uses it. */
   triangle->samples = input->samples == VK_SAMPLE_COUNT_4_BIT ? VK_SAMPLE_COUNT_4_BIT
                                                               : VK_SAMPLE_COUNT_1_BIT;
   /* A resolve's destination is the program's own image, which is created
    * before the four-sample source is: the flag has to be set here, not in
    * create_resolve_target (Phase C8). */
   triangle->resolve_output = input->resolve_output;
   triangle->resolve_destination_transfer_only = input->resolve_destination_transfer_only;
   triangle->dynamic_depth_bias = input->dynamic_depth_bias;
   memcpy(triangle->depth_bias_first, input->depth_bias_first, sizeof(triangle->depth_bias_first));
   memcpy(triangle->depth_bias_second, input->depth_bias_second, sizeof(triangle->depth_bias_second));
   triangle->push_constant_bytes = input->push_constant_bytes;
   triangle->push_constant_stages = input->push_constant_stages;
   memcpy(triangle->push_constant_first, input->push_constant_first,
          sizeof(triangle->push_constant_first));
   memcpy(triangle->push_constant_second, input->push_constant_second,
          sizeof(triangle->push_constant_second));
   triangle->first_draw_indices = input->first_draw_indices;
   triangle->first_index = input->first_index;
   /* The index count a frame's draws ask for, which V0-robust raises past what
    * the buffer holds (ps5vk_triangle_set_draw_index_count) and R64 keeps below
    * it. Zero keeps the buffer's own count. It was set only with a uniform
    * buffer, so a frame without one drew the whole index buffer: R64's frames,
    * whose buffers hold poison past their draws, drew it. */
   triangle->draw_index_count = input->draw_index_count;
   triangle->two_descriptor_sets = input->two_descriptor_sets;
   triangle->two_passes = input->two_passes;
   triangle->texture_address_mode_set = input->texture_address_mode_set;
   triangle->colour_attachment_count =
      input->color_attachment_count > 1 ? input->color_attachment_count : 1u;
   triangle->separated_texture_pair = input->separated_texture_pair;
   triangle->display_readback = input->display_readback;
   /* Every colour attachment's format, whatever creates them: the caller's, or
    * the RGBA8 the canary ran (ps5vk_triangle.c, create_image and
    * create_color_attachments). */
   triangle->format = input->target_format != VK_FORMAT_UNDEFINED ? input->target_format
                                                                 : VK_FORMAT_R8G8B8A8_UNORM;
   VkPhysicalDeviceProperties properties;
   CALL(triangle, GetPhysicalDeviceProperties)(physical, &properties);
   triangle->max_color_attachments = properties.limits.maxColorAttachments;
   triangle->sampler_anisotropy = input->sampler_anisotropy;
   triangle->sampler_max_anisotropy = input->sampler_max_anisotropy;
   triangle->texture_address_mode = input->texture_address_mode;
   triangle->rasterization_cull_mode = input->rasterization_cull_mode;
   triangle->rasterization_discard = input->rasterization_discard;
   triangle->depth_bias_enable = input->depth_bias_enable;
   triangle->depth_bias_constant = input->depth_bias_constant;
   triangle->depth_bias_slope = input->depth_bias_slope;
   triangle->depth_bias_clamp = input->depth_bias_clamp;
   if (display ? !create_swapchain(triangle, physical)
               : (input->color_attachment_count > 1
                     ? !create_color_attachments(triangle, physical, input)
                     : !create_image(triangle, physical)))
      return PS5VK_TRIANGLE_FAILED;

   if (triangle->display_readback &&
       !create_buffer(triangle, physical,
                      (VkDeviceSize)PS5VK_TRIANGLE_WIDTH * PS5VK_TRIANGLE_HEIGHT * 4u,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, "display readback", NULL,
                      &triangle->display_readback_buffer, &triangle->display_readback_memory,
                      &triangle->display_readback_mapped))
      return PS5VK_TRIANGLE_FAILED;

   /* The caller's geometry, when it draws an indexed frame (Phase C2). */
   if (!create_geometry(triangle, physical, input))
      return PS5VK_TRIANGLE_FAILED;

   /* The caller's uniform buffer and its set layout (Phase C3), before the
    * pipeline layout below that names the set: a set layout must be alive
    * when a pipeline layout holding it is created. */
   if (!create_uniform(triangle, physical, input))
      return PS5VK_TRIANGLE_FAILED;

   /* Phase C4's render-to-texture case: the second colour image the frame
    * renders the caller's texels into and then samples. Before create_texture,
    * whose set has to name that image's view. Phase C5's depth attachment joins
    * it, before the render passes and framebuffers that name it, and Phase C7's
    * copied texture the same way: its view is what the frame samples. */
   if (!create_subpass_target(triangle, physical) ||
       !create_rendered_texture(triangle, physical, input) ||
       !create_copied_texture(triangle, physical, input) ||
       !create_resolve_target(triangle, physical, input) ||
       !create_depth_attachment(triangle, physical))
      return PS5VK_TRIANGLE_FAILED;

   /* The caller's texture, its view, its samplers and its sets (Phase C4),
    * before the pipeline layout for the same reason. A caller that fetches a
    * uniform texel buffer instead (V0-formats' descriptor-type rows) gets its
    * buffer, view and set here, in the same one set at 0. */
   if (!create_texture(triangle, physical, input) ||
       !create_multiset_textures(triangle, physical, input) ||
       !create_texel_buffer(triangle, physical, input) ||
       !create_storage_image(triangle, physical, input))
      return PS5VK_TRIANGLE_FAILED;

   /* A view and a framebuffer per target, both render passes. */
   const VkImageLayout target_layout = display ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                              : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   if (!step(triangle, "create_render_pass",
             triangle->subpass_input
                ? create_subpass_pass(triangle, &triangle->first_pass)
                : create_render_pass(triangle, triangle->format, input->load_op, target_layout,
                                     triangle->colour_attachment_count, &triangle->first_pass),
             "first") ||
       !step(triangle, "create_render_pass",
             create_render_pass(triangle, triangle->format, VK_ATTACHMENT_LOAD_OP_LOAD,
                                target_layout, triangle->colour_attachment_count,
                                &triangle->load_pass),
             "loading"))
      return PS5VK_TRIANGLE_FAILED;
   for (uint32_t index = 0; index < triangle->image_count; index++) {
      const VkImageViewCreateInfo view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = triangle->images[index],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = triangle->format,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      if (!step(triangle, "create_image_view",
                CALL(triangle, CreateImageView)(triangle->device, &view_info, NULL,
                                                &triangle->views[index]),
                NULL))
         return PS5VK_TRIANGLE_FAILED;
      /* Phase C5: a frame with a depth attachment gives every framebuffer the
       * depth view after its colour view. */
      /* Every colour attachment the pass declares, in its order, then the depth
       * view (Phase C5). A one-attachment frame keeps the view of the frame's own
       * image -- the display's per-image view, or the resolve destination's. */
      VkImageView framebuffer_views[PS5VK_TRIANGLE_MAX_TARGETS + 1];
      /* R10: the probe's framebuffer names the subpass target first, then the
       * program's own image -- the two attachments the two-subpass pass
       * declares. Every other frame's views are unchanged. */
      uint32_t attachment_count = triangle->colour_attachment_count;
      if (triangle->subpass_input) {
         framebuffer_views[0] = triangle->subpass_view;
         framebuffer_views[1] = triangle->resolve_output ? triangle->resolve_view
                                                         : triangle->views[index];
         attachment_count = 2;
      } else {
         for (uint32_t at = 0; at < triangle->colour_attachment_count; at++)
            framebuffer_views[at] =
               triangle->colour_attachment_count > 1
                  ? triangle->views[at]
                  : (triangle->resolve_output ? triangle->resolve_view : triangle->views[index]);
      }
      framebuffer_views[attachment_count] = triangle->depth_view;
      const VkFramebufferCreateInfo framebuffer_info = {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = triangle->first_pass,
         .attachmentCount = attachment_count + (triangle->depth ? 1u : 0u),
         .pAttachments = framebuffer_views,
         .width = PS5VK_TRIANGLE_WIDTH,
         .height = PS5VK_TRIANGLE_HEIGHT,
         .layers = 1,
      };
      if (!step(triangle, "create_framebuffer",
                CALL(triangle, CreateFramebuffer)(triangle->device, &framebuffer_info, NULL,
                                                  &triangle->framebuffers[index]),
                NULL))
         return PS5VK_TRIANGLE_FAILED;
   }
   /* The set layouts the pipeline layout names, and the sets a frame binds at
    * those indices, in one order: the caller's uniform buffer, then its
    * texture. The uniform keeps set 0 whenever the caller supplied one, as
    * before Phase C4; the texture's set is then the second one, and set 0 when
    * it is the only set -- where the probe sets that sample declare their
    * combined image sampler (probes/m3-texture/bindings.txt). A caller that
    * supplies both gets two sets with a table each, which is R7's own case
    * (driver/ps5vk_draw.c). No descriptors at all leaves the layout exactly as
    * it was before Phase C3. */
   /* R7: two empty set layouts, so the frame's pipeline is valid and its set
    * count is the only thing the draw can refuse. No descriptors are written and
    * none are bound: a *pipeline* that declares more than one set is refused
    * when it is drawn (driver/ps5vk_pipeline.c, ps5vk_draw_refusal). */
   if (triangle->multiset_set_layout != VK_NULL_HANDLE) {
      /* R7's vkQuake shape: three textures at set 0 and the block the set
       * create_uniform built is set 1 -- the order vkQuake's world and md5
       * pipeline layouts have, which is what needs one table per set. */
      triangle->set_layouts[0] = triangle->multiset_set_layout;
      triangle->descriptor_sets[0] = triangle->multiset_set;
      triangle->set_layouts[1] = triangle->set_layout;
      triangle->descriptor_sets[1] = triangle->descriptor_set;
      triangle->set_count = 2;
   } else if (triangle->subpass_input) {
      /* R10's probe: set 0 binding 0 is the subpass input attachment the second
       * pipeline's fragment shader reads. Vulkan forbids writing a descriptor of
       * this type -- the entry comes from the subpass, which is what the driver
       * builds it from (driver/ps5vk_draw.c) -- but the *layout* is the
       * application's, and it is what tells the driver's compiler that the
       * shader's binding exists at all: without it the stage's metadata names no
       * binding, no table is built, and the read fetches through an empty
       * descriptor. The set itself is never bound and never written, which is
       * what this type means. */
      const VkDescriptorSetLayoutBinding input_attachment = {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };
      const VkDescriptorSetLayoutCreateInfo set_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings = &input_attachment,
      };
      if (!step(triangle, "create_descriptor_set_layout",
                CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &set_info, NULL,
                                                          &triangle->set_layout),
                "set 0 binding 0 is an input attachment"))
         return PS5VK_TRIANGLE_FAILED;
      triangle->set_layouts[triangle->set_count++] = triangle->set_layout;
   } else if (input->two_descriptor_sets) {
      const VkDescriptorSetLayoutCreateInfo empty_set = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 0,
      };
      if (!step(triangle, "create_descriptor_set_layout",
                CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &empty_set, NULL,
                                                          &triangle->set_layout),
                "an empty set 0") ||
          !step(triangle, "create_descriptor_set_layout",
                CALL(triangle, CreateDescriptorSetLayout)(triangle->device, &empty_set, NULL,
                                                          &triangle->second_set_layout),
                "an empty set 1"))
         return PS5VK_TRIANGLE_FAILED;
      triangle->set_layouts[triangle->set_count++] = triangle->set_layout;
      triangle->set_layouts[triangle->set_count++] = triangle->second_set_layout;
   } else if (triangle->set_layout != VK_NULL_HANDLE) {
      triangle->set_layouts[triangle->set_count] = triangle->set_layout;
      triangle->descriptor_sets[triangle->set_count] = triangle->descriptor_set;
      triangle->set_count++;
   }
   if (triangle->texture_set_layout != VK_NULL_HANDLE) {
      triangle->set_layouts[triangle->set_count] = triangle->texture_set_layout;
      triangle->descriptor_sets[triangle->set_count] = triangle->texture_set;
      triangle->set_count++;
   }
   /* The sampler's own set follows the texture's, which is the order the
    * separated pair's pipeline layout has: set 0 the image, set 1 the sampler
    * (input->separated_texture_pair). */
   if (triangle->sampler_set_layout != VK_NULL_HANDLE) {
      triangle->set_layouts[triangle->set_count] = triangle->sampler_set_layout;
      triangle->descriptor_sets[triangle->set_count] = triangle->sampler_set;
      triangle->set_count++;
   }
   /* R9: the push-constant range the caller's shaders read, for the stages it
    * names. A caller that declares none leaves the layout exactly as it was. */
   const VkPushConstantRange push_range = {
      .stageFlags = triangle->push_constant_stages,
      .offset = 0,
      .size = triangle->push_constant_bytes,
   };
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = triangle->set_count,
      .pSetLayouts = triangle->set_layouts,
      .pushConstantRangeCount = triangle->push_constant_bytes != 0 ? 1u : 0u,
      .pPushConstantRanges = triangle->push_constant_bytes != 0 ? &push_range : NULL,
   };
   if (!step(triangle, "create_pipeline_layout",
             CALL(triangle, CreatePipelineLayout)(triangle->device, &layout_info, NULL,
                                                  &triangle->layout),
             NULL))
      return PS5VK_TRIANGLE_FAILED;
   for (uint32_t index = 0; index < input->pipeline_count; index++) {
      if (!create_pipeline(triangle, index, &input->shaders[index], input))
         return PS5VK_TRIANGLE_FAILED;
   }

   if (input->detach_depth) {
      assert(input->depth && input->pipeline_count == 1 && !display &&
             triangle->colour_attachment_count == 1);
      triangle->depth = false;
      const VkResult created = create_render_pass(triangle, triangle->format,
         VK_ATTACHMENT_LOAD_OP_LOAD, target_layout, 1, &triangle->detached_pass);
      triangle->depth = true;
      if (!step(triangle, "create_detached_pass", created, "colour only after depth"))
         return PS5VK_TRIANGLE_FAILED;
      const VkFramebufferCreateInfo framebuffer = {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = triangle->detached_pass,
         .attachmentCount = 1,
         .pAttachments = &triangle->views[0],
         .width = PS5VK_TRIANGLE_WIDTH,
         .height = PS5VK_TRIANGLE_HEIGHT,
         .layers = 1,
      };
      if (!step(triangle, "create_detached_framebuffer",
                CALL(triangle, CreateFramebuffer)(triangle->device, &framebuffer, NULL,
                                                  &triangle->detached_framebuffer), NULL))
         return PS5VK_TRIANGLE_FAILED;
      struct ps5vk_triangle_input colour_input = *input;
      colour_input.depth = false;
      const VkRenderPass saved = triangle->first_pass;
      triangle->first_pass = triangle->detached_pass;
      const bool pipeline_ok = create_pipeline(triangle, PS5VK_TRIANGLE_MAX_PIPELINES - 1,
                                                &input->shaders[0], &colour_input);
      triangle->first_pass = saved;
      if (!pipeline_ok)
         return PS5VK_TRIANGLE_FAILED;
   }

   /* Two command buffers, a fence, and for the display the two semaphores. */
   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   if (!step(triangle, "create_command_pool",
             CALL(triangle, CreateCommandPool)(triangle->device, &pool_info, NULL, &triangle->pool),
             NULL))
      return PS5VK_TRIANGLE_FAILED;
   const VkCommandBufferAllocateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = triangle->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 2,
   };
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   /* A frame through a secondary command buffer (Phase B8) needs one: the
    * level is fixed at allocation, and a primary recorded with the render-pass
    * continue flag is not a secondary the driver would execute. */
   const VkCommandBufferAllocateInfo secondary_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = triangle->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
      .commandBufferCount = 1,
   };
   if (!step(triangle, "allocate_command_buffers",
             CALL(triangle, AllocateCommandBuffers)(triangle->device, &command_info,
                                                    triangle->commands),
             NULL) ||
       (triangle->secondary &&
        !step(triangle, "allocate_secondary_command_buffer",
              CALL(triangle, AllocateCommandBuffers)(triangle->device, &secondary_info,
                                                     &triangle->secondary_command),
              NULL)) ||
       !step(triangle, "create_fence",
             CALL(triangle, CreateFence)(triangle->device, &fence_info, NULL, &triangle->fence),
             NULL) ||
       (display &&
        (!step(triangle, "create_semaphore",
               CALL(triangle, CreateSemaphore)(triangle->device, &semaphore_info, NULL,
                                               &triangle->image_available),
               "image available") ||
         !step(triangle, "create_semaphore",
               CALL(triangle, CreateSemaphore)(triangle->device, &semaphore_info, NULL,
                                               &triangle->render_finished),
               "render finished"))))
      return PS5VK_TRIANGLE_FAILED;
   return PS5VK_TRIANGLE_OK;
}

/* One of a frame's draws: the caller's indexed geometry, or the three
 * vertices the B7/B8/C1 sets draw with no binding at all. Every draw binds
 * its own buffers, since the driver builds a draw's vertex-buffer table from
 * the bindings in effect when it records the draw (driver/ps5vk_draw.c), and
 * the second draw of a frame must not depend on what the first one bound. */
static void
draw(struct ps5vk_triangle *triangle, VkCommandBuffer command)
{
   /* R8/R9: the state the *draw* carries rather than the pipeline. Each draw
    * sets its own values before it records, so a frame of two draws tells a
    * driver that reads them from the first draw's and one that ignores them
    * apart: the first draw takes the first values and the second the second. */
   const bool first_draw = triangle->draws_recorded == 0;
   if (triangle->dynamic_depth_bias) {
      const float *const bias = first_draw ? triangle->depth_bias_first : triangle->depth_bias_second;
      /* vkCmdSetDepthBias's own argument order: the constant factor, the clamp,
       * then the slope factor (vulkan_core.h). */
      CALL(triangle, CmdSetDepthBias)(command, bias[0], bias[1], bias[2]);
   }
   if (triangle->push_constant_bytes != 0) {
      const void *const bytes =
         first_draw ? triangle->push_constant_first : triangle->push_constant_second;
      CALL(triangle, CmdPushConstants)(command, triangle->layout,
                                       triangle->push_constant_stages, 0,
                                       triangle->push_constant_bytes, bytes);
   }
   triangle->draws_recorded++;
   const uint32_t instances = triangle->instance_count != 0 ? triangle->instance_count : 1u;
   /* No vertex buffer at all: the three vertices the B7/B8/C1 sets generate from
    * gl_VertexIndex. A frame with vertices and no indices is *not* this draw --
    * it binds its buffer and draws its own count below. R6's first strip frame
    * took this branch, because it tested the index count alone: it drew three
    * vertices where it declared four, through a pipeline whose vertex input reads
    * binding 0, with no buffer bound (docs/M5_PHASE_C.md, the strip wedge). */
   if (triangle->index_count == 0 && triangle->vertex_count == 0) {
      if (!triangle->indirect) {
         CALL(triangle, CmdDraw)(command, 3, instances, 0, 0);
         return;
      }
      /* Single-draw stride is ignored; use zero like vkQuake.
       * The same parameters through an indirect buffer: 3 vertices, the
       * instances the caller asked for, nothing else (VkDrawIndirectCommand). */
      const uint32_t parameters[4] = {3, instances, 0, 0};
      memcpy(triangle->indirect_mapped, parameters, sizeof(parameters));
      CALL(triangle, CmdDrawIndirect)(command, triangle->indirect_buffer, 0, 1, 0);
      return;
   }
   const VkDeviceSize zero = 0;
   CALL(triangle, CmdBindVertexBuffers)(command, 0, 1, &triangle->vertex_buffer, &zero);
   if (triangle->index_count == 0) {
      /* The caller's vertices with no indices: VkCmdDraw, which is the shape a
       * triangle strip is (input.primitive_topology). */
      if (!triangle->indirect) {
         CALL(triangle, CmdDraw)(command, triangle->vertex_count, instances, 0, 0);
         return;
      }
      const uint32_t parameters[4] = {triangle->vertex_count, instances, 0, 0};
      memcpy(triangle->indirect_mapped, parameters, sizeof(parameters));
      CALL(triangle, CmdDrawIndirect)(command, triangle->indirect_buffer, 0, 1, 0);
      return;
   }
   CALL(triangle, CmdBindIndexBuffer)(command, triangle->index_buffer, 0, triangle->index_type);
   uint32_t indices =
      triangle->draw_index_count != 0 ? triangle->draw_index_count : triangle->index_count;
   /* A frame whose two draws must differ in what they drew splits the index
    * buffer between them: the first draw takes first_draw_indices, the second
    * the rest. Zero leaves both draws the whole buffer. */
   uint32_t first_index = 0;
   if (triangle->first_draw_indices != 0) {
      if (first_draw) {
         indices = triangle->first_draw_indices < indices ? triangle->first_draw_indices : indices;
      } else {
         first_index = triangle->first_draw_indices < indices ? triangle->first_draw_indices
                                                              : indices;
         indices -= first_index;
      }
   }
   first_index += triangle->first_index;
   if (!triangle->indirect) {
      CALL(triangle, CmdDrawIndexed)(command, indices, instances, first_index, triangle->base_vertex,
                                     0);
      return;
   }
   /* VkDrawIndexedIndirectCommand: indexCount, instanceCount, firstIndex,
    * vertexOffset, firstInstance. */
   const uint32_t parameters[5] = {indices, instances, first_index,
                                   (uint32_t)triangle->base_vertex, 0};
   memcpy(triangle->indirect_mapped, parameters, sizeof(parameters));
   CALL(triangle, CmdDrawIndexedIndirect)(command, triangle->indirect_buffer, 0, 1, 0);
}

/* Records the fill pass a render-to-texture frame starts with (Phase C4), into
 * the same command buffer as the frame that samples what it drew: the caller's
 * texels over the whole of the second colour image, one full-target quad drawn
 * with the frame's own first pipeline. The draw samples the uploaded image --
 * through the set that names it, in place of the frame's, which names the image
 * this pass is writing -- and the pattern it leaves covers every pixel of the
 * image, so the frame's pass reads the caller's texels back. */
static void
record_fill(struct ps5vk_triangle *triangle, VkCommandBuffer command)
{
   /* PS5VK_TRIANGLE_CLEAR_WORD's bytes, as the frame's own pass clears to; the
    * quad covers the whole target, so nothing of it survives. */
   const VkClearValue clear = {
      .color = {.float32 = {64.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f}}};
   const VkRenderPassBeginInfo pass_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = triangle->rendered_pass,
      .framebuffer = triangle->rendered_framebuffer,
      .renderArea = {{0, 0}, {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT}},
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   CALL(triangle, CmdBeginRenderPass)(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
   CALL(triangle, CmdBindPipeline)(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   triangle->pipelines[0]);
   /* The frame's sets with the uploaded texture's set in the last slot, which
    * is the texture's: create_texture names the uniform buffer first and the
    * texture second. */
   VkDescriptorSet sets[2];
   memcpy(sets, triangle->descriptor_sets, sizeof(sets));
   if (triangle->set_count != 0) {
      sets[triangle->set_count - 1] = triangle->texture_source_set;
      CALL(triangle, CmdBindDescriptorSets)(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            triangle->layout, 0, triangle->set_count, sets, 0,
                                            NULL);
   }
   const VkDeviceSize zero = 0;
   CALL(triangle, CmdBindVertexBuffers)(command, 0, 1, &triangle->fill_buffer, &zero);
   CALL(triangle, CmdDraw)(command, PS5VK_TRIANGLE_FILL_VERTICES, 1, 0, 0);
   CALL(triangle, CmdEndRenderPass)(command);
}

/* Records one render pass into framebuffer with one draw per pipeline given;
 * then is VK_NULL_HANDLE for a single draw. fill asks for the render-to-texture
 * case's fill pass first, in this command buffer: the frame's draw samples the
 * image that pass rendered into, and the two passes have to reach the queue in
 * one command buffer for the driver to see the sample follow the render
 * (driver/ps5vk_draw.c, ps5vk_sampled_image). */
static void
record_body(struct ps5vk_triangle *triangle, VkCommandBuffer command, VkPipeline pipeline,
            VkPipeline then)
{
   /* A frame that sets its own viewport and scissor does it inside the render
    * pass, as an application that does not trust the pipeline's static state
    * would: the same full-target rect the pipeline declares, which every earlier
    * frame gets from the pipeline alone. */
   if (triangle->explicit_viewport) {
      const VkViewport viewport = {0.0f, 0.0f, PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 0.0f,
                                   1.0f};
      const VkRect2D scissor = {{0, 0}, {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT}};
      CALL(triangle, CmdSetViewport)(command, 0, 1, &viewport);
      CALL(triangle, CmdSetScissor)(command, 0, 1, &scissor);
   }
   /* V0-query's sample: beginning the query inside the render pass, after its
    * clear load operation has been recorded, brackets the draws alone. A
    * render pass clear is not a fragment that passes a depth test, and the
    * console measured a full-target draw with a clear in front of it as exactly
    * the geometry's samples (docs/M5_PHASE_C.md, V0-query); the sample must not
    * count the clear. */
   if (triangle->query_pool != VK_NULL_HANDLE)
      CALL(triangle, CmdBeginQuery)(command, triangle->query_pool, triangle->query, 0);
   /* V0-query's timestamps: the clock before the frame's draws and after them,
    * so the two readings bracket the geometry the way the z-pass samples do.
    * The stage flags are ignored by a driver that records the write inline;
    * what matters is where the packet lands. */
   if (triangle->timestamp_pool != VK_NULL_HANDLE)
      CALL(triangle, CmdWriteTimestamp)(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                        triangle->timestamp_pool, 0);
   CALL(triangle, CmdBindPipeline)(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   /* The caller's descriptor sets, bound with every pipeline: a draw must not
    * depend on what an earlier draw left bound (Phase C3, as the C2 vertex
    * buffers). No descriptors leaves the recording exactly as it was. */
   if (triangle->set_count != 0) {
      const uint32_t dynamic_offsets[2] = {
         triangle->uniform_dynamic_offset,
         PS5VK_TRIANGLE_UNIFORM_BYTES - triangle->uniform_dynamic_offset,
      };
      CALL(triangle, CmdBindDescriptorSets)(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            triangle->layout, 0, triangle->set_count,
                                            triangle->descriptor_sets,
                                            triangle->uniform_dynamic_pair ? 2u : triangle->uniform_dynamic ? 1u : 0u,
                                            triangle->uniform_dynamic ? dynamic_offsets : NULL);
   }
   draw(triangle, command);
   if (triangle->subpass_input && then != VK_NULL_HANDLE) {
      /* R10: subpass 1. The next subpass is what puts the second pipeline's
       * draws into the pass that reads attachment 0 as its input attachment,
       * which is the whole point of the probe: the draw after this packet is
       * the one whose shader reads what the draw before it wrote. */
      CALL(triangle, CmdNextSubpass)(command, VK_SUBPASS_CONTENTS_INLINE);
   }
   if (then != VK_NULL_HANDLE) {
      CALL(triangle, CmdBindPipeline)(command, VK_PIPELINE_BIND_POINT_GRAPHICS, then);
      if (triangle->set_count != 0) {
         CALL(triangle, CmdBindDescriptorSets)(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                               triangle->layout, 0, triangle->set_count,
                                               triangle->descriptor_sets, 0, NULL);
      }
      draw(triangle, command);
   }
   if (triangle->query_pool != VK_NULL_HANDLE)
      CALL(triangle, CmdEndQuery)(command, triangle->query_pool, triangle->query);
   if (triangle->timestamp_pool != VK_NULL_HANDLE)
      CALL(triangle, CmdWriteTimestamp)(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                        triangle->timestamp_pool, 1);
}

/* The frame inside a render pass this command buffer begins itself: the
 * M2-M4/C1 frames' recording. A secondary records the same body without the
 * pass, because its pass comes from the primary that executes it
 * (record_secondary, ps5vk_triangle_draw). */
static VkResult
record(struct ps5vk_triangle *triangle, VkCommandBuffer command, VkRenderPass pass,
       VkFramebuffer framebuffer, VkPipeline pipeline, VkPipeline then, bool fill)
{
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   const VkResult began = CALL(triangle, BeginCommandBuffer)(command, &begin_info);
   if (began != VK_SUCCESS)
      return began;
   if (triangle->query_pool != VK_NULL_HANDLE)
      CALL(triangle, CmdResetQueryPool)(command, triangle->query_pool, triangle->query, 1);
   if (triangle->depth && triangle->depth_clear_image) {
      /* Phase C5's image clear, before the pass that loads what it wrote. */
      const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      const VkClearDepthStencilValue value = {.depth = triangle->depth_clear_value, .stencil = 0};
      CALL(triangle, CmdClearDepthStencilImage)(command, triangle->depth_image,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                &value, 1, &range);
   }
   if (fill)
      record_fill(triangle, command);
   /* PS5VK_TRIANGLE_CLEAR_WORD's bytes, unless the caller passed its own clear
    * colour: a colour no probe shader draws, so a readback tells a cleared pixel
    * from an untouched one. A blending frame passes its destination colour and
    * draws the source over it. */
   const VkClearValue clear = {
      .color = {.float32 = {triangle->clear_colour[0], triangle->clear_colour[1],
                            triangle->clear_colour[2], triangle->clear_colour[3]}}};
   const VkClearValue clears[2] = {
      {.color = {.float32 = {triangle->clear_colour[0], triangle->clear_colour[1],
                             triangle->clear_colour[2], triangle->clear_colour[3]}}},
      {.depthStencil = {.depth = triangle->depth_clear_value, .stencil = 0}},
   };
   const VkRenderPassBeginInfo pass_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT}},
      /* One clear value per attachment whose load operation is CLEAR: the
       * colour always has one, and a frame with a depth attachment whose pass
       * clears it needs the second (Vulkan 1.0 requires the count to cover
       * every CLEAR attachment). Without it the depth clear drew whatever the
       * stack held -- the M4 canary's clear value is what the frame's readback
       * compares against (Phase C5). */
      .clearValueCount = triangle->depth ? 2u : 1u,
      .pClearValues = triangle->depth ? clears : &clear,
   };
   CALL(triangle, CmdBeginRenderPass)(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
   record_body(triangle, command, pipeline, then);
   CALL(triangle, CmdEndRenderPass)(command);
   if (triangle->detached_pass != VK_NULL_HANDLE) {
      const VkRenderPassBeginInfo detached = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = triangle->detached_pass,
         .framebuffer = triangle->detached_framebuffer,
         .renderArea = {{0, 0}, {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT}},
      };
      CALL(triangle, CmdBeginRenderPass)(command, &detached, VK_SUBPASS_CONTENTS_INLINE);
      record_body(triangle, command, triangle->pipelines[PS5VK_TRIANGLE_MAX_PIPELINES - 1],
                    VK_NULL_HANDLE);
      CALL(triangle, CmdEndRenderPass)(command);
   }
   if (triangle->two_passes) {
      /* R6: a *second* render pass in the same command buffer, over the same
       * framebuffer and with the same clear, which is what a renderer that
       * renders offscreen and then draws its target records -- and the shape
       * whose recorded passes corrupted the heap (docs/M5_PHASE_C.md, R6). A
       * frame that resolves its offscreen target records the resolve between
       * the two passes, where an application that resolves its multisampled
       * pass before the main one records it; that copy is what splits the
       * submission, and the second pass's words are what follow the split. */
      if (triangle->resolve_output) {
         const VkImageResolve region = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffset = {0, 0, 0},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffset = {0, 0, 0},
            .extent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
         };
         CALL(triangle, CmdResolveImage)(command, triangle->resolve_image,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         triangle->images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                         &region);
      }
      CALL(triangle, CmdBeginRenderPass)(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
      record_body(triangle, command, pipeline, then);
      CALL(triangle, CmdEndRenderPass)(command);
   }
   if ((triangle->query_pool != VK_NULL_HANDLE || triangle->timestamp_pool != VK_NULL_HANDLE) &&
       triangle->query_copy_buffer != VK_NULL_HANDLE) {
      /* Phase V0-query: the same result vkGetQueryPoolResults answers, written
       * into the harness's buffer by the command this phase implements. It runs
       * after the pass, so the query it copies has ended. */
      const bool timestamp = triangle->timestamp_pool != VK_NULL_HANDLE;
      CALL(triangle, CmdCopyQueryPoolResults)(
         command, timestamp ? triangle->timestamp_pool : triangle->query_pool,
         timestamp ? 1u : triangle->query, 1, triangle->query_copy_buffer, 0,
         2u * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
   }
   if (triangle->resolve_output && !triangle->two_passes) {
      /* Phase C8: the four-sample image the pass just rendered into, resolved
       * into the program's one-sample image, which is what a caller reads back.
       * Its own view and image are the ones the framebuffers named. A frame
       * with a second pass resolves *between* the passes instead
       * (record_resolve below), which is where a renderer that resolves its
       * offscreen target and then draws the main pass records it. */
      const VkImageResolve region = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .srcOffset = {0, 0, 0},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .dstOffset = {0, 0, 0},
         .extent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
      };
      CALL(triangle, CmdResolveImage)(command, triangle->resolve_image,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, triangle->images[0],
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
   }
   if (triangle->display_readback) {
      VkImageMemoryBarrier barrier = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = triangle->images[triangle->image_index],
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      CALL(triangle, CmdPipelineBarrier)(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                                         &barrier);
      const VkBufferImageCopy region = {
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT, 1},
      };
      CALL(triangle, CmdCopyImageToBuffer)(command, barrier.image,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          triangle->display_readback_buffer, 1, &region);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = 0;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      CALL(triangle, CmdPipelineBarrier)(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                                         &barrier);
   }
   return CALL(triangle, EndCommandBuffer)(command);
}

/* The same frame through a secondary command buffer the primary executes: the
 * secondary names its pass but leaves the optional framebuffer unspecified,
 * as vkQuake does. The primary supplies the attachments (R11). The two command buffers
 * submit as one, so the driver's stream is the primary's pass around the
 * secondary's draws. */
static VkResult
record_secondary(struct ps5vk_triangle *triangle, VkCommandBuffer secondary, VkCommandBuffer primary,
                 VkRenderPass pass, VkFramebuffer framebuffer, VkPipeline pipeline)
{
   const VkCommandBufferInheritanceInfo inheritance = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
      .renderPass = pass,
      .subpass = 0,
      /* vkQuake leaves this optional hint unset; the primary supplies it. */
      .framebuffer = VK_NULL_HANDLE,
   };
   const VkCommandBufferBeginInfo secondary_begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
               VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      .pInheritanceInfo = &inheritance,
   };
   VkResult result = CALL(triangle, BeginCommandBuffer)(secondary, &secondary_begin);
   if (result != VK_SUCCESS)
      return result;
   if (triangle->query_pool != VK_NULL_HANDLE)
      CALL(triangle, CmdResetQueryPool)(secondary, triangle->query_pool, triangle->query, 1);
   record_body(triangle, secondary, pipeline, VK_NULL_HANDLE);
   result = CALL(triangle, EndCommandBuffer)(secondary);
   if (result != VK_SUCCESS)
      return result;

   const VkCommandBufferBeginInfo primary_begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   result = CALL(triangle, BeginCommandBuffer)(primary, &primary_begin);
   if (result != VK_SUCCESS)
      return result;
   const VkClearValue clear = {
      .color = {.float32 = {64.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f}}};
   const VkClearValue clears[2] = {
      {.color = {.float32 = {64.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f}}},
      {.depthStencil = {.depth = triangle->depth_clear_value, .stencil = 0}},
   };
   const VkRenderPassBeginInfo pass_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {PS5VK_TRIANGLE_WIDTH, PS5VK_TRIANGLE_HEIGHT}},
      .clearValueCount = triangle->depth ? 2u : 1u,
      .pClearValues = triangle->depth ? clears : &clear,
   };
   CALL(triangle, CmdBeginRenderPass)(primary, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
   CALL(triangle, CmdExecuteCommands)(primary, 1, &secondary);
   CALL(triangle, CmdEndRenderPass)(primary);
   return CALL(triangle, EndCommandBuffer)(primary);
}

/* The uploads a frame submits before its render pass, in a command buffer of
 * their own: the staged geometry's copies (Phase C2), into the buffers the draw
 * binds, and the texture's vkCmdCopyBufferToImage (Phase C4), out of its
 * staging buffer into the sampled image. The driver runs a copy on the CPU
 * where a submission splits, so the frame that follows in the same submission
 * reads what the caller uploaded -- and a copy recorded inside the render pass
 * would split the submission in the middle of it. */
static VkResult
record_uploads(struct ps5vk_triangle *triangle, VkCommandBuffer command)
{
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   const VkResult began = CALL(triangle, BeginCommandBuffer)(command, &begin_info);
   if (began != VK_SUCCESS)
      return began;
   if (triangle->staged) {
      /* The layout create_geometry wrote: the records at the staging buffer's
       * start, the indices right after them. */
      const VkBufferCopy vertex_region = {
         .srcOffset = 0,
         .dstOffset = 0,
         .size = triangle->vertex_bytes,
      };
      const VkBufferCopy index_region = {
         .srcOffset = triangle->vertex_bytes,
         .dstOffset = 0,
         .size = triangle->index_bytes,
      };
      CALL(triangle, CmdCopyBuffer)(command, triangle->staging_buffer, triangle->vertex_buffer, 1,
                                    &vertex_region);
      if (triangle->index_bytes != 0)
         CALL(triangle, CmdCopyBuffer)(command, triangle->staging_buffer, triangle->index_buffer,
                                       1, &index_region);
   }
   if (triangle->texture_image != VK_NULL_HANDLE && !triangle->texture_host_filled &&
       (!triangle->texture_tiled || triangle->texture_upload)) {
      /* One region per level (Phase C7): the staging buffer holds them one
       * after another, each level's rows tightly packed, so a copy reads whole
       * rows and the image's 256-byte row pitch -- or the level's place in the
       * tiled chain -- is the driver's to apply (driver/ps5vk_image.c). A tiled
       * chain copies only when the caller asked for the upload; otherwise the
       * caller fills those texels itself. */
      for (uint32_t layer = 0; layer < triangle->texture_layers; layer++) {
         for (uint32_t level = 0; level < triangle->texture_levels; level++) {
            const uint32_t width = PS5VK_MAX2(triangle->texture_extent.width >> level, 1u);
            const uint32_t height = PS5VK_MAX2(triangle->texture_extent.height >> level, 1u);
            const VkBufferImageCopy region = {
               .bufferOffset = triangle->texture_level_offsets
                               [layer * PS5VK_TRIANGLE_MAX_MIP_LEVELS + level],
               .bufferRowLength = 0,
               .bufferImageHeight = 0,
               .imageSubresource = {texture_is_depth(triangle->texture_format)
                                       ? VK_IMAGE_ASPECT_DEPTH_BIT
                                       : VK_IMAGE_ASPECT_COLOR_BIT,
                                    level, layer, 1},
               .imageOffset = {0, 0, 0},
               .imageExtent = {width, height, 1},
            };
            CALL(triangle, CmdCopyBufferToImage)(command, triangle->texture_staging_buffer,
                                                 triangle->texture_image,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
         }
      }
   }
   if (triangle->texture_blit_mips) {
      const VkMemoryBarrier barrier = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      };
      for (uint32_t level = 1; level < triangle->texture_levels; level++) {
         CALL(triangle, CmdPipelineBarrier)(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
         const VkImageBlit region = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1},
            .srcOffsets = {{0, 0, 0}, {(int32_t)(triangle->texture_extent.width >> (level - 1)),
                                     (int32_t)(triangle->texture_extent.height >> (level - 1)), 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
            .dstOffsets = {{0, 0, 0}, {(int32_t)(triangle->texture_extent.width >> level),
                                     (int32_t)(triangle->texture_extent.height >> level), 1}},
         };
         CALL(triangle, CmdBlitImage)(command, triangle->texture_image, VK_IMAGE_LAYOUT_GENERAL,
            triangle->texture_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_LINEAR);
      }
   }
   if (triangle->texture_copied || triangle->texture_blitted || triangle->texture_blit_scaled) {
      /* Phase C7's copy or one-to-one blit of the uploaded texels into the
       * tiled image the frame samples: the whole image, one region, both
       * offsets at the origin (driver/ps5vk_image.c, ps5vk_CmdCopyImage2 and
       * ps5vk_CmdBlitImage2). The upload above wrote the source in the same
       * command buffer, and the driver splits the submission at each CPU copy,
       * so the copy reads what the upload left. */
      const VkImageAspectFlags transfer_aspect = texture_is_depth(triangle->texture_format)
                                                    ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                    : VK_IMAGE_ASPECT_COLOR_BIT;
      const VkImageSubresourceLayers source = {transfer_aspect, 0, 0, 1};
      const VkImageSubresourceLayers destination = {transfer_aspect, 0, 0, 1};
      const VkExtent3D extent = {triangle->texture_extent.width, triangle->texture_extent.height,
                                 1};
      if (triangle->texture_blit_scaled) {
         /* The middle half of the source, over the whole destination: two
          * source texels per destination texel on each axis. */
         const int32_t middle_x = (int32_t)(extent.width / 4u);
         const int32_t middle_y = (int32_t)(extent.height / 4u);
         const VkImageBlit region = {
            .srcSubresource = source,
            .srcOffsets = {{middle_x, middle_y, 0},
                           {(int32_t)extent.width - middle_x, (int32_t)extent.height - middle_y,
                            1}},
            .dstSubresource = destination,
            .dstOffsets = {{0, 0, 0}, {(int32_t)extent.width, (int32_t)extent.height, 1}},
         };
         CALL(triangle, CmdBlitImage)(command, triangle->texture_image,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, triangle->copied_image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                                      triangle->texture_blit_linear ? VK_FILTER_LINEAR
                                                                    : VK_FILTER_NEAREST);
      } else if (triangle->texture_blitted) {
         const VkImageBlit region = {
            .srcSubresource = source,
            .srcOffsets = {{0, 0, 0}, {(int32_t)extent.width, (int32_t)extent.height, 1}},
            .dstSubresource = destination,
            .dstOffsets = {{0, 0, 0}, {(int32_t)extent.width, (int32_t)extent.height, 1}},
         };
         CALL(triangle, CmdBlitImage)(command, triangle->texture_image,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, triangle->copied_image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                                      VK_FILTER_NEAREST);
      } else {
         const VkImageCopy region = {
            .srcSubresource = source,
            .srcOffset = {0, 0, 0},
            .dstSubresource = destination,
            .dstOffset = {0, 0, 0},
            .extent = extent,
         };
         CALL(triangle, CmdCopyImage)(command, triangle->texture_image,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, triangle->copied_image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      }
   }
   return CALL(triangle, EndCommandBuffer)(command);
}

/* A failed submission or presentation: the GPU may still hold the objects
 * when the device is lost or the wait timed out. */
static enum ps5vk_triangle_status
failure(VkResult result)
{
   return result == VK_ERROR_DEVICE_LOST || result == VK_TIMEOUT ? PS5VK_TRIANGLE_IN_FLIGHT
                                                                 : PS5VK_TRIANGLE_FAILED;
}

enum ps5vk_triangle_status
ps5vk_triangle_draw(struct ps5vk_triangle *triangle, enum ps5vk_triangle_grouping grouping)
{
   const VkDevice device = triangle->device;
   const char *const name = ps5vk_triangle_grouping_name(grouping);
   const bool display = triangle->output == PS5VK_TRIANGLE_OUTPUT_DISPLAY;
   const VkPipeline first = triangle->pipelines[0];
   const VkPipeline second = triangle->pipelines[triangle->pipeline_count > 1 ? 1 : 0];
   const bool two_buffers = grouping == PS5VK_TRIANGLE_TWO_COMMAND_BUFFERS ||
                            grouping == PS5VK_TRIANGLE_TWO_SUBMIT_INFOS ||
                            grouping == PS5VK_TRIANGLE_TWO_SUBMISSIONS;
   /* A frame that uploads -- staged geometry (Phase C2) or a texture (Phase
    * C4) -- needs the pool's first command buffer for its copies and the second
    * for the frame itself: the two-draw groupings have no third buffer to
    * record their second draw into. */
   const bool uploads = triangle->staged || triangle->texture_image != VK_NULL_HANDLE;
   /* A secondary frame is one primary command buffer around the secondary's
    * draws, so it has neither uploads of its own nor a second command buffer. */
   if (triangle->secondary && (uploads || two_buffers || display)) {
      step(triangle, "frame_secondary", VK_ERROR_INITIALIZATION_FAILED,
           "a frame through a secondary command buffer draws into the program's image once");
      return PS5VK_TRIANGLE_FAILED;
   }
   if (uploads && two_buffers) {
      step(triangle, "frame_uploads", VK_ERROR_INITIALIZATION_FAILED,
           "a frame that uploads geometry or texels submits one command buffer");
      return PS5VK_TRIANGLE_FAILED;
   }
   triangle->target = NULL;
   triangle->target_bytes = 0;
   if (triangle->display_readback_mapped)
      memset(triangle->display_readback_mapped, 0xcd,
             (size_t)PS5VK_TRIANGLE_WIDTH * PS5VK_TRIANGLE_HEIGHT * 4u);
   /* R8/R9: the draws of this frame are counted from zero, so the first takes
    * the first per-draw values and the second the second. */
   triangle->draws_recorded = 0;
   if (display) {
      if (triangle->acquired) {
         step(triangle, "acquire_next_image", VK_ERROR_INITIALIZATION_FAILED,
              "the image drawn last is not presented yet");
         return PS5VK_TRIANGLE_FAILED;
      }
      if (!step(triangle, "acquire_next_image",
                CALL(triangle, AcquireNextImageKHR)(device, triangle->swapchain, UINT64_MAX,
                                                    triangle->image_available, VK_NULL_HANDLE,
                                                    &triangle->image_index),
                NULL))
         return PS5VK_TRIANGLE_FAILED;
      triangle->acquired = true;
   } else {
      triangle->image_index = 0;
      memset(triangle->mapped, 0, triangle->memory_bytes);
   }
   const VkFramebuffer framebuffer = triangle->framebuffers[triangle->image_index];
   /* A frame's uploads go in the first command buffer and the frame in the
    * second (Phase C2's copies, Phase C4's image copy); either way the frame
    * itself is the same record(...) call this program has always made for its
    * first buffer. */
   const VkCommandBuffer frame = triangle->commands[uploads ? 1 : 0];

   if (!step(triangle, "reset_command_pool",
             CALL(triangle, ResetCommandPool)(device, triangle->pool, 0), name) ||
       !step(triangle, "reset_fence", CALL(triangle, ResetFences)(device, 1, &triangle->fence),
             NULL) ||
       (uploads &&
        !step(triangle, "record_upload_command_buffer",
              record_uploads(triangle, triangle->commands[0]), name)) ||
       !step(triangle,
             triangle->secondary ? "record_secondary_command_buffer" : "record_command_buffer",
             triangle->secondary
                ? record_secondary(triangle, triangle->secondary_command, triangle->commands[0],
                                   triangle->first_pass, framebuffer, first)
                : record(triangle, frame, triangle->first_pass, framebuffer, first,
                         grouping == PS5VK_TRIANGLE_ONE_COMMAND_BUFFER ? second : VK_NULL_HANDLE,
                         triangle->texture_is_rendered),
             name) ||
       (two_buffers &&
        !step(triangle, "record_second_command_buffer",
              record(triangle, triangle->commands[1], triangle->load_pass, framebuffer, second,
                     VK_NULL_HANDLE, false),
              name)))
      return PS5VK_TRIANGLE_FAILED;

   VkSubmitInfo infos[2] = {
      {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
       .pCommandBuffers = &triangle->commands[0]},
      {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
       .pCommandBuffers = &triangle->commands[1]},
   };
   /* On the display, the first submission waits for the acquired image and
    * the last signals that the frame is rendered. A frame that uploads (Phase
    * C2's copies, Phase C4's image copy) reaches the queue as one submission,
    * so it is the first and the last. */
   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   const unsigned last = !uploads && (grouping == PS5VK_TRIANGLE_TWO_SUBMIT_INFOS ||
                                      grouping == PS5VK_TRIANGLE_TWO_SUBMISSIONS);
   if (display) {
      infos[0].waitSemaphoreCount = 1;
      infos[0].pWaitSemaphores = &triangle->image_available;
      infos[0].pWaitDstStageMask = &wait_stage;
      infos[last].signalSemaphoreCount = 1;
      infos[last].pSignalSemaphores = &triangle->render_finished;
   }
   VkResult result = VK_SUCCESS;
   if (uploads) {
      /* The uploads and then the frame, both of the pool's command buffers in
       * one VkSubmitInfo: one submission, which the driver splits at the
       * copies, is what Phase C2 and C4 probe. */
      infos[0].commandBufferCount = 2;
      infos[0].pCommandBuffers = triangle->commands;
      result = CALL(triangle, QueueSubmit)(triangle->queue, 1, &infos[0], triangle->fence);
   } else {
      switch (grouping) {
      case PS5VK_TRIANGLE_ONE_DRAW:
         result = CALL(triangle, QueueSubmit)(triangle->queue, 1, &infos[0], triangle->fence);
         break;
      case PS5VK_TRIANGLE_ONE_COMMAND_BUFFER:
         result = CALL(triangle, QueueSubmit)(triangle->queue, 1, &infos[0], triangle->fence);
         break;
      case PS5VK_TRIANGLE_TWO_COMMAND_BUFFERS:
         infos[0].commandBufferCount = 2;
         result = CALL(triangle, QueueSubmit)(triangle->queue, 1, &infos[0], triangle->fence);
         break;
      case PS5VK_TRIANGLE_TWO_SUBMIT_INFOS:
         result = CALL(triangle, QueueSubmit)(triangle->queue, 2, infos, triangle->fence);
         break;
      case PS5VK_TRIANGLE_TWO_SUBMISSIONS:
         result = CALL(triangle, QueueSubmit)(triangle->queue, 1, &infos[0], VK_NULL_HANDLE);
         if (result == VK_SUCCESS)
            result = CALL(triangle, QueueSubmit)(triangle->queue, 1, &infos[1], triangle->fence);
         break;
      }
   }
   if (result == VK_SUCCESS)
      result = CALL(triangle, WaitForFences)(device, 1, &triangle->fence, VK_TRUE,
                                             PS5VK_TRIANGLE_FENCE_TIMEOUT_NS);
   if (!step(triangle, "submit_and_wait", result, name))
      return failure(result);
   if (!display) {
      triangle->target = triangle->mapped;
      triangle->target_bytes = triangle->memory_bytes;
      triangle->depth_target = triangle->depth_mapped;
      triangle->depth_target_bytes = triangle->depth_bytes;
      if (triangle->staged) {
         /* The copies are the only thing that wrote the buffers the draw
          * binds, so a mapping taken now, after the fence, holds what reached
          * them, which a test compares with the geometry it passed (Phase C2).
          * Each memory is mapped once -- vkMapMemory takes unmapped memory --
          * and ps5vk_triangle_finish unmaps it. */
         VkResult map = VK_SUCCESS;
         if (triangle->vertex_copied == NULL) {
            void *vertex = NULL;
            map = CALL(triangle, MapMemory)(device, triangle->vertex_memory, 0, VK_WHOLE_SIZE, 0,
                                            &vertex);
            triangle->vertex_copied = vertex;
         }
         if (map == VK_SUCCESS && triangle->index_copied == NULL) {
            void *index = NULL;
            map = CALL(triangle, MapMemory)(device, triangle->index_memory, 0, VK_WHOLE_SIZE, 0,
                                            &index);
            triangle->index_copied = index;
         }
         if (!step(triangle, "map_staged_geometry", map, name))
            return failure(map);
      }
   }
   return PS5VK_TRIANGLE_OK;
}

enum ps5vk_triangle_status
ps5vk_triangle_present(struct ps5vk_triangle *triangle)
{
   if (triangle->output != PS5VK_TRIANGLE_OUTPUT_DISPLAY || !triangle->acquired) {
      step(triangle, "queue_present", VK_ERROR_INITIALIZATION_FAILED,
           "no drawn swapchain image to present");
      return PS5VK_TRIANGLE_FAILED;
   }
   const VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &triangle->render_finished,
      .swapchainCount = 1,
      .pSwapchains = &triangle->swapchain,
      .pImageIndices = &triangle->image_index,
   };
   const VkResult result = CALL(triangle, QueuePresentKHR)(triangle->queue, &present_info);
   triangle->acquired = false;
   char detail[32];
   snprintf(detail, sizeof(detail), "image %u", (unsigned)triangle->image_index);
   return step(triangle, "queue_present", result, detail) ? PS5VK_TRIANGLE_OK : failure(result);
}

void
ps5vk_triangle_finish(struct ps5vk_triangle *triangle)
{
   if (triangle->device != VK_NULL_HANDLE) {
      const VkDevice device = triangle->device;
      /* As the Vulkan Tutorial cleans up: nothing is released while the queue
       * may still use it. */
      step(triangle, "device_wait_idle", CALL(triangle, DeviceWaitIdle)(device), NULL);
      if (triangle->display_readback_mapped)
         CALL(triangle, UnmapMemory)(device, triangle->display_readback_memory);
      CALL(triangle, DestroyBuffer)(device, triangle->display_readback_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->display_readback_memory, NULL);
      CALL(triangle, DestroySemaphore)(device, triangle->render_finished, NULL);
      CALL(triangle, DestroySemaphore)(device, triangle->image_available, NULL);
      CALL(triangle, DestroyFence)(device, triangle->fence, NULL);
      CALL(triangle, DestroyCommandPool)(device, triangle->pool, NULL);
      for (unsigned index = 0; index < PS5VK_TRIANGLE_MAX_PIPELINES; index++) {
         CALL(triangle, DestroyPipeline)(device, triangle->pipelines[index], NULL);
         CALL(triangle, DestroyShaderModule)(device, triangle->pixel[index], NULL);
         CALL(triangle, DestroyShaderModule)(device, triangle->vertex[index], NULL);
      }
      CALL(triangle, DestroyPipelineLayout)(device, triangle->layout, NULL);
      CALL(triangle, DestroyFramebuffer)(device, triangle->detached_framebuffer, NULL);
      CALL(triangle, DestroyRenderPass)(device, triangle->detached_pass, NULL);
      /* The caller's uniform buffer (Phase C3): every handle is zero when it
       * supplied no uniform data, and the driver's destroys and frees ignore
       * zero. The set goes with its pool, and the pool and the set layout are
       * destroyed only now that the pipeline layout that named them is gone.
       * A mapped pointer is the one handle the driver did not give it, as in
       * the C2 geometry above: no mapping, no unmapping. */
      CALL(triangle, DestroyDescriptorPool)(device, triangle->descriptor_pool, NULL);
      CALL(triangle, DestroyDescriptorSetLayout)(device, triangle->set_layout, NULL);
      if (triangle->second_set_layout != VK_NULL_HANDLE)
         CALL(triangle, DestroyDescriptorSetLayout)(device, triangle->second_set_layout, NULL);
      if (triangle->uniform_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->uniform_memory);
      CALL(triangle, DestroyBuffer)(device, triangle->uniform_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->uniform_memory, NULL);
      /* The caller's texture (Phase C4): every handle is zero when it passed no
       * texture, and the driver's destroys and frees ignore zero. Its set goes
       * with its pool, and the pool and the set layout are destroyed only now
       * that the pipeline layout that named them is gone. The image memory was
       * mapped by create_texture, as the staging buffer below was, so both are
       * unmapped here. */
      /* R7's vkQuake shape: its three images, their views, their mappings and
       * the set that names them. Zero handles when the caller asked for another
       * layout, and the driver's destroys and frees ignore zero. */
      CALL(triangle, DestroyDescriptorPool)(device, triangle->multiset_pool, NULL);
      CALL(triangle, DestroyDescriptorSetLayout)(device, triangle->multiset_set_layout, NULL);
      for (unsigned index = 0; index < PS5VK_TRIANGLE_MULTISET_TEXTURES; index++) {
         CALL(triangle, DestroyImageView)(device, triangle->multiset_views[index], NULL);
         if (triangle->multiset_mappings[index] != NULL)
            CALL(triangle, UnmapMemory)(device, triangle->multiset_memories[index]);
         CALL(triangle, DestroyImage)(device, triangle->multiset_images[index], NULL);
         CALL(triangle, FreeMemory)(device, triangle->multiset_memories[index], NULL);
      }
      CALL(triangle, DestroyDescriptorPool)(device, triangle->texture_pool, NULL);
      CALL(triangle, DestroyDescriptorSetLayout)(device, triangle->texture_set_layout, NULL);
      CALL(triangle, DestroyDescriptorSetLayout)(device, triangle->sampler_set_layout, NULL);
      for (unsigned index = 0; index < PS5VK_TRIANGLE_TEXTURE_SAMPLERS; index++)
         CALL(triangle, DestroySampler)(device, triangle->texture_samplers[index], NULL);
      if (triangle->texture_lod_sampler != VK_NULL_HANDLE)
         CALL(triangle, DestroySampler)(device, triangle->texture_lod_sampler, NULL);
      CALL(triangle, DestroyImageView)(device, triangle->texture_view, NULL);
      if (triangle->texture_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->texture_memory);
      CALL(triangle, DestroyImage)(device, triangle->texture_image, NULL);
      CALL(triangle, FreeMemory)(device, triangle->texture_memory, NULL);
      if (triangle->texture_staging_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->texture_staging_memory);
      CALL(triangle, DestroyBuffer)(device, triangle->texture_staging_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->texture_staging_memory, NULL);
      /* The caller's uniform texel buffer (V0-formats' descriptor-type rows):
       * its view, its buffer and its memory. Zero handles when the caller
       * passed none, and its set went with the pool above. */
      CALL(triangle, DestroyBufferView)(device, triangle->texel_buffer_view, NULL);
      if (triangle->texel_buffer_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->texel_buffer_memory);
      CALL(triangle, DestroyBuffer)(device, triangle->texel_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->texel_buffer_memory, NULL);
      /* The caller's storage image: its view, its image and its memory. Zero
       * handles when the caller passed none, and its set went with the pool
       * above. */
      CALL(triangle, DestroyImageView)(device, triangle->storage_image_view, NULL);
      CALL(triangle, DestroyImage)(device, triangle->storage_image, NULL);
      if (triangle->storage_image_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->storage_image_memory);
      CALL(triangle, FreeMemory)(device, triangle->storage_image_memory, NULL);
      /* Phase C4's render-to-texture case: the image the frame rendered the
       * texels into and sampled, its view, pass, framebuffer and the fill
       * quad's buffer. Every handle is zero when the caller did not ask for
       * the case, and the driver's destroys and frees ignore zero. Its set went
       * with the texture's pool above. */
      CALL(triangle, DestroyFramebuffer)(device, triangle->rendered_framebuffer, NULL);
      /* R10's subpass-input probe: the second colour attachment the frame's
       * two-subpass pass declares, with its view and memory. All three are zero
       * for a frame that did not ask for the probe. */
      CALL(triangle, DestroyImageView)(device, triangle->subpass_view, NULL);
      CALL(triangle, DestroyImage)(device, triangle->subpass_image, NULL);
      if (triangle->subpass_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->subpass_memory);
      CALL(triangle, FreeMemory)(device, triangle->subpass_memory, NULL);
      CALL(triangle, DestroyRenderPass)(device, triangle->rendered_pass, NULL);
      CALL(triangle, DestroyImageView)(device, triangle->rendered_view, NULL);
      CALL(triangle, DestroyImage)(device, triangle->rendered_image, NULL);
      if (triangle->rendered != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->rendered_memory);
      CALL(triangle, FreeMemory)(device, triangle->rendered_memory, NULL);
      /* Phase C7's copied texture: the tiled image, its view and its memory.
       * Zero handles when the caller did not ask for the case. */
      CALL(triangle, DestroyImageView)(device, triangle->copied_view, NULL);
      CALL(triangle, DestroyImage)(device, triangle->copied_image, NULL);
      if (triangle->copied != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->copied_memory);
      CALL(triangle, FreeMemory)(device, triangle->copied_memory, NULL);
      if (triangle->fill_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->fill_memory);
      CALL(triangle, DestroyImageView)(device, triangle->depth_view, NULL);
      CALL(triangle, DestroyImage)(device, triangle->depth_image, NULL);
      if (triangle->depth_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->depth_memory);
      CALL(triangle, FreeMemory)(device, triangle->depth_memory, NULL);
      CALL(triangle, DestroyBuffer)(device, triangle->fill_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->fill_memory, NULL);
      for (unsigned index = 0; index < PS5VK_TRIANGLE_MAX_IMAGES; index++) {
         CALL(triangle, DestroyFramebuffer)(device, triangle->framebuffers[index], NULL);
         CALL(triangle, DestroyImageView)(device, triangle->views[index], NULL);
      }
      /* Phase C8's resolve target: its view, its image and its memory. Zero
       * handles when the caller did not ask for a resolve. */
      CALL(triangle, DestroyImageView)(device, triangle->resolve_view, NULL);
      CALL(triangle, DestroyImage)(device, triangle->resolve_image, NULL);
      CALL(triangle, FreeMemory)(device, triangle->resolve_memory, NULL);
      CALL(triangle, DestroyRenderPass)(device, triangle->load_pass, NULL);
      CALL(triangle, DestroyRenderPass)(device, triangle->first_pass, NULL);
      /* The caller's geometry (Phase C2): zero handles when it drew the sets'
       * unbound triangles, which the driver's destroys and frees ignore. A
       * mapped pointer is the one handle the driver did not give it: no
       * mapping, no unmapping. */
      if (triangle->vertex_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->vertex_memory);
      CALL(triangle, DestroyBuffer)(device, triangle->vertex_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->vertex_memory, NULL);
      if (triangle->index_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->index_memory);
      CALL(triangle, DestroyBuffer)(device, triangle->index_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->index_memory, NULL);
      /* Phase V0-query's copied result: zero handles when no frame asked for
       * the copy. */
      if (triangle->query_copy_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->query_copy_memory);
      CALL(triangle, DestroyBuffer)(device, triangle->query_copy_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->query_copy_memory, NULL);
      /* A staged upload (Phase C2): the two buffers the copies filled were
       * mapped after the submission for a test to read, each at most once, so
       * each is unmapped at most once here; the staging buffer itself was
       * mapped by create_geometry. Handles are zero when the caller did not
       * stage, and the driver's unmaps, destroys and frees ignore zero. */
      if (triangle->vertex_copied != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->vertex_memory);
      if (triangle->index_copied != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->index_memory);
      if (triangle->staging_mapped != NULL)
         CALL(triangle, UnmapMemory)(device, triangle->staging_memory);
      CALL(triangle, DestroyBuffer)(device, triangle->staging_buffer, NULL);
      CALL(triangle, FreeMemory)(device, triangle->staging_memory, NULL);
      /* Swapchain images belong to the swapchain. */
      if (triangle->output == PS5VK_TRIANGLE_OUTPUT_DISPLAY) {
         if (triangle->swapchain != VK_NULL_HANDLE)
            CALL(triangle, DestroySwapchainKHR)(device, triangle->swapchain, NULL);
      } else {
         CALL(triangle, DestroyImage)(device, triangle->images[0], NULL);
         CALL(triangle, FreeMemory)(device, triangle->memory, NULL);
      }
      CALL(triangle, DestroyDevice)(device, NULL);
   }
   if (triangle->instance != VK_NULL_HANDLE) {
      if (triangle->surface != VK_NULL_HANDLE)
         CALL(triangle, DestroySurfaceKHR)(triangle->instance, triangle->surface, NULL);
      if (triangle->messenger != VK_NULL_HANDLE)
         CALL(triangle, DestroyDebugUtilsMessengerEXT)(triangle->instance, triangle->messenger,
                                                       NULL);
      CALL(triangle, DestroyInstance)(triangle->instance, NULL);
   }
   const ps5vk_get_instance_proc_addr get_instance_proc_addr = triangle->get_instance_proc_addr;
   const struct ps5vk_triangle_report *const report = triangle->report;
   memset(triangle, 0, sizeof(*triangle));
   triangle->get_instance_proc_addr = get_instance_proc_addr;
   triangle->report = report;
}
