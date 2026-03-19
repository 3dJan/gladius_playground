#pragma once

#include "nodes/IssueList.h"
#include "nodesfwd.h"
#include <functional>
#include <map>

namespace gladius::ui
{
    /**
     * @brief Renders a collapsible overlay displaying validation issues.
     * 
     * Displays errors and warnings from the IssueList, grouping them by
     * (model, node, type) for smart imperative messages. Clicking an issue
     * navigates to the corresponding node.
     */
    class ValidationOverlay
    {
      public:
        /// Callback signature for navigating to a node in a specific model
        using NavigationCallback = std::function<void(nodes::NodeId, nodes::ResourceId)>;

        ValidationOverlay() = default;

        /// Set the callback invoked when user clicks an issue to navigate
        void setNavigationCallback(NavigationCallback callback);

        /**
         * @brief Render the validation overlay if there are issues.
         * @param issueList The issue list to display
         * @return true if any issue was clicked (navigation requested)
         */
        bool render(nodes::IssueList const& issueList);

      private:
        NavigationCallback m_navigationCallback;

        /// Key for grouping issues by (model, node, type)
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

        /// Build a smart imperative message for a group of issues
        static std::string buildMessage(nodes::IssueType type,
                                        std::string const& modelName,
                                        std::string const& nodeName,
                                        std::vector<nodes::ValidationIssue const*> const& issues);

        /// Render a group of issues (either todo or error style)
        bool renderIssueGroup(
            std::map<NodeTypeKey, std::vector<nodes::ValidationIssue const*>> const& groupedIssues,
            char const* childId,
            bool isTodo);
    };

} // namespace gladius::ui
