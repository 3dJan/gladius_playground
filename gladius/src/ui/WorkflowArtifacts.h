#pragma once

#include "AgentWorkflow.h"

#include "../design/DesignGraph.h"
#include "../design/TodoStore.h"
#include "../tools/CheckpointManager.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace gladius
{
    class Document;
}

namespace gladius::ui
{
    class AgentWorkflowAnimationState;

    /**
     * @brief Draws the artifacts that live under each workflow phase.
     * @details Everything is read straight from the authoritative design state (graph, todos,
     *          checkpoints). Nothing is synthesised: no thumbnails are invented, no evaluation is
     *          claimed unless a tool actually recorded it, and measured results are kept visibly
     *          apart from agent assertions.
     */
    class WorkflowArtifacts
    {
      public:
        /// @brief Textures are owned by the host; this resolves a cache key to a GPU texture id
        using PreviewTextureResolver =
          std::function<unsigned int(std::string const & key,
                                     std::vector<std::byte> const & pngBytes,
                                     int & outWidth,
                                     int & outHeight)>;

        void bind(design::DesignGraph & graph,
                  design::TodoStore & todos,
                  tools::CheckpointManager & checkpoints,
                  Document * document,
                  std::function<void()> onDocumentModified,
                  PreviewTextureResolver previewTexture) noexcept
        {
            m_graph = &graph;
            m_todos = &todos;
            m_checkpoints = &checkpoints;
            m_document = document;
            m_onDocumentModified = std::move(onDocumentModified);
            m_previewTexture = std::move(previewTexture);
        }

        /// @brief Requirements under Understand, separated by hard/soft and marked as unverified
        void renderUnderstand(AgentWorkflowAnimationState & animation);

        /// @brief Functional and component structure under Decompose
        void renderDecompose(AgentWorkflowAnimationState & animation);

        /// @brief Alternatives and configurations under Explore, with real previews only
        void renderExplore(AgentWorkflowAnimationState & animation);

        /// @brief Planning tasks and the element being developed under Develop
        void renderDevelop(AgentWorkflowAnimationState & animation);

        /// @brief Measured checks versus agent assertions under Validate
        void renderValidate(AgentWorkflowAnimationState & animation);

        /// @brief Configurations the user checked for recombination
        std::vector<std::string> selectedRecombineConfigs;

        /// @brief Name of the tool the agent is currently running, empty when idle
        std::string activeToolName;

      private:
        struct NodeRow
        {
            std::string id;
            std::string name;
            std::size_t depth = 0;
        };

        void collectNodeRows(std::string const & nodeId,
                             std::size_t depth,
                             std::vector<NodeRow> & out) const;

        [[nodiscard]] std::vector<NodeRow> designStructureRows() const;

        void renderRequirementRow(design::Requirement const & requirement,
                                  AgentWorkflowAnimationState & animation);

        void renderVariantCard(design::DesignVariant const & variant,
                               bool selected,
                               AgentWorkflowAnimationState & animation);

        void renderConfigurationRow(design::Configuration const & configuration,
                                    AgentWorkflowAnimationState & animation);

        void renderCheckpointRow(tools::CheckpointSummary const & checkpoint,
                                 AgentWorkflowAnimationState & animation);

        /// @brief Fades a row in when it first appears; returns the row's alpha
        float noteArrival(std::string const & key, AgentWorkflowAnimationState & animation) const;

        design::DesignGraph * m_graph = nullptr;
        design::TodoStore * m_todos = nullptr;
        tools::CheckpointManager * m_checkpoints = nullptr;
        Document * m_document = nullptr;
        std::function<void()> m_onDocumentModified;
        PreviewTextureResolver m_previewTexture;

        char m_newRequirementBuf[256] = "";
        char m_newRequirementCategoryBuf[64] = "functional";
        bool m_newRequirementMandatory = true;
        std::string m_exploredNodeId;
    };
} // namespace gladius::ui
