#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace gladius::compute
{
    /**
     * @brief Physical API used to execute Gladius compute workloads.
     */
    enum class ComputeBackendKind
    {
        OpenCL,
        WebGPU
    };

    /**
     * @brief Converts a backend kind to its stable configuration value.
     */
    [[nodiscard]] std::string_view toString(ComputeBackendKind backend) noexcept;

    /**
     * @brief Parses a stable configuration value into a backend kind.
     */
    [[nodiscard]] std::optional<ComputeBackendKind> parseComputeBackend(std::string_view value) noexcept;

    /**
     * @brief Returns whether this binary contains the requested backend.
     */
    [[nodiscard]] bool isComputeBackendBuilt(ComputeBackendKind backend) noexcept;
}