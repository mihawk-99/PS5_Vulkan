# PS5 Vulkan driver - objects for one target.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase B2 (docs/M5_PHASE_B.md). Compiles the driver and its
# generated entry points into one archive. Run from the opengnm-psbc tree,
# whose relative include paths CFLAGS carries, by tools/build-driver.sh:
#
#   DRIVER     this directory
#   GENERATED  directory holding ps5vk_entrypoints.{c,h}
#   RUNTIME    installed Mesa Vulkan runtime (.deps/native/vulkan-runtime)
#   OUT        object and archive directory
#   ARCHIVE    archive file name
#   CC, AR     target compiler and archiver
#   CFLAGS     the compiler tree's flags for the target

DRIVER ?= $(error DRIVER is not set)
GENERATED ?= $(error GENERATED is not set)
RUNTIME ?= $(error RUNTIME is not set)
OUT ?= $(error OUT is not set)
ARCHIVE ?= $(error ARCHIVE is not set)

PSBC_INCLUDE ?= $(error PSBC_INCLUDE is not set)

DRIVER_SRCS := \
	ps5vk_buffer.c \
	ps5vk_cmd_buffer.c \
	ps5vk_compute.c \
	ps5vk_descriptor_set.c \
	ps5vk_descriptor_set_layout.c \
	ps5vk_device.c \
	ps5vk_direct_memory.c \
	ps5vk_draw.c \
	ps5vk_image.c \
	ps5vk_instance.c \
	ps5vk_memory.c \
	ps5vk_nir.c \
	ps5vk_physical_device.c \
	ps5vk_pipeline.c \
	ps5vk_pipeline_cache.c \
	ps5vk_queue.c \
	ps5vk_refusals.c \
	ps5vk_query.c \
	ps5vk_shader_module.c \
	ps5vk_sync.c \
	ps5vk_wsi.c

OBJS := $(DRIVER_SRCS:%.c=$(OUT)/%.o) $(OUT)/ps5vk_entrypoints.o

# The driver, the generated entry points and the runtime's installed headers
# come before the tree's include paths. The driver's own sources are held
# to -Werror.
INCLUDES := -I$(DRIVER) -I$(GENERATED) -I$(RUNTIME)/include/vulkan/runtime \
	-I$(RUNTIME)/include/vulkan/util -I$(RUNTIME)/include -I$(PSBC_INCLUDE)

.PHONY: all
all: $(OUT)/$(ARCHIVE)

# psbc_compile.h declares PsbcShaderMetadata, which the driver fills by value
# through libpsbc, so its layout is part of the link ABI: a driver object built
# against a different copy of that header lets the library overwrite the
# caller's stack, which aborts at the next return. make does not track it on
# its own, and the first symptom is a stack-smashing abort inside a shader
# compile rather than a link error.
$(OUT)/%.o: $(DRIVER)/%.c $(DRIVER)/ps5vk_private.h $(GENERATED)/ps5vk_entrypoints.h \
	$(PSBC_INCLUDE)/psbc_compile.h
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) -Werror -c $< -o $@

$(OUT)/ps5vk_entrypoints.o: $(GENERATED)/ps5vk_entrypoints.c $(GENERATED)/ps5vk_entrypoints.h
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

$(OUT)/$(ARCHIVE): $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)
