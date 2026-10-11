#include "NumericInputOverlay.h"

#include <IconFontCppHeaders/IconsFontAwesome5.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

#include <fmt/format.h>

namespace gladius::ui
{
    namespace numeric_input_detail
    {
        std::optional<float> parseFloat(std::string_view text)
        {
            if (text.empty())
            {
                return std::nullopt;
            }

            std::string const input{text};
            char * end = nullptr;
            float const value = std::strtof(input.c_str(), &end);
            if (end == input.c_str() || end != input.c_str() + input.size() || !std::isfinite(value))
            {
                return std::nullopt;
            }
            return value;
        }

        std::string formatFloat(float value)
        {
            return fmt::format("{:.9g}", value);
        }

        std::string formatInputValue(float value)
        {
            std::string text = fmt::format("{:.6g}", value);
            if (!std::isfinite(value) || text.find_first_of("eE") != std::string::npos)
            {
                return text;
            }

            std::size_t const decimalPosition = text.find('.');
            if (decimalPosition == std::string::npos)
            {
                text += ".00";
                return text;
            }

            std::size_t const decimalPlaces = text.size() - decimalPosition - 1;
            if (decimalPlaces < 2)
            {
                text.append(2 - decimalPlaces, '0');
            }
            return text;
        }

        std::string formatStep(float step)
        {
            if (!std::isfinite(step))
            {
                return formatFloat(step);
            }
            return fmt::format("{:.3g}", step);
        }

        float initialStep(float value)
        {
            if (!std::isfinite(value) || value == 0.f)
            {
                return 0.01f;
            }

            double const magnitude = std::abs(static_cast<double>(value));
            int const orderOfMagnitude = static_cast<int>(std::floor(std::log10(magnitude)));
            int const stepExponent = std::clamp(orderOfMagnitude - 3, -45, 36);
            float const step = static_cast<float>(std::pow(10.0, stepExponent));
            return std::isfinite(step) && step > 0.f ? step : 0.01f;
        }

        double rulerOffsetForValue(float value, float step)
        {
            if (!std::isfinite(value) || !std::isfinite(step) || step <= 0.f)
            {
                return 0.0;
            }

            double const minorStep = static_cast<double>(step);
            double const majorStep = minorStep * 10.0;
            return -std::remainder(static_cast<double>(value), majorStep) / minorStep;
        }

        float scaleValueByDecade(float value, int decadeDelta)
        {
            if (!std::isfinite(value))
            {
                return 0.f;
            }

            int const boundedDelta = std::clamp(decadeDelta, -30, 30);
            double const scaled = static_cast<double>(value) *
                                  std::pow(10.0, static_cast<double>(boundedDelta));
            double const limit = static_cast<double>(std::numeric_limits<float>::max());
            return static_cast<float>(std::clamp(scaled, -limit, limit));
        }

        float scaleStepByDecade(float step, int decadeDelta)
        {
            if (!std::isfinite(step) || step <= 0.f)
            {
                step = 0.01f;
            }

            int const boundedDelta = std::clamp(decadeDelta, -30, 30);
            double const scaled = static_cast<double>(step) *
                                  std::pow(10.0, static_cast<double>(boundedDelta));
            double const MIN_STEP = static_cast<double>(std::numeric_limits<float>::denorm_min());
            double const MAX_STEP = static_cast<double>(std::numeric_limits<float>::max());
            return static_cast<float>(std::clamp(scaled, MIN_STEP, MAX_STEP));
        }

        int wheelStepCount(float wheelDelta)
        {
            if (!std::isfinite(wheelDelta) || wheelDelta == 0.f)
            {
                return 0;
            }
            return wheelDelta > 0.f ? 1 : -1;
        }

        std::string formatRulerValue(float value, float step)
        {
            if (!std::isfinite(value))
            {
                return formatFloat(value);
            }

            double const majorStep = static_cast<double>(step) * 10.0;
            int const decimals = std::isfinite(majorStep) && majorStep > 0.0
                                   ? std::clamp(
                                       static_cast<int>(std::ceil(-std::log10(majorStep) - 1e-6)),
                                       0,
                                       8)
                                   : 0;
            float const magnitude = std::abs(value);
            if (magnitude >= 1e6f || (magnitude > 0.f && magnitude < 1e-4f))
            {
                return fmt::format("{:.3g}", value);
            }
            return fmt::format("{:.{}f}", value, decimals);
        }

