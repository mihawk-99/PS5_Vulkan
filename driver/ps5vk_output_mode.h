/*
 * PS5 Vulkan driver - VideoOut high-refresh selection policy.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PS5VK_OUTPUT_MODE_H
#define PS5VK_OUTPUT_MODE_H

#include <stdint.h>

#define PS5VK_ATTRIBUTE3_HIGH_FRAME_RATE UINT32_C(0x40)
#define PS5VK_ATTRIBUTE3_VRR UINT32_C(0x40000)
#define PS5VK_ATTRIBUTE3_VRR_DISABLED UINT32_C(0x80000)

enum ps5vk_high_refresh_policy {
   PS5VK_HIGH_REFRESH_UNAVAILABLE,
   PS5VK_HIGH_REFRESH_VERIFY_FIXED,
   PS5VK_HIGH_REFRESH_TRUST_VRR,
};

static inline enum ps5vk_high_refresh_policy
ps5vk_choose_high_refresh(uint32_t attribute3, int supported, int configured)
{
   if ((attribute3 & PS5VK_ATTRIBUTE3_HIGH_FRAME_RATE) == 0 || supported <= 0 || configured != 0)
      return PS5VK_HIGH_REFRESH_UNAVAILABLE;
   if ((attribute3 & PS5VK_ATTRIBUTE3_VRR) != 0 &&
       (attribute3 & PS5VK_ATTRIBUTE3_VRR_DISABLED) == 0)
      return PS5VK_HIGH_REFRESH_TRUST_VRR;
   return PS5VK_HIGH_REFRESH_VERIFY_FIXED;
}

/* Bare VRR vblanks may idle slowly; only the observed 59.94 Hz band
 * contradicts the configured high-refresh mode. Fixed-rate titles retain
 * the stricter midpoint check. A failed measurement never qualifies. */
static inline int
ps5vk_keep_high_refresh(enum ps5vk_high_refresh_policy policy, uint64_t period_ns)
{
   if (period_ns == 0)
      return 0;
   if (policy == PS5VK_HIGH_REFRESH_VERIFY_FIXED)
      return period_ns <= UINT64_C(12500000);
   if (policy == PS5VK_HIGH_REFRESH_TRUST_VRR)
      return period_ns < UINT64_C(16200000) || period_ns > UINT64_C(17200000);
   return 0;
}

#endif
