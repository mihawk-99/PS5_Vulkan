/*
 * PS5 Vulkan compatibility probe - window-system stub for Mesa's runtime.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B1 (docs/M5_PHASE_B.md). The opengnm-psbc tree carries
 * Mesa's Vulkan runtime without src/vulkan/wsi. The runtime includes this
 * header for the WSI image-creation structure and its structure type, which
 * Mesa keeps in vk_internal_exts.h. The PS5 has no Mesa window system:
 * presentation will be this driver's own VideoOut path (Phase C1).
 */

#ifndef PS5VK_WSI_COMMON_H
#define PS5VK_WSI_COMMON_H

#include "vk_internal_exts.h"

#endif
