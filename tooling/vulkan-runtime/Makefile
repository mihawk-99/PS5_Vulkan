# PS5 Vulkan compatibility probe - Mesa's common Vulkan runtime.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase B1 (docs/M5_PHASE_B.md). Compiles Mesa's Vulkan util
# library and "lite" runtime (src/vulkan/util and src/vulkan/runtime, MIT),
# their sources generated from vk.xml and the window-system stubs into one
# archive, following src/vulkan/{util,runtime}/meson.build. Run from the
# opengnm-psbc tree, whose relative include paths CFLAGS carries, by
# tools/build-vulkan-runtime.sh once per target:
#
#   SRC      prepared copy holding vulkan/{runtime,util,wsi} with the
#            generated files beside the sources that include them, and
#            ../include/drm-uapi from the Mesa release
#   OUT      object and archive directory
#   ARCHIVE  archive file name
#   CC, AR   target compiler and archiver
#   CFLAGS   the compiler tree's flags for the target
#   MESA_VERSION  the Mesa release, for PACKAGE_VERSION

SRC ?= $(error SRC is not set)
OUT ?= $(error OUT is not set)
ARCHIVE ?= $(error ARCHIVE is not set)
MESA_VERSION ?= $(error MESA_VERSION is not set)

# src/vulkan/util/meson.build: files_vulkan_util and its generated sources.
UTIL_SRCS := \
	vk_alloc.c \
	vk_dispatch_table.c \
	vk_enum_to_str.c \
	vk_extensions.c \
	vk_format.c \
	vk_util.c

# src/vulkan/runtime/meson.build: vulkan_lite_runtime_files without the
# Android and DRM sync-object files (neither platform applies), plus its
# generated sources. vk_instance.c builds separately.
#
# vk_meta.c, vk_meta_clear.c and vk_meta_draw_rects.c belong to the full
# runtime rather than the lite one, because they build shaders with NIR. The
# driver links the same NIR compiler beside this archive (libpsbc), so they
# build here too; Phase C1b draws colour clears with them, and
# vk_meta_blit_resolve.c draws the colour blits and copies that PPSSPP's
# framebuffers need on the GPU (the CPU path cost 370 ms a frame at 10x).
RUNTIME_SRCS := \
	rmv/vk_rmv_common.c \
	rmv/vk_rmv_exporter.c \
	vk_blend.c \
	vk_buffer.c \
	vk_buffer_view.c \
	vk_cmd_copy.c \
	vk_cmd_enqueue.c \
	vk_command_buffer.c \
	vk_command_pool.c \
	vk_debug_report.c \
	vk_debug_utils.c \
	vk_deferred_operation.c \
	vk_descriptor_set_layout.c \
	vk_descriptors.c \
	vk_descriptor_update_template.c \
	vk_device.c \
	vk_device_generated_commands.c \
	vk_device_memory.c \
	vk_fence.c \
	vk_framebuffer.c \
	vk_graphics_state.c \
	vk_image.c \
	vk_log.c \
	vk_meta.c \
	vk_meta_blit_resolve.c \
	vk_meta_clear.c \
	vk_meta_draw_rects.c \
	vk_meta_object_list.c \
	vk_object.c \
	vk_physical_device.c \
	vk_pipeline_layout.c \
	vk_query_pool.c \
	vk_queue.c \
	vk_render_pass.c \
	vk_sampler.c \
	vk_semaphore.c \
	vk_standard_sample_locations.c \
	vk_sync.c \
	vk_sync_binary.c \
	vk_sync_dummy.c \
	vk_sync_timeline.c \
	vk_synchronization.c \
	vk_video.c \
	vk_ycbcr_conversion.c \
	vk_cmd_enqueue_entrypoints.c \
	vk_cmd_queue.c \
	vk_common_entrypoints.c \
	vk_dispatch_trampolines.c \
	vk_format_info.c \
	vk_physical_device_features.c \
	vk_physical_device_properties.c \
	vk_physical_device_spirv_caps.c \
	vk_synchronization_helpers.c

WSI_SRCS := vk_wsi_stubs.c

OBJS := \
	$(UTIL_SRCS:%.c=$(OUT)/util/%.o) \
	$(RUNTIME_SRCS:%.c=$(OUT)/runtime/%.o) \
	$(OUT)/runtime/vk_instance.o \
	$(WSI_SRCS:%.c=$(OUT)/wsi/%.o)

# The prepared copy comes first, so generated headers win over the tree's
# placeholders and "vulkan/wsi/..." finds the stubs. MESA_VK_LOG=0 matches
# meson's release builds of the lite runtime. PACKAGE_VERSION is the one
# global meson define these sources use: vk_util.c derives the driver
# version from it. $(SRC)/../include holds only Mesa's drm-uapi headers:
# vk_image.c takes DRM format-modifier constants from drm_fourcc.h on Linux
# and BSD, which includes the PS5. Mesa's whole include directory would also
# bring its own Vulkan headers over the Vulkan-Headers copy the compiler
# archive is built with.
INCLUDES := -I$(SRC)/vulkan/runtime -I$(SRC)/vulkan/util -I$(SRC) -I$(SRC)/../include
DEFINES := -DMESA_VK_LOG=0 '-DPACKAGE_VERSION="$(MESA_VERSION)"'

.PHONY: all
all: $(OUT)/$(ARCHIVE)

$(OUT)/util/%.o: $(SRC)/vulkan/util/%.c
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) $(DEFINES) -c $< -o $@

$(OUT)/runtime/vk_instance.o: $(SRC)/vulkan/runtime/vk_instance.c
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) $(DEFINES) -DVK_LITE_RUNTIME_INSTANCE=1 -c $< -o $@

$(OUT)/runtime/%.o: $(SRC)/vulkan/runtime/%.c
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) $(DEFINES) -c $< -o $@

$(OUT)/wsi/%.o: $(SRC)/vulkan/wsi/%.c
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) $(DEFINES) -c $< -o $@

$(OUT)/$(ARCHIVE): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)
