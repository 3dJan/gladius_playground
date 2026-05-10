# Data Model: FunctionCall Node Navigation

**Date**: 2026-01-24  
**Branch**: `013-func-call-nav`

## Overview

This feature is a bug fix and does not introduce new data structures. The existing data model is sufficient.

## Existing Structures (No Changes)

### FunctionNavigationHistory

**Location**: `gladius/src/ui/FunctionNavigationHistory.h`

```cpp
class FunctionNavigationHistory
{
    std::vector<nodes::ResourceId> m_history;  // Stack of visited function IDs
    std::size_t m_index{0};                     // Current position in history
    bool m_inHistoryNav{false};                 // Flag to prevent recording during back/forward
};
```

**Purpose**: Browser-like navigation history for function browsing.

### FunctionCall Node

**Location**: `gladius/src/nodes/DerivedNodes.h`

Relevant method:
```cpp
nodes::ResourceId getFunctionId() const;  // Returns the ID of the referenced function
```

### FunctionGradient Node

**Location**: `gladius/src/nodes/LowerFunctionGradient.h`

Relevant methods:
```cpp
void resolveFunctionId();                 // Resolves function reference from parameter
nodes::ResourceId getFunctionId() const;  // Returns the resolved function ID
```

## No New Entities

This fix:
- Uses existing `FunctionNavigationHistory` for tracking
- Uses existing node type detection via `dynamic_cast`
- Uses existing `navigateToFunction()` API in ModelEditor
- Uses existing ImGui Node Editor API `ed::GetHoveredNode()`

## Validation Rules

| Rule | Source | Implementation |
|------|--------|----------------|
| Function ID must be non-zero | Existing code | Check `functionId != 0` before navigation |
| Function must exist | Existing code | `navigateToFunction()` returns false if not found |
| Cannot navigate to self | Existing code | `recordNavigation()` rejects same-function navigation |
