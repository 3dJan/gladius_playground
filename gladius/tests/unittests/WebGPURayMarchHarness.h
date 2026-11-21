#pragma once

#if defined(GLADIUS_ENABLE_WEBGPU)

#include "Document.h"
#include "EventLogger.h"
#include "compute/ComputeRendererFactory.h"
#include "compute/IBoundsService.h"
#include "compute/IComputeBackend.h"
#include "compute/RenderBackendSession.h"
#include "compute/RenderContracts.h"
#include "kernel/types.h"
#include "nodes/Assembly.h"
#include "ui/OrbitalCamera.h"
#include "webgpu/WebGPUComputeContext.h"
#include "webgpu/WebGPUComputeRenderer.h"
#include "webgpu/WebGPUBoundsService.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gladius::tests::bench
{
    /**
     * @brief Workgroup tile shape requested through GLADIUS_BENCH_WORKGROUP ("8x32").
     *
     * Returns the shipped 16x16 default when the variable is unset or malformed, so
     * baselines and reference captures stay unaffected.
     */
    [[nodiscard]] inline compute::WorkgroupShape workgroupShapeFromEnvironment()
    {
        auto const * value = std::getenv("GLADIUS_BENCH_WORKGROUP");
        if (value == nullptr)
        {
            return {};
        }

        std::string const text{value};
        auto const separator = text.find('x');
        if (separator == std::string::npos || separator == 0u || separator + 1u >= text.size())
        {
            return {};
        }

        try
        {
            compute::WorkgroupShape shape{
              .width = static_cast<std::uint32_t>(std::stoul(text.substr(0u, separator))),
              .height = static_cast<std::uint32_t>(std::stoul(text.substr(separator + 1u)))};
            if (!shape.isValid())
            {
                return {};
            }
            return shape;
        }
        catch (std::exception const &)
        {
            return {};
        }
    }

    /// @brief Rendering flags requested through GLADIUS_BENCH_FLAGS (hex bitmask).
    ///
    /// Returns zero when the variable is unset or malformed, so baselines and
    /// reference captures stay unaffected.
    [[nodiscard]] inline std::uint32_t renderFlagsFromEnvironment()
    {
        auto const * value = std::getenv("GLADIUS_BENCH_FLAGS");
        if (value == nullptr)
        {
            return 0u;
        }

        try
        {
            return static_cast<std::uint32_t>(std::stoul(std::string{value}, nullptr, 16));
        }
        catch (std::exception const &)
        {
            return 0u;
        }
    }

    /// @brief High-quality settings matching the application's neutral render path.
    inline compute::RenderSettingsSnapshot createHqSettings()
    {
        return {.flags = RF_SHOW_BUILDPLATE | RF_SHOW_COORDINATE_SYSTEM | RF_ENABLE_TRANSLUCENCY |
                         renderFlagsFromEnvironment(),
                .workgroupShape = workgroupShapeFromEnvironment(),
                .normalOffset = 0.0001f,
                .maxRaySteps = 2000u,
                .maxTravelDistance = 100000.0f};
    }

    /// @brief Low-quality settings matching the LQ gate (preview-like path).
    ///
    /// Preview-path accelerators are allowed to change LQ output, so experiments
    /// targeting the fast path must be measured here rather than on the HQ path.
    inline compute::RenderSettingsSnapshot createLqSettings()
    {
        auto settings = createHqSettings();
        settings.flags |= RF_FAST_SHADOW | RF_FAST_AO | RF_COARSE_REFINEMENT;
        settings.maxRaySteps = 300u;
        settings.normalOffset = 0.0005f;
        settings.quality = 0.5f;
        settings.translucencyInteriorStepSize = 4.0f;
        settings.translucencyMaxInteriorSteps = 512u;
        settings.translucencyMaxSurfaces = 2u;
        return settings;
    }

    /// @brief Timing of one progressive chunk, split into the parts we optimize separately.
    struct ProgressiveChunkStat
    {
        std::uint32_t firstRow{};
        std::uint32_t endRow{};
        /// End-to-end latency of the chunk as the application would experience it.
        double wallMs{};
        /// CPU time spent building and submitting the chunk (buffer setup, bind group, submit).
        double submitCallMs{};
        /// CPU time spent pumping the completion callbacks until the pixels arrived.
        double pumpMs{};
        /// Simulated UI frame time spent before the next chunk could be submitted.
        double paceMs{};
        /// GPU queue time from submission until the queue signalled work completion.
        double submitToWorkDoneMs{};
        /// GPU-internal compute pass duration; zero when timestamp queries are unavailable.
        double gpuPassMs{};
        /// Latency between GPU completion and the readback delivering the pixels.
        double readbackMs{};

        /// @brief GPU-side latency that is not compute-pass time (queue and readback delivery).
        [[nodiscard]] double gpuSideOverheadMs() const noexcept
        {
            return gpuPassMs > 0.0 ? submitToWorkDoneMs - gpuPassMs + readbackMs : submitToWorkDoneMs;
        }

        /// @brief Everything that is not GPU compute: CPU submit setup plus completion pumping.
        [[nodiscard]] double fixedOverheadMs() const noexcept
        {
            return submitCallMs + pumpMs + gpuSideOverheadMs();
        }
    };

    /// @brief Result of filling one frame with progressive chunks.
    struct ProgressiveRun
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::size_t chunkPixels{};
        std::uint32_t chunks{};
        /// Wall time from the start of the sequence until the first chunk was delivered.
        double timeToFirstPixelMs{};
        /// Wall time from the start of the sequence until the last chunk completed the frame.
        double timeToHqMs{};
        std::vector<ProgressiveChunkStat> chunkStats;
    };

    /// @brief A model materialized through the authoritative application render path.
    class BenchScene
    {
      public:
        BenchScene(std::shared_ptr<webgpu::WebGPUComputeContext> context,
                   std::unique_ptr<Document> document,
                   std::unique_ptr<compute::RenderBackendSession> session,
                   compute::RenderBounds modelBounds,
                   std::optional<float> boundsErrorBound)
            : m_context{std::move(context)}
            , m_document{std::move(document)}
            , m_session{std::move(session)}
            , m_modelBounds{modelBounds}
            , m_boundsErrorBound{boundsErrorBound}
        {
        }

        [[nodiscard]] compute::RenderBounds const & getModelBounds() const noexcept
        {
            return m_modelBounds;
        }

        [[nodiscard]] compute::RenderBackendSession & getSession() noexcept
        {
            return *m_session;
        }

        [[nodiscard]] std::uint64_t getSceneGeneration() const noexcept
        {
            return m_session->getSceneGeneration();
        }

        [[nodiscard]] Document & getDocument() noexcept
        {
            return *m_document;
        }

        /// @brief Renders one full frame, framing the model exactly like the application does.
        [[nodiscard]] compute::RenderFrame renderFrame(std::uint32_t const width,
                                                       std::uint32_t const height,
                                                       compute::RenderSettingsSnapshot settings,
                                                       bool const collectGpuTiming,
                                                       bool const collectMetrics)
        {
            auto submission = submitRowRange(width,
                                             height,
                                             0u,
                                             height,
                                             settings,
                                             collectGpuTiming,
                                             collectMetrics);
            return takeValidFrame(*submission);
        }

        /**
         * @brief Fills a frame in row-band chunks the way the progressive scheduler does.
         *
         * Chunks are submitted back to back, one at a time, which mirrors the application
         * constraint of a single in-flight display submission. The measured quantities are the
         * ones the optimization targets: how long the sequence takes to reach full quality and
         * how much of each chunk is GPU work versus fixed per-submission overhead.
         *
         * @param framePaceMs Simulated UI frame period. The application starts at most one
         *                    display submission per frame, so a chunk that finishes early still
         *                    waits for the next frame before its successor is submitted. Zero
         *                    measures the best case with no UI pacing at all.
         */
        [[nodiscard]] ProgressiveRun renderProgressive(std::uint32_t const width,
                                                       std::uint32_t const height,
                                                       compute::RenderSettingsSnapshot settings,
                                                       std::size_t const chunkPixels,
                                                       double const framePaceMs = 0.0)
        {
            if (chunkPixels == 0u)
            {
                throw std::invalid_argument("Progressive chunk budget must be greater than zero");
            }

            ProgressiveRun run{.width = width, .height = height, .chunkPixels = chunkPixels};
            auto const rowsPerChunk = std::max<std::size_t>(1u, chunkPixels / std::max<std::size_t>(1u, width));
            auto const diagnostic = std::getenv("GLADIUS_PROGRESSIVE_BENCH_DIAGNOSTIC") != nullptr;
            auto const sequenceStart = std::chrono::steady_clock::now();
            std::uint32_t nextRow = 0u;
            while (nextRow < height)
            {
                auto const endRow = std::min<std::uint32_t>(height,
                                                            nextRow + static_cast<std::uint32_t>(rowsPerChunk));
                auto const chunkStart = std::chrono::steady_clock::now();
                auto submission = submitRowRange(width, height, nextRow, endRow, settings, true, diagnostic);
                auto const submittedAt = std::chrono::steady_clock::now();
                auto frame = takeValidFrame(*submission);
                auto const chunkEnd = std::chrono::steady_clock::now();

                if (frame.pixels.size() != static_cast<std::size_t>(width) * (endRow - nextRow))
                {
                    throw std::runtime_error("Progressive chunk returned an unexpected pixel count");
                }

                ProgressiveChunkStat stat{
                  .firstRow = nextRow,
                  .endRow = endRow,
                  .wallMs = std::chrono::duration<double, std::milli>(chunkEnd - chunkStart).count(),
                  .submitCallMs =
                    std::chrono::duration<double, std::milli>(submittedAt - chunkStart).count(),
                  .pumpMs = std::chrono::duration<double, std::milli>(chunkEnd - submittedAt).count()};
                if (auto const timing = getLastTiming(); timing.has_value())
                {
                    stat.submitToWorkDoneMs = timing->submitToWorkDoneMs;
                    stat.readbackMs = timing->workDoneToReadbackMs;
                    if (timing->gpuTimestampsAvailable)
                    {
                        stat.gpuPassMs = timing->gpuPassMs;
                    }
                }

                if (diagnostic)
                {
                    if (auto const metrics = getLastMetrics(); metrics.has_value())
                    {
                        std::cout << "chunk metrics rows [" << nextRow << ".." << endRow
                                  << ") rays=" << metrics->totalRays << " steps=" << metrics->totalSteps
                                  << " hits=" << metrics->totalHits << " refinement=" << metrics->refinementTaps
                                  << " shadow=" << metrics->shadowTaps << " ao=" << metrics->aoTaps << "\n";
                    }
                }

                if (run.chunkStats.empty())
                {
                    run.timeToFirstPixelMs = std::chrono::duration<double, std::milli>(chunkEnd - sequenceStart)
                                              .count();
                }
                run.chunkStats.push_back(stat);
                run.chunks++;
                nextRow = endRow;

                if (framePaceMs > 0.0 && nextRow < height)
                {
                    // The application starts at most one display submission per UI frame, so a
                    // chunk that finishes early still waits for the next frame before its
                    // successor is submitted.
                    auto const frameEnd =
                        chunkStart + std::chrono::microseconds(static_cast<long long>(framePaceMs * 1000.0));
                    if (auto const now = std::chrono::steady_clock::now(); now < frameEnd)
                    {
                        std::this_thread::sleep_until(frameEnd);
                        run.chunkStats.back().paceMs =
                            std::chrono::duration<double, std::milli>(frameEnd - now).count();
                    }
                }
            }

            run.timeToHqMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - sequenceStart)
                               .count();
            if (std::getenv("GLADIUS_PROGRESSIVE_BENCH_DIAGNOSTIC") != nullptr)
            {
                // Re-submit the final band to tell a genuinely expensive band apart from a
                // one-off cost that happens to land on the last submission.
                auto const & last = run.chunkStats.back();
                auto const retryStart = std::chrono::steady_clock::now();
                auto submission = submitRowRange(width, height, last.firstRow, last.endRow, settings, true, true);
                auto const retrySubmittedAt = std::chrono::steady_clock::now();
                auto frame = takeValidFrame(*submission);
                (void)frame;
                ProgressiveChunkStat retry{
                  .firstRow = last.firstRow,
                  .endRow = last.endRow,
                  .wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - retryStart)
                              .count(),
                  .submitCallMs =
                    std::chrono::duration<double, std::milli>(retrySubmittedAt - retryStart).count(),
                  .pumpMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - retrySubmittedAt)
                      .count()};
                if (auto const timing = getLastTiming(); timing.has_value())
                {
                    retry.submitToWorkDoneMs = timing->submitToWorkDoneMs;
                    retry.readbackMs = timing->workDoneToReadbackMs;
                    if (timing->gpuTimestampsAvailable)
                    {
                        retry.gpuPassMs = timing->gpuPassMs;
                    }
                }
                std::cout << "retry-last rows [" << retry.firstRow << ".." << retry.endRow
                          << ") wallMs=" << retry.wallMs << " gpuMs=" << retry.gpuPassMs
                          << " workDoneMs=" << retry.submitToWorkDoneMs
                          << " fixedMs=" << retry.fixedOverheadMs() << "\n";
            }
            if (std::getenv("GLADIUS_PROGRESSIVE_BENCH_VERBOSE") != nullptr)
            {
                for (auto const & chunk : run.chunkStats)
                {
                    std::cout << "chunk rows [" << chunk.firstRow << ".." << chunk.endRow
                              << ") wallMs=" << chunk.wallMs << " gpuMs=" << chunk.gpuPassMs
                              << " submitCallMs=" << chunk.submitCallMs
                              << " pumpMs=" << chunk.pumpMs
                              << " paceMs=" << chunk.paceMs
                              << " workDoneMs=" << chunk.submitToWorkDoneMs
                              << " readbackMs=" << chunk.readbackMs
                              << " fixedMs=" << chunk.fixedOverheadMs() << "\n";
                }
                std::cout << "sequence chunks=" << run.chunks << " firstPixelMs=" << run.timeToFirstPixelMs
                          << " hqMs=" << run.timeToHqMs << "\n";
            }
            return run;
        }

        /// @brief Submits one row band exactly like a progressive chunk would.
        [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
        submitRowRange(std::uint32_t const width,
                       std::uint32_t const height,
                       std::uint32_t const firstRow,
                       std::uint32_t const endRow,
                       compute::RenderSettingsSnapshot settings,
                       bool const collectGpuTiming,
                       bool const collectMetrics)
        {
            auto const request = buildRequest(width,
                                              height,
                                              firstRow,
                                              endRow,
                                              settings,
                                              collectGpuTiming,
                                              collectMetrics);
            return m_session->submitFrame(request);
        }

        /// @brief Waits for a submitted band and returns its validated frame.
        [[nodiscard]] compute::RenderFrame takeValidFrame(compute::IRenderSubmission & submission)
        {
            submission.wait();
            if (submission.getStatus() != compute::RenderSubmissionStatus::Succeeded)
            {
                throw std::runtime_error("Benchmark frame failed: " + submission.getErrorMessage());
            }

            auto frame = submission.takeFrame();
            if (!frame.has_value() || !frame->isValid())
            {
                throw std::runtime_error("Benchmark frame returned an invalid frame");
            }

            m_lastTiming = submission.getFrameTiming();
            m_lastMetrics = submission.getFrameMetrics();
            return *frame;
        }

        /// @brief Builds a camera-framed request for one row band of the given viewport.
        [[nodiscard]] compute::RenderRequest buildRequest(std::uint32_t const width,
                                                          std::uint32_t const height,
                                                          std::uint32_t const firstRow,
                                                          std::uint32_t const endRow,
                                                          compute::RenderSettingsSnapshot settings,
                                                          bool const collectGpuTiming,
                                                          bool const collectMetrics) const
        {
            if (width == 0u || height == 0u)
            {
                throw std::invalid_argument("Benchmark frame dimensions must be greater than zero");
            }
            if (firstRow >= endRow || endRow > height)
            {
                throw std::invalid_argument("Benchmark row range must lie inside the viewport");
            }

            BoundingBox const cameraBounds{{m_modelBounds.min[0], m_modelBounds.min[1], m_modelBounds.min[2], 0.0f},
                                           {m_modelBounds.max[0], m_modelBounds.max[1], m_modelBounds.max[2], 0.0f}};
            ui::OrbitalCamera orbitalCamera;
            orbitalCamera.setAngle(0.6f, -2.0f);
            orbitalCamera.setLookAt({(m_modelBounds.min[0] + m_modelBounds.max[0]) * 0.5f,
                                     (m_modelBounds.min[1] + m_modelBounds.max[1]) * 0.5f,
                                     (m_modelBounds.min[2] + m_modelBounds.max[2]) * 0.5f});
            orbitalCamera.snapToTarget();
            orbitalCamera.adjustDistanceToTarget(cameraBounds,
                                                 static_cast<float>(width),
                                                 static_cast<float>(height));
            orbitalCamera.snapToTarget();

            auto const eye = orbitalCamera.getEyePosition();
            auto const matrix = orbitalCamera.computeModelViewPerspectiveMatrix();
            return compute::RenderRequest{
              .camera = {.eyePosition = {eye.x, eye.y, eye.z},
                         .forwardDirection = {matrix.s8, matrix.s9, matrix.sa},
                         .rightDirection = {matrix.s0, matrix.s1, matrix.s2},
                         .upDirection = {matrix.s4, matrix.s5, matrix.s6}},
              .frustum = {.horizontalScale = 0.5f,
                          .verticalScale = 0.5f * static_cast<float>(height) / static_cast<float>(width)},
              .settings = settings,
              .modelBounds = m_modelBounds,
              .boundsErrorBound = m_boundsErrorBound,
              .viewport = {.width = width,
                           .height = height,
                           .firstRow = firstRow,
                           .endRow = endRow},
              .freshness = {.sceneGeneration = m_session->getSceneGeneration()},
              .collectGpuTiming = collectGpuTiming,
              .collectMetrics = collectMetrics};
        }

        [[nodiscard]] std::optional<compute::FrameTiming> getLastTiming() const noexcept
        {
            return m_lastTiming;
        }

        [[nodiscard]] std::optional<compute::FrameMetrics> getLastMetrics() const noexcept
        {
            return m_lastMetrics;
        }

      private:
        std::shared_ptr<webgpu::WebGPUComputeContext> m_context;
        std::unique_ptr<Document> m_document;
        std::unique_ptr<compute::RenderBackendSession> m_session;
        compute::RenderBounds m_modelBounds;
        std::optional<float> m_boundsErrorBound{};
        std::optional<compute::FrameTiming> m_lastTiming{};
        std::optional<compute::FrameMetrics> m_lastMetrics{};
    };

    /// @brief Loads a model and materializes it exactly like the application's WebGPU window does.
    [[nodiscard]] inline std::unique_ptr<BenchScene> createScene(
      std::filesystem::path const & modelPath,
      std::shared_ptr<webgpu::WebGPUComputeContext> context)
    {
        if (!context || !context->isValid())
        {
            throw std::runtime_error("WebGPU benchmark context is unavailable: " +
                                     (context ? context->getErrorMessage() : "no context"));
        }

        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        auto document = std::make_unique<Document>(logger);
        document->load(modelPath);
        // Document::load only rebuilds the flattened graph; authored values must be copied in
        // explicitly, otherwise every runtime parameter would evaluate as zero.
        document->updateFlatAssemblyParameterValues();

        auto assembly = document->getAssembly();
        auto flatAssembly = document->getFlatAssembly();
        if (assembly == nullptr || assembly->assemblyModel() == nullptr)
        {
            throw std::runtime_error("Benchmark model has no assembly: " + modelPath.string());
        }

        auto const generation = std::max<std::uint64_t>(document->structuralEditEpoch(), 1u);
        std::string materializationError;
        auto snapshot = flatAssembly != nullptr && flatAssembly->assemblyModel()
                          ? compute::ComputeRendererFactory::materializeRuntimeFlatScene(
                              *flatAssembly, generation, &document->getResourceManager(), &materializationError)
                          : compute::ComputeRendererFactory::materializeRuntimeScene(
                              *assembly, generation, &document->getResourceManager(), &materializationError);
        if (!snapshot.isValid() && flatAssembly != nullptr)
        {
            snapshot = compute::ComputeRendererFactory::materializeRuntimeScene(
              *assembly, generation, &document->getResourceManager(), &materializationError);
        }
        if (!snapshot.isValid())
        {
            throw std::runtime_error("Unable to materialize benchmark scene: " + materializationError);
        }

        auto session = std::make_unique<compute::RenderBackendSession>(
          std::make_unique<webgpu::WebGPUComputeRenderer>(context),
          std::make_shared<webgpu::WebGPUBoundsService>(context));
        {
            webgpu::WebGPUComputeContext::DeviceLock const deviceLock(*context);
            if (!session->replaceScene(std::move(snapshot), generation))
            {
                throw std::runtime_error("Unable to replace benchmark scene: " + session->getErrorMessage());
            }
            if (!session->updateParameterValues(*assembly, generation))
            {
                throw std::runtime_error("Unable to pack benchmark parameter values: " +
                                         session->getErrorMessage());
            }
        }

        auto * const boundsService = session->getBoundsService();
        compute::RenderBounds modelBounds{.min = {0.0f, 0.0f, 0.0f}, .max = {400.0f, 400.0f, 400.0f}};
        std::optional<float> boundsErrorBound{};
        if (boundsService != nullptr && boundsService->isAvailable())
        {
            auto boundsRequest = compute::BoundsRequest{};
            boundsRequest.freshness.sceneGeneration = session->getSceneGeneration();
            boundsRequest.probeSettings.resolution = {64u, 64u, 64u};
            boundsRequest.probeSettings.tileSize = {32u, 32u, 32u};
            auto submission = boundsService->submit(std::move(boundsRequest));
            submission->wait();
            auto result = submission->takeResult();
            if (result.has_value() && result->isUsable())
            {
                modelBounds = *result->modelBounds;
                boundsErrorBound = result->errorBound;
            }
        }

        return std::make_unique<BenchScene>(std::move(context), std::move(document), std::move(session),
                                            modelBounds, boundsErrorBound);
    }
}

#endif
