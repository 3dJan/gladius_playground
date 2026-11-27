#pragma once

#include "DesignTypes.h"

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace gladius::design
{
    /// @brief Hierarchical design graph storing requirements, functions, interfaces, variants, and configurations
    class DesignGraph
    {
      public:
        void setPhase(DesignPhase phase) noexcept
        {
            m_phase = phase;
        }

        /// @brief One-line summary of what is being designed; shown in the Design Agent header
        [[nodiscard]] std::string const & goal() const noexcept
        {
            return m_goal;
        }

        void setGoal(std::string goal)
        {
            m_goal = std::move(goal);
        }

        [[nodiscard]] DesignPhase phase() const noexcept
        {
            return m_phase;
        }

        std::string addRequirement(std::string description,
                                   std::string category = "functional",
                                   bool mandatory = true);

        bool updateRequirement(std::string const & id,
                               std::string description,
                               std::string category,
                               bool mandatory,
                               bool trackUserChange = true);

        bool removeRequirement(std::string const & id, bool trackUserChange = true);

        void recordUserAddedRequirement(std::string const & id);

        /// @brief Re-open a design that already reached DesignPhase::Complete so a follow-up user
        ///        request extends the requirements instead of being treated as already finished.
        /// @param userRequest New wish of the user; recorded as a pending requirement change.
        /// @return True when the design was complete and has been re-opened.
        bool reopenForFollowUpRequest(std::string const & userRequest);

        [[nodiscard]] std::vector<std::string> consumePendingRequirementChanges();
        [[nodiscard]] bool hasPendingRequirementChanges() const noexcept
        {
            return !m_pendingRequirementChanges.empty();
        }

        std::string addNode(DesignNodeType type,
                            std::string name,
                            std::string const & parentNodeId = "",
                            std::vector<std::string> addressedRequirements = {});

        std::string createVariant(std::string const & nodeId,
                                  std::string name,
                                  std::string rationale,
                                  FidelityLevel fidelity = FidelityLevel::Symbolic,
                                  nlohmann::json parameters = nlohmann::json::object());

        bool rejectVariant(std::string const & variantId, std::string const & reason = "");
        bool freezeNode(std::string const & nodeId);
        bool unfreezeNode(std::string const & nodeId);
        bool isNodeFrozen(std::string const & nodeId) const;

        bool defineInterface(std::string const & variantId, Interface iface);
        bool setVariantResource(std::string const & variantId,
                                uint32_t modelResourceId,
                                std::string functionName = "");
        bool setVariantEvaluation(std::string const & variantId, Evaluation eval);
        bool setVariantPreview(std::string const & variantId, std::vector<std::byte> pngBytes);
        bool setConfigurationPreview(std::string const & configId, std::vector<std::byte> pngBytes);
        bool setVariantFeedback(std::string const & variantId, std::string feedback);
        bool setConfigurationFeedback(std::string const & configId, std::string feedback);

        /// @brief Substitute a variant for a node in a configuration after checking frozen state and interfaces
        bool substituteVariant(std::string const & configId,
                               std::string const & nodeId,
                               std::string const & newVariantId,
                               std::string & errorOut);

        [[nodiscard]] DesignNode const * findNode(std::string const & nodeId) const;
        /// @brief Node whose children contain `nodeId`, nullptr when the node is a root
        [[nodiscard]] DesignNode const * findParentNode(std::string const & nodeId) const;
        [[nodiscard]] DesignVariant const * findVariant(std::string const & variantId) const;
        [[nodiscard]] Configuration const * findConfiguration(std::string const & configId) const;

        std::string createConfiguration(std::string name,
                                        std::unordered_map<std::string, std::string> selectedVariants);

        bool activateConfiguration(std::string const & configId);

        /// @brief Create a new configuration by merging baseConfigId with overrides from overrideConfigId
        std::optional<std::string> hybridizeConfigurations(std::string const & newName,
                                                           std::string const & baseConfigId,
                                                           std::string const & overrideConfigId,
                                                           std::vector<std::string> const & overrideNodeIds);

        /// @brief Store a generated or attached reference image and return its ID
        std::string addReferenceImage(std::string prompt,
                                      std::string caption,
                                      std::vector<std::byte> pngBytes,
                                      std::string mimeType = "image/png",
                                      std::string linkedNodeId = "",
                                      std::string linkedVariantId = "");

        bool removeReferenceImage(std::string const & id);

        [[nodiscard]] std::vector<ReferenceImage> const & referenceImages() const noexcept
        {
            return m_referenceImages;
        }

        [[nodiscard]] ReferenceImage const * findReferenceImage(std::string const & id) const;

        [[nodiscard]] std::vector<Requirement> const & requirements() const noexcept
        {
            return m_requirements;
        }

        [[nodiscard]] std::map<std::string, DesignNode> const & nodes() const noexcept
        {
            return m_nodes;
        }

        [[nodiscard]] std::map<std::string, DesignVariant> const & variants() const noexcept
        {
            return m_variants;
        }

        [[nodiscard]] std::map<std::string, Configuration> const & configurations() const noexcept
        {
            return m_configurations;
        }

        [[nodiscard]] std::optional<Configuration> activeConfiguration() const;

        /// @brief Tree lines for `nodeId` and its descendants only
        /// @details A scoped agent must not pay for the whole graph; sibling branches are cost
        ///          without signal for the component it has been given.
        [[nodiscard]] std::string describeSubtree(std::string const & nodeId) const;

        /// @brief Interface specifications of the variants of `nodeId`, one line per interface
        /// @details Empty when the node has no interfaces: nothing was specified to obey yet.
        [[nodiscard]] std::string describeInheritedContract(std::string const & nodeId) const;

        [[nodiscard]] nlohmann::json toJson() const;
        bool fromJson(nlohmann::json const & json);
        [[nodiscard]] std::string summarize() const;

        void clear();

      private:
        DesignPhase m_phase = DesignPhase::ClarifyRequirements;
        std::string m_goal;
        std::vector<Requirement> m_requirements;
        std::map<std::string, DesignNode> m_nodes;
        std::map<std::string, DesignVariant> m_variants;
        std::map<std::string, Configuration> m_configurations;
        std::vector<ReferenceImage> m_referenceImages;
        std::vector<std::string> m_pendingRequirementChanges;
        std::size_t m_nextReqId = 1;
        std::size_t m_nextNodeId = 1;
        std::size_t m_nextVariantId = 1;
        std::size_t m_nextConfigId = 1;
        std::size_t m_nextRefImageId = 1;
    };
} // namespace gladius::design
