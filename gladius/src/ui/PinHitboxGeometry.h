#pragma once

#include <imgui.h>

namespace gladius::ui
{
    struct PinHitboxGeometry
    {
        ImVec2 markerCenter;
        ImVec2 hitRectMin;
        ImVec2 hitRectMax;
        ImVec2 pivotRectMin;
        ImVec2 pivotRectMax;
        ImVec2 menuButtonPosition;
        float hitTargetSize{};
        float markerRadius{};
    };

    inline constexpr float PIN_MINIMUM_HIT_TARGET_SIZE = 24.f;
    inline constexpr float PIN_MARKER_RADIUS = 4.f;
    inline constexpr float PIN_MENU_BUTTON_GAP = 4.f;
    inline constexpr float PIN_MINIMUM_ROW_GAP = 2.f;

    inline float pinHitTargetSize(float uiScale)
    {
        float const scale = uiScale > 0.f ? uiScale : 1.f;
        return PIN_MINIMUM_HIT_TARGET_SIZE * scale;
    }

    inline float pinMinimumRowHeight(float uiScale)
    {
        return pinHitTargetSize(uiScale) + PIN_MINIMUM_ROW_GAP;
    }

    inline float pinRailWidth(float menuButtonSize, float cellPaddingX, float uiScale)
    {
        float const scale = uiScale > 0.f ? uiScale : 1.f;
        return pinHitTargetSize(scale) + menuButtonSize + PIN_MENU_BUTTON_GAP * scale +
               2.f * cellPaddingX;
    }

    inline PinHitboxGeometry makePinHitboxGeometry(ImVec2 cellCursor,
                                                   float columnContentWidth,
                                                   float menuButtonSize,
                                                   float uiScale,
                                                   bool isInput)
    {
        float const scale = uiScale > 0.f ? uiScale : 1.f;
        float const targetSize = PIN_MINIMUM_HIT_TARGET_SIZE * scale;
        float const halfTargetSize = targetSize * 0.5f;
        float const markerRadius = PIN_MARKER_RADIUS * scale;
        float const markerX = isInput ? cellCursor.x + halfTargetSize
                                      : cellCursor.x + columnContentWidth - halfTargetSize;
        float const markerY = cellCursor.y + menuButtonSize * 0.5f;

        ImVec2 menuButtonPosition = cellCursor;
        if (isInput)
        {
            menuButtonPosition.x += targetSize + PIN_MENU_BUTTON_GAP * scale;
        }
        else
        {
            menuButtonPosition.x +=
              columnContentWidth - targetSize - PIN_MENU_BUTTON_GAP * scale - menuButtonSize;
        }

        return {{markerX, markerY},
                {markerX - halfTargetSize, markerY - halfTargetSize},
                {markerX + halfTargetSize, markerY + halfTargetSize},
                {markerX - markerRadius, markerY - markerRadius},
                {markerX + markerRadius, markerY + markerRadius},
                menuButtonPosition,
                targetSize,
                markerRadius};
    }
} // namespace gladius::ui