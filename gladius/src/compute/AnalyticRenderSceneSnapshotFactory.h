#pragma once

#include "compute/RenderSceneSnapshot.h"

#include <cstdint>

namespace gladius::nodes
{
    class Assembly;
    class Model;
}

namespace gladius
{
    class ResourceManager;
}

namespace gladius::compute
{
    /**
     * @brief Lowers supported resource-independent graph models into immutable scene snapshots.
     *
     * This factory belongs to the neutral compute layer: it creates no Dawn or OpenCL resources.
     * Mesh-backed graphs are supported through the backend-neutral mesh payload ABI; other
     * resource types (VDB, beam lattice, image stacks) remain explicitly unsupported.
     */
    class AnalyticRenderSceneSnapshotFactory
    {
      public:
        [[nodiscard]] static RenderSceneSnapshot create(nodes::Model & model,
                                                        std::uint64_t sceneGeneration);
        [[nodiscard]] static RenderSceneSnapshot create(nodes::Assembly const & assembly,
                                                        std::uint64_t sceneGeneration);

        /// Create a snapshot including mesh payloads from the given resource manager.
        /// Mesh resources referenced by the model are serialized with local offsets and
        /// indexed by their resource id in the snapshot.
        [[nodiscard]] static RenderSceneSnapshot create(nodes::Model & model,
                                                        std::uint64_t sceneGeneration,
                                                        ResourceManager const & resourceManager);

        [[nodiscard]] static RenderSceneSnapshot create(nodes::Assembly const & assembly,
                                                        std::uint64_t sceneGeneration,
                                                        ResourceManager const * resourceManager);
    };
}
