#include "NumericWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <fmt/format.h>

namespace gladius::ui
{
    namespace numeric_widget_detail
    {
        float computeAdaptiveStep(float value)
        {
            float constexpr EPSILON = 1e-6f;
            float const magnitude = std::abs(value) + EPSILON;
            float const order = std::floor(std::log10(magnitude));
            return std::pow(10.f, order) * 0.01f;
        }

        float applyModifierStep(float baseStep, bool isFineAdjustment, bool isCoarseAdjustment)
        {
            if (isFineAdjustment)
            {
                return baseStep * 0.01f; // Fine control
            }
            if (isCoarseAdjustment)
            {
                return baseStep * 100.f; // Coarse control
            }
            return baseStep;
        }

        float clampToBounds(float value, std::optional<float> minValue, std::optional<float> maxValue)
        {
            if (minValue.has_value() && maxValue.has_value())
            {
                return std::clamp(value, *minValue, *maxValue);
            }

            if (minValue.has_value())
            {
                return std::max(value, *minValue);
            }

            if (maxValue.has_value())
            {
                return std::min(value, *maxValue);
            }

            return value;
        }

        int computeDisplayPrecision(float value)
        {
            int constexpr SIGNIFICANT_DIGITS = 6;
            if (value == 0.f)
            {
                return 3;
            }
            int const order = static_cast<int>(std::floor(std::log10(std::abs(value))));
            int const decimalPlaces = SIGNIFICANT_DIGITS - order - 1;
            return std::clamp(decimalPlaces, 1, 10);
        }

        float computeAcceleration(ImGuiID widgetId)
        {
            float const currentTime = static_cast<float>(ImGui::GetTime());
            ImGuiID const storageKey = widgetId ^ ImHashStr("##accel_time");
            float * lastTime = ImGui::GetStateStorage()->GetFloatRef(storageKey, 0.f);
            float const timeDelta = currentTime - *lastTime;
            *lastTime = currentTime;

            float constexpr FAST_THRESHOLD = 0.15f;  // Rapid input window in seconds
            float constexpr MAX_ACCELERATION = 10.f;

            if (timeDelta > 0.f && timeDelta < FAST_THRESHOLD)
            {
                return std::min(MAX_ACCELERATION, FAST_THRESHOLD / timeDelta);
            }
            return 1.f;
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
    } // namespace numeric_widget_detail

    bool adaptiveDragFloat(char const * label,
                           float * value,
                           nodes::ContentType contentType)
    {
        if (value == nullptr)
        {
            return false;
        }

        bool changed = false;
        float const baseStep = numeric_widget_detail::computeAdaptiveStep(*value);
        auto const & io = ImGui::GetIO();
        float const step = numeric_widget_detail::applyModifierStep(baseStep, io.KeyShift, io.KeyCtrl);

        // Format string with appropriate significant-digit precision
        int const decimalPlaces = numeric_widget_detail::computeDisplayPrecision(*value);
        std::string const format = fmt::format("%.{}f", decimalPlaces);

        float const dragSpeed = std::max(step, 0.001f);

        changed = ImGui::DragFloat(label,
                                   value,
                                   dragSpeed,
                                   -std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max(),
                                   format.c_str());

        bool const isFocused = ImGui::IsItemFocused();
        bool const isHovered = ImGui::IsItemHovered();
        ImGuiID const itemId = ImGui::GetItemID();

        // Claim Up/Down arrow keys when focused so they don't trigger navigation
        if (isFocused)
        {
            ImGui::SetItemKeyOwner(ImGuiKey_UpArrow);
            ImGui::SetItemKeyOwner(ImGuiKey_DownArrow);
        }

        // Keyboard Up/Down arrow support with acceleration
        if (isFocused)
        {
            float constexpr REPEAT_DELAY = 0.25f;
            float constexpr REPEAT_RATE = 0.05f;

            int const keyPressCountUp =
              ImGui::GetKeyPressedAmount(ImGuiKey_UpArrow, REPEAT_DELAY, REPEAT_RATE);
            if (keyPressCountUp > 0)
            {
                float const accel = numeric_widget_detail::computeAcceleration(itemId);
                *value += step * accel * static_cast<float>(keyPressCountUp);
                changed = true;
            }

            int const keyPressCountDown =
              ImGui::GetKeyPressedAmount(ImGuiKey_DownArrow, REPEAT_DELAY, REPEAT_RATE);
            if (keyPressCountDown > 0)
            {
                float const accel = numeric_widget_detail::computeAcceleration(itemId);
                *value -= step * accel * static_cast<float>(keyPressCountDown);
                changed = true;
            }
        }

        // Scroll wheel support: only when both focused and hovered (to not interfere with zoom)
        if (isFocused && isHovered)
        {
            float const wheel = io.MouseWheel;
            if (wheel != 0.f)
            {
                float const accel = numeric_widget_detail::computeAcceleration(itemId);
                *value += step * accel * wheel;
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
            float const currentAngle = numeric_widget_detail::computeAngle(center, mousePos);

            // Use per-frame delta for smooth rotation
            ImVec2 const mouseDelta = ImGui::GetIO().MouseDelta;
            if (std::abs(mouseDelta.x) > 0.f || std::abs(mouseDelta.y) > 0.f)
            {
                ImVec2 const prevPos = {mousePos.x - mouseDelta.x, mousePos.y - mouseDelta.y};
                float const prevAngle = numeric_widget_detail::computeAngle(center, prevPos);
                float const angleDelta =
                    numeric_widget_detail::normalizeAngleDelta(currentAngle - prevAngle);

                auto const & io = ImGui::GetIO();
                float const step =
                    numeric_widget_detail::applyModifierStep(1.f, io.KeyShift, io.KeyCtrl);

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
                    float const baseStep = numeric_widget_detail::computeAdaptiveStep(*value);
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
            if (params.minValue.has_value() && params.maxValue.has_value())
            {
                changed = ImGui::SliderFloat(label, params.value, *params.minValue, *params.maxValue);
            }
            else
            {
                changed = adaptiveDragFloat(fmt::format("##{}", label).c_str(),
                                            params.value,
                                            params.contentType);
            }
        }

        ImGui::PopID();

        // Clamp if bounded
        if (params.minValue.has_value() && params.maxValue.has_value())
        {
            *params.value = numeric_widget_detail::clampToBounds(*params.value,
                                                                 params.minValue,
                                                                 params.maxValue);
        }

        return changed;
    }

} // namespace gladius::ui
