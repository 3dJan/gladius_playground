#pragma once

#include "compute/RenderSceneSnapshot.h"

#include <cstdint>

namespace gladius::nodes
{
    class Assembly;
    class Model;
}

namespace gladius::compute
{
    /**
     * @brief Lowers supported resource-independent graph models into immutable scene snapshots.
     *
     * This factory belongs to the neutral compute layer: it creates no Dawn or OpenCL resources.
     * Resource-backed graph nodes remain explicitly unsupported until their CPU payload ABI exists.
     */
    class AnalyticRenderSceneSnapshotFactory
    {
      public:
        [[nodiscard]] static RenderSceneSnapshot create(nodes::Model & model,
                                                        std::uint64_t sceneGeneration);
        [[nodiscard]] static RenderSceneSnapshot create(nodes::Assembly const & assembly,
                                                        std::uint64_t sceneGeneration);
    };
}
