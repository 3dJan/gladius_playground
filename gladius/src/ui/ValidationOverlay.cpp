#include "ValidationOverlay.h"

#include <algorithm>
#include <fmt/format.h>
#include <imgui.h>
#include <map>

#include "../IconFontCppHeaders/IconsFontAwesome5.h"

namespace gladius::ui
{
    void ValidationOverlay::setNavigationCallback(NavigationCallback callback)
    {
        m_navigationCallback = std::move(callback);
    }

    bool ValidationOverlay::render(nodes::IssueList const& issueList)
    {
        if (!issueList.hasErrors())
        {
            return false;
        }

        bool navigationRequested = false;
        auto const allIssues = issueList.getAll();
        size_t const errorCount = issueList.errorCount();
        size_t const warningCount = issueList.warningCount();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.3f, 0.1f, 0.1f, 0.9f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);

        std::string const headerText =
            fmt::format("{} Validation Issues ({} errors, {} warnings)",
                        reinterpret_cast<const char*>(ICON_FA_EXCLAMATION_TRIANGLE),
                        errorCount,
                        warningCount);

        if (ImGui::CollapsingHeader(headerText.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Group issues by (modelId, nodeId, type) to create smart messages
            struct NodeTypeKey
            {
                nodes::ResourceId modelId;
                nodes::NodeId nodeId;
                nodes::IssueType type;
                bool operator<(NodeTypeKey const& other) const
                {
                    if (modelId != other.modelId)
                        return modelId < other.modelId;
                    if (nodeId != other.nodeId)
                        return nodeId < other.nodeId;
                    return static_cast<int>(type) < static_cast<int>(other.type);
                }
            };

            std::map<NodeTypeKey, std::vector<nodes::ValidationIssue const*>> groupedIssues;
            for (auto const& issue : allIssues)
            {
                groupedIssues[{issue.modelId, issue.nodeId, issue.type}].push_back(&issue);
            }

            // Calculate actual content height based on item count
            float const itemHeight = ImGui::GetTextLineHeightWithSpacing();
            float const maxHeight =
                std::min(150.0f, static_cast<float>(groupedIssues.size()) * itemHeight + 8.0f);
            ImGui::BeginChild("IssuesListChild", ImVec2(0, maxHeight), false);

            int groupIndex = 0;
            for (auto const& [key, issues] : groupedIssues)
            {
                ImGui::PushID(groupIndex++);

                // Determine worst severity for color
                bool hasError = std::any_of(issues.begin(),
                                            issues.end(),
                                            [](auto const* i)
                                            { return i->severity == nodes::IssueSeverity::Error; });

                char const* icon = hasError
                                       ? reinterpret_cast<const char*>(ICON_FA_TIMES_CIRCLE)
                                       : reinterpret_cast<const char*>(ICON_FA_EXCLAMATION_CIRCLE);

                ImVec4 const color =
                    hasError ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(1.0f, 0.8f, 0.2f, 1.0f);

                std::string const& nodeName = issues.front()->node;
                std::string const& modelName = issues.front()->model;
                std::string message = buildMessage(key.type, modelName, nodeName, issues);

                bool const hasNode = (key.nodeId != 0);
                std::string buttonLabel = fmt::format("{} {}", icon, message);

                ImGui::PushStyleColor(ImGuiCol_Text, color);

                if (hasNode)
                {
                    // Clickable button for navigation
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.2f, 0.2f, 0.5f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.3f, 0.3f, 0.7f));

                    if (ImGui::Button(buttonLabel.c_str()))
                    {
                        if (m_navigationCallback)
                        {
                            m_navigationCallback(key.nodeId, key.modelId);
                        }
                        navigationRequested = true;
                    }

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted("Click to navigate to node");
                        // Show fix suggestion if available
                        if (!issues.front()->fixSuggestion.empty())
                        {
                            ImGui::Separator();
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 0.7f, 1.0f));
                            ImGui::TextWrapped("%s", issues.front()->fixSuggestion.c_str());
                            ImGui::PopStyleColor();
                        }
                        ImGui::EndTooltip();
                    }

                    ImGui::PopStyleColor(3);
                }
                else
                {
                    // Non-clickable text for issues without specific node
                    ImGui::TextUnformatted(buttonLabel.c_str());
                }

                ImGui::PopStyleColor();
                ImGui::PopID();
            }

            ImGui::EndChild();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::Separator();

        return navigationRequested;
    }

    std::string ValidationOverlay::buildMessage(
        nodes::IssueType type,
        std::string const& modelName,
        std::string const& nodeName,
        std::vector<nodes::ValidationIssue const*> const& issues)
    {
        std::string nodePrefix =
            modelName.empty() ? nodeName : fmt::format("{}.{}", modelName, nodeName);

        switch (type)
        {
        case nodes::IssueType::MissingConnection:
        {
            std::vector<std::string> params;
            for (auto const* issue : issues)
            {
                if (!issue->parameter.empty())
                    params.push_back(fmt::format("'{}'", issue->parameter));
            }
            if (params.size() == 1)
            {
                return fmt::format("{}: Connect input {}", nodePrefix, params[0]);
            }
            else if (params.size() == 2)
            {
                return fmt::format(
                    "{}: Connect inputs {} and {}", nodePrefix, params[0], params[1]);
            }
            else if (params.size() > 2)
            {
                std::string paramList;
                for (size_t i = 0; i < params.size() - 1; ++i)
                {
                    if (i > 0)
                        paramList += ", ";
                    paramList += params[i];
                }
                return fmt::format(
                    "{}: Connect inputs {}, and {}", nodePrefix, paramList, params.back());
            }
            return fmt::format("{}: Connect missing inputs", nodePrefix);
        }
        case nodes::IssueType::TypeMismatch:
        {
            std::vector<std::string> params;
            for (auto const* issue : issues)
            {
                if (!issue->parameter.empty())
                    params.push_back(fmt::format("'{}'", issue->parameter));
            }
            if (params.size() == 1)
            {
                return fmt::format("{}: Fix type mismatch on {}", nodePrefix, params[0]);
            }
            else if (params.size() >= 2)
            {
                return fmt::format(
                    "{}: Fix type mismatches on {} inputs", nodePrefix, params.size());
            }
            return fmt::format("{}: Fix type mismatch", nodePrefix);
        }
        case nodes::IssueType::InvalidReference:
            return fmt::format("{}: Remove invalid reference", nodePrefix);
        case nodes::IssueType::CyclicDependency:
            return fmt::format("{}: Break cycle in graph", nodePrefix);
        case nodes::IssueType::FunctionNotFound:
            return fmt::format("{}: Function not found", nodePrefix);
        default:
            return fmt::format("{}: {}", nodePrefix, issues.front()->message);
        }
    }

} // namespace gladius::ui
