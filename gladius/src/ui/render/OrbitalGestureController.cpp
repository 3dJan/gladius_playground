#include "OrbitalGestureController.h"

#include <cmath>

namespace gladius::ui::async_rendering
{
    namespace
    {
        [[nodiscard]] float distanceBetween(float x0,
                                            float y0,
                                            float x1,
                                            float y1)
        {
            float const dx = x1 - x0;
            float const dy = y1 - y0;
            return std::sqrt(dx * dx + dy * dy);
        }
    } // namespace

    void OrbitalGestureController::beginTouch(int32_t pointerId, float x, float y)
    {
        TouchPoint * slot = freeSlot();
        if (slot == nullptr)
        {
            return;
        }

        slot->active = true;
        slot->pointerId = pointerId;
        slot->x = x;
        slot->y = y;
        // A new finger must not contribute the jump from wherever the previous finger was.
        slot->anchorX = x;
        slot->anchorY = y;
    }

    void OrbitalGestureController::moveTouch(int32_t pointerId, float x, float y)
    {
        if (TouchPoint * moved = findTouch(pointerId))
        {
            moved->x = x;
            moved->y = y;
        }
    }

    void OrbitalGestureController::endTouch(int32_t pointerId)
    {
        if (TouchPoint * touch = findTouch(pointerId))
        {
            touch->active = false;
            touch->pointerId = -1;
        }
    }

    void OrbitalGestureController::reset()
    {
        for (TouchPoint & touch : m_touches)
        {
            touch.active = false;
            touch.pointerId = -1;
        }
    }

    bool OrbitalGestureController::isMultiTouchActive() const noexcept
    {
        return countActiveTouches() > 1;
    }

    TouchCameraGesture OrbitalGestureController::consumeGesture()
    {
        TouchCameraGesture gesture;
        TouchPoint * first = nullptr;
        TouchPoint * second = nullptr;
        for (TouchPoint & touch : m_touches)
        {
            if (!touch.active)
            {
                continue;
            }
            if (first == nullptr)
            {
                first = &touch;
            }
            else
            {
                second = &touch;
                break;
            }
        }

        if (first == nullptr || second == nullptr)
        {
            return gesture;
        }

        float const previousDistance = distanceBetween(first->anchorX,
                                                       first->anchorY,
                                                       second->anchorX,
                                                       second->anchorY);

        gesture.deltaX = (first->x + second->x) * 0.5f - (first->anchorX + second->anchorX) * 0.5f;
        gesture.deltaY = (first->y + second->y) * 0.5f - (first->anchorY + second->anchorY) * 0.5f;
        if (previousDistance > kMinPinchDistancePx)
        {
            float const distance = distanceBetween(first->x, first->y, second->x, second->y);
            gesture.pinchDelta = (distance - previousDistance) / previousDistance;
        }

        anchorAllActiveTouches();

        bool const pinched = std::fabs(gesture.pinchDelta) > kMinPinchRatio;
        bool const panned = std::fabs(gesture.deltaX) > kMinPanDeltaPx ||
                            std::fabs(gesture.deltaY) > kMinPanDeltaPx;

        // A pinch wins over a pan: fingers that spread while drifting still mean zoom.
        gesture.kind = pinched ? TouchCameraGestureKind::Dolly
                               : (panned ? TouchCameraGestureKind::Pan
                                         : TouchCameraGestureKind::None);
        return gesture;
    }

    OrbitalGestureController::TouchPoint * OrbitalGestureController::findTouch(int32_t pointerId)
    {
        for (TouchPoint & touch : m_touches)
        {
            if (touch.active && touch.pointerId == pointerId)
            {
                return &touch;
            }
        }
        return nullptr;
    }

    OrbitalGestureController::TouchPoint * OrbitalGestureController::freeSlot()
    {
        for (TouchPoint & touch : m_touches)
        {
            if (!touch.active)
            {
                return &touch;
            }
        }
        return nullptr;
    }

    void OrbitalGestureController::anchorAllActiveTouches()
    {
        for (TouchPoint & touch : m_touches)
        {
            if (touch.active)
            {
                touch.anchorX = touch.x;
                touch.anchorY = touch.y;
            }
        }
    }

    int OrbitalGestureController::countActiveTouches() const noexcept
    {
        int activeTouches = 0;
        for (TouchPoint const & touch : m_touches)
        {
            activeTouches += touch.active ? 1 : 0;
        }
        return activeTouches;
    }
} // namespace gladius::ui::async_rendering
