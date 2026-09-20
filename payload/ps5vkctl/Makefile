# PS5 Vulkan compatibility probe - resident control payload build.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds build/ps5vkctl/ps5vkctl.elf, the agent the console keeps resident so a
# probe battery can launch and close the runner title without a hand on the
# console. Run tools/build-ps5vkctl.sh, which resolves PS5_PAYLOAD_SDK.

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is undefined; run tools/build-ps5vkctl.sh)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ROOT := $(abspath $(CURDIR)/../..)
OUTPUT := $(ROOT)/build/ps5vkctl
ELF := $(OUTPUT)/ps5vkctl.elf

CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror
LDLIBS := -lSceSystemService -lSceUserService

all: $(ELF)

$(ELF): main.c | $(OUTPUT)
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

$(OUTPUT):
	mkdir -p $(OUTPUT)

clean:
	rm -f $(ELF)

.PHONY: all clean
