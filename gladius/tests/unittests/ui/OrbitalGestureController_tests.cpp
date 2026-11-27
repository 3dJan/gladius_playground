/**
 * @file OrbitalGestureController_tests.cpp
 * @brief Unit tests for the two finger camera gesture tracker.
 * @details The tricky part of a pinch is that both fingers moving apart also drags the midpoint
 *          around, so pan and dolly have to stay separated. A single finger must stay out of this
 *          controller entirely, it is orbit input for the pointer path.
 */

#include "ui/render/OrbitalGestureController.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        void beginPair(OrbitalGestureController & controller,
                       float x0,
                       float y0,
                       float x1,
                       float y1)
        {
            controller.beginTouch(0, x0, y0);
            controller.beginTouch(1, x1, y1);
        }

        void movePair(OrbitalGestureController & controller,
                      float x0,
                      float y0,
                      float x1,
                      float y1)
        {
            controller.moveTouch(0, x0, y0);
            controller.moveTouch(1, x1, y1);
        }
    } // namespace

    TEST(ConsumeGesture, TwoFingersMovingApart_ReportsDollyWithoutPan)
    {
        // Arrange
        OrbitalGestureController controller;
        beginPair(controller, 0.f, 0.f, 100.f, 0.f);

        // Act
        movePair(controller, -25.f, 0.f, 125.f, 0.f);
        TouchCameraGesture const gesture = controller.consumeGesture();

        // Assert
        EXPECT_EQ(gesture.kind, TouchCameraGestureKind::Dolly);
        EXPECT_FLOAT_EQ(0.5f, gesture.pinchDelta);
        EXPECT_FLOAT_EQ(gesture.deltaX, 0.f);
        EXPECT_FLOAT_EQ(gesture.deltaY, 0.f);
    }

    TEST(ConsumeGesture, TwoFingersPinching_ReportsDollyWithoutPan)
    {
        // Arrange
        OrbitalGestureController controller;
        beginPair(controller, 0.f, 0.f, 100.f, 0.f);

        // Act
        movePair(controller, 20.f, 0.f, 80.f, 0.f);
        TouchCameraGesture const gesture = controller.consumeGesture();

        // Assert
        EXPECT_EQ(gesture.kind, TouchCameraGestureKind::Dolly);
        EXPECT_LT(gesture.pinchDelta, 0.f);
        EXPECT_FLOAT_EQ(gesture.deltaX, 0.f);
        EXPECT_FLOAT_EQ(gesture.deltaY, 0.f);
    }

    TEST(ConsumeGesture, TwoFingersMovingTogether_ReportsPanWithoutDolly)
    {
        // Arrange
        OrbitalGestureController controller;
        beginPair(controller, 0.f, 0.f, 100.f, 0.f);

        // Act
        movePair(controller, 10.f, 20.f, 110.f, 20.f);
        TouchCameraGesture const gesture = controller.consumeGesture();

        // Assert
        EXPECT_EQ(gesture.kind, TouchCameraGestureKind::Pan);
        EXPECT_FLOAT_EQ(gesture.pinchDelta, 0.f);
        EXPECT_FLOAT_EQ(gesture.deltaX, 10.f);
        EXPECT_FLOAT_EQ(gesture.deltaY, 20.f);
    }

    TEST(ConsumeGesture, SingleFinger_ReportsNothing)
    {
        // Arrange
        OrbitalGestureController controller;
        controller.beginTouch(0, 10.f, 10.f);
        controller.moveTouch(0, 40.f, 60.f);

        // Act
        TouchCameraGesture const gesture = controller.consumeGesture();

        // Assert
        EXPECT_EQ(gesture.kind, TouchCameraGestureKind::None);
        EXPECT_FALSE(controller.isMultiTouchActive());
    }

    TEST(ConsumeGesture, GestureConsumedTwice_SecondReadIsEmpty)
    {
        // Arrange
        OrbitalGestureController controller;
        beginPair(controller, 0.f, 0.f, 100.f, 0.f);
        movePair(controller, 10.f, 20.f, 110.f, 20.f);
        TouchCameraGesture const first = controller.consumeGesture();
        EXPECT_EQ(first.kind, TouchCameraGestureKind::Pan);

        // Act
        TouchCameraGesture const gesture = controller.consumeGesture();

        // Assert
        EXPECT_EQ(gesture.kind, TouchCameraGestureKind::None);
    }

    TEST(IsMultiTouchActive, OneOfTwoFingersLifted_StopsTrackingTheGesture)
    {
        // Arrange
        OrbitalGestureController controller;
        beginPair(controller, 0.f, 0.f, 100.f, 0.f);
        controller.endTouch(1);

        // Act / Assert
        EXPECT_FALSE(controller.isMultiTouchActive());
        movePair(controller, 10.f, 20.f, 110.f, 20.f);
        EXPECT_EQ(controller.consumeGesture().kind, TouchCameraGestureKind::None);
    }

    TEST(BeginTouch, ThirdFinger_KeepsOriginalPairTracking)
    {
        // Arrange
        OrbitalGestureController controller;
        beginPair(controller, 0.f, 0.f, 100.f, 0.f);
        controller.beginTouch(2, 50.f, 50.f);

        // Act
        movePair(controller, 0.f, 30.f, 100.f, 30.f);
        TouchCameraGesture const gesture = controller.consumeGesture();

        // Assert
        EXPECT_EQ(gesture.kind, TouchCameraGestureKind::Pan);
        EXPECT_FLOAT_EQ(gesture.deltaY, 30.f);
        EXPECT_FLOAT_EQ(gesture.pinchDelta, 0.f);
    }

    TEST(Reset, ActivePair_DropsAccumulatedGestureAndTracking)
    {
        // Arrange
        OrbitalGestureController controller;
        beginPair(controller, 0.f, 0.f, 100.f, 0.f);
        movePair(controller, 10.f, 20.f, 110.f, 20.f);

        // Act
        controller.reset();

        // Assert
        EXPECT_FALSE(controller.isMultiTouchActive());
        EXPECT_EQ(controller.consumeGesture().kind, TouchCameraGestureKind::None);
    }
} // namespace gladius::ui::async_rendering::tests
