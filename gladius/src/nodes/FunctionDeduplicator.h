#pragma once

#include "Assembly.h"
#include <vector>

namespace gladius::nodes
{
    /// @brief Represents a group of functionally identical functions
    struct DuplicateGroup
    {
        std::vector<ResourceId> members; ///< All functions in this equivalence class
        ResourceId canonical;            ///< The function to retain
    };

    /// @brief Result of a deduplication operation
    struct DeduplicationResult
    {
        size_t removedCount{0};             ///< Number of functions removed
        size_t updatedReferences{0};        ///< Number of references updated
        std::vector<DuplicateGroup> groups; ///< Duplicate groups found (for reporting)
    };

    /// @brief Identifies and removes duplicate functions from an Assembly
    class FunctionDeduplicator
    {
      public:
        /// @brief Finds all groups of functionally identical functions
        /// @param assembly The assembly to analyze
        /// @return Vector of duplicate groups (each with 2+ members)
        [[nodiscard]] static std::vector<DuplicateGroup> findDuplicateGroups(
            Assembly const & assembly);

        /// @brief Removes duplicate functions and updates all references
        /// @param assembly The assembly to deduplicate (modified in place)
        /// @return Result containing counts and groups
        static DeduplicationResult deduplicate(Assembly & assembly);

        /// @brief Selects which function to retain from a duplicate group
        /// @param group The group of duplicates
        /// @param assembly The assembly (for reference counting)
        /// @return ResourceId of the function to keep
        [[nodiscard]] static ResourceId selectCanonical(
            DuplicateGroup const & group,
            Assembly const & assembly);

      private:
        /// @brief Counts internal references (FunctionCall/FunctionGradient) to a function
        [[nodiscard]] static size_t countInternalReferences(
            ResourceId functionId,
            Assembly const & assembly);

        /// @brief Updates all FunctionCall/FunctionGradient nodes referencing oldId
        static size_t updateInternalReferences(
            Assembly & assembly,
            ResourceId oldId,
            ResourceId newId);
    };
} // namespace gladius::nodes
