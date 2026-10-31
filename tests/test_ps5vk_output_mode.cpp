/*
 * PS5 Vulkan driver - Host tests for VideoOut high-refresh selection.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ps5vk_output_mode.h"

#include <gtest/gtest.h>

namespace
{
TEST(VideoOutModePolicyTest, TrustsOnlyConfiguredRetailVrr)
{
    EXPECT_EQ(ps5vk_choose_high_refresh(UINT32_C(0x40040), 1, 0), PS5VK_HIGH_REFRESH_TRUST_VRR);
    EXPECT_EQ(ps5vk_choose_high_refresh(UINT32_C(0x40000), 1, 0), PS5VK_HIGH_REFRESH_UNAVAILABLE);
    EXPECT_EQ(ps5vk_choose_high_refresh(UINT32_C(0x40040), 0, 0), PS5VK_HIGH_REFRESH_UNAVAILABLE);
    EXPECT_EQ(ps5vk_choose_high_refresh(UINT32_C(0x40040), 1, -1), PS5VK_HIGH_REFRESH_UNAVAILABLE);
}

TEST(VideoOutModePolicyTest, KeepsFixedRateValidationForNonVrrTitles)
{
    EXPECT_EQ(ps5vk_choose_high_refresh(UINT32_C(0x40), 1, 0), PS5VK_HIGH_REFRESH_VERIFY_FIXED);
    EXPECT_EQ(ps5vk_choose_high_refresh(UINT32_C(0x80040), 1, 0), PS5VK_HIGH_REFRESH_VERIFY_FIXED);
    EXPECT_EQ(ps5vk_choose_high_refresh(UINT32_C(0xc0040), 1, 0), PS5VK_HIGH_REFRESH_VERIFY_FIXED);
}

TEST(VideoOutModePolicyTest, MeasuredPeriodsDetermineWhetherToKeepMode)
{
    const auto fixed = PS5VK_HIGH_REFRESH_VERIFY_FIXED;
    const auto vrr = PS5VK_HIGH_REFRESH_TRUST_VRR;
    EXPECT_FALSE(ps5vk_keep_high_refresh(PS5VK_HIGH_REFRESH_UNAVAILABLE, UINT64_C(8340000)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(fixed, 0));
    EXPECT_FALSE(ps5vk_keep_high_refresh(vrr, 0));
    EXPECT_TRUE(ps5vk_keep_high_refresh(fixed, UINT64_C(8340000)));
    EXPECT_TRUE(ps5vk_keep_high_refresh(fixed, UINT64_C(12500000)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(fixed, UINT64_C(12500001)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(fixed, UINT64_C(16680000)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(fixed, UINT64_C(20800000)));
    EXPECT_TRUE(ps5vk_keep_high_refresh(vrr, UINT64_C(8340000)));
    EXPECT_TRUE(ps5vk_keep_high_refresh(vrr, UINT64_C(16199999)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(vrr, UINT64_C(16200000)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(vrr, UINT64_C(16680000)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(vrr, UINT64_C(17200000)));
    EXPECT_TRUE(ps5vk_keep_high_refresh(vrr, UINT64_C(17200001)));
    EXPECT_TRUE(ps5vk_keep_high_refresh(vrr, UINT64_C(20800000)));
    EXPECT_TRUE(ps5vk_keep_high_refresh(vrr, UINT64_C(25000000)));
}

TEST(VideoOutModePolicyTest, RefusalCannotKeepMeasuredHighRefresh)
{
    EXPECT_FALSE(ps5vk_keep_high_refresh(ps5vk_choose_high_refresh(UINT32_C(0x40040), 0, 0),
                                         UINT64_C(8340000)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(ps5vk_choose_high_refresh(UINT32_C(0x40040), -1, 0),
                                         UINT64_C(8340000)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(ps5vk_choose_high_refresh(UINT32_C(0x40040), 1, -1),
                                         UINT64_C(8340000)));
    EXPECT_FALSE(ps5vk_keep_high_refresh(ps5vk_choose_high_refresh(UINT32_C(0x40000), 1, 0),
                                         UINT64_C(8340000)));
}
} // namespace
