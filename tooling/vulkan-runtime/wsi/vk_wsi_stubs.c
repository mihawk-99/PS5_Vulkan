/*
 * PS5 Vulkan compatibility probe - window-system stubs for Mesa's runtime.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B1 (docs/M5_PHASE_B.md). See wsi_common_private.h.
 */

#include "wsi_common_private.h"

#include <assert.h>

/* Only reachable through VK_EXT_present_timing with a Mesa swapchain, and
 * this driver has neither. */
VkTimeDomainKHR
wsi_common_get_time_domain(VkSwapchainKHR swapchain, VkPresentStageFlagsEXT stage,
                           uint64_t time_domain_id)
{
   (void)swapchain;
   (void)stage;
   (void)time_domain_id;
   assert(!"no Mesa window system on the PS5");
   return VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
}
