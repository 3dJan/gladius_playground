#pragma once

#include "nodesfwd.h"

#include <cstddef>

namespace gladius::nodes
{
    class Model;

    /**
     * @brief Summary statistics produced by GraphOptimizer passes on a Model.
     */
    struct GraphOptimizationStats
    {
        std::size_t initialNodeCount{0u};
        std::size_t finalNodeCount{0u};
        std::size_t peepholeRewrites{0u};
        std::size_t deduplicatedNodes{0u};
        std::size_t prunedDeadNodes{0u};
    };

    /**
     * @brief Native C++ DAG optimizer for Gladius node graphs.
     *
     * Performs safe, bit-exact (or float-preserving) graph cleanup without external
     * CAS dependencies:
     *  - Lightweight algebraic peephole rewrites (e.g. x+0 -> x, x*1 -> x, pow(x,2) -> x*x)
     *  - Hash-consing Common Subexpression Elimination (CSE) with canonicalization of
     *    commutative binary ops (Addition, Multiplication, Min, Max, DotProduct)
     *  - Reverse-BFS Dead Code Elimination (DCE) from the End node's wired inputs
     *
     * Modifiable user parameters are never merged or folded away.
     */
    class GraphOptimizer
    {
      public:
        static GraphOptimizationStats optimize(Model & model);
        static std::size_t applyPeepholeRewrites(Model & model);
        static std::size_t eliminateCommonSubexpressions(Model & model);
        static std::size_t eliminateDeadCode(Model & model);
    };
}