        float nudgeValue(float value, float step, int stepCount)
        {
            if (!std::isfinite(value))
            {
                value = 0.f;
            }
            if (!std::isfinite(step) || step <= 0.f || stepCount == 0)
            {
                return value;
            }

            double const candidate = static_cast<double>(value) +
                                     static_cast<double>(step) * stepCount;
            double const limit = static_cast<double>(std::numeric_limits<float>::max());
            return static_cast<float>(std::clamp(candidate, -limit, limit));
        }

        float constantValue(NumericConstant constant, bool angleInDegrees)
        {
            constexpr float pi = 3.14159265358979323846f;
            switch (constant)
            {
            case NumericConstant::Pi:
                return angleInDegrees ? 180.f : pi;
            case NumericConstant::Euler:
                return 2.71828182845904523536f;
            case NumericConstant::SqrtTwo:
                return 1.41421356237309504880f;
            case NumericConstant::Tau:
                return angleInDegrees ? 360.f : 2.f * pi;
            case NumericConstant::GoldenRatio:
                return 1.61803398874989484820f;
            case NumericConstant::SqrtThree:
                return 1.73205080756887729353f;
            case NumericConstant::LnTwo:
                return 0.69314718055994530942f;
            case NumericConstant::LnTen:
                return 2.30258509299404568402f;
            }
            return 0.f;
        }

        float degreesToRadians(float degrees)
        {
            constexpr float pi = 3.14159265358979323846f;
            return degrees * pi / 180.f;
        }

        float radiansToDegrees(float radians)
        {
            constexpr float pi = 3.14159265358979323846f;
            return radians * 180.f / pi;
        }
    } // namespace numeric_input_detail

    void NumericInputOverlay::open(std::string label,
                                   float value,
                                   bool angleInDegrees,
                                   ImVec2 anchorPosition,
                                   float uiScale)
    {
        m_label = std::move(label);
        m_angleInDegrees = angleInDegrees;
        m_value = std::isfinite(value) ? value : 0.f;
        m_step = numeric_input_detail::initialStep(m_value);
        m_inputText = numeric_input_detail::formatInputValue(m_value);
        m_hasValidValue = true;
        m_initialValue = m_value;
        m_valueChangedThisFrame = false;
        m_focusInputOnOpen = true;
        m_rulerTapValue.reset();
        m_rulerWasDragged = false;
        m_rulerOffsetTicks = numeric_input_detail::rulerOffsetForValue(m_value, m_step);
        m_rulerDragStartOffsetTicks = 0.0;
        m_uiScale = std::clamp(uiScale, 0.75f, 1.5f);
        m_anchorPosition = anchorPosition;
        m_isActive = true;
    }

