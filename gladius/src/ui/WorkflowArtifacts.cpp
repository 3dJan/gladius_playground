#include "WorkflowArtifacts.h"

#include "AgentWorkflowAnimation.h"

#include "../Document.h"
#include "../nodes/Assembly.h"

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <vector>

namespace gladius::ui
{
    namespace
    {
        constexpr ImVec4 kMeasuredColor = {0.40f, 0.78f, 0.46f, 1.00f};
        constexpr ImVec4 kWarningColor = {1.00f, 0.72f, 0.32f, 1.00f};

        void pushRowAlpha(float alpha)
        {
            if (alpha < 0.999f)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            }
        }

        void popRowAlpha(float alpha)
        {
            if (alpha < 0.999f)
            {
                ImGui::PopStyleVar();
            }
        }

        void sectionLabel(char const * label)
        {
            ImGui::TextDisabled("%s", label);
        }

        ImVec4 accentColor()
        {
            return ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
        }
    } // namespace

    float WorkflowArtifacts::noteArrival(std::string const & key,
                                         AgentWorkflowAnimationState & animation) const
    {
        float const now = static_cast<float>(ImGui::GetTime());
        animation.noteArtifact(key, now);
        return animation.artifactArrival(key, now);
    }

    void WorkflowArtifacts::renderRequirementRow(design::Requirement const & requirement,
                                                 AgentWorkflowAnimationState & animation)
    {
        std::string const key = "req_" + requirement.id;
        float const alpha = noteArrival(key, animation);
        pushRowAlpha(alpha);
        ImGui::PushID(requirement.id.c_str());

        float const width = ImGui::GetContentRegionAvail().x;
        char descBuf[512];
        std::snprintf(descBuf, sizeof(descBuf), "%s", requirement.description.c_str());
        ImGui::SetNextItemWidth(width * 0.50f);
        bool edited = ImGui::InputText("##desc", descBuf, sizeof(descBuf));
        ImGui::SameLine();
        char catBuf[64];
        std::snprintf(catBuf, sizeof(catBuf), "%s", requirement.category.c_str());
        ImGui::SetNextItemWidth(width * 0.18f);
        edited |= ImGui::InputText("##cat", catBuf, sizeof(catBuf));
        ImGui::SameLine();
        bool mandatory = requirement.mandatory;
        edited |= ImGui::Checkbox("Constraint", &mandatory);
        ImGui::SameLine();
        bool removed = ImGui::SmallButton("x");
        if (edited)
        {
            m_graph->updateRequirement(requirement.id, descBuf, catBuf, mandatory, true);
        }
        if (removed)
        {
            m_graph->removeRequirement(requirement.id, true);
        }

        ImGui::PopID();
        popRowAlpha(alpha);
    }

    void WorkflowArtifacts::renderUnderstand(AgentWorkflowAnimationState & animation)
    {
        sectionLabel("Requirements - extracted, not yet verified");
        auto const requirementsCopy = m_graph->requirements();
        if (requirementsCopy.empty())
        {
            ImGui::TextDisabled("Nothing captured yet.");
        }

        std::vector<design::Requirement> hard;
        std::vector<design::Requirement> soft;
        for (auto const & req : requirementsCopy)
        {
            (req.mandatory ? hard : soft).push_back(req);
        }
        for (auto const & req : hard)
        {
            renderRequirementRow(req, animation);
        }
        if (!soft.empty())
        {
            sectionLabel("Preferences and objectives");
            for (auto const & req : soft)
            {
                renderRequirementRow(req, animation);
            }
        }

        float const width = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(width * 0.50f);
        ImGui::InputTextWithHint("##newReqDesc", "Add requirement...", m_newRequirementBuf,
                                 sizeof(m_newRequirementBuf));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width * 0.18f);
        ImGui::InputText("##newReqCat", m_newRequirementCategoryBuf,
                         sizeof(m_newRequirementCategoryBuf));
        ImGui::SameLine();
        ImGui::Checkbox("Constraint##new", &m_newRequirementMandatory);
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Add") && m_newRequirementBuf[0] != '\0')
        {
            std::string const newId = m_graph->addRequirement(
              m_newRequirementBuf, m_newRequirementCategoryBuf, m_newRequirementMandatory);
            m_graph->recordUserAddedRequirement(newId);
            m_newRequirementBuf[0] = '\0';
        }
    }

    void WorkflowArtifacts::collectNodeRows(std::string const & nodeId,
                                            std::size_t depth,
                                            std::vector<NodeRow> & out) const
    {
        auto const it = m_graph->nodes().find(nodeId);
        if (it == m_graph->nodes().end())
        {
            return;
        }
        out.push_back(NodeRow{nodeId, it->second.name, depth});
        for (auto const & childId : it->second.children)
        {
            collectNodeRows(childId, depth + 1, out);
        }
    }

    std::vector<WorkflowArtifacts::NodeRow> WorkflowArtifacts::designStructureRows() const
    {
        auto const & nodes = m_graph->nodes();
        std::vector<std::string> roots;
        for (auto const & [id, node] : nodes)
        {
            bool const isChild = std::any_of(nodes.begin(),
                                             nodes.end(),
                                             [&](auto const & entry) {
                                                 auto const & children = entry.second.children;
                                                 return std::find(children.begin(),
                                                                  children.end(),
                                                                  id)
                                                        != children.end();
                                             });
            if (!isChild)
            {
                roots.push_back(id);
            }
        }
        std::vector<NodeRow> rows;
        for (auto const & rootId : roots)
        {
            collectNodeRows(rootId, 0, rows);
        }
        return rows;
    }

    void WorkflowArtifacts::renderDecompose(AgentWorkflowAnimationState & animation)
    {
        sectionLabel("Design structure");
        auto const rows = designStructureRows();
        if (rows.empty())
        {
            ImGui::TextDisabled("The problem has not been decomposed yet.");
            return;
        }
        float const indentUnit = ImGui::GetFontSize() * 0.75f;
        for (auto const & row : rows)
        {
            auto const nodeIt = m_graph->nodes().find(row.id);
            if (nodeIt == m_graph->nodes().end())
            {
                continue;
            }
            std::string const key = "node_" + row.id;
            float const alpha = noteArrival(key, animation);
            pushRowAlpha(alpha);
            ImGui::PushID(row.id.c_str());
            ImGui::Indent(static_cast<float>(row.depth) * indentUnit);
            bool const frozen = nodeIt->second.status == design::DesignStatus::Frozen;
            ImGui::Text("%s", row.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s - %zu alternatives%s",
                                design::toString(nodeIt->second.type),
                                nodeIt->second.variantIds.size(),
                                frozen ? ", frozen" : "");
            ImGui::SameLine();
            if (ImGui::SmallButton(frozen ? "Unfreeze" : "Freeze"))
            {
                if (frozen)
                {
                    m_graph->unfreezeNode(row.id);
                }
                else
                {
                    m_graph->freezeNode(row.id);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Show alternatives"))
            {
                m_exploredNodeId = row.id;
            }
            ImGui::Unindent(static_cast<float>(row.depth) * indentUnit);
            ImGui::PopID();
            popRowAlpha(alpha);
        }
    }

    void WorkflowArtifacts::renderVariantCard(design::DesignVariant const & variant,
                                              bool selected,
                                              AgentWorkflowAnimationState & animation)
    {
        std::string const key = "var_" + variant.id;
        float const alpha = noteArrival(key, animation);
        pushRowAlpha(alpha);
        ImGui::PushID(variant.id.c_str());

        float const box = ImGui::GetFontSize() * 2.6f;
        ImGui::BeginGroup();
        int w = 0;
        int h = 0;
        unsigned int const texId =
          m_previewTexture ? m_previewTexture("var_" + variant.id, variant.previewPng, w, h) : 0u;
        if (texId != 0u)
        {
            ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texId)),
                         ImVec2(box, box));
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texId)),
                             ImVec2(box * 3.5f, box * 3.5f));
                ImGui::Text("%s (%s)", variant.name.c_str(), design::toString(variant.fidelity));
                if (!variant.rationale.empty())
                {
                    ImGui::TextWrapped("Rationale: %s", variant.rationale.c_str());
                }
                if (!variant.userFeedback.empty())
                {
                    ImGui::TextWrapped("Your comment: %s", variant.userFeedback.c_str());
                }
                ImGui::EndTooltip();
            }
        }
        else
        {
            // No preview has been rendered for this alternative: say so instead of inventing one.
            ImGui::Dummy(ImVec2(box, box));
            ImDrawList & dl = *ImGui::GetWindowDrawList();
            dl.AddRect(ImGui::GetItemRectMin(),
                       ImGui::GetItemRectMax(),
                       ImGui::GetColorU32(ImGuiCol_Border),
                       0.f,
                       0,
                       1.f);
            dl.AddText(ImVec2(ImGui::GetItemRectMin().x + 4.f,
                              ImGui::GetItemRectMin().y + box * 0.38f),
                       ImGui::GetColorU32(ImGuiCol_TextDisabled),
                       "no preview");
        }
        ImGui::EndGroup();
        ImVec2 const boxMin = ImGui::GetItemRectMin();
        ImVec2 const boxMax = ImGui::GetItemRectMax();

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s", variant.name.c_str());
        ImGui::TextDisabled("%s - %s",
                            design::toString(variant.fidelity),
                            design::toString(variant.status));
        if (!variant.rationale.empty())
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextWrapped("%s", variant.rationale.c_str());
            ImGui::PopTextWrapPos();
        }
        char fbBuf[256];
        std::snprintf(fbBuf, sizeof(fbBuf), "%s", variant.userFeedback.c_str());
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputTextWithHint("##varFb", "Comment...", fbBuf, sizeof(fbBuf)))
        {
            m_graph->setVariantFeedback(variant.id, fbBuf);
        }
        if (ImGui::SmallButton("Reject") && variant.status != design::DesignStatus::Rejected)
        {
            m_graph->rejectVariant(variant.id, "Rejected in the Design Agent panel");
        }
        ImGui::EndGroup();

        if (selected)
        {
            ImGui::GetWindowDrawList()->AddRect(
              ImVec2(boxMin.x - 3.f, boxMin.y - 3.f),
              ImVec2(std::max(boxMax.x, ImGui::GetItemRectMax().x) + 3.f,
                     std::max(boxMax.y, ImGui::GetItemRectMax().y) + 3.f),
              ImGui::GetColorU32(accentColor()),
              3.f,
              0,
              1.6f);
        }

        ImGui::PopID();
        popRowAlpha(alpha);
        ImGui::Spacing();
    }

    void WorkflowArtifacts::renderConfigurationRow(design::Configuration const & configuration,
                                                   AgentWorkflowAnimationState & animation)
    {
        std::string const key = "cfg_" + configuration.id;
        float const alpha = noteArrival(key, animation);
        pushRowAlpha(alpha);
        ImGui::PushID(configuration.id.c_str());

        float const box = ImGui::GetFontSize() * 2.2f;
        int w = 0;
        int h = 0;
        unsigned int const texId = m_previewTexture
                                     ? m_previewTexture("cfg_" + configuration.id,
                                                        configuration.previewPng,
                                                        w,
                                                        h)
                                     : 0u;
        if (texId != 0u)
        {
            ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texId)),
                         ImVec2(box, box));
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texId)),
                             ImVec2(box * 3.5f, box * 3.5f));
                ImGui::Text("%s (%s)%s",
                            configuration.name.c_str(),
                            configuration.id.c_str(),
                            configuration.active ? " [active]" : "");
                ImGui::Text("Alternatives selected: %zu", configuration.selectedVariants.size());
                if (!configuration.userFeedback.empty())
                {
                    ImGui::TextWrapped("Your comment: %s", configuration.userFeedback.c_str());
                }
                for (auto const & pass : configuration.evaluation.deterministicPassed)
                {
                    ImGui::TextColored(kMeasuredColor, "Measured OK: %s", pass.c_str());
                }
                for (auto const & warn : configuration.evaluation.deterministicWarnings)
                {
                    ImGui::TextColored(kWarningColor, "Warning: %s", warn.c_str());
                }
                ImGui::EndTooltip();
            }
        }
        ImGui::SameLine();

        ImGui::BeginGroup();
        auto selIt = std::find(selectedRecombineConfigs.begin(),
                               selectedRecombineConfigs.end(),
                               configuration.id);
        bool selectedForRecombine = selIt != selectedRecombineConfigs.end();
        if (ImGui::Checkbox("##recombine", &selectedForRecombine))
        {
            if (selectedForRecombine)
            {
                selectedRecombineConfigs.push_back(configuration.id);
            }
            else if (selIt != selectedRecombineConfigs.end())
            {
                selectedRecombineConfigs.erase(selIt);
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s%s", configuration.name.c_str(), configuration.active ? " [active]" : "");
        if (!configuration.active && ImGui::SmallButton("Activate"))
        {
            m_graph->activateConfiguration(configuration.id);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Inspect"))
        {
            m_exploredNodeId.clear();
            for (auto const & [nodeId, variantId] : configuration.selectedVariants)
            {
                m_exploredNodeId = nodeId;
                break;
            }
        }
        char fbBuf[256];
        std::snprintf(fbBuf, sizeof(fbBuf), "%s", configuration.userFeedback.c_str());
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputTextWithHint("##cfgFb", "Comment...", fbBuf, sizeof(fbBuf)))
        {
            m_graph->setConfigurationFeedback(configuration.id, fbBuf);
        }
        ImGui::EndGroup();

        ImGui::PopID();
        popRowAlpha(alpha);
        ImGui::Separator();
    }

    void WorkflowArtifacts::renderCheckpointRow(tools::CheckpointSummary const & checkpoint,
                                                AgentWorkflowAnimationState & animation)
    {
        std::string const key = "cp_" + checkpoint.id;
        float const alpha = noteArrival(key, animation);
        pushRowAlpha(alpha);
        ImGui::PushID(checkpoint.id.c_str());

        float const box = ImGui::GetFontSize() * 1.7f;
        int w = 0;
        int h = 0;
        unsigned int const texId = m_previewTexture
                                     ? m_previewTexture("cp_" + checkpoint.id,
                                                        checkpoint.previewPng,
                                                        w,
                                                        h)
                                     : 0u;
        if (texId != 0u)
        {
            ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texId)),
                         ImVec2(box, box));
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texId)),
                             ImVec2(box * 3.5f, box * 3.5f));
                ImGui::Text("%s (%s)", checkpoint.label.c_str(), checkpoint.id.c_str());
                ImGui::Text("Functions: %zu | Level: %d", checkpoint.functionCount, checkpoint.level);
                if (!checkpoint.userFeedback.empty())
                {
                    ImGui::TextWrapped("Your comment: %s", checkpoint.userFeedback.c_str());
                }
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
        }
        ImGui::Text("%s (%zu functions)", checkpoint.label.c_str(), checkpoint.functionCount);
        ImGui::SameLine();
        if (ImGui::SmallButton("Restore"))
        {
            if (m_document != nullptr && m_document->getAssembly()
                && m_checkpoints->restoreCheckpoint(checkpoint.id, *m_document->getAssembly()))
            {
                m_document->update3mfModel();
                m_document->rebuildResourceDependencyGraph();
                m_document->markFileAsChanged();
                if (m_onDocumentModified)
                {
                    m_onDocumentModified();
                }
            }
        }
        char cpFb[256];
        std::snprintf(cpFb, sizeof(cpFb), "%s", checkpoint.userFeedback.c_str());
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputTextWithHint("##cpFb", "Snapshot comment...", cpFb, sizeof(cpFb)))
        {
            m_checkpoints->setFeedback(checkpoint.id, cpFb);
        }

        ImGui::PopID();
        popRowAlpha(alpha);
        ImGui::Spacing();
    }

    void WorkflowArtifacts::renderExplore(AgentWorkflowAnimationState & animation)
    {
        auto const & nodes = m_graph->nodes();
        if (nodes.empty())
        {
            ImGui::TextDisabled("No design elements to compare alternatives for yet.");
            return;
        }

        if (m_exploredNodeId.empty() || nodes.find(m_exploredNodeId) == nodes.end())
        {
            for (auto const & [id, node] : nodes)
            {
                if (node.variantIds.size()
                    > (nodes.find(m_exploredNodeId) != nodes.end()
                         ? nodes.find(m_exploredNodeId)->second.variantIds.size()
                         : 0))
                {
                    m_exploredNodeId = id;
                }
            }
        }
        auto const explored = nodes.find(m_exploredNodeId);
        if (explored == nodes.end())
        {
            ImGui::TextDisabled("Select an element under Decompose to see its alternatives.");
            return;
        }

        std::vector<std::string> nodeIds;
        std::vector<std::string> labels;
        for (auto const & [id, node] : nodes)
        {
            nodeIds.push_back(id);
            labels.push_back(node.name + " (" + std::to_string(node.variantIds.size()) + ")");
        }
        int currentIndex = 0;
        for (std::size_t i = 0; i < nodeIds.size(); ++i)
        {
            if (nodeIds[i] == m_exploredNodeId)
            {
                currentIndex = static_cast<int>(i);
            }
        }
        ImGui::SetNextItemWidth(-1.f);
        auto comboGetter = [](void * data, int index) -> char const * {
            auto const & items = *static_cast<std::vector<std::string> *>(data);
            if (index < 0 || static_cast<std::size_t>(index) >= items.size())
            {
                return "";
            }
            return items[static_cast<std::size_t>(index)].c_str();
        };
        if (ImGui::Combo("##exploredNode",
                         &currentIndex,
                         comboGetter,
                         &labels,
                         static_cast<int>(labels.size()))
            && currentIndex >= 0 && static_cast<std::size_t>(currentIndex) < nodeIds.size())
        {
            m_exploredNodeId = nodeIds[static_cast<std::size_t>(currentIndex)];
        }
        ImGui::Spacing();

        std::size_t ready = 0;
        std::size_t rejected = 0;
        for (auto const & vid : explored->second.variantIds)
        {
            auto const vit = m_graph->variants().find(vid);
            if (vit == m_graph->variants().end())
            {
                continue;
            }
            if (vit->second.status == design::DesignStatus::Rejected)
            {
                ++rejected;
            }
            else
            {
                ++ready;
            }
        }
        ImGui::TextDisabled("%zu of %zu alternatives ready (%zu rejected)",
                            ready,
                            explored->second.variantIds.size(),
                            rejected);

        auto const activeConfig = m_graph->activeConfiguration();
        for (auto const & vid : explored->second.variantIds)
        {
            auto const vit = m_graph->variants().find(vid);
            if (vit == m_graph->variants().end())
            {
                continue;
            }
            bool selected = false;
            if (activeConfig)
            {
                auto const sel = activeConfig->selectedVariants.find(explored->first);
                selected = sel != activeConfig->selectedVariants.end() && sel->second == vid;
            }
            renderVariantCard(vit->second, selected, animation);
        }

        if (!m_graph->configurations().empty())
        {
            ImGui::Spacing();
            sectionLabel("System configurations");
            if (selectedRecombineConfigs.size() >= 2)
            {
                if (ImGui::Button("Recombine the two selected"))
                {
                    std::string const & baseId = selectedRecombineConfigs[0];
                    std::string const & overrideId = selectedRecombineConfigs[1];
                    std::vector<std::string> overrideNodes;
                    auto const overrideIt = m_graph->configurations().find(overrideId);
                    if (overrideIt != m_graph->configurations().end())
                    {
                        for (auto const & [nodeId, variantId] : overrideIt->second.selectedVariants)
                        {
                            overrideNodes.push_back(nodeId);
                        }
                    }
                    std::string const hybridName = "Hybrid (" + baseId + " + " + overrideId + ")";
                    if (auto const hybridId = m_graph->hybridizeConfigurations(
                          hybridName, baseId, overrideId, overrideNodes))
                    {
                        m_graph->activateConfiguration(*hybridId);
                    }
                    selectedRecombineConfigs.clear();
                }
                ImGui::Separator();
            }
            auto const configsCopy = m_graph->configurations();
            for (auto const & [cid, cfg] : configsCopy)
            {
                renderConfigurationRow(cfg, animation);
            }
        }

        auto const checkpoints = m_checkpoints->listCheckpoints();
        if (!checkpoints.empty())
        {
            ImGui::Spacing();
            sectionLabel("Iteration snapshots");
            for (auto const & cp : checkpoints)
            {
                renderCheckpointRow(cp, animation);
            }
        }
    }

    void WorkflowArtifacts::renderDevelop(AgentWorkflowAnimationState & animation)
    {
        sectionLabel("Development tasks");
        if (!activeToolName.empty())
        {
            ImGui::TextDisabled("Running: %s", activeToolName.c_str());
        }
        if (m_todos->items().empty())
        {
            ImGui::TextDisabled("No planning tasks recorded yet.");
        }
        for (auto const & todo : m_todos->items())
        {
            std::string const key = "todo_" + todo.id;
            float const alpha = noteArrival(key, animation);
            pushRowAlpha(alpha);
            ImGui::PushID(todo.id.c_str());
            char const * marker = "○";
            ImVec4 todoColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            switch (todo.status)
            {
            case design::TodoStatus::Done:
                marker = "✓";
                todoColor = kMeasuredColor;
                break;
            case design::TodoStatus::InProgress:
                marker = "◉";
                break;
            case design::TodoStatus::Blocked:
                marker = "!";
                todoColor = kWarningColor;
                break;
            case design::TodoStatus::Pending:
                break;
            }
            ImGui::TextColored(todoColor, "%s %s", marker, todo.title.c_str());
            for (std::size_t ci = 0; ci < todo.checklist.size(); ++ci)
            {
                bool checked = todo.checklist[ci].checked;
                if (ImGui::Checkbox(
                      (todo.checklist[ci].label + "##chk" + std::to_string(ci)).c_str(), &checked))
                {
                    m_todos->toggleChecklistEntry(todo.id, ci, checked);
                }
            }
            ImGui::PopID();
            popRowAlpha(alpha);
        }
    }

    void WorkflowArtifacts::renderValidate(AgentWorkflowAnimationState & animation)
    {
        sectionLabel("Measured by Gladius evaluators");
        auto const active = m_graph->activeConfiguration();
        if (!active)
        {
            ImGui::TextDisabled("No active configuration has been measured.");
        }
        else
        {
            std::size_t const measured = active->evaluation.deterministicPassed.size()
                                         + active->evaluation.deterministicWarnings.size();
            if (measured == 0)
            {
                ImGui::TextDisabled("No deterministic checks recorded for \"%s\".",
                                    active->name.c_str());
            }
            for (auto const & pass : active->evaluation.deterministicPassed)
            {
                ImGui::TextColored(kMeasuredColor, "✓ %s", pass.c_str());
            }
            for (auto const & warn : active->evaluation.deterministicWarnings)
            {
                ImGui::TextColored(kWarningColor, "! %s", warn.c_str());
            }
        }

        ImGui::Spacing();
        sectionLabel("Agent assessment - not independently measured");
        bool const anyNote =
          active && (!active->evaluation.heuristicNotes.empty()
                     || !active->evaluation.subjectiveNotes.empty());
        if (!anyNote)
        {
            ImGui::TextDisabled("No agent assertions recorded.");
        }
        else
        {
            for (auto const & note : active->evaluation.heuristicNotes)
            {
                ImGui::TextDisabled("~ %s", note.c_str());
            }
            for (auto const & note : active->evaluation.subjectiveNotes)
            {
                ImGui::TextDisabled("~ %s", note.c_str());
            }
        }
        (void) animation;
    }
} // namespace gladius::ui
