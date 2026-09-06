#include "webgpu/WebGPUThumbnailRenderer.h"

#include "compute/ComputeRendererFactory.h"
#include "compute/IBoundsService.h"
#include "compute/RenderBackendSession.h"
#include "kernel/types.h"
#include "nodes/Assembly.h"
#include "ui/OrbitalCamera.h"

#include <algorithm>
#include <lodepng.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        constexpr std::uint32_t BOUNDS_RESOLUTION = 64u;
        constexpr std::uint32_t BOUNDS_TILE_SIZE = 32u;

        [[nodiscard]] compute::RenderBounds determineBounds(compute::RenderBackendSession & session,
                                                             std::uint64_t const sceneGeneration)
        {
            compute::RenderBounds fallback{.min = {0.0f, 0.0f, 0.0f},
                                           .max = {400.0f, 400.0f, 400.0f}};
            auto * const boundsService = session.getBoundsService();
            if (boundsService == nullptr || !boundsService->isAvailable())
            {
                return fallback;
            }

            compute::BoundsRequest request;
            request.freshness.sceneGeneration = sceneGeneration;
            request.probeSettings.resolution = {BOUNDS_RESOLUTION,
                                                 BOUNDS_RESOLUTION,
                                                 BOUNDS_RESOLUTION};
            request.probeSettings.tileSize = {BOUNDS_TILE_SIZE, BOUNDS_TILE_SIZE, BOUNDS_TILE_SIZE};

            auto submission = boundsService->submit(std::move(request));
            submission->wait();
            auto result = submission->takeResult();
            if (result.has_value() && result->isUsable())
            {
                return *result->modelBounds;
            }
            return fallback;
        }

        [[nodiscard]] compute::RenderRequest createRequest(compute::RenderBounds const & bounds,
                                                            std::uint32_t const size,
                                                            std::uint64_t const sceneGeneration)
        {
            BoundingBox const cameraBounds{{bounds.min[0], bounds.min[1], bounds.min[2], 0.0f},
                                           {bounds.max[0], bounds.max[1], bounds.max[2], 0.0f}};
            ui::OrbitalCamera camera;
            camera.setAngle(0.6f, -2.0f);
            camera.setLookAt({(bounds.min[0] + bounds.max[0]) * 0.5f,
                              (bounds.min[1] + bounds.max[1]) * 0.5f,
                              (bounds.min[2] + bounds.max[2]) * 0.5f});
            camera.snapToTarget();
            camera.adjustDistanceToTarget(cameraBounds,
                                          static_cast<float>(size),
                                          static_cast<float>(size));
            camera.snapToTarget();

            auto const eye = camera.getEyePosition();
            auto const matrix = camera.computeModelViewPerspectiveMatrix();
            return {.camera = {.eyePosition = {eye.x, eye.y, eye.z},
                               .forwardDirection = {matrix.s8, matrix.s9, matrix.sa},
                               .rightDirection = {matrix.s0, matrix.s1, matrix.s2},
                               .upDirection = {matrix.s4, matrix.s5, matrix.s6}},
                    .frustum = {.horizontalScale = 0.5f, .verticalScale = 0.5f},
                    .settings = {},
                    .modelBounds = bounds,
                    .viewport = {.width = size, .height = size, .firstRow = 0u, .endRow = size},
                    .freshness = {.sceneGeneration = sceneGeneration}};
        }
    }

    PlainImage WebGPUThumbnailRenderer::renderPng(compute::RenderBackendSession & session,
                                                  nodes::Assembly & assembly,
                                                  ResourceManager const & resourceManager,
                                                  std::uint32_t const size,
                                                  std::uint64_t const sceneGeneration)
    {
        if (size == 0u)
        {
            throw std::invalid_argument("WebGPU thumbnail size must be greater than zero");
        }
        if (!assembly.assemblyModel())
        {
            throw std::runtime_error("WebGPU thumbnail requires an assembly model");
        }

        auto snapshot = compute::ComputeRendererFactory::materializeScene(
          &assembly,
          *assembly.assemblyModel(),
          std::max<std::uint64_t>(sceneGeneration, 1u),
          &resourceManager);
        if (!session.replaceScene(snapshot))
        {
            throw std::runtime_error("Unable to materialize WebGPU thumbnail scene: " +
                                     session.getErrorMessage());
        }

        auto const generation = session.getSceneGeneration();
        auto const bounds = determineBounds(session, generation);
        auto submission = session.submitFrame(createRequest(bounds, size, generation));
        submission->wait();
        if (submission->getStatus() != compute::RenderSubmissionStatus::Succeeded)
        {
            throw std::runtime_error("WebGPU thumbnail render failed: " +
                                     submission->getErrorMessage());
        }

        auto frame = submission->takeFrame();
        if (!frame.has_value() || !frame->isValid() || frame->firstRow != 0u ||
            frame->endRow != size)
        {
            throw std::runtime_error("WebGPU thumbnail render returned an invalid frame");
        }

        std::vector<unsigned char> rgba;
        rgba.reserve(frame->pixels.size() * 4u);
        for (auto const pixel : frame->pixels)
        {
            rgba.push_back(static_cast<unsigned char>(pixel & 0xffu));
            rgba.push_back(static_cast<unsigned char>((pixel >> 8u) & 0xffu));
            rgba.push_back(static_cast<unsigned char>((pixel >> 16u) & 0xffu));
            rgba.push_back(static_cast<unsigned char>((pixel >> 24u) & 0xffu));
        }

        PlainImage image;
        image.width = size;
        image.height = size;
        auto const error = lodepng::encode(image.data, rgba, size, size);
        if (error != 0u)
        {
            throw std::runtime_error("Unable to encode WebGPU thumbnail PNG: " +
                                     std::string{lodepng_error_text(error)});
        }
        return image;
    }
}