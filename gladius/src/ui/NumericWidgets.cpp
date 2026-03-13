#include "NumericWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <fmt/format.h>

namespace gladius::ui
{
    namespace
    {
        /// Compute adaptive step size based on current value magnitude.
        /// Uses logarithmic scaling: increment ∝ pow(10, floor(log10(|value| + ε)))
        float computeAdaptiveStep(float value)
        {
            float constexpr EPSILON = 1e-6f;
            float const magnitude = std::abs(value) + EPSILON;
            float const order = std::floor(std::log10(magnitude));
            return std::pow(10.f, order) * 0.01f;
        }

        /// Apply modifier key multipliers to the base step.
        float applyModifiers(float baseStep)
        {
            auto const & io = ImGui::GetIO();
            if (io.KeyShift)
            {
                return baseStep * 0.01f; // Fine control
            }
            if (io.KeyCtrl)
            {
                return baseStep * 100.f; // Coarse control
            }
            return baseStep;
        }

        /// Compute the angle from center to a screen position.
        float computeAngle(ImVec2 center, ImVec2 pos)
        {
            return std::atan2(pos.y - center.y, pos.x - center.x);
        }

        /// Normalize angle difference to [-PI, PI].
        float normalizeAngleDelta(float delta)
        {
            float constexpr PI = 3.14159265358979323846f;
            while (delta > PI)
            {
                delta -= 2.f * PI;
            }
            while (delta < -PI)
            {
                delta += 2.f * PI;
            }
            return delta;
        }
    } // namespace

    bool adaptiveDragFloat(char const * label,
                           float * value,
                           nodes::ContentType contentType)
    {
        if (value == nullptr)
        {
            return false;
        }

        bool changed = false;
        float const baseStep = computeAdaptiveStep(*value);
        float const step = applyModifiers(baseStep);

        // Format string with appropriate precision
        int const digitCount = (*value != 0.f)
                                 ? static_cast<int>(std::log10(std::abs(*value))) + 3
                                 : 3;
        int const clampedDigits = std::clamp(digitCount, 1, 8);
        std::string const format = fmt::format("%.{}f", clampedDigits);

        float const dragSpeed = std::max(step, 0.001f);

        // Check for double-click to enter text input mode
        // ImGui::DragFloat already supports double-click-to-type natively via InputFloat fallback

        changed = ImGui::DragFloat(label,
                                   value,
                                   dragSpeed,
                                   -std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max(),
                                   format.c_str());

        // Keyboard Up/Down arrow support
        if (ImGui::IsItemFocused())
        {
            float const deltaTime = ImGui::GetIO().DeltaTime;

            int const keyPressCountUp =
              ImGui::GetKeyPressedAmount(ImGui::GetKeyIndex(ImGuiKey_UpArrow), deltaTime, 0.1f);
            if (keyPressCountUp > 0)
            {
                *value += step * static_cast<float>(keyPressCountUp);
                changed = true;
            }

            int const keyPressCountDown =
              ImGui::GetKeyPressedAmount(ImGui::GetKeyIndex(ImGuiKey_DownArrow), deltaTime, 0.1f);
            if (keyPressCountDown > 0)
            {
                *value -= step * static_cast<float>(keyPressCountDown);
                changed = true;
            }
        }

        return changed;
    }

