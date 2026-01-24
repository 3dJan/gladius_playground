# Quickstart: Function Deduplication

**Feature**: 014-dedupe-functions  
**Date**: 2026-01-24

## Overview

This guide provides a quick path to implementing function deduplication in Gladius.

## Implementation Order

### Step 1: FunctionalEquality (Core Algorithm)

**Files**: `FunctionalEquality.h`, `FunctionalEquality.cpp`

```cpp
// FunctionalEquality.h
namespace gladius::nodes
{
    class FunctionalEquality
    {
      public:
        [[nodiscard]] static size_t computeHash(Model const & model);
        [[nodiscard]] static bool areEqual(Model const & lhs, Model const & rhs);
    };
}
```

**Key Implementation Details**:

1. Use existing `model.getOutputOrder()` for topological traversal
2. Compare nodes in topological order to ensure consistent comparison
3. For each node pair, compare:
   - `node->getTypeName()` (must match)
   - `node->parameter()` values for constants
   - Input connectivity (which relative position each input connects to)

**Test First**:
```cpp
TEST(FunctionalEquality, IdenticalModels_AreEqual)
{
    auto model1 = createSimpleAdditionModel();
    auto model2 = createSimpleAdditionModel();
    EXPECT_TRUE(FunctionalEquality::areEqual(model1, model2));
}

TEST(FunctionalEquality, DifferentConstants_AreNotEqual)
{
    auto model1 = createConstantModel(1.0);
    auto model2 = createConstantModel(2.0);
    EXPECT_FALSE(FunctionalEquality::areEqual(model1, model2));
}
```

### Step 2: FunctionDeduplicator 

**Files**: `FunctionDeduplicator.h`, `FunctionDeduplicator.cpp`

```cpp
// Usage
auto groups = FunctionDeduplicator::findDuplicateGroups(assembly);
auto result = FunctionDeduplicator::deduplicate(assembly, history);
```

**Key Implementation Details**:

1. Group functions by hash first (O(n) grouping)
2. Within each hash bucket, verify with `areEqual()` (handles collisions)
3. For each group, select canonical via `selectCanonical()`
4. Update references then delete duplicates

**Reference Update Pattern**:
```cpp
for (auto & [resourceId, model] : assembly.getFunctions())
{
    for (auto & [nodeId, node] : *model)
    {
        if (auto* fc = dynamic_cast<FunctionCall*>(node.get()))
        {
            if (fc->getFunctionId() == oldId)
            {
                fc->setFunctionId(newId);
            }
        }
    }
}
```

### Step 3: UI Integration

**File**: `ModelEditor.cpp` (modify `outline()` method)

Add menu item after existing "Delete unused resources":

```cpp
if (ImGui::MenuItem(ICON_FA_LAYER_GROUP "\tRemove duplicate functions"))
{
    m_history.storeState(*m_assembly, "Remove duplicate functions");
    auto result = nodes::FunctionDeduplicator::deduplicate(*m_assembly);
    
    if (result.removedCount > 0)
    {
        markModelAsModified();
        m_logger->addEvent({
            fmt::format("Removed {} duplicate function(s), updated {} reference(s)",
                       result.removedCount, result.updatedReferences),
            events::Severity::Info
        });
    }
    else
    {
        m_logger->addEvent({"No duplicate functions found", events::Severity::Info});
    }
}
```

## Build & Test

```bash
# Build (use VS Code task)
# Task: "Build ALL (linux-releaseWithDebug)"

# Run tests
cd gladius/out/build/linux-releaseWithDebug/tests/unittests
./gladius_test --gtest_filter=FunctionalEquality*
./gladius_test --gtest_filter=FunctionDeduplicator*
```

## Checklist

- [ ] `FunctionalEquality::computeHash()` implemented
- [ ] `FunctionalEquality::areEqual()` implemented
- [ ] `FunctionalEquality_tests.cpp` passes
- [ ] `FunctionDeduplicator::findDuplicateGroups()` implemented
- [ ] `FunctionDeduplicator::deduplicate()` implemented
- [ ] `FunctionDeduplicator_tests.cpp` passes
- [ ] UI menu item added
- [ ] Undo works correctly
- [ ] All tests pass via "Run Gladius Tests" task
