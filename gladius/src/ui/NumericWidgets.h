#pragma once

#include "nodes/types.h"

#include <imgui.h>

#include <optional>

namespace gladius::ui
{
    namespace numeric_widget_detail
    {
        /// Compute adaptive step size based on current value magnitude.
        [[nodiscard]] float computeAdaptiveStep(float value);

        /// Apply fine/coarse sensitivity modifiers to a base step.
        [[nodiscard]] float applyModifierStep(float baseStep,
                                              bool isFineAdjustment,
                                              bool isCoarseAdjustment);

        /// Clamp a numeric value when bounds are present.
        [[nodiscard]] float clampToBounds(float value,
                                          std::optional<float> minValue,
                                          std::optional<float> maxValue);

        /// Derive a readable decimal precision for the current value magnitude.
        [[nodiscard]] int computeDisplayPrecision(float value);
    } // namespace numeric_widget_detail

    /// Layout mode for numeric parameter widgets.
    /// Persisted per parameter in the 3MF document metadata.
    enum class WidgetLayoutMode
    {
        DialPlusDragFloat, ///< Orbital dial paired with drag-float (default)
        Slider             ///< Linear slider with value label
    };

    /// Parameters for the composite numeric widget.
    struct NumericWidgetParams
    {
        float * value = nullptr;
        nodes::ContentType contentType = nodes::ContentType::Length;
        WidgetLayoutMode layoutMode = WidgetLayoutMode::DialPlusDragFloat;
        std::optional<float> minValue;
        std::optional<float> maxValue;
        float dragSensitivity = 1.0f; ///< Base sensitivity multiplier
    };

    /// Transient per-frame state for the orbital dial interaction.
    struct OrbitalDialState
    {
        bool isActive = false;
        float dragStartAngle = 0.f;
        float currentAngle = 0.f;
        ImVec2 centerPos = {0.f, 0.f};
        float radius = 0.f;
    };

    /// Renders an enhanced numeric widget (dial+drag-float or slider).
    /// @return true if the value was changed.
    bool numericWidget(char const * label, NumericWidgetParams & params);

    /// Renders an orbital dial knob using ImDrawList primitives.
    /// @return true if the value was changed by dial interaction.
    bool orbitalDial(char const * label,
                     float * value,
                     float radius,
                     std::optional<float> minValue = std::nullopt,
                     std::optional<float> maxValue = std::nullopt);

    /// Renders an enhanced drag-float with adaptive logarithmic sensitivity.
    /// Supports Shift (fine ×0.01), Ctrl (coarse ×100) modifier keys,
    /// keyboard Up/Down arrows, and double-click text entry.
    /// @return true if the value was changed.
    bool adaptiveDragFloat(char const * label,
                           float * value,
                           nodes::ContentType contentType = nodes::ContentType::Length);

} // namespace gladius::ui
