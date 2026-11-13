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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace gladius::tests::bench
{
    /// @brief High-quality settings matching the application's neutral render path.
    inline compute::RenderSettingsSnapshot createHqSettings()
    {
        return {.flags = RF_SHOW_BUILDPLATE | RF_SHOW_COORDINATE_SYSTEM | RF_ENABLE_TRANSLUCENCY,
                .normalOffset = 0.0001f,
                .maxRaySteps = 2000u,
                .maxTravelDistance = 100000.0f};
    }

    /// @brief A model materialized through the authoritative application render path.
    class BenchScene
    {
      public:
        BenchScene(std::shared_ptr<webgpu::WebGPUComputeContext> context,
                   std::unique_ptr<Document> document,
                   std::unique_ptr<compute::RenderBackendSession> session,
                   compute::RenderBounds modelBounds)
            : m_context{std::move(context)}
            , m_document{std::move(document)}
            , m_session{std::move(session)}
            , m_modelBounds{modelBounds}
        {
        }

        [[nodiscard]] compute::RenderBounds const & getModelBounds() const noexcept
        {
            return m_modelBounds;
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
            if (width == 0u || height == 0u)
            {
                throw std::invalid_argument("Benchmark frame dimensions must be greater than zero");
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
            auto const request = compute::RenderRequest{
              .camera = {.eyePosition = {eye.x, eye.y, eye.z},
                         .forwardDirection = {matrix.s8, matrix.s9, matrix.sa},
                         .rightDirection = {matrix.s0, matrix.s1, matrix.s2},
                         .upDirection = {matrix.s4, matrix.s5, matrix.s6}},
              .frustum = {.horizontalScale = 0.5f,
                          .verticalScale = 0.5f * static_cast<float>(height) / static_cast<float>(width)},
              .settings = settings,
              .modelBounds = m_modelBounds,
              .viewport = {.width = width,
                           .height = height,
                           .firstRow = 0u,
                           .endRow = height},
              .freshness = {.sceneGeneration = m_session->getSceneGeneration()},
              .collectGpuTiming = collectGpuTiming,
              .collectMetrics = collectMetrics};

            auto submission = m_session->submitFrame(request);
            submission->wait();
            if (submission->getStatus() != compute::RenderSubmissionStatus::Succeeded)
            {
                throw std::runtime_error("Benchmark frame failed: " + submission->getErrorMessage());
            }

            auto frame = submission->takeFrame();
            if (!frame.has_value() || !frame->isValid())
            {
                throw std::runtime_error("Benchmark frame returned an invalid frame");
            }
            m_lastTiming = submission->getFrameTiming();
            m_lastMetrics = submission->getFrameMetrics();
            return *frame;
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
            }
        }

        return std::make_unique<BenchScene>(std::move(context), std::move(document), std::move(session),
                                            modelBounds);
    }
}

#endif
