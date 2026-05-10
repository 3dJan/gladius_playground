#pragma once
#include "imgui.h"
#include "nodesfwd.h"

namespace gladius::ui
{
    enum class PinVisualState
    {
        Normal,
        Highlighted,
        Dimmed
    };

    struct LinkColors
    {
        static constexpr ImVec4 ColorFloat = {0.6f, 0.6f, 1.f, 1.f};
        static constexpr ImVec4 ColorFloat3 = {0.6f, 1.f, 0.6f, 1.f};
        static constexpr ImVec4 ColorMatrix = {1.f, 0.6f, 0.6f, 1.f};
        static constexpr ImVec4 ColorResource = {1.f, 1.f, 0.6f, 1.f};
        static constexpr ImVec4 ColorString = {1.f, 0.6f, 1.f, 1.f};
        static constexpr ImVec4 ColorInt = {0.6f, 1.f, 1.f, 1.f};
        static constexpr ImVec4 ColorInvalid = {1.f, 0.f, 0.f, 1.f};

        static constexpr ImVec4 DarkColorFloat = {0.3f, 0.3f, 0.5f, 1.f};
        static constexpr ImVec4 DarkColorFloat3 = {0.3f, 0.5f, 0.3f, 1.f};
        static constexpr ImVec4 DarkColorMatrix = {0.5f, 0.3f, 0.3f, 1.f};
        static constexpr ImVec4 DarkColorResource = {0.5f, 0.5f, 0.3f, 1.f};
        static constexpr ImVec4 DarkColorString = {0.5f, 0.3f, 0.5f, 1.f};
        static constexpr ImVec4 DarkColorInt = {0.3f, 0.5f, 0.5f, 1.f};
        static constexpr ImVec4 DarkColorInvalid = {0.5f, 0.f, 0.f, 1.f};

        /// Alpha value for dimming incompatible ports during link drag
        static constexpr float DimmedAlpha = 0.3f;

        /// Brightness multiplier for highlighting compatible ports during link drag
        static constexpr float HighlightBrightness = 1.4f;

        /// Apply dimming to a color (reduce alpha)
        static constexpr ImVec4 dimmed(ImVec4 color)
        {
            return {color.x, color.y, color.z, color.w * DimmedAlpha};
        }

        /// Apply highlighting to a color (increase brightness, clamp to 1.0)
        static constexpr ImVec4 highlighted(ImVec4 color)
        {
            float const r = color.x * HighlightBrightness;
            float const g = color.y * HighlightBrightness;
            float const b = color.z * HighlightBrightness;
            return {r < 1.f ? r : 1.f, g < 1.f ? g : 1.f, b < 1.f ? b : 1.f, color.w};
        }

        /// Apply a shared visual-state transform to a base pin color.
        static constexpr ImVec4 applyPinVisualState(ImVec4 color, PinVisualState visualState)
        {
            switch (visualState)
            {
            case PinVisualState::Highlighted:
                return highlighted(color);
            case PinVisualState::Dimmed:
                return dimmed(color);
            case PinVisualState::Normal:
            default:
                return color;
            }
        }
    };
} // namespace gladius::ui
