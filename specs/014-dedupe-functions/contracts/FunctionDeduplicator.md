# API Contract: FunctionDeduplicator

**Feature**: 014-dedupe-functions  
**File**: `gladius/src/nodes/FunctionDeduplicator.h`

## Interface

```cpp
#pragma once

#include "Assembly.h"
#include "History.h"
#include <vector>

namespace gladius::nodes
{
    /// @brief Represents a group of functionally identical functions
    struct DuplicateGroup
    {
        std::vector<ResourceId> members;  ///< All functions in this equivalence class
        ResourceId canonical;             ///< The function to retain
    };

    /// @brief Result of a deduplication operation
    struct DeduplicationResult
    {
        size_t removedCount{0};           ///< Number of functions removed
        size_t updatedReferences{0};      ///< Number of references updated
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
        /// @param history History object for undo support (state stored before modification)
        /// @return Result containing counts and groups
        static DeduplicationResult deduplicate(Assembly & assembly, History & history);

        /// @brief Removes duplicate functions without undo support
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

        /// @brief Checks if function has external references (level sets, VolumeData, etc.)
        [[nodiscard]] static bool hasExternalReferences(
            ResourceId functionId,
            Document const & document);

        /// @brief Updates all FunctionCall/FunctionGradient nodes referencing oldId
        static size_t updateInternalReferences(
            Assembly & assembly,
            ResourceId oldId,
            ResourceId newId);
    };
} // namespace gladius::nodes
```

## Behavior Contract

### `findDuplicateGroups()`

**Preconditions**:
- Assembly must be valid (`assembly.isValid()`)

**Postconditions**:
- Returns only groups with 2+ members (no singletons)
- Each function appears in at most one group
- Assembly model (`getAssemblyModelId()`) never appears in groups
- Groups are deterministic for same input

**Complexity**: O(n²) where n = number of functions (hash reduces average case)

### `deduplicate()`

**Preconditions**:
- Assembly must be valid

**Postconditions**:
- All duplicate functions removed except canonical
- All references to removed functions updated to canonical
- Assembly model never removed
- With History: previous state stored for undo
- Result accurately reflects changes made

**Side Effects**:
- Modifies assembly in place
- Stores undo state in history (if provided)

**Complexity**: O(n² + r) where n = functions, r = total references

### `selectCanonical()`

**Selection Criteria** (in priority order):
1. Function with external references (level sets, VolumeData, mesh colors) — prefer keeping
2. Function with highest internal reference count (FunctionCall/FunctionGradient nodes)
3. Function with lowest ResourceId (tie-breaker for determinism)

**Rationale**: External references are user-visible and harder to update; internal references are updated automatically. Deterministic tie-breaker ensures reproducible behavior.

## Test Scenarios

| Scenario | Input | Expected |
|----------|-------|----------|
| No duplicates | 3 unique functions | `removedCount == 0` |
| Single pair | 2 identical, 1 unique | `removedCount == 1` |
| Multiple groups | 2 pairs of duplicates | `removedCount == 2`, `groups.size() == 2` |
| Reference update | FunctionCall → removed func | FunctionCall updated to canonical |
| Assembly model duplicate | Assembly model same as func_A | func_A removed, assembly kept |
| External ref prioritized | func_A (no external ref), func_B (has level set ref) | func_B kept |
| Higher ref count kept | func_A (5 internal refs), func_B (1 ref), no external refs | func_A kept |
| Undo support | Deduplicate then undo | Original state restored |
