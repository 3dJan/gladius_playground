#pragma once

#include "compute/ComputeBackend.h"
#include "compute/RenderContracts.h"

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
        float timeSeconds{};
        float sliceHeight{};
        float quality{1.0f};
        float normalOffset{0.001f};
        std::uint32_t renderingFlags{};
        std::uint32_t renderingMode{};
        /// x: reference thickness mm, y: interior step size, z: max interior steps, w: transmittance floor.
        std::array<float, 4> translucencyParams{};
        /// Surfaces shaded per ray while translucency is active; zero selects the shader default.
        std::uint32_t translucencyMaxSurfaces{};
        std::optional<RenderBounds> modelBounds{};
        /// Upper bound on the error of `modelBounds`; consumed by conservative
        /// frame-shader optimizations such as background-ray skipping.
        std::optional<float> boundsErrorBound{};
        /// Request per-submission GPU timing (queue work-done and, where supported,
        /// timestamp queries). Off by default; benchmark harnesses enable it.
        bool collectGpuTiming{false};
        /// Request ray-march step counters from the shader (requires the shader to be
        /// composed with the metrics module). Off by default; never enable together with
        /// timing measurements because the counters perturb what they measure.
        bool collectMetrics{false};
        /// When true, skip staging-buffer CPU readback and return a GPU output handle in
        /// `FrameResult::gpuOutputHandle` for zero-copy GPU texture presentation.
        bool skipCpuReadback{false};
        /// Compute workgroup tile shape used for the frame dispatch. The shader source must
        /// declare the same workgroup size, otherwise the dispatch would not cover the frame.
        compute::WorkgroupShape workgroupShape{};
        std::string shaderSource;
        std::vector<float> parameterValues;
        /// Mesh payloads indexed by mesh resource id (empty slots = no mesh).
        /// Only used when the shader source was composed with mesh support.
        std::vector<std::vector<float>> meshPayloadTable;
        /// Beam lattice payloads indexed by beam resource id (empty slots = none).
        /// Only used when the shader source was composed with beam support.
        std::vector<std::vector<float>> beamPayloadTable;
        /// Word-aligned image stack payloads indexed by image resource id (empty slots = none).
        /// Only used when the shader source was composed with image support.
        std::vector<std::vector<std::uint32_t>> imagePayloadTable;
    };

    /**
     * @brief Packed RGBA8 output from a completed headless frame render.
     */
    struct FrameResult
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint32_t> pixels;
        std::shared_ptr<void> gpuOutputHandle;
    };

    /**
     * @brief Timing of one completed frame submission. All values are milliseconds.
     */
    struct FrameTiming
    {
        /// CPU wall time from queue submission until the GPU signalled work completion.
        double submitToWorkDoneMs{};
        /// Latency between GPU work completion and the readback callback delivering the pixels.
        double workDoneToReadbackMs{};
        /// GPU-internal compute pass duration from timestamp queries; zero unless
        /// gpuTimestampsAvailable is true.
        double gpuPassMs{};
        bool gpuTimestampsAvailable{false};
    };

    /**
     * @brief Ray-march counters collected by the shader metrics module.
     *
     * Layout mirrors the OpenCL RayMarchMetrics counters so both backends are comparable.
     */
    struct FrameMetrics
    {
        std::uint32_t totalRays{};
        std::uint32_t totalSteps{};
        std::uint32_t totalHits{};
        std::uint32_t nonConverged{};
        std::uint32_t refinementTaps{};
        std::uint32_t shadowTaps{};
        std::uint32_t aoTaps{};
        std::uint32_t backtracks{};
        std::uint32_t interiorSteps{};
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

        /// @brief Advance backend callbacks without blocking the calling thread.
        virtual void progress() noexcept
        {
        }

        [[nodiscard]] virtual ComputeCompletionStatus getStatus() const noexcept = 0;
        virtual void wait() = 0;
        [[nodiscard]] virtual std::optional<FrameResult> takeResult() = 0;
        [[nodiscard]] virtual std::string getErrorMessage() const = 0;

        /// @brief GPU timing of the completed submission; only valid after Succeeded.
        [[nodiscard]] virtual std::optional<FrameTiming> getFrameTiming() const
        {
            return std::nullopt;
        }

        /// @brief Ray-march counters; only valid when FrameRequest::collectMetrics was set.
        [[nodiscard]] virtual std::optional<FrameMetrics> getFrameMetrics() const
        {
            return std::nullopt;
        }
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
