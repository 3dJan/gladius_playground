// Large-form prompt editor bound to the same draft as the composer card. Split out of
// PromptInput.cpp so the card rendering and the standalone window stay separately readable.

#include "PromptInput.h"
#include "PromptInputShared.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace gladius::ui
{
    namespace
    {
        char const * const expandedWindowTitle =
          "Agent Prompt Editor###GladiusAgentPromptEditor";
    } // namespace

    std::optional<PromptInput::RenderResult> PromptInput::renderExpandedWindow(bool sendEnabled)
    {
        if (!m_expandedOpen)
        {
            return std::nullopt;
        }

        ImGuiViewport const * viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.2f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.15f),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.6f,
                                        viewport->WorkSize.y * 0.7f),
                                 ImGuiCond_FirstUseEver);

        RenderResult result;
        bool open = true;
        if (!ImGui::Begin(expandedWindowTitle, &open))
        {
            m_expandedOpen = open;
            ImGui::End();
            return result;
        }

        ImGuiStyle const & style = ImGui::GetStyle();
        char const * const chordHelp = sendChordHelp();
        float const footerHeight =
          ImGui::GetFrameHeight() + ImGui::CalcTextSize(chordHelp).y + style.ItemSpacing.y * 3.f;
        ImGui::InputTextMultiline("##expandedField", &m_draft, ImVec2(-1.f, -footerHeight));
        result.focused = ImGui::IsItemFocused();
        if (result.focused)
        {
            m_activeFieldId = ImGui::GetItemID();
        }
        bool const chordPressed = m_lastExpandedFocused && sendEnabled && isSendChordPressed();

        float const innerWidth =
          std::max(1.f, ImGui::GetItemRectSize().x - style.FramePadding.x * 2.f);
        std::size_t const charsPerLine =
          std::max<std::size_t>(1u, static_cast<std::size_t>(innerWidth / averageGlyphWidth()));

        ImGui::TextDisabled("%zu chars, ~%zu lines - %s",
                            m_draft.size(),
                            countVisualLines(m_draft, charsPerLine),
                            chordHelp);
        ImGui::SameLine();
        ImGui::BeginDisabled(!sendEnabled);
        bool const sendClicked = ImGui::Button("Send");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close"))
        {
            open = false;
        }

        ImGui::End();
        m_expandedOpen = open;

        result.sendRequested = sendClicked || chordPressed;
        m_lastExpandedFocused = result.focused;
        return result;
    }
} // namespace gladius::ui
