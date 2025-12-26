#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gladius::nodes
{
    class Assembly;
}

namespace gladius
{
    /**
     * @brief Represents the structure of parameters in an Assembly for fast comparison.
     *
     * This signature allows us to detect when parameter values change without changing
     * the parameter structure (count, types, order). When only values change, we can
     * use a fast path that updates GPU buffers without recompiling OpenCL code.
     *
     * @note Thread-safe for reading after construction. Comparison operations are
     * lock-free.
     */
    struct ParameterSignature
    {
        /// Total number of floats in the flattened parameter buffer
        size_t totalFloatCount{0};

        /// Size of each parameter in floats (1 for float/int, 3 for float3, 16 for
        /// Matrix4x4)
        std::vector<size_t> parameterSizes;

        /// Fast hash for quick comparison (combined from all parameter metadata)
        uint64_t signatureHash{0};

        /**
         * @brief Compare two signatures for equality.
         *
         * Uses hash for fast path, falls back to full comparison for safety against
         * hash collisions.
         *
         * @param other The signature to compare against
         * @return true if signatures match (parameter structure is identical)
         */
        [[nodiscard]] bool matches(ParameterSignature const & other) const;

        /**
         * @brief Compute the parameter signature from an Assembly.
         *
         * Walks through all functions in the assembly and extracts parameter metadata
         * (count, types, order) to create a signature that can be compared efficiently.
         *
         * @param assembly The assembly containing the parameter structure
         * @return The computed signature
         *
         * @note This operation is relatively fast (microseconds) compared to OpenCL
         * compilation (milliseconds to seconds).
         */
        [[nodiscard]] static ParameterSignature compute(nodes::Assembly const & assembly);

        /**
         * @brief Create an empty/invalid signature.
         *
         * Used as a default before any compilation has occurred.
         */
        [[nodiscard]] static ParameterSignature empty();

        /**
         * @brief Check if this signature is valid (non-empty).
         */
        [[nodiscard]] bool isValid() const;

        /**
         * @brief Get a human-readable string representation for debugging.
         */
        [[nodiscard]] std::string toString() const;

      private:
        /**
         * @brief Compute hash from parameter sizes.
         *
         * Uses a combination of totalFloatCount and parameter structure to create
         * a hash that minimizes collisions while being fast to compute.
         */
        [[nodiscard]] static uint64_t computeHash(size_t totalFloats,
                                                  std::vector<size_t> const & sizes);
    };
}