    NumericInputOverlayUpdate NumericInputOverlay::draw(bool targetAvailable)
    {
        constexpr char const * windowName = "##GladiusNumericInput";
        NumericInputOverlayUpdate update;
        if (!m_isActive)
        {
            return update;
        }

        m_valueChangedThisFrame = false;
        auto takeChangedValue = [this]() -> std::optional<float>
        {
            if (!m_valueChangedThisFrame)
            {
                return std::nullopt;
            }
            m_valueChangedThisFrame = false;
            return m_value;
        };

        ImGuiViewport const * viewport = ImGui::GetMainViewport();
        ImVec2 const workPosition = viewport ? viewport->WorkPos : ImVec2(0.f, 0.f);
        ImVec2 const workSize = viewport ? viewport->WorkSize : ImGui::GetIO().DisplaySize;
        float const layoutScale = std::clamp(m_uiScale, 0.9f, 1.1f);
        float const margin = 12.f * layoutScale;
        float const width = std::max(1.f, std::min(920.f * layoutScale, workSize.x - 2.f * margin));
        float const maxHeight = std::max(1.f, workSize.y - 2.f * margin);
        float const estimatedHeight = std::min(380.f * layoutScale, maxHeight);
        bool const useCenteredLayout = workSize.x < 1020.f * layoutScale ||
                           workSize.y < 560.f * layoutScale;

        ImVec2 position;
        if (useCenteredLayout)
        {
            position = ImVec2(workPosition.x + (workSize.x - width) * 0.5f,
                               workPosition.y + (workSize.y - estimatedHeight) * 0.5f);
        }
        else
        {
            position = ImVec2(m_anchorPosition.x,
                              m_anchorPosition.y + 8.f * m_uiScale);
            if (position.y + estimatedHeight > workPosition.y + workSize.y - margin)
            {
                position.y = m_anchorPosition.y - estimatedHeight - 8.f * m_uiScale;
            }
        }

        float const minX = workPosition.x + margin;
        float const minY = workPosition.y + margin;
        float const maxX = std::max(minX, workPosition.x + workSize.x - width - margin);
        float const maxY = std::max(minY, workPosition.y + workSize.y - estimatedHeight - margin);
        position.x = std::clamp(position.x, minX, maxX);
        position.y = std::clamp(position.y, minY, maxY);

        ImGui::SetNextWindowPos(position, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(width, 0.f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(
                    ImVec2(width, std::min(300.f * layoutScale, maxHeight)),
          ImVec2(width, maxHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f * layoutScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                    ImVec2(20.f * layoutScale, 18.f * layoutScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                    ImVec2(10.f * layoutScale, 10.f * layoutScale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f * layoutScale);

        bool windowOpen = true;
        bool const visible = ImGui::Begin(
          windowName,
          &windowOpen,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoTitleBar);
        if (!windowOpen)
        {
            ImGui::End();
            ImGui::PopStyleVar(5);
            update.value = m_initialValue;
            update.action = NumericInputOverlayAction::Cancel;
            close();
            return update;
        }
        if (!visible)
        {
            ImGui::End();
            ImGui::PopStyleVar(5);
            return update;
        }

        ImVec2 const windowPosition = ImGui::GetWindowPos();
        ImVec2 const windowSize = ImGui::GetWindowSize();
        ImDrawList * const backgroundDrawList = ImGui::GetBackgroundDrawList();
        for (int layer = 4; layer > 0; --layer)
        {
            float const shadowOffset = static_cast<float>(layer) * 2.f * layoutScale;
            float const alpha = 0.012f * static_cast<float>(5 - layer);
            backgroundDrawList->AddRectFilled(
              ImVec2(windowPosition.x - shadowOffset, windowPosition.y + shadowOffset),
              ImVec2(windowPosition.x + windowSize.x + shadowOffset,
                     windowPosition.y + windowSize.y + shadowOffset),
              ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, alpha)),
              10.f * layoutScale);
        }

        float const touchScale = std::clamp(layoutScale, 1.f, 1.1f);
        float const buttonHeight = std::max(44.f * touchScale, ImGui::GetFrameHeight());
        float const spacing = ImGui::GetStyle().ItemSpacing.x;
        float const scaleButtonWidth = 64.f * touchScale;
        float const actionButtonWidth = 82.f * touchScale;
        bool applyRequested = false;
        bool cancelRequested = false;

        std::string const headerLabel = m_label.empty() || m_label == "value"
                                          ? "Value"
                                          : fmt::format("Value · {}", m_label);
        float const contentWidth = ImGui::GetContentRegionAvail().x;
        float const scaleColumnWidth = scaleButtonWidth * 2.f + spacing;
        float const actionsColumnWidth = actionButtonWidth * 2.f + spacing;
        if (ImGui::BeginTable("NumericInputHeader",
                              3,
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX,
                              ImVec2(contentWidth, 0.f)))
        {
            ImGui::TableSetupColumn("NumericValue", ImGuiTableColumnFlags_WidthStretch, 1.f);
            ImGui::TableSetupColumn("ScaleValue",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    scaleColumnWidth);
            ImGui::TableSetupColumn("EditActions",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    actionsColumnWidth);

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", headerLabel.c_str());
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(12.f * layoutScale, 8.f * layoutScale));
            ImGui::SetWindowFontScale(1.55f);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (m_focusInputOnOpen)
            {
                ImGui::SetKeyboardFocusHere();
                m_focusInputOnOpen = false;
            }
            if (ImGui::InputText("##numeric_input_value",
                                 &m_inputText,
                                 ImGuiInputTextFlags_CharsScientific))
            {
                refreshValueFromText();
            }
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopStyleVar();

            ImGui::TableNextColumn();
            ImGui::TextDisabled("Scale value");
            if (ImGui::Button(ICON_FA_DIVIDE "10", ImVec2(scaleButtonWidth, buttonHeight)))
            {
                setValue(numeric_input_detail::scaleValueByDecade(m_value, -1));
            }
            ImGui::SameLine(0.f, spacing);
            if (ImGui::Button(ICON_FA_TIMES "10", ImVec2(scaleButtonWidth, buttonHeight)))
            {
                setValue(numeric_input_detail::scaleValueByDecade(m_value, 1));
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("EDIT");
            cancelRequested = ImGui::Button("Cancel",
                                            ImVec2(actionButtonWidth, buttonHeight));
            ImGui::SameLine(0.f, spacing);
            ImGui::BeginDisabled(!targetAvailable || !m_hasValidValue);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.40f, 0.78f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.50f, 0.92f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.32f, 0.68f, 1.f));
            applyRequested = ImGui::Button("Apply",
                                           ImVec2(actionButtonWidth, buttonHeight));
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        if (!targetAvailable)
        {
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                               "The target value is no longer available.");
        }
        if (!m_hasValidValue)
        {
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f),
                               "Enter a valid finite number.");
        }

