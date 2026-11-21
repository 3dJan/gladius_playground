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

        [[nodiscard]] DesignPhase phase() const noexcept
        {
            return m_phase;
        }

        std::string addRequirement(std::string description,
                                   std::string category = "functional",
                                   bool mandatory = true);

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
        bool setVariantEvaluation(std::string const & variantId, Evaluation eval);
        bool setVariantPreview(std::string const & variantId, std::vector<std::byte> pngBytes);
        bool setConfigurationPreview(std::string const & configId, std::vector<std::byte> pngBytes);

        std::string createConfiguration(std::string name,
                                        std::unordered_map<std::string, std::string> selectedVariants);

        bool activateConfiguration(std::string const & configId);

        /// @brief Create a new configuration by merging baseConfigId with overrides from overrideConfigId
        std::optional<std::string> hybridizeConfigurations(std::string const & newName,
                                                           std::string const & baseConfigId,
                                                           std::string const & overrideConfigId,
                                                           std::vector<std::string> const & overrideNodeIds);

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

        [[nodiscard]] nlohmann::json toJson() const;
        [[nodiscard]] std::string summarize() const;

        void clear();

      private:
        DesignPhase m_phase = DesignPhase::ClarifyRequirements;
        std::vector<Requirement> m_requirements;
        std::map<std::string, DesignNode> m_nodes;
        std::map<std::string, DesignVariant> m_variants;
        std::map<std::string, Configuration> m_configurations;
        std::size_t m_nextReqId = 1;
        std::size_t m_nextNodeId = 1;
        std::size_t m_nextVariantId = 1;
        std::size_t m_nextConfigId = 1;
    };
} // namespace gladius::design
