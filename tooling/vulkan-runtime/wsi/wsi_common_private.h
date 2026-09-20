/*
 * PS5 Vulkan compatibility probe - window-system stub for Mesa's runtime.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B1 (docs/M5_PHASE_B.md). vk_device.c calls
 * wsi_common_get_time_domain for VK_EXT_present_timing's calibrated
 * swapchain timestamps. Without Mesa's window system there are no Mesa
 * swapchains, and the driver does not expose that extension.
 */

#ifndef PS5VK_WSI_COMMON_PRIVATE_H
#define PS5VK_WSI_COMMON_PRIVATE_H

#include "wsi_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

VkTimeDomainKHR
wsi_common_get_time_domain(VkSwapchainKHR swapchain, VkPresentStageFlagsEXT stage,
                           uint64_t time_domain_id);

#ifdef __cplusplus
}
#endif

#endif
