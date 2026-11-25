// Model picker for the Design Agent composer card: the primary model combo and the
// fallback selection combo, both sized by the card toolbar layout.

#include "AgentPanels.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace gladius::ui
{
    void AgentPanels::renderModelCombos(PromptInput::ToolbarLayout const & layout)
    {
        std::vector<agent::ModelSelection> allChoices;
        std::vector<std::string> allLabels;
        for (auto const & prof : m_providerProfiles)
        {
            for (auto const & mdl : prof.models)
            {
                if (!prof.isModelAvailable(mdl))
                {
                    continue;
                }
                allChoices.push_back(agent::ModelSelection{prof.id, mdl});
                allLabels.push_back(prof.name + " / " + mdl);
            }
        }
        if (allChoices.empty())
        {
            if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_EXCLAMATION_TRIANGLE
                                                            " Configure providers"),
                              ImVec2(layout.hostWidth, layout.rowHeight)))
            {
                m_showSettings = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("No model is available for the configured providers");
            }
            return;
        }

        // A chip glyph marks the combo as the model picker without spending width on a label.
        std::string primaryLabel = std::string(ICON_FA_MICROCHIP) + " Select model";
        for (std::size_t i = 0; i < allChoices.size(); ++i)
        {
            if (allChoices[i] == m_primarySelection)
            {
                primaryLabel = std::string(ICON_FA_MICROCHIP) + " " + allLabels[i];
                break;
            }
        }

        auto showModelTooltip = [&](agent::ModelSelection const & sel) {
            if (!ImGui::IsItemHovered())
            {
                return;
            }
            for (auto const & prof : m_providerProfiles)
            {
                if (prof.id != sel.profileId)
                {
                    continue;
                }
                auto mit = prof.modelMetadata.find(sel.model);
                if (mit != prof.modelMetadata.end())
                {
                    auto const & meta = mit->second;
                    std::string ctxStr = meta.contextLength > 0
                                           ? (std::to_string(meta.contextLength / 1000) + "k ctx")
                                           : "Context unknown";
                    ImGui::SetTooltip("Price: %s\nContext: %s\nVision: %s",
                                      meta.formatPriceSummary().c_str(),
                                      ctxStr.c_str(),
                                      meta.supportsVision ? "Yes" : "No");
                }
                else if (agent::isLocalEndpoint(prof.endpoint))
                {
                    ImGui::SetTooltip("Local model (free / no API cost)");
                }
                else
                {
                    ImGui::SetTooltip("Provider: %s\nPrice info not queried", prof.name.c_str());
                }
                break;
            }
        };

        ImGui::SetNextItemWidth(layout.comboWidth);
        if (ImGui::BeginCombo("##PrimaryModel", primaryLabel.c_str()))
        {
            for (std::size_t i = 0; i < allChoices.size(); ++i)
            {
                bool const selected = allChoices[i] == m_primarySelection;
                if (ImGui::Selectable(allLabels[i].c_str(), selected))
                {
                    m_primarySelection = allChoices[i];
                    saveSettings();
                }
                showModelTooltip(allChoices[i]);
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        showModelTooltip(m_primarySelection);
        ImGui::SameLine();
        std::string const fbPreview
          = m_fallbackSelections.empty()
              ? std::string(ICON_FA_LAYER_GROUP) + " Fallbacks"
              : (std::string(ICON_FA_LAYER_GROUP) + " Fallbacks ("
                 + std::to_string(m_fallbackSelections.size()) + ")");
        ImGui::SetNextItemWidth(layout.comboWidth);
        if (ImGui::BeginCombo("##FallbackModels", fbPreview.c_str()))
        {
            for (std::size_t i = 0; i < allChoices.size(); ++i)
            {
                auto it = std::find(
                  m_fallbackSelections.begin(), m_fallbackSelections.end(), allChoices[i]);
                bool checked = it != m_fallbackSelections.end();
                if (ImGui::Checkbox(allLabels[i].c_str(), &checked))
                {
                    if (checked && it == m_fallbackSelections.end())
                    {
                        m_fallbackSelections.push_back(allChoices[i]);
                    }
                    else if (!checked && it != m_fallbackSelections.end())
                    {
                        m_fallbackSelections.erase(it);
                    }
                    saveSettings();
                }
                showModelTooltip(allChoices[i]);
            }
            ImGui::EndCombo();
        }
    }
} // namespace gladius::ui
