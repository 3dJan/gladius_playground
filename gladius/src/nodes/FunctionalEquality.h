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
