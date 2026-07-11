#pragma once

#include "nodesfwd.h"
#include "types.h"

#include <mutex>
#include <string>
#include <vector>

namespace gladius::nodes
{
    /**
     * @brief Categorizes validation issues for fix suggestions and filtering.
     *
     * Used by Validator to classify different types of graph validation problems.
     * Each type maps to a specific fix suggestion via getFixSuggestion().
     */
    enum class IssueType
    {
        MissingConnection,  ///< Required input has no source
        TypeMismatch,       ///< Connected port type doesn't match parameter
        InvalidReference,   ///< Referenced node/port no longer exists
        CyclicDependency,   ///< Graph contains a cycle
        FunctionNotFound,   ///< Referenced function doesn't exist
        GraphSyncError      ///< Graph sync/validation failed unexpectedly
    };

    /**
     * @brief Determines whether an issue blocks code generation and rendering.
     *
     * Error-severity issues cause Document::validateAssembly() to return false,
     * preventing OpenCL compilation and preview rendering until resolved.
     */
    enum class IssueSeverity
    {
        Error,   ///< Blocks code generation and rendering
        Warning  ///< Informational, does not block (future use)
    };

    /**
     * @brief Indicates the trigger for validation, determines event logging behavior.
     *
     * - Interactive: User editing graph in ModelEditor - silent (no events logged)
     * - FileLoad: Loading 3MF file - events logged once per issue
     * - Api: API call (MCP tools) - events logged once per issue
     */
    enum class ValidationContext
    {
        Interactive,  ///< User editing graph - no events, issues list only
        FileLoad,     ///< Loading 3MF file - emit events once
        Api           ///< API call - emit events once
    };

    /**
     * @brief Represents a single validation problem in the function graph.
     *
     * Contains human-readable details for display in UI (EventViewer overlay),
     * categorization for fix suggestions, and IDs for click-to-navigate feature.
     */
    struct ValidationIssue
    {
        std::string message;        ///< Human-readable description
        std::string model;          ///< Model display name with ID
        std::string node;           ///< Node display name
        std::string port;           ///< Port name (if applicable)
        std::string parameter;      ///< Parameter name

        IssueType type = IssueType::MissingConnection;  ///< Category for fix suggestion lookup
        IssueSeverity severity = IssueSeverity::Error;  ///< Error (blocks) or Warning (informational)
        std::string fixSuggestion;  ///< Actionable fix hint for user
        ResourceId modelId = 0;     ///< Numeric model ID for navigation
        NodeId nodeId = 0;          ///< Numeric node ID for navigation

        /**
         * @brief Generate unique key for deduplication/comparison.
         * @return String combining model, node, port, and type for uniqueness
         */
        [[nodiscard]] std::string key() const;
    };

    /**
     * @brief Thread-safe collection of validation issues with query capabilities.
     *
     * Owned by Document, populated by Validator during validateAssembly().
     * Used by ModelEditor to display collapsible issues overlay.
     *
     * @note All public methods are thread-safe via internal mutex.
     *
     * @see Document::getIssueList()
     * @see Validator::validate()
     */
    class IssueList
    {
      public:
        /// Clear all issues (called before validation pass)
        void clear();

        /// Add a validation issue to the list
        /// @param issue The issue to add
        void add(ValidationIssue issue);

        /// Get a copy of all issues (thread-safe)
        /// @return Vector of all current issues
        [[nodiscard]] std::vector<ValidationIssue> getAll() const;

        /// Get issues filtered by model ID
        /// @param modelId The resource ID of the model to filter by
        /// @return Vector of issues belonging to the specified model
        [[nodiscard]] std::vector<ValidationIssue> getForModel(ResourceId modelId) const;

        /// Check if any blocking errors exist
        /// @return true if at least one Error-severity issue exists
        [[nodiscard]] bool hasErrors() const;

        /// Count of Error-severity issues
        [[nodiscard]] size_t errorCount() const;

        /// Count of Warning-severity issues
        [[nodiscard]] size_t warningCount() const;

        /// Check if list contains no issues
        [[nodiscard]] bool empty() const;

        /// Total issue count
        [[nodiscard]] size_t size() const;

      private:
        mutable std::mutex m_mutex;
        std::vector<ValidationIssue> m_issues;
    };
} // namespace gladius::nodes
