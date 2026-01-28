# API Contract: FunctionalEquality

**Feature**: 014-dedupe-functions  
**File**: `gladius/src/nodes/FunctionalEquality.h`

## Interface

```cpp
#pragma once

#include "Model.h"
#include <cstddef>

namespace gladius::nodes
{
    /// @brief Computes structural equality between Model instances
    /// @details Two models are considered functionally equal if they would produce
    /// the same output for any given input. This ignores decorational properties
    /// like node names, IDs, and display names.
    class FunctionalEquality
    {
      public:
        /// @brief Computes a structural hash of a model for fast inequality detection
        /// @param model The model to hash
        /// @return Hash value; equal models will have equal hashes (but not vice versa)
        [[nodiscard]] static size_t computeHash(Model const & model);

        /// @brief Compares two models for functional equality
        /// @param lhs First model to compare
        /// @param rhs Second model to compare
        /// @return true if models are functionally equivalent
        [[nodiscard]] static bool areEqual(Model const & lhs, Model const & rhs);

        /// @brief Sets the epsilon for floating-point comparisons
        /// @param epsilon Relative tolerance (default: 1e-6)
        static void setEpsilon(double epsilon);

        /// @brief Gets the current epsilon for floating-point comparisons
        [[nodiscard]] static double getEpsilon();

      private:
        static double s_epsilon;
    };
} // namespace gladius::nodes
```

## Behavior Contract

### `computeHash()`

**Preconditions**:
- Model must be valid (`model.isValid()`)

**Postconditions**:
- Returns deterministic hash for same model structure
- `areEqual(a, b) == true` implies `computeHash(a) == computeHash(b)`
- Note: Hash collision possible; equal hashes do not guarantee equality

**Complexity**: O(n) where n = number of nodes

### `areEqual()`

**Preconditions**:
- Both models must be valid

**Postconditions**:
- Returns `true` iff models would produce identical outputs for all inputs
- Symmetric: `areEqual(a, b) == areEqual(b, a)`
- Reflexive: `areEqual(a, a) == true`
- Transitive: `areEqual(a, b) && areEqual(b, c)` implies `areEqual(a, c)`

**Complexity**: O(n) average case (after hash check), O(n²) worst case (hash collision)

## Test Scenarios

| Scenario | Input | Expected |
|----------|-------|----------|
| Identical graphs | Two copies of same model | `areEqual` returns `true` |
| Different constants | Same structure, `Constant(1.0)` vs `Constant(2.0)` | `areEqual` returns `false` |
| Different node types | `Addition` vs `Subtraction` | `areEqual` returns `false` |
| Reordered creation | Same DAG, different node creation order | `areEqual` returns `true` |
| Different names | Same structure, different `setDisplayName()` | `areEqual` returns `true` |
| Empty models | Two `Begin→End` only models | `areEqual` returns `true` |
