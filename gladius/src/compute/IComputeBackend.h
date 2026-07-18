#pragma once

#include "compute/ComputeBackend.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gladius::compute
{
    /**
     * @brief The lifecycle state of an asynchronously submitted compute operation.
     */
    enum class ComputeCompletionStatus
    {
        Pending,
        Succeeded,
        Failed
    };

    /**
     * @brief Owned input data for a surfaceless two-dimensional implicit-model slice.
     */
    struct SliceRequest
    {
        std::uint32_t width{};
        std::uint32_t height{};
        float sliceZ{};
        float scale{1.0f};
        std::string shaderSource;
        std::vector<float> parameterValues;
    };

    /**
     * @brief Packed RGBA8 output from a completed two-dimensional slice operation.
     */
    struct SliceResult
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint32_t> pixels;
    };

    /**
     * @brief Represents a slice operation and owns its asynchronous completion state.
     */
    class ISliceSubmission
    {
      public:
        virtual ~ISliceSubmission() = default;

        [[nodiscard]] virtual ComputeCompletionStatus getStatus() const noexcept = 0;
        virtual void wait() = 0;
        [[nodiscard]] virtual std::optional<SliceResult> takeResult() = 0;
        [[nodiscard]] virtual std::string getErrorMessage() const = 0;
    };

    /**
     * @brief Physical compute backend interface for API-independent compute operations.
     */
    class IComputeBackend
    {
      public:
        virtual ~IComputeBackend() = default;

        [[nodiscard]] virtual ComputeBackendKind getKind() const noexcept = 0;
        [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
        [[nodiscard]] virtual std::unique_ptr<ISliceSubmission> submitSlice(SliceRequest request) = 0;
    };
}
