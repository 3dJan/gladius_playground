# IssueList Internal API Contract

**Feature**: 015-graph-error-states  
**Component**: `gladius::nodes::IssueList`

## Overview

Thread-safe container for validation issues with model-specific querying.

## Class Interface

```cpp
namespace gladius::nodes
{
    class IssueList
    {
    public:
        /// Clear all issues. Called at start of validation pass.
        void clear();
        
        /// Add a validation issue to the list.
        /// @param issue The issue to add
        void add(ValidationIssue issue);
        
        /// Get a copy of all issues (thread-safe).
        /// @return Vector of all current issues
        [[nodiscard]] std::vector<ValidationIssue> getAll() const;
        
        /// Get issues filtered by model ID.
        /// @param modelId The resource ID of the model to filter by
        /// @return Vector of issues belonging to the specified model
        [[nodiscard]] std::vector<ValidationIssue> getForModel(ResourceId modelId) const;
        
        /// Check if any blocking errors exist.
        /// @return true if at least one Error-severity issue exists
        [[nodiscard]] bool hasErrors() const;
        
        /// Count of Error-severity issues.
        [[nodiscard]] size_t errorCount() const;
        
        /// Count of Warning-severity issues.
        [[nodiscard]] size_t warningCount() const;
        
        /// Check if list contains no issues.
        [[nodiscard]] bool empty() const;
        
        /// Total issue count.
        [[nodiscard]] size_t size() const;
    };
}
```

## Thread Safety

All public methods are thread-safe. Internal mutex protects all operations.

## Usage Example

```cpp
// In Document::validateAssembly()
m_issueList.clear();
validator.validate(*m_assembly, m_issueList);

if (m_issueList.hasErrors())
{
    // Don't proceed with compilation
    return false;
}

// In ModelEditor::renderIssuesOverlay()
auto issues = m_doc->getIssueList().getForModel(m_currentModel->getResourceId());
for (auto const& issue : issues)
{
    // Render issue UI
}
```
