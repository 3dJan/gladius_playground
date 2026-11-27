#include "WorkflowArtifacts.h"

#include "AgentPanelMetrics.h"
#include "AgentPanelWidgets.h"
#include "AgentWorkflowAnimation.h"
#include "SignalColors.h"

#include <IconsFontAwesome5.h>

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
        /// @brief ImGui has no ImVec4+alpha overload of GetColorU32
        ImU32 tinted(ImVec4 color, float alpha)
        {
            color.w *= alpha;
            return ImGui::GetColorU32(color);
        }

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

        void sectionLabel(char const * icon, char const * label)
        {
            agentWidgets::sectionLabel(icon, label);
        }

        ImVec4 accentColor()
        {
            return agentWidgets::accent();
        }

        /// @brief Style the borderless table cells use: they read as text, not input boxes
        void pushFlatInputStyle()
        {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(0.f, ImGui::GetStyle().FramePadding.y));
        }

        void popFlatInputStyle()
        {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
        }

        bool flatInput(char const * label, char * buf, std::size_t bufSize, float width)
        {
            pushFlatInputStyle();
            ImGui::SetNextItemWidth(width);
            bool const submitted = ImGui::InputText(label, buf, bufSize) != false;
            popFlatInputStyle();
            return submitted;
        }

        bool flatInputWithHint(char const * label,
                               char const * hint,
                               char * buf,
                               std::size_t bufSize,
                               float width,
                               ImGuiInputTextFlags flags = ImGuiInputTextFlags_None)
        {
            pushFlatInputStyle();
            ImGui::SetNextItemWidth(width);
            bool const submitted
              = ImGui::InputTextWithHint(label, hint, buf, bufSize, flags) != false;
            popFlatInputStyle();
            return submitted;
        }

        /// @brief Continue the current row at an absolute screen x.
        ///
        /// SameLine(offset) is measured from the window origin without the active indent, so
        /// passing a value taken from GetCursorPosX() inside an indented child window snaps the
        /// following items back to the left. Nudging from the current cursor is space agnostic.
        void sameLineAt(float screenX)
        {
            ImGui::SameLine();
            float const nudge = screenX - ImGui::GetCursorScreenPos().x;
            if (nudge != 0.f)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + nudge);
            }
        }

        /// @brief Faint rule under a table row, the way the draft separates requirement rows
        void drawRowRule(float left, float right)
        {
            ImDrawList & drawList = *ImGui::GetWindowDrawList();
            float const y = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
            drawList.AddLine(ImVec2(left, y), ImVec2(right, y),
                             ImGui::GetColorU32(ImGuiCol_Border, 0.35f), 1.f);
        }
    } // namespace

    float WorkflowArtifacts::noteArrival(std::string const & key,
                                         AgentWorkflowAnimationState & animation) const
    {
        float const now = static_cast<float>(ImGui::GetTime());
        animation.noteArtifact(key, now);
        return animation.artifactArrival(key, now);
    }

    /// @brief Horizontal layout of one requirement row, measured right to left in screen space
    WorkflowArtifacts::Columns WorkflowArtifacts::measureColumns()
    {
        ImGuiStyle const & style = ImGui::GetStyle();

        Columns columns;
        columns.rowLeft = ImGui::GetCursorScreenPos().x;
        columns.rowRight = columns.rowLeft + ImGui::GetContentRegionAvail().x;
        columns.classWidth = ImGui::CalcTextSize("Required").x + style.FramePadding.x;
        columns.trailingWidth = ImGui::CalcTextSize(ICON_FA_TIMES).x + style.FramePadding.x * 2.f;

        columns.trailingX = columns.rowRight - columns.trailingWidth;
        columns.classX = columns.trailingX - style.ItemSpacing.x * 2.f - columns.classWidth;
        columns.descriptionWidth
          = std::max(40.f, columns.classX - style.ItemSpacing.x * 2.f - columns.rowLeft);
        return columns;
    }

    bool WorkflowArtifacts::classToggle(Columns const & columns, bool mandatory)
    {
        // A plain right-aligned word, not a framed button: it reads as a value in the table.
        sameLineAt(columns.classX);
        bool const clicked
          = ImGui::InvisibleButton("##class", ImVec2(columns.classWidth, ImGui::GetTextLineHeight()))
          != false;
        char const * label = mandatory ? "Required" : "Optional";
        // A requirement class is a property of the row, not a problem: emphasis carries the
        // difference, colour would make every constraint look like a failure.
        ImVec4 color = ImGui::GetStyleColorVec4(mandatory ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        // The rect of the submitted button is in screen coordinates; the measured columns are
        // window-local, so the glyph position has to be derived from the rect, not from the column.
        ImVec2 const rectMin = ImGui::GetItemRectMin();
        float const textX = rectMin.x + std::max(0.f, columns.classWidth
                                                        - ImGui::CalcTextSize(label).x);
        ImGui::GetWindowDrawList()->AddText(ImVec2(textX, rectMin.y),
                                            ImGui::GetColorU32(color),
                                            label);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Click to make this a %s",
                              mandatory ? "preference" : "required constraint");
        }
        return clicked;
    }

    void WorkflowArtifacts::renderRequirementRow(design::Requirement const & requirement,
                                                 AgentWorkflowAnimationState & animation)
    {
        std::string const key = "req_" + requirement.id;
        float const alpha = noteArrival(key, animation);
        pushRowAlpha(alpha);
        ImGui::PushID(requirement.id.c_str());

        // Table look from the draft: the requirement fills the left, its class sits right-aligned
        // in its own column and the remove control stays on the right edge.
        Columns const columns = measureColumns();

        char descBuf[512];
        std::snprintf(descBuf, sizeof(descBuf), "%s", requirement.description.c_str());
        char catBuf[64];
        std::snprintf(catBuf, sizeof(catBuf), "%s", requirement.category.c_str());

        bool edited = false;
        edited |= flatInput("##desc", descBuf, sizeof(descBuf), columns.descriptionWidth);
        edited |= ImGui::IsItemDeactivatedAfterEdit();

        bool const mandatory = !classToggle(columns, requirement.mandatory)
                             ? requirement.mandatory
                             : !requirement.mandatory;

        sameLineAt(columns.trailingX);
        bool const removed = ImGui::SmallButton(ICON_FA_TIMES);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Remove requirement");
        }
        drawRowRule(columns.rowLeft, columns.rowRight);

        if (edited || mandatory != requirement.mandatory)
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
        sectionLabel(ICON_FA_CLIPBOARD_LIST, "Requirements");
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
            sectionLabel(ICON_FA_ADJUST, "Preferences");
            for (auto const & req : soft)
            {
                renderRequirementRow(req, animation);
            }
        }

        // The add row mirrors the requirement rows so the section reads as one table.
        Columns const columns = measureColumns();

        bool const descSubmitted
          = flatInputWithHint("##newReqDesc",
                              "Add requirement...",
                              m_newRequirementBuf,
                              sizeof(m_newRequirementBuf),
                              columns.descriptionWidth,
                              ImGuiInputTextFlags_EnterReturnsTrue);

        if (classToggle(columns, m_newRequirementMandatory))
        {
            m_newRequirementMandatory = !m_newRequirementMandatory;
        }

        sameLineAt(columns.trailingX);
        if ((ImGui::SmallButton(ICON_FA_PLUS) || descSubmitted) && m_newRequirementBuf[0] != '\0')
        {
            std::string const newId = m_graph->addRequirement(
              m_newRequirementBuf, m_newRequirementCategoryBuf, m_newRequirementMandatory);
            m_graph->recordUserAddedRequirement(newId);
            m_newRequirementBuf[0] = '\0';
        }
        drawRowRule(columns.rowLeft, columns.rowRight);
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
        using namespace agentMetrics;
        sectionLabel(ICON_FA_SITEMAP, "Design structure");
        auto const rows = designStructureRows();
        if (rows.empty())
        {
            ImGui::TextDisabled("The problem has not been decomposed yet.");
            return;
        }
        float const fontSize = ImGui::GetFontSize();
        float const lineHeight = ImGui::GetTextLineHeight();
        float const indentUnit = fontSize * 0.85f;
        ImDrawList & drawList = *ImGui::GetWindowDrawList();

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

            bool const frozen = nodeIt->second.status == design::DesignStatus::Frozen;
            bool const explored = row.id == m_exploredNodeId;
            float const nameIndent = static_cast<float>(row.depth) * indentUnit;
            agentWidgets::ReservedRow frame = agentWidgets::reserveRow(lineHeight * 2.f,
                                                                      "##nodeRow");
            frame.origin.x += nameIndent;
            frame.max.x = std::max(frame.origin.x + 1.f, frame.max.x);
            agentWidgets::drawBand(drawList, frame.origin, frame.max, explored, frame.hovered);

            // Tree ticks carry the hierarchy; a heavy tree widget would fight the rail.
            if (row.depth > 0)
            {
                ImU32 const tick = ImGui::GetColorU32(ImGuiCol_Border);
                float const tickX = frame.origin.x - indentUnit * 0.5f;
                drawList.AddLine(ImVec2(tickX, frame.origin.y - lineHeight * 0.55f),
                                 ImVec2(tickX, frame.origin.y + lineHeight * 0.55f),
                                 tick,
                                 1.f);
                drawList.AddLine(ImVec2(tickX, frame.origin.y + lineHeight * 0.45f),
                                 ImVec2(frame.origin.x - fontSize * 0.30f,
                                        frame.origin.y + lineHeight * 0.45f),
                                 tick,
                                 1.f);
            }

            drawList.AddText(ImVec2(frame.origin.x, frame.origin.y),
                             ImGui::GetColorU32(ImGuiCol_Text),
                             ICON_FA_CUBES);
            ImU32 const nameColor = explored ? ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 1.f))
                                             : ImGui::GetColorU32(ImGuiCol_Text);
            agentWidgets::drawEmphasized(drawList,
                                         ImVec2(frame.origin.x + fontSize * 1.40f,
                                                frame.origin.y),
                                         ImGui::GetStyleColorVec4(ImGuiCol_Text),
                                         row.name.c_str());
            std::string const summary = std::string(design::toString(nodeIt->second.type))
                                      + " - " + std::to_string(nodeIt->second.variantIds.size())
                                      + " alternatives" + (frozen ? ", frozen" : "");
            drawList.AddText(ImVec2(frame.origin.x + fontSize * 1.40f, frame.origin.y + lineHeight),
                             ImGui::GetColorU32(ImGuiCol_Text),
                             summary.c_str());

            char const * freezeLabel = frozen ? ICON_FA_LOCK_OPEN " Unfreeze"
                                              : ICON_FA_LOCK " Freeze";
            float const actionsWidth
              = ImGui::CalcTextSize(freezeLabel).x
              + ImGui::CalcTextSize(ICON_FA_COLUMNS " Alternatives").x
              + ImGui::GetStyle().FramePadding.x * 4.f + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorScreenPos(ImVec2(std::max(frame.origin.x,
                                                      frame.max.x - actionsWidth),
                                             frame.origin.y));
            if (ImGui::SmallButton(freezeLabel))
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
            if (ImGui::SmallButton(ICON_FA_COLUMNS " Alternatives"))
            {
                m_exploredNodeId = row.id;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Show the alternatives of this element");
            }

            agentWidgets::endReservedRow(frame);
            ImGui::PopID();
            popRowAlpha(alpha);
        }
    }

    void WorkflowArtifacts::renderVariantCard(design::DesignVariant const & variant,
                                              bool selected,
                                              AgentWorkflowAnimationState & animation,
                                              char letter)
    {
        using namespace agentMetrics;
        std::string const key = "var_" + variant.id;
        float const alpha = noteArrival(key, animation);
        pushRowAlpha(alpha);
        ImGui::PushID(variant.id.c_str());
        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        float const fontSize = ImGui::GetFontSize();
        float const lineHeight = ImGui::GetTextLineHeight();
        float const radius = fontSize * cardThumbSize * 0.5f;

        // Two text lines next to a circular preview, plus the comment field.
        float const height = std::max(cardThumbSize * fontSize, lineHeight * 2.f)
                           + ImGui::GetFrameHeight() + fontSize * cardPadding;
        agentWidgets::ReservedRow const row = agentWidgets::reserveRow(height, "##varRow");
        agentWidgets::drawRowBand(row, selected, row.hovered);

        int w = 0;
        int h = 0;
        unsigned int const texId =
          m_previewTexture ? m_previewTexture(key, variant.previewPng, w, h) : 0u;
        ImVec2 const thumbCenter(row.origin.x + radius + fontSize * 0.20f,
                                 row.origin.y + row.size().y * 0.42f);
        agentWidgets::drawCircularThumb(drawList, thumbCenter, radius, texId);

        float const textX = thumbCenter.x + radius + fontSize * 0.55f;
        float const statusY = row.origin.y + lineHeight * 1.05f;
        ImVec2 const statusCenter(textX + lineHeight * 0.45f, statusY + lineHeight * 0.5f);
        char const * statusText = nullptr;
        ImVec4 statusColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        switch (variant.status)
        {
        case design::DesignStatus::Rejected:
            drawList.AddCircle(statusCenter,
                               lineHeight * 0.42f,
                               ImGui::GetColorU32(colorWarn()),
                               24,
                               1.4f);
            statusText = "Rejected";
            statusColor = colorWarn();
            break;
        case design::DesignStatus::Active:
        case design::DesignStatus::Frozen:
            drawList.AddCircleFilled(statusCenter,
                                     lineHeight * 0.42f,
                                     tinted(colorOk(), 0.16f),
                                     24);
            drawList.AddText(ImVec2(statusCenter.x - lineHeight * 0.24f, statusY),
                             ImGui::GetColorU32(colorOk()),
                             ICON_FA_CHECK);
            statusText = "Preview ready";
            statusColor = colorOk();
            break;
        case design::DesignStatus::Proposed:
            drawList.AddCircle(statusCenter,
                               lineHeight * 0.42f,
                               ImGui::GetColorU32(ImGuiCol_Border),
                               24,
                               1.4f);
            statusText = "Not rendered yet";
            statusColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            break;
        }
        agentWidgets::drawEmphasized(drawList,
                                     ImVec2(textX, row.origin.y + fontSize * 0.18f),
                                     ImGui::GetStyleColorVec4(ImGuiCol_Text),
                                     variant.name.c_str());
        drawList.AddText(ImVec2(statusCenter.x + lineHeight * 0.65f, statusY),
                         ImGui::GetColorU32(statusColor),
                         statusText);

        // Letter chip: the draft labels each alternative so a comment can refer to it by letter.
        agentWidgets::drawLetterChip(drawList,
                                     ImVec2(row.max.x - lineHeight * 0.62f,
                                            row.origin.y + lineHeight * 0.62f),
                                     lineHeight * 0.52f,
                                     letter,
                                     selected ? accentColor()
                                              : ImGui::GetStyleColorVec4(ImGuiCol_Border));

        char fbBuf[256];
        std::snprintf(fbBuf, sizeof(fbBuf), "%s", variant.userFeedback.c_str());
        ImGui::SetCursorScreenPos(ImVec2(textX, row.max.y - ImGui::GetFrameHeight() - fontSize * 0.10f));
        float const commentWidth = row.max.x - textX - lineHeight * 1.9f;
        ImGui::SetNextItemWidth(std::max(40.f, commentWidth));
        bool const feedbackEdited
          = ImGui::InputTextWithHint("##varFb", "Comment...", fbBuf, sizeof(fbBuf)) != false;
        ImGui::SameLine();
        bool const rejected = ImGui::SmallButton(ICON_FA_TIMES) != false;
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Reject this alternative");
        }
        if (feedbackEdited)
        {
            m_graph->setVariantFeedback(variant.id, fbBuf);
        }
        if (rejected && variant.status != design::DesignStatus::Rejected)
        {
            m_graph->rejectVariant(variant.id, "Rejected in the Design Agent panel");
        }

        if (row.hovered)
        {
            ImGui::SetTooltip("%s\nFidelity: %s\nStatus: %s%s%s",
                              variant.name.c_str(),
                              design::toString(variant.fidelity),
                              design::toString(variant.status),
                              variant.rationale.empty() ? "" : "\n\n",
                              variant.rationale.c_str());
        }

        agentWidgets::endReservedRow(row);
        ImGui::Spacing();
        ImGui::PopID();
        popRowAlpha(alpha);
    }

    void WorkflowArtifacts::renderConfigurationRow(design::Configuration const & configuration,
                                                   AgentWorkflowAnimationState & animation)
    {
        using namespace agentMetrics;
        std::string const key = "cfg_" + configuration.id;
        float const alpha = noteArrival(key, animation);
        pushRowAlpha(alpha);
        ImGui::PushID(configuration.id.c_str());
        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        float const fontSize = ImGui::GetFontSize();
        float const lineHeight = ImGui::GetTextLineHeight();

        float const height = std::max(fontSize * 1.8f, lineHeight)
                           + ImGui::GetFrameHeight() + fontSize * cardPadding * 0.5f;
        agentWidgets::ReservedRow const row = agentWidgets::reserveRow(height, "##cfgRow");
        agentWidgets::drawRowBand(row, configuration.active, row.hovered);

        int w = 0;
        int h = 0;
        unsigned int const texId = m_previewTexture
                                     ? m_previewTexture(key, configuration.previewPng, w, h)
                                     : 0u;
        float const radius = fontSize * 0.75f;
        ImVec2 const thumbCenter(row.origin.x + radius, row.origin.y + lineHeight * 0.60f);
        agentWidgets::drawCircularThumb(drawList, thumbCenter, radius, texId);

        float const textX = thumbCenter.x + radius + fontSize * 0.45f;
        std::string const title = configuration.name
                                + (configuration.active ? "  (active)" : "");
        drawList.AddText(ImVec2(textX, row.origin.y + fontSize * 0.10f),
                         ImGui::GetColorU32(ImGuiCol_Text),
                         title.c_str());
        if (row.hovered)
        {
            ImGui::SetTooltip("%s (%s)\nAlternatives selected: %zu%s",
                              configuration.name.c_str(),
                              configuration.id.c_str(),
                              configuration.selectedVariants.size(),
                              configuration.evaluation.deterministicWarnings.empty()
                                ? ""
                                : "\nHas measurement warnings");
        }

        auto selIt = std::find(selectedRecombineConfigs.begin(),
                               selectedRecombineConfigs.end(),
                               configuration.id);
        bool selectedForRecombine = selIt != selectedRecombineConfigs.end();

        char fbBuf[256];
        std::snprintf(fbBuf, sizeof(fbBuf), "%s", configuration.userFeedback.c_str());
        ImGui::SetCursorScreenPos(ImVec2(textX, row.max.y - ImGui::GetFrameHeight()));
        float const actionsWidth = ImGui::CalcTextSize(ICON_FA_EXCHANGE_ALT " Swap").x
                                 + ImGui::CalcTextSize(ICON_FA_EYE " Inspect").x
                                 + ImGui::CalcTextSize("x").x * 2.f
                                 + ImGui::GetStyle().FramePadding.x * 4.f
                                 + ImGui::GetStyle().ItemSpacing.x * 2.f;
        ImGui::SetNextItemWidth(std::max(40.f, row.max.x - textX - actionsWidth));
        bool const edited
          = ImGui::InputTextWithHint("##cfgFb", "Comment...", fbBuf, sizeof(fbBuf)) != false;
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_EXCHANGE_ALT))
        {
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Use this configuration as the recombination source");
            }
            selectedForRecombine = !selectedForRecombine;
            if (selectedForRecombine)
            {
                selectedRecombineConfigs.push_back(configuration.id);
            }
            else if (selIt != selectedRecombineConfigs.end())
            {
                selectedRecombineConfigs.erase(selIt);
            }
        }
        if (selectedForRecombine)
        {
            ImDrawList & dl = *ImGui::GetWindowDrawList();
            dl.AddCircleFilled(ImVec2(ImGui::GetItemRectMax().x - fontSize * 0.10f,
                                      ImGui::GetItemRectMin().y + fontSize * 0.10f),
                               fontSize * 0.12f,
                               ImGui::GetColorU32(accentColor()));
        }
        ImGui::SameLine();
        if (!configuration.active && ImGui::SmallButton(ICON_FA_BOLT))
        {
            m_graph->activateConfiguration(configuration.id);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Make this the active configuration");
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_EYE))
        {
            m_exploredNodeId.clear();
            for (auto const & [nodeId, variantId] : configuration.selectedVariants)
            {
                (void) variantId;
                m_exploredNodeId = nodeId;
                break;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Compare the alternatives of this configuration");
            }
        }
        if (edited)
        {
            m_graph->setConfigurationFeedback(configuration.id, fbBuf);
        }

        agentWidgets::endReservedRow(row);
        ImGui::PopID();
        popRowAlpha(alpha);
    }

    void WorkflowArtifacts::renderCheckpointRow(tools::CheckpointSummary const & checkpoint,
                                                AgentWorkflowAnimationState & animation)
    {
        using namespace agentMetrics;
        std::string const key = "cp_" + checkpoint.id;
        float const alpha = noteArrival(key, animation);
        pushRowAlpha(alpha);
        ImGui::PushID(checkpoint.id.c_str());
        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        float const fontSize = ImGui::GetFontSize();
        float const lineHeight = ImGui::GetTextLineHeight();

        float const height = std::max(fontSize * 1.7f, lineHeight)
                           + ImGui::GetFrameHeight() + fontSize * cardPadding * 0.5f;
        agentWidgets::ReservedRow const row = agentWidgets::reserveRow(height, "##cpRow");
        agentWidgets::drawRowBand(row, false, row.hovered);

        int w = 0;
        int h = 0;
        unsigned int const texId = m_previewTexture
                                     ? m_previewTexture(key, checkpoint.previewPng, w, h)
                                     : 0u;
        float const radius = fontSize * 0.68f;
        ImVec2 const thumbCenter(row.origin.x + radius, row.origin.y + row.size().y * 0.40f);
        agentWidgets::drawCircularThumb(drawList, thumbCenter, radius, texId);

        float const textX = thumbCenter.x + radius + fontSize * 0.45f;
        drawList.AddText(ImVec2(textX, row.origin.y + fontSize * 0.08f),
                         ImGui::GetColorU32(ImGuiCol_Text),
                         checkpoint.label.c_str());
        std::string const meta = std::to_string(checkpoint.functionCount)
                               + " functions - level " + std::to_string(checkpoint.level);
        drawList.AddText(ImVec2(textX, row.origin.y + lineHeight * 1.02f),
                         ImGui::GetColorU32(ImGuiCol_Text),
                         meta.c_str());

        char cpFb[256];
        std::snprintf(cpFb, sizeof(cpFb), "%s", checkpoint.userFeedback.c_str());
        ImGui::SetCursorScreenPos(ImVec2(textX, row.max.y - ImGui::GetFrameHeight()));
        float const restoreWidth = ImGui::CalcTextSize(ICON_FA_HISTORY " Restore").x
                                 + ImGui::GetStyle().FramePadding.x * 2.f;
        float const forkWidth = ImGui::CalcTextSize(ICON_FA_CODE_BRANCH " Fork").x
                              + ImGui::GetStyle().FramePadding.x * 2.f;
        ImGui::SetNextItemWidth(std::max(40.f, row.max.x - textX - restoreWidth - forkWidth
                                                     - ImGui::GetStyle().ItemSpacing.x * 2.f));
        bool const edited = ImGui::InputTextWithHint("##cpFb",
                                                     "Snapshot comment...",
                                                     cpFb,
                                                     sizeof(cpFb))
                            != false;
        ImGui::SameLine();
        bool const doRestore = ImGui::SmallButton(ICON_FA_HISTORY " Restore");
        ImGui::SameLine();
        bool const doFork = ImGui::SmallButton(ICON_FA_CODE_BRANCH " Fork");
        if (doRestore || doFork)
        {
            bool restored = false;
            if (m_document != nullptr && m_document->getAssembly())
            {
                // Token: replacing the assembly races with UI-thread graph traversal and
                // background workers. Released before update3mfModel(), which locks itself.
                auto const assemblyToken = m_document->waitForAssemblyToken();
                restored = m_checkpoints->restoreCheckpoint(checkpoint.id,
                                                            *m_document->getAssembly());
            }
            if (restored)
            {
                m_document->update3mfModel();
                m_document->rebuildResourceDependencyGraph();
                m_document->markFileAsChanged();
                if (m_onDocumentModified)
                {
                    m_onDocumentModified();
                }
            }
            if (doFork)
            {
                std::unordered_map<std::string, std::string> baseSelections;
                if (auto const active = m_graph->activeConfiguration())
                {
                    baseSelections = active->selectedVariants;
                }
                std::string const newCfgId = m_graph->createConfiguration(
                  "Fork: " + checkpoint.label, std::move(baseSelections));
                (void) m_graph->activateConfiguration(newCfgId);
            }
        }
        if (edited)
        {
            m_checkpoints->setFeedback(checkpoint.id, cpFb);
        }

        agentWidgets::endReservedRow(row);
        ImGui::Spacing();
        ImGui::PopID();
        popRowAlpha(alpha);
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

        if (explored->second.variantIds.size() >= 2
            && ImGui::BeginTable("##variantCompare", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Option");
            ImGui::TableSetupColumn("Fidelity");
            ImGui::TableSetupColumn("Score");
            ImGui::TableSetupColumn("Checks");
            ImGui::TableHeadersRow();
            std::size_t idx = 0;
            for (auto const & vid : explored->second.variantIds)
            {
                auto const vit = m_graph->variants().find(vid);
                if (vit == m_graph->variants().end())
                {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char const letter = static_cast<char>('A' + static_cast<int>(idx % 26u));
                ++idx;
                ImGui::Text("%c: %s", letter, vit->second.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(design::toString(vit->second.fidelity));
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", vit->second.evaluation.pruningScore);
                ImGui::TableNextColumn();
                ImGui::Text("%zu ok / %zu warn",
                            vit->second.evaluation.deterministicPassed.size(),
                            vit->second.evaluation.deterministicWarnings.size());
            }
            ImGui::EndTable();
            ImGui::Spacing();
        }

        auto const activeConfig = m_graph->activeConfiguration();
        std::size_t cardIndex = 0;
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
            char const letter = static_cast<char>('A' + static_cast<int>(cardIndex % 26u));
            ++cardIndex;
            renderVariantCard(vit->second, selected, animation, letter);
        }

        if (!m_graph->configurations().empty())
        {
            ImGui::Spacing();
            sectionLabel(ICON_FA_CUBES, "System configurations");
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
            sectionLabel(ICON_FA_HISTORY, "Iteration snapshots");
            for (auto const & cp : checkpoints)
            {
                renderCheckpointRow(cp, animation);
            }
        }

        if (!m_graph->referenceImages().empty())
        {
            ImGui::Spacing();
            renderReferenceImages(animation);
        }
    }

    void WorkflowArtifacts::renderReferenceImages(AgentWorkflowAnimationState & animation)
    {
        sectionLabel(ICON_FA_IMAGE, "Reference images");
        auto const refs = m_graph->referenceImages();
        for (auto const & ref : refs)
        {
            std::string const key = "ref_" + ref.id;
            float const alpha = noteArrival(key, animation);
            pushRowAlpha(alpha);
            ImGui::PushID(ref.id.c_str());

            int w = 0;
            int h = 0;
            unsigned int const texId =
              m_previewTexture ? m_previewTexture(key, ref.pngBytes, w, h) : 0u;
            if (texId != 0)
            {
                ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(texId)),
                             ImVec2(48.f, 48.f));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::TextWrapped("%s", ref.caption.empty() ? ref.prompt.c_str() : ref.caption.c_str());
            bool const isComparing = (m_comparedReferenceId == ref.id);
            if (ImGui::SmallButton(isComparing ? "Hide Compare" : "Compare with Viewport"))
            {
                m_comparedReferenceId = isComparing ? "" : ref.id;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_TIMES " Remove"))
            {
                if (m_comparedReferenceId == ref.id)
                {
                    m_comparedReferenceId.clear();
                }
                (void) m_graph->removeReferenceImage(ref.id);
                ImGui::EndGroup();
                ImGui::PopID();
                popRowAlpha(alpha);
                break;
            }
            ImGui::EndGroup();
            ImGui::PopID();
            popRowAlpha(alpha);
        }

        if (!m_comparedReferenceId.empty())
        {
            if (auto const * ref = m_graph->findReferenceImage(m_comparedReferenceId))
            {
                ImGui::Spacing();
                if (ImGui::BeginTable("##refVsViewport", 2, ImGuiTableFlags_BordersInnerV))
                {
                    ImGui::TableSetupColumn("Reference Concept");
                    ImGui::TableSetupColumn("Active Viewport / Config");
                    ImGui::TableHeadersRow();
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    int rw = 0;
                    int rh = 0;
                    unsigned int const refTex =
                      m_previewTexture ? m_previewTexture("ref_" + ref->id, ref->pngBytes, rw, rh)
                                       : 0u;
                    if (refTex != 0)
                    {
                        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(refTex)),
                                     ImVec2(140.f, 140.f));
                    }
                    else
                    {
                        ImGui::TextDisabled("No reference preview");
                    }

                    ImGui::TableNextColumn();
                    unsigned int vpTex = 0u;
                    if (auto const active = m_graph->activeConfiguration();
                        active.has_value() && !active->previewPng.empty() && m_previewTexture)
                    {
                        int vw = 0;
                        int vh = 0;
                        vpTex = m_previewTexture("cfg_" + active->id, active->previewPng, vw, vh);
                    }
                    if (vpTex != 0)
                    {
                        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(vpTex)),
                                     ImVec2(140.f, 140.f));
                    }
                    else
                    {
                        ImGui::TextDisabled("Capture viewport or activate a configuration to compare");
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    void WorkflowArtifacts::renderDevelop(AgentWorkflowAnimationState & animation)
    {
        sectionLabel(ICON_FA_TASKS, "Development tasks");
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
                todoColor = colorOk();
                break;
            case design::TodoStatus::InProgress:
                marker = "◉";
                break;
            case design::TodoStatus::Blocked:
                marker = "!";
                todoColor = colorWarn();
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
        sectionLabel(ICON_FA_CHECK_CIRCLE, "Measured results");
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
                ImGui::TextColored(colorOk(), "✓ %s", pass.c_str());
            }
            for (auto const & warn : active->evaluation.deterministicWarnings)
            {
                ImGui::TextColored(colorWarn(), "! %s", warn.c_str());
            }
        }

        ImGui::Spacing();
        sectionLabel(ICON_FA_COMMENT_ALT, "Agent assessment");
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
