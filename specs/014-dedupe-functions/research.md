# Research: Functional Equality Comparison Algorithm

**Feature**: 014-dedupe-functions  
**Date**: 2026-01-24

## Overview

This document captures research findings for implementing function deduplication in Gladius. The core challenge is comparing two `Model` instances (implicit function graphs) for mathematical equivalence while ignoring decorational metadata.

## Decision 1: Graph Comparison Algorithm

### Problem

Two `Model` instances may be mathematically identical but have:
- Different node IDs (auto-generated)
- Different node names (user-defined or auto-generated)
- Different display names
- Different function names
- Nodes in different creation order

### Options Evaluated

1. **Direct Serialization Comparison**
   - Serialize both models to a canonical string and compare
   - Rejected: Requires complex normalization, fragile to format changes

2. **Hash-Based Comparison**
   - Compute a structural hash for each model
   - Selected for grouping candidates (fast O(1) equality check for non-equal functions)

3. **Topological Graph Isomorphism**
   - Traverse both graphs in topological order and compare node-by-node
   - Selected for definitive comparison (handles hash collisions)

### Decision

**Two-phase approach**: Hash for candidate grouping + topological comparison for verification.

**Rationale**:
- Hash provides O(1) rejection of non-equal functions (most comparisons)
- Topological traversal provides definitive equality (handles hash collisions)
- Existing `getOutputOrder()` provides topological ordering

## Decision 2: Canonical Node Representation

### Problem

How to compare individual nodes while ignoring IDs and names?

### Decision

Compare nodes by:
1. **Node Type**: Use `typeid()` or existing `getTypeName()` 
2. **Input Connectivity**: Port indices connected to which relative node position in topological order
3. **Constant Values**: For `Constant`, `ConstVec`, `ConstMat` nodes, compare values with epsilon
4. **Resource References**: For `FunctionCall`, `Resource` nodes, compare the `ResourceId` value

**Ignored**:
- `NodeId`, `PortId`, `ParameterId`
- `getUniqueName()`, `getDisplayName()`
- Function `getModelName()`, `getDisplayName()`

**Rationale**: These properties affect evaluation output; metadata does not.

## Decision 3: Floating-Point Comparison Tolerance

### Problem

How to compare float constants in `Constant`, `ConstVec`, `ConstMat` nodes?

### Decision

Use relative epsilon `1e-6` with absolute fallback for near-zero values:

```cpp
bool floatsEqual(double a, double b, double epsilon = 1e-6)
{
    double const diff = std::abs(a - b);
    double const largest = std::max(std::abs(a), std::abs(b));
    return diff <= largest * epsilon || diff < 1e-12; // absolute fallback
}
```

**Rationale**: 
- Relative comparison handles wide value ranges
- Absolute fallback handles near-zero comparisons
- `1e-6` matches typical single-precision tolerance

## Decision 4: Handling FunctionCall References

### Problem

`FunctionCall` nodes reference other functions by `ResourceId`. Should we:
1. Compare `ResourceId` values directly?
2. Recursively compare the referenced functions?

### Decision

**Compare `ResourceId` values directly** (Option 1).

**Rationale**:
- Recursive comparison would create circular dependency issues
- If two `FunctionCall` nodes reference different functions (even if those functions happen to be duplicates), the containing functions should still be considered different
- Deduplication runs on the assembly level, so if inner functions are duplicates, they will be deduplicated first, then outer functions will naturally become equal

## Decision 5: Reference Update Strategy

### Problem

When removing a duplicate function, what references need updating?

### References to Update

1. **FunctionCall nodes**: Traverse all models, find `FunctionCall` nodes referencing removed function
2. **FunctionGradient nodes**: Same pattern as FunctionCall
3. **Level Sets**: Update via Lib3MF API (`levelSet->SetFunctionResourceID()`)
4. **VolumeData/Color**: Update function references in mesh objects

### Decision

Create `ReferenceUpdater` helper that:
1. Collects all references before removal
2. Updates all references atomically
3. Integrates with `History` for undo support

**Rationale**: Centralizes reference management, ensures consistency.

## Decision 6: Selection of Retained Function

### Problem

When N functions are identical, which one to keep?

### Decision

Retain function with (in priority order):
1. **Has external references** (level sets, VolumeData, mesh object color/property functions)
2. **Highest internal reference count** (FunctionCall/FunctionGradient nodes)
3. **Tie-breaker**: Lowest `ResourceId` (deterministic, preserves older functions)

**Rationale**: 
- External references are harder to update and more visible to users (affect mesh output)
- Internal references are easier to update automatically
- Deterministic tie-breaker ensures reproducible behavior

## Decision 7: Undo Integration

### Problem

How to make deduplication undoable?

### Decision

Use existing `History::storeState()` mechanism:

```cpp
void deduplicateFunctions(Assembly& assembly, History& history)
{
    history.storeState(assembly, "Remove duplicate functions");
    // ... perform deduplication ...
}
```

**Rationale**: Existing undo infrastructure stores complete `Assembly` snapshots.

## Decision 8: UI Integration

### Problem

Where to place the deduplicate button?

### Decision

Add menu item in `ModelEditor::outline()` menu bar, next to existing "Delete unused resources":

```cpp
if (ImGui::MenuItem(ICON_FA_COPY_ALT "\tRemove duplicate functions"))
{
    // trigger deduplication
}
```

**Rationale**: 
- Groups with similar cleanup operation
- Non-destructive location (menu, not direct action)
- Consistent with existing UI patterns

## Structural Hash Algorithm

### Proposed Hash Function

```cpp
size_t computeStructuralHash(Model const& model)
{
    size_t hash = 0;
    for (NodeId nodeId : model.getOutputOrder())
    {
        auto node = model.getNode(nodeId);
        // Hash node type
        hash ^= std::hash<std::string>{}(node->getTypeName()) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        
        // Hash input count
        hash ^= node->parameter().size() + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        
        // Hash constants (for Constant nodes)
        if (auto* constNode = dynamic_cast<Constant const*>(*node))
        {
            hash ^= std::hash<double>{}(constNode->getValue()) + ...;
        }
    }
    return hash;
}
```

## Test Coverage Requirements

1. **Identical functions**: Same structure, same constants → equal
2. **Different constants**: Same structure, different values → not equal
3. **Different structure**: Different node types → not equal
4. **Reordered nodes**: Same DAG, different creation order → equal
5. **Self-referencing**: Function calls itself → handled correctly
6. **Empty functions**: Begin→End only → equal if both empty
7. **Complex graphs**: Real-world functions from test files
