#include "RenderWindow.h"

#include "../../components/IconFontCppHeaders/IconsFontAwesome5.h"
#include "../Profiling.h"

#include "imgui.h"

namespace gladius::ui
{
    void RenderWindow::setNeutralSliceHeight(float const sliceHeight)
    {
        if (m_neutralSliceHeightInitialized && m_neutralRenderSettings.sliceHeight == sliceHeight)
        {
            return;
        }

        m_neutralRenderSettings.sliceHeight = sliceHeight;
        m_neutralSliceHeightInitialized = true;
        if (m_sliceHeightChangedCallback)
        {
            m_sliceHeightChangedCallback(sliceHeight);
        }
        invalidateViewDueToRenderSettingsChange();
    }

    void RenderWindow::invalidateViewDueToRenderSettingsChange()
    {
        m_dirty = true;
        queueRenderDecision(m_renderUpdateCoordinator.notifyRenderSettingsChanged());
        m_neutralRenderScheduler.requestCancellationForStale();
        if (m_view != nullptr)
        {
            m_view->startAnimationMode();
        }
    }

    void RenderWindow::slider(ImVec2 const & areaMin, ImVec2 const & areaMax)
    {
        ProfileFunction;

        bool const useNeutralBackend = m_core == nullptr && m_neutralBackendActive;
        if (m_core == nullptr && !useNeutralBackend)
        {
            return;
        }

        int renderingFlags = static_cast<int>(m_neutralRenderSettings.flags);
#if defined(GLADIUS_ENABLE_OPENCL)
        if (m_core != nullptr)
        {
            renderingFlags = m_core->getResourceContext()->getRenderingSettings().flags;
        }
#endif

        auto const boundingBox = tryFetchBoundingBox(false);
        bool const hasBoundingBox = boundingBox.has_value();
        compute::RenderEvaluationDomain const fallbackDomain;
        float const minZ = hasBoundingBox ? boundingBox->min.z : fallbackDomain.min[2];
        float const maxZ = hasBoundingBox ? boundingBox->max.z : fallbackDomain.max[2];

        float z = m_neutralRenderSettings.sliceHeight;
#if defined(GLADIUS_ENABLE_OPENCL)
        if (m_core != nullptr)
        {
            z = hasBoundingBox ? m_core->getSliceHeight() : 0.0f;
        }
#endif
        if (useNeutralBackend && hasBoundingBox && !m_neutralSliceHeightInitialized)
        {
            z = 0.5f * (minZ + maxZ);
            setNeutralSliceHeight(z);
        }

        float constexpr sliderWidth = 20.0f;
        float constexpr inputWidth = 80.0f;
        float constexpr overlayWidth = std::max(sliderWidth, inputWidth);
        float constexpr padding = 6.0f;
        float const areaHeight = areaMax.y - areaMin.y;
        float const inputHeight = ImGui::GetFrameHeightWithSpacing();
        float const buttonRowHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
        float const sliderHeight =
          std::max(areaHeight - inputHeight - buttonRowHeight - padding * 4.0f, 30.0f);
        float const overlayHeight =
          buttonRowHeight + sliderHeight + inputHeight + padding * 4.0f;
        float const totalWidth = overlayWidth + padding * 2.0f;

        ImVec2 const overlayPos = {areaMax.x - totalWidth - padding,
                                   areaMin.y + (areaHeight - overlayHeight) * 0.5f};
        auto * const drawList = ImGui::GetWindowDrawList();
        ImVec2 const backgroundMax = {overlayPos.x + totalWidth,
                                      overlayPos.y + overlayHeight};
        auto const & frameBackground = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        ImU32 const backgroundColor = ImGui::ColorConvertFloat4ToU32(
          ImVec4(frameBackground.x, frameBackground.y, frameBackground.z, 0.4f));
        drawList->AddRectFilled(overlayPos, backgroundMax, backgroundColor, 8.0f);

        ImGui::SetCursorScreenPos(ImVec2(overlayPos.x + padding, overlayPos.y + padding));

        bool zChanged = false;
        bool flagsChanged = false;
        ImGui::BeginGroup();
        {
            bool cutOff = (renderingFlags & RF_CUT_OFF_OBJECT) != 0;
            bool showField = (renderingFlags & RF_SHOW_FIELD) != 0;
            ImVec4 const activeColor = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            ImVec4 const inactiveColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);

            ImGui::PushStyleColor(ImGuiCol_Button, cutOff ? activeColor : inactiveColor);
            if (ImGui::Button(ICON_FA_CUT "##CutToggle", ImVec2(0, 0)))
            {
                cutOff = !cutOff;
                flagsChanged = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Cut Off Object");
            }

            ImGui::PushStyleColor(ImGuiCol_Button, showField ? activeColor : inactiveColor);
            if (ImGui::Button(ICON_FA_GLOBE "##FieldToggle", ImVec2(0, 0)))
            {
                showField = !showField;
                flagsChanged = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Show Distance Field");
            }

            if (flagsChanged)
            {
                if (cutOff)
                {
                    renderingFlags |= RF_CUT_OFF_OBJECT;
                }
                else
                {
                    renderingFlags &= ~RF_CUT_OFF_OBJECT;
                }
                if (showField)
                {
                    renderingFlags |= RF_SHOW_FIELD;
                }
                else
                {
                    renderingFlags &= ~RF_SHOW_FIELD;
                }

#if defined(GLADIUS_ENABLE_OPENCL)
                if (m_core != nullptr)
                {
                    m_core->getResourceContext()->getRenderingSettings().flags = renderingFlags;
                    invalidateView();
                }
                else
#endif
                {
                    m_neutralRenderSettings.flags = static_cast<std::uint32_t>(renderingFlags);
                    invalidateViewDueToRenderSettingsChange();
                }
                if (m_renderSettingsChangedCallback)
                {
                    m_renderSettingsChangedCallback();
                }
            }

            float const sliderX =
              overlayPos.x + padding + (overlayWidth - sliderWidth) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(sliderX, ImGui::GetCursorScreenPos().y));
            zChanged = ImGui::VSliderFloat(
              "##CutHeight", ImVec2(sliderWidth, sliderHeight), &z, minZ, maxZ, "");

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%.2f mm\nDouble-click to reset", z);
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                z = minZ;
                zChanged = true;
            }

            float const inputX = overlayPos.x + padding + (overlayWidth - inputWidth) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(inputX, ImGui::GetCursorScreenPos().y));
            ImGui::SetNextItemWidth(inputWidth);
            if (ImGui::InputFloat("##CutHeightInput", &z, 0.0f, 0.0f, "%.1f"))
            {
                z = std::clamp(z, minZ, maxZ);
                zChanged = true;
            }
        }
        ImGui::EndGroup();

        m_dirty = m_dirty || zChanged;
        m_renderWindowState.isMoving = m_renderWindowState.isMoving || zChanged;

#if defined(GLADIUS_ENABLE_OPENCL)
        if (m_core != nullptr)
        {
            if (hasBoundingBox)
            {
                m_core->setSliceHeight(z);
            }
            if (zChanged)
            {
                m_core->invalidatePreCompSdf("sliderZChanged");
                m_core->precomputeSdfForWholeBuildPlatform();
                invalidateView();
            }
            return;
        }
#endif

        if (zChanged)
        {
            setNeutralSliceHeight(z);
        }
    }
}
