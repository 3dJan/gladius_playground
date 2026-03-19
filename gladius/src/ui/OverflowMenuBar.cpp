#include "OverflowMenuBar.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>

namespace gladius::ui
{
    std::unordered_map<ImGuiID, OverflowMenuBar::BarState> OverflowMenuBar::s_barStates;

    void OverflowMenuBar::begin(char const * id)
    {
        m_items.clear();
        m_barId = ImGui::GetID(id);
    }

    void OverflowMenuBar::item(char const * label, std::function<void()> render)
    {
        m_items.push_back({label, std::move(render)});
    }

    void OverflowMenuBar::end()
    {
        if (m_items.empty())
        {
            return;
        }

        auto & state = s_barStates[m_barId];

        // Available width for the menu bar content
        float const availableWidth =
          ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;

        // Estimate the width of the "..." overflow button
        float const overflowBtnWidth =
          ImGui::CalcTextSize(ICON_FA_ELLIPSIS_H).x + ImGui::GetStyle().ItemSpacing.x * 2.f +
          ImGui::GetStyle().FramePadding.x * 2.f;

        // Determine which items fit inline using previous-frame measurements.
        // On the very first frame (no measurements yet) estimate from label text.
        int overflowStart = static_cast<int>(m_items.size()); // all fit by default
        {
            float accum = 0.f;
            for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
            {
                float w = 0.f;
                if (i < static_cast<int>(state.itemWidths.size()))
                {
                    w = state.itemWidths[i];
                }
                else
                {
                    // First-frame estimate: text size + padding
                    w = ImGui::CalcTextSize(m_items[i].label.c_str()).x +
                        ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x * 2.f;
                }

                float const reservedForOverflow =
                  (i + 1 < static_cast<int>(m_items.size())) ? overflowBtnWidth : 0.f;

                if (accum + w + reservedForOverflow > availableWidth)
                {
                    overflowStart = i;
                    break;
                }
                accum += w;
            }
        }

        // Resize measurement storage
        state.itemWidths.resize(m_items.size(), 0.f);

        // Render inline items and measure their widths
        for (int i = 0; i < overflowStart; ++i)
        {
            float const beforeX = ImGui::GetCursorPosX();
            m_items[i].render();
            float const afterX = ImGui::GetCursorPosX();
            state.itemWidths[i] = afterX - beforeX;
        }

        // Render overflow menu if there are items that didn't fit
        if (overflowStart < static_cast<int>(m_items.size()))
        {
            if (ImGui::BeginMenu(ICON_FA_ELLIPSIS_H))
            {
                for (int i = overflowStart; i < static_cast<int>(m_items.size()); ++i)
                {
                    m_items[i].render();
                    // Use label-based estimate for overflow items (they aren't measured inline)
                    state.itemWidths[i] =
                      ImGui::CalcTextSize(m_items[i].label.c_str()).x +
                      ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x * 2.f;
                }
                ImGui::EndMenu();
            }
        }
    }

} // namespace gladius::ui