        ImGui::Separator();
        std::string stepText = numeric_input_detail::formatStep(m_step);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Ruler step");
        ImGui::SameLine(0.f, spacing * 1.5f);
        if (ImGui::Button(ICON_FA_MINUS, ImVec2(44.f * touchScale, buttonHeight)))
        {
            m_step = numeric_input_detail::scaleStepByDecade(m_step, -1);
            m_rulerOffsetTicks = numeric_input_detail::rulerOffsetForValue(m_value, m_step);
            stepText = numeric_input_detail::formatStep(m_step);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Make the ruler step finer");
        }
        ImGui::SameLine(0.f, spacing);
        ImGui::SetNextItemWidth(100.f * layoutScale);
        if (ImGui::BeginCombo("##numeric_ruler_step", stepText.c_str()))
        {
            float const stepExponent = std::log10(m_step);
            if (stepExponent < -8.f || stepExponent > 8.f)
            {
                if (ImGui::Selectable(stepText.c_str(), true))
                {
                    m_step = numeric_input_detail::initialStep(m_value);
                    m_rulerOffsetTicks = numeric_input_detail::rulerOffsetForValue(m_value, m_step);
                }
            }
            for (int exponent = -8; exponent <= 8; ++exponent)
            {
                float const option = std::pow(10.f, static_cast<float>(exponent));
                std::string const optionText = numeric_input_detail::formatStep(option);
                bool const selected = std::abs(stepExponent - static_cast<float>(exponent)) < 1e-4f;
                if (ImGui::Selectable(optionText.c_str(), selected))
                {
                    m_step = option;
                    m_rulerOffsetTicks = numeric_input_detail::rulerOffsetForValue(m_value, m_step);
                    stepText = numeric_input_detail::formatStep(m_step);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.f, spacing);
        if (ImGui::Button(ICON_FA_PLUS, ImVec2(44.f * touchScale, buttonHeight)))
        {
            m_step = numeric_input_detail::scaleStepByDecade(m_step, 1);
            m_rulerOffsetTicks = numeric_input_detail::rulerOffsetForValue(m_value, m_step);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Make the ruler step coarser");
        }

        ImGui::Spacing();
        float const rulerWidth = std::max(1.f, ImGui::GetContentRegionAvail().x);
        float const rulerHeight = 104.f * layoutScale;
        ImVec2 const rulerMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##numeric_ruler", ImVec2(rulerWidth, rulerHeight));

        auto clampValue = [](double value)
            {
                double const limit = static_cast<double>(std::numeric_limits<float>::max());
                return static_cast<float>(std::clamp(value, -limit, limit));
            };
            float const centerX = rulerMin.x + rulerWidth * 0.5f;
            float const tickSpacing = rulerWidth / 40.f;
            bool const rulerActivated = ImGui::IsItemActivated();
            bool const rulerActive = ImGui::IsItemActive();
            bool const rulerDeactivated = ImGui::IsItemDeactivated();
            bool const rulerHovered = ImGui::IsItemHovered();
            bool const rulerFocused = ImGui::IsItemFocused();
            bool const rulerHasWheelFocus =
                  rulerHovered || (rulerFocused && ImGui::IsWindowHovered() &&
                           !ImGui::IsAnyItemHovered());
                if (rulerHovered)
                {
                ImGui::SetTooltip(
                                    "Click to focus. Left/Right: value; Up/Down: ruler step. "
                                    "Scroll: value; Ctrl+Shift+scroll: ruler step.");
                }
            ImGuiIO const & io = ImGui::GetIO();
                        if (rulerFocused)
                        {
                                ImGui::SetItemKeyOwner(ImGuiKey_LeftArrow);
                                ImGui::SetItemKeyOwner(ImGuiKey_RightArrow);
                                ImGui::SetItemKeyOwner(ImGuiKey_UpArrow);
                                ImGui::SetItemKeyOwner(ImGuiKey_DownArrow);
                        }
                        if (rulerFocused && !rulerActive)
                        {
                                float constexpr repeatDelay = 0.25f;
                                float constexpr repeatRate = 0.05f;
                                int const upCount =
                                    ImGui::GetKeyPressedAmount(ImGuiKey_UpArrow, repeatDelay, repeatRate);
                                int const downCount =
                                    ImGui::GetKeyPressedAmount(ImGuiKey_DownArrow, repeatDelay, repeatRate);
                                int const stepChange = upCount - downCount;
                                if (stepChange != 0)
                                {
                                        m_step = numeric_input_detail::scaleStepByDecade(m_step, stepChange);
                                        m_rulerOffsetTicks =
                                            numeric_input_detail::rulerOffsetForValue(m_value, m_step);
                                }

                                int const leftCount =
                                    ImGui::GetKeyPressedAmount(ImGuiKey_LeftArrow, repeatDelay, repeatRate);
                                int const rightCount =
                                    ImGui::GetKeyPressedAmount(ImGuiKey_RightArrow, repeatDelay, repeatRate);
                                int const valueSteps = rightCount - leftCount;
                                if (valueSteps != 0 && m_hasValidValue)
                                {
                                        setValue(numeric_input_detail::nudgeValue(m_value, m_step, valueSteps));
                                }
                        }
            int const wheelSteps = numeric_input_detail::wheelStepCount(io.MouseWheel);
            if (rulerHasWheelFocus && !rulerActive && wheelSteps != 0 && m_hasValidValue)
            {
                if (io.KeyCtrl && io.KeyShift)
                {
                    m_step = numeric_input_detail::scaleStepByDecade(m_step, wheelSteps);
                    m_rulerOffsetTicks =
                      numeric_input_detail::rulerOffsetForValue(m_value, m_step);
                }
                else
                {
                    setValue(numeric_input_detail::nudgeValue(m_value, m_step, wheelSteps));
                }
            }

            auto findClickableTick = [&](float mouseX) -> std::optional<int>
            {
                double const tickOffset =
                  (static_cast<double>(mouseX) - centerX) / tickSpacing - m_rulerOffsetTicks;
                int const tick = static_cast<int>(std::lround(tickOffset));
                double const tickPositionOffset = static_cast<double>(tick) + m_rulerOffsetTicks;
                float const tickX = centerX + static_cast<float>(tickPositionOffset) * tickSpacing;
                if (std::abs(mouseX - tickX) <= tickSpacing * 0.5f)
                {
                    return tick;
                }
                return std::nullopt;
            };

            if (rulerActivated)
            {
                ImVec2 const mousePosition = ImGui::GetIO().MousePos;
                m_rulerDragStartMouseX = mousePosition.x;
                m_rulerDragStartValue = m_value;
                m_rulerDragStartStep = m_step;
                m_rulerDragStartOffsetTicks = m_rulerOffsetTicks;
                m_rulerWasDragged = false;
                m_rulerTapValue.reset();

                if (auto const tick = findClickableTick(mousePosition.x))
                {
                    double const tickOffset = static_cast<double>(*tick) + m_rulerOffsetTicks;
                    m_rulerTapValue = clampValue(static_cast<double>(m_value) +
                                                 tickOffset * m_step);
                }
            }

            if (rulerActive)
            {
                double const dragDistance =
                  static_cast<double>(ImGui::GetIO().MousePos.x) - m_rulerDragStartMouseX;
                double const deltaTicks = dragDistance / tickSpacing;
                double nextOffsetTicks = m_rulerDragStartOffsetTicks + deltaTicks;
                m_rulerWasDragged |= std::abs(dragDistance) > 2.f * touchScale;

                // Rebase by whole major decades without changing the visible scale.
                double const offsetRebase = std::trunc(nextOffsetTicks / 10.0) * 10.0;
                nextOffsetTicks -= offsetRebase;
                m_rulerDragStartOffsetTicks -= offsetRebase;

                double const valuePerPixel =
                  static_cast<double>(m_rulerDragStartStep) / tickSpacing;
                setRulerValue(clampValue(static_cast<double>(m_rulerDragStartValue) -
                                         dragDistance * valuePerPixel),
                              nextOffsetTicks);
            }

            if (rulerDeactivated)
            {
                if (!m_rulerWasDragged && m_rulerTapValue)
                {
                    setValue(*m_rulerTapValue);
                }
                m_rulerTapValue.reset();
                m_rulerWasDragged = false;
            }

            std::optional<int> hoveredTick;
            if (rulerHovered)
            {
                hoveredTick = findClickableTick(ImGui::GetIO().MousePos.x);
            }

            ImVec2 const rulerMax(rulerMin.x + rulerWidth, rulerMin.y + rulerHeight);
            ImDrawList * const drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(rulerMin,
                                    rulerMax,
                                    ImGui::GetColorU32(ImGuiCol_FrameBg),
                                    4.f * layoutScale);
                        ImU32 const rulerBorderColor = ImGui::GetColorU32(
                            rulerFocused ? ImGuiCol_NavHighlight : ImGuiCol_Border);
            drawList->AddRect(rulerMin,
                              rulerMax,
                                                            rulerBorderColor,
                              4.f * layoutScale);

            int const firstTick = static_cast<int>(std::ceil(-20.0 - m_rulerOffsetTicks));
            int const lastTick = static_cast<int>(std::floor(20.0 - m_rulerOffsetTicks));
            int const nearestMajorTick =
              static_cast<int>(std::lround(-m_rulerOffsetTicks / 10.0)) * 10;
            ImVec4 constexpr hoverColor(1.f, 0.82f, 0.24f, 1.f);
            ImU32 const hoveredColor = ImGui::GetColorU32(hoverColor);
                        ImVec4 constexpr markerTint(0.12f, 0.68f, 0.78f, 1.f);
                        ImU32 const markerColor = ImGui::GetColorU32(markerTint);
            drawList->PushClipRect(rulerMin, rulerMax, true);
            for (int tick = firstTick; tick <= lastTick; ++tick)
            {
                double const tickOffset = static_cast<double>(tick) + m_rulerOffsetTicks;
                float const x = centerX + static_cast<float>(tickOffset) * tickSpacing;
                bool const isMajor = tick % 10 == 0;
                bool const isHoveredTick = hoveredTick && tick == *hoveredTick;
                bool const isHoveredMajor = isMajor && isHoveredTick;
                float const tickTop = rulerMax.y - (isMajor ? 23.f : 13.f) * layoutScale;
                ImU32 const tickColor = isHoveredTick
                                          ? hoveredColor
                                          : ImGui::GetColorU32(isMajor ? ImGuiCol_TextDisabled
                                                                       : ImGuiCol_Border);
                drawList->AddLine(ImVec2(x, tickTop),
                                  ImVec2(x, rulerMax.y - 5.f * layoutScale),
                                  tickColor,
                                  isHoveredTick ? 3.f : (isMajor ? 1.5f : 1.f));
                if (isMajor)
                {
                    double const tickValue = static_cast<double>(m_value) +
                                             tickOffset * m_step;
                    std::string const tickLabel = numeric_input_detail::formatRulerValue(
                      clampValue(tickValue), m_step);
                    ImVec2 const textSize = ImGui::CalcTextSize(tickLabel.c_str());
                    float const textX = std::clamp(x - textSize.x * 0.5f,
                                                   rulerMin.x + 3.f,
                                                   rulerMax.x - textSize.x - 3.f);
                    ImU32 const textColor = isHoveredMajor
                                              ? hoveredColor
                                              : (tick == nearestMajorTick
                                                 ? markerColor
                                                   : ImGui::GetColorU32(ImGuiCol_Text));
                    ImVec2 const labelPosition(textX, rulerMin.y + 5.f * layoutScale);
                    if (isHoveredMajor)
                    {
                        float const boldOffset = 0.7f * layoutScale;
                        drawList->AddText(ImVec2(labelPosition.x - boldOffset, labelPosition.y),
                                          textColor,
                                          tickLabel.c_str());
                        drawList->AddText(ImVec2(labelPosition.x + boldOffset, labelPosition.y),
                                          textColor,
                                          tickLabel.c_str());
                    }
                    drawList->AddText(labelPosition, textColor, tickLabel.c_str());
                }
            }
            std::string const centerValueLabel = numeric_input_detail::formatInputValue(m_value);
            ImVec2 const centerValueSize = ImGui::CalcTextSize(centerValueLabel.c_str());
            ImVec2 const badgePadding(8.f * layoutScale, 4.f * layoutScale);
            ImVec2 const badgeMin(centerX - (centerValueSize.x + 2.f * badgePadding.x) * 0.5f,
                                  rulerMin.y + 35.f * layoutScale);
            ImVec2 const badgeMax(badgeMin.x + centerValueSize.x + 2.f * badgePadding.x,
                                  badgeMin.y + centerValueSize.y + 2.f * badgePadding.y);
            drawList->AddLine(ImVec2(centerX, badgeMax.y),
                              ImVec2(centerX, rulerMax.y - 4.f * layoutScale),
                              markerColor,
                              2.5f);
            drawList->AddTriangleFilled(ImVec2(centerX - 6.f * layoutScale,
                                               rulerMin.y + 25.f * layoutScale),
                                        ImVec2(centerX + 6.f * layoutScale,
                                               rulerMin.y + 25.f * layoutScale),
                                        ImVec2(centerX, rulerMin.y + 33.f * layoutScale),
                                        markerColor);
            drawList->AddRectFilled(badgeMin, badgeMax, markerColor, 5.f * layoutScale);
            drawList->AddText(ImVec2(badgeMin.x + badgePadding.x,
                                     badgeMin.y + badgePadding.y),
                              ImGui::GetColorU32(ImGuiCol_WindowBg),
                              centerValueLabel.c_str());
            drawList->PopClipRect();

            ImGui::Spacing();
            float const footerWidth = ImGui::GetContentRegionAvail().x;
            float const nudgeButtonWidth = 150.f * touchScale;
            float const nudgeButtonHeight = 50.f * touchScale;
            if (ImGui::BeginTable("NumericInputFooter",
                                  1,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX,
                                  ImVec2(footerWidth, 0.f)))
            {
                ImGui::TableSetupColumn("ValueNudge", ImGuiTableColumnFlags_WidthStretch, 1.f);
                ImGui::TableNextColumn();
                if (ImGui::Button(ICON_FA_MINUS " one step",
                                  ImVec2(nudgeButtonWidth, nudgeButtonHeight)))
                {
                    setValue(numeric_input_detail::nudgeValue(m_value, m_step, -1));
                }
                ImGui::SameLine(0.f, spacing);
                if (ImGui::Button(ICON_FA_PLUS " one step",
                                  ImVec2(nudgeButtonWidth, nudgeButtonHeight)))
                {
                    setValue(numeric_input_detail::nudgeValue(m_value, m_step, 1));
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            if (ImGui::CollapsingHeader(ICON_FA_KEYBOARD "  Input tools"))
            {
                if (ImGui::BeginTable("NumericInputTools",
                                      2,
                                      ImGuiTableFlags_SizingStretchProp |
                                        ImGuiTableFlags_NoPadOuterX,
                                      ImVec2(footerWidth, 0.f)))
                {
                    ImGui::TableSetupColumn("NumericKeypad",
                                            ImGuiTableColumnFlags_WidthFixed,
                                            220.f * layoutScale);
                    ImGui::TableSetupColumn("NumericConstants",
                                            ImGuiTableColumnFlags_WidthStretch,
                                            1.f);

                    float const keyWidth = 60.f * touchScale;
                    float const keyHeight = 42.f * touchScale;
                    constexpr char const * keypad[3][3] = { {"7", "8", "9"},
                                                             {"4", "5", "6"},
                                                             {"1", "2", "3"} };
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("KEYPAD");
                    for (auto const & row : keypad)
                    {
                        for (int column = 0; column < 3; ++column)
                        {
                            char const * key = row[column];
                            if (ImGui::Button(key, ImVec2(keyWidth, keyHeight)))
                            {
                                appendDigit(key[0]);
                            }
                            if (column < 2)
                            {
                                ImGui::SameLine(0.f, spacing);
                            }
                        }
                    }
                    if (ImGui::Button("0", ImVec2(keyWidth, keyHeight)))
                    {
                        appendDigit('0');
                    }
                    ImGui::SameLine(0.f, spacing);
                    if (ImGui::Button(".", ImVec2(keyWidth, keyHeight)))
                    {
                        appendDecimal();
                    }
                    ImGui::SameLine(0.f, spacing);
                    if (ImGui::Button("Del", ImVec2(keyWidth, keyHeight)))
                    {
                        eraseLastCharacter();
                    }
                    if (ImGui::Button("+/-",
                                      ImVec2(keyWidth * 3.f + spacing * 2.f, keyHeight)))
                    {
                        toggleSign();
                    }

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("CONSTANTS");
                    if (ImGui::BeginTable("NumericInputConstantGrid",
                                          3,
                                          ImGuiTableFlags_SizingStretchProp |
                                            ImGuiTableFlags_NoPadOuterX))
                    {
                        auto addConstant = [this, keyHeight](char const * label,
                                                             NumericConstant constant)
                        {
                            ImGui::TableNextColumn();
                            if (ImGui::Button(label,
                                              ImVec2(ImGui::GetContentRegionAvail().x, keyHeight)))
                            {
                                                                float const value = numeric_input_detail::constantValue(
                                                                    constant, m_angleInDegrees);
                                                                m_step = numeric_input_detail::initialStep(value);
                                                                setValue(value);
                            }
                        };
                        constexpr std::pair<char const *, NumericConstant> constants[] = {
                          {"pi", NumericConstant::Pi},
                          {"e", NumericConstant::Euler},
                          {"sqrt(2)", NumericConstant::SqrtTwo},
                          {"tau", NumericConstant::Tau},
                          {"phi", NumericConstant::GoldenRatio},
                          {"sqrt(3)", NumericConstant::SqrtThree},
                          {"ln(2)", NumericConstant::LnTwo},
                          {"ln(10)", NumericConstant::LnTen}};
                        for (auto const & [label, constant] : constants)
                        {
                            addConstant(label, constant);
                        }
                        ImGui::EndTable();
                    }
                    if (m_angleInDegrees)
                    {
                        ImGui::Spacing();
                        ImGui::TextDisabled("ANGLE IN DEGREES");
                        float const angleButtonWidth =
                          std::max(1.f,
                                   (ImGui::GetContentRegionAvail().x - spacing * 3.f) / 4.f);
                        constexpr std::pair<char const *, float> anglePresets[] = {
                          {"45", 45.f}, {"90", 90.f}, {"180", 180.f}, {"270", 270.f}};
                        int angleIndex = 0;
                        for (auto const & [label, value] : anglePresets)
                        {
                            if (angleIndex++ > 0)
                            {
                                ImGui::SameLine(0.f, spacing);
                            }
                            if (ImGui::Button(label,
                                              ImVec2(angleButtonWidth, keyHeight)))
                            {
                                m_step = numeric_input_detail::initialStep(value);
                                setValue(value);
                            }
                        }
                    }
                    ImGui::EndTable();
                }
            }

        bool const escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        bool const enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        auto const changedValue = takeChangedValue();
        ImGui::End();
        ImGui::PopStyleVar(5);

        if (cancelRequested || escapePressed)
        {
            update.value = m_initialValue;
            update.action = NumericInputOverlayAction::Cancel;
            close();
        }
        else if (applyRequested || (enterPressed && targetAvailable && m_hasValidValue))
        {
            update.value = changedValue;
            update.action = NumericInputOverlayAction::Apply;
            close();
        }
        else
        {
            update.value = changedValue;
        }
        return update;
    }

    bool NumericInputOverlay::isActive() const
    {
        return m_isActive;
    }

    void NumericInputOverlay::close()
    {
        m_isActive = false;
        m_valueChangedThisFrame = false;
        m_focusInputOnOpen = false;
        m_rulerTapValue.reset();
        m_rulerWasDragged = false;
        m_rulerOffsetTicks = 0.0;
        m_rulerDragStartOffsetTicks = 0.0;
    }

    void NumericInputOverlay::setValue(float value)
    {
        setRulerValue(value, numeric_input_detail::rulerOffsetForValue(value, m_step));
    }

    void NumericInputOverlay::setRulerValue(float value, double offsetTicks)
    {
        if (!std::isfinite(value))
        {
            m_hasValidValue = false;
            return;
        }
        m_rulerOffsetTicks = offsetTicks;
        m_valueChangedThisFrame |= m_value != value;
        m_value = value;
        m_inputText = numeric_input_detail::formatInputValue(value);
        m_hasValidValue = true;
    }

    void NumericInputOverlay::appendDigit(char digit)
    {
        m_inputText.push_back(digit);
        refreshValueFromText();
    }

    void NumericInputOverlay::appendDecimal()
    {
        if (m_inputText.find('.') != std::string::npos ||
            m_inputText.find('e') != std::string::npos ||
            m_inputText.find('E') != std::string::npos)
        {
            return;
        }
        if (m_inputText.empty())
        {
            m_inputText = "0.";
        }
        else if (m_inputText == "-")
        {
            m_inputText = "-0.";
        }
        else
        {
            m_inputText.push_back('.');
        }
        refreshValueFromText();
    }

    void NumericInputOverlay::toggleSign()
    {
        if (!m_inputText.empty() && m_inputText.front() == '-')
        {
            m_inputText.erase(m_inputText.begin());
        }
        else
        {
            m_inputText.insert(m_inputText.begin(), '-');
        }
        refreshValueFromText();
    }

    void NumericInputOverlay::eraseLastCharacter()
    {
        if (!m_inputText.empty())
        {
            m_inputText.pop_back();
        }
        refreshValueFromText();
    }

    void NumericInputOverlay::refreshValueFromText()
    {
        auto const parsedValue = numeric_input_detail::parseFloat(m_inputText);
        m_hasValidValue = parsedValue.has_value();
        if (parsedValue)
        {
            m_rulerOffsetTicks = numeric_input_detail::rulerOffsetForValue(*parsedValue, m_step);
            m_valueChangedThisFrame |= m_value != *parsedValue;
            m_value = *parsedValue;
        }
    }
} // namespace gladius::ui
