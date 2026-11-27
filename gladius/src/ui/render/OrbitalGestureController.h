#pragma once

#include <cstdint>

namespace gladius::ui::async_rendering
{
    /**
     * @brief Camera gesture recognised from multi-touch input.
     * @details A phone has no right or middle mouse button, so orbiting, panning and dollying have
     *          to come from fingers instead. One finger is handled by the existing pointer path;
     *          this controller only reports what needs two fingers.
     */
    enum class TouchCameraGestureKind
    {
        None,
        Pan,
        Dolly
    };

    /// @brief Camera gesture accumulated between two reads
    struct TouchCameraGesture
    {
        TouchCameraGestureKind kind{TouchCameraGestureKind::None};
        float deltaX{0.f};
        float deltaY{0.f};
        /// @brief Relative pinch distance change, e.g. 0.1 for a 10% wider pinch
        float pinchDelta{0.f};
    };

    /**
     * @brief Turns raw touch points into pan and dolly gestures.
     * @details Stateful: touches are added, moved and removed as the browser reports them, and the
     *          accumulated gesture is taken away once per frame. Two fingers that move together pan;
     *          a pinch dollies. A single finger produces nothing, which keeps the pointer-driven
     *          orbit untouched.
     */
    class OrbitalGestureController
    {
      public:
        /// @brief One tracked finger in CSS pixel coordinates
        struct TouchPoint
        {
            int32_t pointerId{-1};
            float x{0.f};
            float y{0.f};
            /// @brief Where the finger was when the gesture was read last
            float anchorX{0.f};
            float anchorY{0.f};
            bool active{false};
        };

        void beginTouch(int32_t pointerId, float x, float y);
        void moveTouch(int32_t pointerId, float x, float y);
        void endTouch(int32_t pointerId);
        void reset();

        /// @brief True while two or more fingers are down, so the pointer-driven orbit must pause
        [[nodiscard]] bool isMultiTouchActive() const noexcept;

        /// @brief Returns the gesture since the last read and moves the anchor to the fingers
        [[nodiscard]] TouchCameraGesture consumeGesture();

      private:
        static constexpr int kMaxTrackedTouches = 4;
        /// @brief Below this pinch width a distance ratio is meaningless noise
        static constexpr float kMinPinchDistancePx = 20.f;
        /// @brief Pinch ratio that has to be exceeded before a read counts as a dolly
        static constexpr float kMinPinchRatio = 0.002f;
        /// @brief Movement that has to be exceeded before a read counts as a pan
        static constexpr float kMinPanDeltaPx = 0.5f;

        [[nodiscard]] TouchPoint * findTouch(int32_t pointerId);
        [[nodiscard]] TouchPoint * freeSlot();
        void anchorAllActiveTouches();
        [[nodiscard]] int countActiveTouches() const noexcept;

        TouchPoint m_touches[kMaxTrackedTouches];
    };
} // namespace gladius::ui::async_rendering
