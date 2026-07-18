#pragma once

#include "compute/ComputeBackend.h"

#include <array>
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
     * @brief Owned camera and model data for a headless implicit-model frame render.
     */
    struct FrameRequest
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t firstRow{};
        std::uint32_t endRow{};
        std::array<float, 3> eyePosition{};
        std::array<float, 3> forwardDirection{0.0f, 0.0f, -1.0f};
        std::array<float, 3> rightDirection{1.0f, 0.0f, 0.0f};
        std::array<float, 3> upDirection{0.0f, 1.0f, 0.0f};
        float horizontalScale{0.5f};
        float verticalScale{0.5f};
        std::uint32_t maxRaySteps{2000u};
        float maxTravelDistance{100000.0f};
        std::string shaderSource;
        std::vector<float> parameterValues;
    };

    /**
     * @brief Packed RGBA8 output from a completed headless frame render.
     */
    struct FrameResult
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
         * @brief Represents a headless frame render and owns its asynchronous completion state.
         */
        class IFrameSubmission
        {
            public:
                virtual ~IFrameSubmission() = default;

                [[nodiscard]] virtual ComputeCompletionStatus getStatus() const noexcept = 0;
                virtual void wait() = 0;
                [[nodiscard]] virtual std::optional<FrameResult> takeResult() = 0;
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
        [[nodiscard]] virtual std::unique_ptr<IFrameSubmission> submitFrame(FrameRequest request) = 0;
    };
}
