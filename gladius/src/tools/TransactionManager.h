#pragma once

#include "../nodes/Assembly.h"

#include <functional>
#include <optional>
#include <string>

namespace gladius::tools
{
    /// @brief Groups multi-step agent edits into a single atomic undoable transaction
    class TransactionManager
    {
      public:
        /// @brief Callback invoked on commit with the pre-edit snapshot and transaction description
        using CommitCallback = std::function<void(nodes::Assembly const & preEditSnapshot, std::string const & description)>;

        explicit TransactionManager(CommitCallback onCommit = nullptr);

        /// @brief Begin a new edit transaction, capturing a snapshot of the current assembly
        /// @return false if a transaction is already active
        bool beginEdit(std::string const & description, nodes::Assembly const & currentAssembly);

        /// @brief Commit the active transaction, invoking the commit callback if the assembly changed
        /// @return false if no transaction is active
        bool commitEdit(nodes::Assembly const & currentAssembly);

        /// @brief Roll back the active transaction, restoring the pre-edit assembly snapshot
        /// @return false if no transaction is active
        bool rollbackEdit(nodes::Assembly & targetAssembly);

        /// @brief Check whether a transaction is currently active
        [[nodiscard]] bool isActive() const noexcept
        {
            return m_active;
        }

        /// @brief Return the description of the active transaction, or empty string if none
        [[nodiscard]] std::string const & activeDescription() const noexcept
        {
            return m_description;
        }

      private:
        CommitCallback m_onCommit;
        bool m_active = false;
        std::string m_description;
        std::optional<nodes::Assembly> m_preEditSnapshot;
    };
} // namespace gladius::tools
