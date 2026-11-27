/**
 * @file SurfaceProfile_tests.cpp
 * @brief Unit tests for the desktop/compact surface decision and the touch scale.
 * @details The layout of the whole UI hangs off this one decision, so both entry criteria (small
 *          surface, coarse pointer) and the manual override are pinned here.
 */

#include "ui/SurfaceProfile.h"

#include <gtest/gtest.h>

namespace gladius::ui::tests
{
    namespace
    {
        [[nodiscard]] SurfaceDescription description(float width,
                                                     float height,
                                                     bool coarsePointer = false)
        {
            return SurfaceDescription{ImVec2(width, height), coarsePointer};
        }
    } // namespace

    TEST(ResolveSurfaceProfile, LargeSurfaceWithFinePointer_ReturnsDesktop)
    {
        EXPECT_EQ(resolveSurfaceProfile(description(1920.f, 1080.f), LayoutModeOverride::Auto),
                  SurfaceProfile::Desktop);
    }

    TEST(ResolveSurfaceProfile, PhoneWidth_ReturnsCompact)
    {
        EXPECT_EQ(resolveSurfaceProfile(description(390.f, 844.f), LayoutModeOverride::Auto),
                  SurfaceProfile::Compact);
    }

    TEST(ResolveSurfaceProfile, PhoneLandscapeHeight_ReturnsCompact)
    {
        // Landscape is wide but short, so the height threshold is what catches it.
        EXPECT_EQ(resolveSurfaceProfile(description(844.f, 390.f), LayoutModeOverride::Auto),
                  SurfaceProfile::Compact);
    }

    TEST(ResolveSurfaceProfile, CoarsePointerOnLargeSurface_ReturnsCompact)
    {
        EXPECT_EQ(resolveSurfaceProfile(description(1920.f, 1080.f, true),
                                        LayoutModeOverride::Auto),
                  SurfaceProfile::Compact);
    }

    TEST(ResolveSurfaceProfile, SmallSurfaceWithDesktopOverride_ReturnsDesktop)
    {
        EXPECT_EQ(resolveSurfaceProfile(description(390.f, 844.f, true),
                                        LayoutModeOverride::Desktop),
                  SurfaceProfile::Desktop);
    }

    TEST(ResolveSurfaceProfile, LargeSurfaceWithCompactOverride_ReturnsCompact)
    {
        EXPECT_EQ(resolveSurfaceProfile(description(1920.f, 1080.f), LayoutModeOverride::Compact),
                  SurfaceProfile::Compact);
    }

    TEST(TouchScaleFor, DesktopProfile_ReturnsOne)
    {
        EXPECT_FLOAT_EQ(touchScaleFor(SurfaceProfile::Desktop, 390.f), 1.0f);
    }

    TEST(TouchScaleFor, CompactNarrowSurface_ScalesUpButStaysCapped)
    {
        EXPECT_FLOAT_EQ(touchScaleFor(SurfaceProfile::Compact, 430.f), 1.0f);
        EXPECT_FLOAT_EQ(touchScaleFor(SurfaceProfile::Compact, 344.f), 1.25f);
        // Below the reference width the scale must not keep growing, or text stops fitting.
        EXPECT_FLOAT_EQ(touchScaleFor(SurfaceProfile::Compact, 100.f),
                        surfaceMetrics::maxTouchScale);
    }

    TEST(ScrollbarSizeFor, DesktopProfile_KeepsThemeSize)
    {
        EXPECT_FLOAT_EQ(scrollbarSizeFor(12.f, SurfaceProfile::Desktop, 1.25f), 12.f);
    }

    TEST(ScrollbarSizeFor, CompactSurfaceWithoutTouchScale_KeepsThemeSize)
    {
        // applyCompactStyle is skipped entirely for a touch scale of 1.0, so nothing is floored.
        EXPECT_FLOAT_EQ(scrollbarSizeFor(12.f, SurfaceProfile::Compact, 1.0f), 12.f);
    }

    TEST(ScrollbarSizeFor, CompactSurfaceWithThinScrollbar_FloorsAtTouchMinimum)
    {
        // Theme default 12 scaled by the phone touch scale still lands below the grab floor.
        float const scaled = 12.f * touchScaleFor(SurfaceProfile::Compact, 390.f);
        EXPECT_LT(scaled, surfaceMetrics::minScrollbarSizePx);
        EXPECT_FLOAT_EQ(scrollbarSizeFor(scaled, SurfaceProfile::Compact, 1.1026f),
                        surfaceMetrics::minScrollbarSizePx);
    }

    TEST(ScrollbarSizeFor, CompactSurfaceWithWideScrollbar_KeepsScaledSize)
    {
        EXPECT_FLOAT_EQ(scrollbarSizeFor(18.f, SurfaceProfile::Compact, 1.25f), 22.5f);
    }

    TEST(LayoutModeOverrideFromString, RoundTripsThroughName_ReturnsSameMode)
    {
        for (auto const mode :
             {LayoutModeOverride::Auto, LayoutModeOverride::Desktop, LayoutModeOverride::Compact})
        {
            EXPECT_EQ(layoutModeOverrideFromString(layoutModeOverrideToString(mode)), mode);
        }
    }

    TEST(LayoutModeOverrideFromString, UnknownName_ReturnsAuto)
    {
        EXPECT_EQ(layoutModeOverrideFromString("tablet"), LayoutModeOverride::Auto);
        EXPECT_EQ(layoutModeOverrideFromString(""), LayoutModeOverride::Auto);
    }
} // namespace gladius::ui::tests
