#pragma once

#include "GpuKernelAccessGuard.h"
#include "Primitives.h"
#include "ResourceContext.h"

#include <vector>

namespace gladius
{
    struct RenderPayloadSnapshot
    {
        GpuResourceHandle primitiveMeta{};
        GpuResourceHandle primitiveData{};
        GpuResourceHandle precomputedSdf{};
        GpuResourceHandle parameterBuffer{};
        GpuResourceHandle commandBuffer{};

        [[nodiscard]] static RenderPayloadSnapshot capture(SharedResources const & resources,
                                                           Primitives const & primitives)
        {
            return RenderPayloadSnapshot{
              .primitiveMeta = primitives.primitives.gpuResourceHandle(),
              .primitiveData = primitives.data.gpuResourceHandle(),
              .precomputedSdf = resources->getPrecompSdfBuffer().gpuResourceHandle(),
              .parameterBuffer = resources->getParameterBuffer().gpuResourceHandle(),
              .commandBuffer = resources->getCommandBuffer().gpuResourceHandle()};
        }

        [[nodiscard]] std::vector<GpuKernelResourceAccess> readAccesses() const
        {
            return {{primitiveMeta, GpuAccessMode::Read},
                    {primitiveData, GpuAccessMode::Read},
                    {precomputedSdf, GpuAccessMode::Read},
                    {parameterBuffer, GpuAccessMode::Read},
                    {commandBuffer, GpuAccessMode::Read}};
        }

        [[nodiscard]] bool isCurrent(GpuAccessCoordinator const & coordinator) const
        {
            return isHandleCurrent(coordinator, primitiveMeta) &&
                   isHandleCurrent(coordinator, primitiveData) &&
                   isHandleCurrent(coordinator, precomputedSdf) &&
                   isHandleCurrent(coordinator, parameterBuffer) &&
                   isHandleCurrent(coordinator, commandBuffer);
        }

      private:
        [[nodiscard]] static bool isHandleCurrent(GpuAccessCoordinator const & coordinator,
                                                  GpuResourceHandle handle)
        {
            auto current = coordinator.currentHandle(handle.resourceId);
            return current.has_value() && *current == handle;
        }
    };
}
