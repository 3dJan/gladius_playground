#include "compute/OpenCLRenderRequestFactory.h"

#include "ResourceContext.h"

#include <stdexcept>

namespace gladius::compute
{
    namespace
    {
        RenderMode toRenderMode(ApproximationMode const approximation)
        {
            auto const flags = static_cast<unsigned int>(approximation);
            if ((flags & static_cast<unsigned int>(AM_USE_DISTANCE_INIT)) != 0u)
            {
                return RenderMode::DistanceInitialized;
            }
            if ((flags & static_cast<unsigned int>(AM_ONLY_PRECOMPSDF)) != 0u)
            {
                return RenderMode::PrecomputedSdf;
            }
            if ((flags & static_cast<unsigned int>(AM_HYBRID)) != 0u)
            {
                return RenderMode::Hybrid;
            }
            return RenderMode::FullModel;
        }
    }

    RenderRequest OpenCLRenderRequestFactory::create(ResourceContext & resources,
                                                      RenderViewport const viewport,
                                                      RenderFreshnessStamp const freshness)
    {
        if (!viewport.isValid())
        {
            throw std::invalid_argument("OpenCL render request requires a valid viewport");
        }

        auto const eye = resources.getEyePosition();
        auto const & matrix = resources.getModelViewPerspectiveMat();
        auto const & sourceSettings = resources.getRenderingSettings();
        auto const aspectRatio = static_cast<float>(viewport.width) / static_cast<float>(viewport.height);

        return RenderRequest{
          .camera = {.eyePosition = {eye.x, eye.y, eye.z},
                     .forwardDirection = {matrix.s8, matrix.s9, matrix.sa},
                     .rightDirection = {matrix.s0, matrix.s1, matrix.s2},
                     .upDirection = {matrix.s4, matrix.s5, matrix.s6}},
          .frustum = {.horizontalScale = 0.5f, .verticalScale = 0.5f / aspectRatio},
          .settings = {.timeSeconds = resources.getTime_s(),
                       .sliceHeight = sourceSettings.z_mm,
                       .flags = static_cast<std::uint32_t>(sourceSettings.flags),
                       .mode = toRenderMode(sourceSettings.approximation),
                       .quality = sourceSettings.quality,
                       .normalOffset = sourceSettings.normalOffset,
                       .meshEarlyExitDistanceSquared = sourceSettings.earlyExitDistanceSq,
                       .meshInflationDistance = sourceSettings.meshInflationDistance,
                       .meshFwnBeta = sourceSettings.meshFwnBeta,
                       .meshFwnFarFieldFactor = sourceSettings.meshFwnFarFieldFactor,
                       .weightDistanceToNeighbor = sourceSettings.weightDistToNb,
                       .weightMidpoint = sourceSettings.weightMidPoint},
          .viewport = viewport,
          .freshness = freshness};
    }
}