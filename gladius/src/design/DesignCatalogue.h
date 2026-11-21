#pragma once

#include "DesignTypes.h"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace gladius::design
{
    struct CatalogueParameter
    {
        std::string type = "length";
        double minimum = 0.0;
        double maximum = 0.0;
        double defaultValue = 0.0;
        bool hasMin = false;
        bool hasMax = false;
    };

    struct CatalogueEntry
    {
        std::string id;
        std::string name;
        std::vector<std::string> functions;
        std::map<std::string, CatalogueParameter> parameters;
        std::vector<std::string> processes;
        bool supportsRequired = false;
        std::vector<std::string> interfaces;
        std::string generator;
        std::string snippetTemplate;
    };

    /// @brief Compatibility rule eliminating invalid combinations of variant/principle IDs
    struct CompatibilityRule
    {
        std::vector<std::pair<std::string, std::string>> conditions; // (nodeNameOrId, variantNameOrId)
        bool compatible = false;
        std::string rationale;
    };

    /// @brief Machine-readable engineering design catalogue with compatibility & beam pruning
    class DesignCatalogue
    {
      public:
        void addEntry(CatalogueEntry entry);
        void addCompatibilityRule(CompatibilityRule rule);
        bool loadFromJson(nlohmann::json const & doc);

        [[nodiscard]] std::vector<CatalogueEntry> findByFunction(std::string const & functionName) const;
        [[nodiscard]] bool validateParameters(std::string const & entryId,
                                              nlohmann::json const & params,
                                              std::vector<std::string> & errors) const;

        /// @brief Check whether a set of (node, variant) selections violates any compatibility rule
        [[nodiscard]] bool checkCompatibility(std::unordered_map<std::string, std::string> const & selections,
                                              std::vector<std::string> & violations) const;

        /// @brief Prune candidate configurations to the top beamWidth compatible concepts
        [[nodiscard]] std::vector<Configuration> pruneBeam(std::vector<Configuration> const & candidates,
                                                           std::size_t beamWidth) const;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return m_entries.size();
        }

      private:
        std::map<std::string, CatalogueEntry> m_entries;
        std::vector<CompatibilityRule> m_rules;
    };
} // namespace gladius::design
