#pragma once

#include "../nodes/Assembly.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace gladius::tools
{
    /// @brief Lightweight metadata describing a stored checkpoint without the full Assembly
    struct CheckpointSummary
    {
        std::string id;
        std::string label;
        int level = 0;
        std::size_t functionCount = 0;
        std::chrono::system_clock::time_point createdAt;
    };

    /// @brief Manages named Assembly checkpoints for design-level branching and rollback
    class CheckpointManager
    {
      public:
        /// @brief Create a checkpoint from the given assembly and return its unique ID
        std::string createCheckpoint(std::string const & label, int level, nodes::Assembly const & assembly);

        /// @brief Restore a checkpoint into targetAssembly; returns false if ID not found
        bool restoreCheckpoint(std::string const & id, nodes::Assembly & targetAssembly) const;

        /// @brief List all stored checkpoints in creation order
        [[nodiscard]] std::vector<CheckpointSummary> listCheckpoints() const;

        /// @brief Compare a stored checkpoint against currentAssembly and return a structured diff summary
        [[nodiscard]] nlohmann::json compareCheckpoint(std::string const & id,
                                                         nodes::Assembly const & currentAssembly) const;

        /// @brief Check whether a checkpoint with the given ID exists
        [[nodiscard]] bool hasCheckpoint(std::string const & id) const;

        /// @brief Remove all stored checkpoints
        void clear();

      private:
        struct Entry
        {
            CheckpointSummary summary;
            nodes::Assembly assembly;
        };

        std::vector<Entry> m_entries;
        std::size_t m_nextId = 1;
    };
} // namespace gladius::tools
