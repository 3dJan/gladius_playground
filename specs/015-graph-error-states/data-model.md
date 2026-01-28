# Data Model: Graph Error States

**Feature**: 015-graph-error-states  
**Date**: January 24, 2026

## Entities

### IssueType (Enum)

Categorizes validation issues for fix suggestions and filtering.

```cpp
enum class IssueType
{
    MissingConnection,    ///< Required input has no source
    TypeMismatch,         ///< Connected port type doesn't match parameter
    InvalidReference,     ///< Referenced node/port no longer exists
    CyclicDependency,     ///< Graph contains a cycle
    FunctionNotFound      ///< Referenced function doesn't exist
};
```

### IssueSeverity (Enum)

Determines whether issue blocks updates.

```cpp
enum class IssueSeverity
{
    Error,    ///< Blocks code generation and rendering
    Warning   ///< Informational, does not block (future use)
};
```

### ValidationContext (Enum)

Indicates trigger for validation, determines event logging behavior.

```cpp
enum class ValidationContext
{
    Interactive,  ///< User editing graph - no events, issues list only
    FileLoad,     ///< Loading 3MF file - emit events once
    Api           ///< API call - emit events once
};
```

### ValidationIssue (Struct)

Extended from existing `ValidationError`. Represents a single validation problem.

```cpp
struct ValidationIssue
{
    // Existing fields (from ValidationError)
    std::string message;       ///< Human-readable description
    std::string model;         ///< Model display name with ID
    std::string node;          ///< Node display name
    std::string port;          ///< Port name (if applicable)
    std::string parameter;     ///< Parameter name

    // New fields
    IssueType type;            ///< Category for fix suggestion lookup
    IssueSeverity severity;    ///< Error (blocks) or Warning (informational)
    std::string fixSuggestion; ///< Actionable fix hint for user
    ResourceId modelId;        ///< Numeric model ID for navigation
    NodeId nodeId;             ///< Numeric node ID for navigation
    
    /// Generate unique key for deduplication/comparison
    [[nodiscard]] std::string key() const;
};
```

**Validation Rules**:
- `message` MUST be non-empty
- `type` MUST be one of the defined enum values
- `severity` defaults to `Error` if not specified
- `nodeId` MAY be invalid (0) for assembly-level issues

### IssueList (Class)

Thread-safe collection of validation issues with query capabilities.

```cpp
class IssueList
{
public:
    /// Clear all issues (called before validation pass)
    void clear();
    
    /// Add an issue to the list
    void add(ValidationIssue issue);
    
    /// Get all issues (thread-safe copy)
    [[nodiscard]] std::vector<ValidationIssue> getAll() const;
    
    /// Get issues for a specific model
    [[nodiscard]] std::vector<ValidationIssue> getForModel(ResourceId modelId) const;
    
    /// Check if any blocking errors exist
    [[nodiscard]] bool hasErrors() const;
    
    /// Get count of errors
    [[nodiscard]] size_t errorCount() const;
    
    /// Get count of warnings
    [[nodiscard]] size_t warningCount() const;
    
    /// Check if list is empty
    [[nodiscard]] bool empty() const;

private:
    mutable std::mutex m_mutex;
    std::vector<ValidationIssue> m_issues;
};
```

**State Transitions**:
```
[Empty] --add()--> [HasIssues] --clear()--> [Empty]
                       |
                       +--add()--> [HasIssues]
```

## Relationships

```
Document (1) ----owns----> (1) IssueList
    |
    +----calls----> Validator
                        |
                        +----populates----> IssueList

Model (1) ----identified-by----> (*) ValidationIssue.modelId

Node (1) ----identified-by----> (*) ValidationIssue.nodeId

ValidationIssue --determines--> IssueType --maps-to--> fixSuggestion
```

## Fix Suggestion Mapping

| IssueType | Default Fix Suggestion |
|-----------|------------------------|
| `MissingConnection` | "Connect an output from another node to the '{parameter}' parameter" |
| `TypeMismatch` | "Type mismatch: expected {expected}, received {actual}. Check compatible node outputs" |
| `InvalidReference` | "The referenced node or port was deleted. Reconnect or clear the connection" |
| `CyclicDependency` | "Graph contains a cycle. Remove one connection to break the cycle" |
| `FunctionNotFound` | "The function '{functionName}' no longer exists. Delete this node or update the reference" |

## Integration Points

### Document Changes

```cpp
class Document
{
    // NEW: Issue list storage
    IssueList m_issueList;
    
    // MODIFIED: Add context parameter
    bool validateAssembly(ValidationContext context = ValidationContext::Interactive) const;
    
    // NEW: Accessor
    IssueList& getIssueList();
    IssueList const& getIssueList() const;
};
```

### Validator Changes

```cpp
class Validator
{
    // MODIFIED: Accept output parameter instead of internal storage
    bool validate(Assembly& assembly, IssueList& issues);
    
    // REMOVED: getErrors() - issues go directly to IssueList
    // REMOVED: m_errors member
};
```

### ModelEditor Changes

```cpp
class ModelEditor
{
    // NEW: Render issues overlay
    void renderIssuesOverlay();
    
    // EXISTING: Used for navigation
    void requestNodeFocus(NodeId nodeId);
};
```
