/**
 * @file ParameterThrottle_tests.cpp
 * @brief Unit tests for the ParameterThrottle debounce controller.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ParameterThrottle.h"

namespace gladius::ui::tests
{
    TEST(ParameterThrottle, OnParameterChanged_FirstCall_ReturnsTrue)
    {
        ParameterThrottle throttle;
        EXPECT_TRUE(throttle.onParameterChanged());
    }

    TEST(ParameterThrottle, OnParameterChanged_WithinDebounceInterval_ReturnsFalse)
    {
        ParameterThrottle throttle(std::chrono::milliseconds(50));
        throttle.onParameterChanged(); // first call
        EXPECT_FALSE(throttle.onParameterChanged()); // second call within interval
    }

    TEST(ParameterThrottle, ShouldRecompile_AfterDebounceExpiry_ReturnsTrue)
    {
        ParameterThrottle throttle(std::chrono::milliseconds(10));
        throttle.onParameterChanged(); // first → immediate
        throttle.onParameterChanged(); // second → pending
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_TRUE(throttle.shouldRecompile());
    }

    TEST(ParameterThrottle, ShouldRecompile_BeforeDebounceExpiry_ReturnsFalse)
    {
        ParameterThrottle throttle(std::chrono::milliseconds(500));
        throttle.onParameterChanged(); // first → immediate
        throttle.onParameterChanged(); // second → pending
        EXPECT_FALSE(throttle.shouldRecompile()); // not enough time elapsed
    }

    TEST(ParameterThrottle, Reset_ClearsPendingState)
    {
        ParameterThrottle throttle(std::chrono::milliseconds(10));
        throttle.onParameterChanged(); // first → immediate
        throttle.onParameterChanged(); // second → pending
        throttle.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_FALSE(throttle.shouldRecompile()); // reset cleared pending
    }

    TEST(ParameterThrottle, ShouldRecompile_NoChangesPending_ReturnsFalse)
    {
        ParameterThrottle throttle;
        EXPECT_FALSE(throttle.shouldRecompile());
    }
} // namespace gladius::ui::tests