    bool orbitalDial(char const * label,
                     float * value,
                     float radius,
                     std::optional<float> minValue,
                     std::optional<float> maxValue)
    {
        if (value == nullptr)
        {
            return false;
        }

        bool changed = false;
        float constexpr PI = 3.14159265358979323846f;
        float constexpr TWO_PI = 2.f * PI;

        ImGui::PushID(label);

        ImVec2 const cursorPos = ImGui::GetCursorScreenPos();
        ImVec2 const center = {cursorPos.x + radius, cursorPos.y + radius};
        float const diameter = radius * 2.f;

        // Invisible button for input handling
        ImGui::InvisibleButton("##dial", ImVec2(diameter, diameter));
        bool const isActive = ImGui::IsItemActive();
        bool const isHovered = ImGui::IsItemHovered();

        // Handle drag interaction
        if (isActive)
        {
            ImVec2 const mousePos = ImGui::GetIO().MousePos;
            float const currentAngle = computeAngle(center, mousePos);

            // Use per-frame delta for smooth rotation
            ImVec2 const mouseDelta = ImGui::GetIO().MouseDelta;
            if (std::abs(mouseDelta.x) > 0.f || std::abs(mouseDelta.y) > 0.f)
            {
                ImVec2 const prevPos = {mousePos.x - mouseDelta.x, mousePos.y - mouseDelta.y};
                float const prevAngle = computeAngle(center, prevPos);
                float const angleDelta = normalizeAngleDelta(currentAngle - prevAngle);

                float const step = applyModifiers(1.f);

                if (minValue.has_value() && maxValue.has_value())
                {
                    // Bounded: map angle delta to value range
                    float const range = *maxValue - *minValue;
                    float const valueDelta = (angleDelta / TWO_PI) * range * step;
                    *value = std::clamp(*value + valueDelta, *minValue, *maxValue);
                }
                else
                {
                    // Unbounded: accumulate angle, scale to reasonable value change
                    float const baseStep = computeAdaptiveStep(*value);
                    float const valueDelta = angleDelta * baseStep * 10.f * step;
                    *value += valueDelta;
                }
                changed = true;
            }
        }

        // Drawing
        auto * drawList = ImGui::GetWindowDrawList();
        ImU32 const bgColor = ImGui::GetColorU32(
          isActive ? ImGuiCol_FrameBgActive
                   : (isHovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg));
        ImU32 const ringColor = ImGui::GetColorU32(ImGuiCol_SliderGrab);
        ImU32 const indicatorColor = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);

        // Background circle
        drawList->AddCircleFilled(center, radius, bgColor);
        drawList->AddCircle(center, radius, ringColor, 0, 2.f);

        // Value indicator arc
        float indicatorAngle;
        if (minValue.has_value() && maxValue.has_value())
        {
            float const range = *maxValue - *minValue;
            float const normalized = (range > 0.f) ? (*value - *minValue) / range : 0.f;
            indicatorAngle = -PI * 0.5f + normalized * TWO_PI * 0.75f; // 270° sweep
            float const startAngle = -PI * 0.5f;
            drawList->PathArcTo(center, radius * 0.7f, startAngle, indicatorAngle, 32);
            drawList->PathStroke(indicatorColor, 0, 3.f);
        }
        else
        {
            // Unbounded: show a rotating indicator line
            float const normalizedAngle = std::fmod(*value * 0.1f, TWO_PI);
            indicatorAngle = -PI * 0.5f + normalizedAngle;
        }

        // Indicator dot
        float const dotX = center.x + std::cos(indicatorAngle) * radius * 0.7f;
        float const dotY = center.y + std::sin(indicatorAngle) * radius * 0.7f;
        drawList->AddCircleFilled(ImVec2(dotX, dotY), 3.f, indicatorColor);

        ImGui::PopID();

        return changed;
    }

    bool numericWidget(char const * label, NumericWidgetParams & params)
    {
        if (params.value == nullptr)
        {
            return false;
        }

        bool changed = false;

        ImGui::PushID(label);

        if (params.layoutMode == WidgetLayoutMode::DialPlusDragFloat)
        {
            float const dialRadius = 16.f;

            // Dial on the left
            changed |= orbitalDial("##dial",
                                   params.value,
                                   dialRadius,
                                   params.minValue,
                                   params.maxValue);
            ImGui::SameLine();

            // Drag-float on the right
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            changed |= adaptiveDragFloat(fmt::format("##{}", label).c_str(),
                                         params.value,
                                         params.contentType);
        }
        else // Slider mode
        {
            float const minVal = params.minValue.value_or(0.f);
            float const maxVal = params.maxValue.value_or(1.f);
            changed = ImGui::SliderFloat(label, params.value, minVal, maxVal);
        }

        ImGui::PopID();

        // Clamp if bounded
        if (params.minValue.has_value() && params.maxValue.has_value())
        {
            *params.value = std::clamp(*params.value, *params.minValue, *params.maxValue);
        }

        return changed;
    }

} // namespace gladius::ui
