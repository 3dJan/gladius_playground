#pragma once

#include "../types.h"
#include "compute/FunctionEvaluatorSnapshot.h"
#include "compute/RenderSceneSnapshot.h"
#include "nodes/nodesfwd.h"

#include <cstdint>
#include <map>

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

        /// Build runtime numerical storage and origin fan-out on a detached assembly.
        [[nodiscard]] static RenderSceneSnapshot createRuntime(nodes::Assembly const & assembly,
                                                               std::uint64_t sceneGeneration,
                                                               ResourceManager const * resourceManager = nullptr);

                /// Build runtime numerical storage from an assembly already lowered and flattened by Document.
                [[nodiscard]] static RenderSceneSnapshot createRuntimeFromFlat(
                    nodes::Assembly const & flattenedAssembly,
                    std::uint64_t sceneGeneration,
                    ResourceManager const * resourceManager = nullptr);

        /// @brief Lower one graph function of the assembly into a standalone evaluatable function.
        /// The function becomes the assembly root, is lowered and flattened, and is emitted with
        /// its own argument list and all of its outputs instead of the render `evaluateModel` contract.
        [[nodiscard]] static FunctionEvaluatorSnapshot createFunctionEvaluator(
            nodes::Assembly const & assembly,
            gladius::ResourceId functionId,
            std::uint64_t sceneGeneration,
            ResourceManager const * resourceManager = nullptr);

            private:
                using ParameterLookupIndices = std::map<nodes::ParameterId, int>;

                [[nodiscard]] static RenderSceneSnapshot createModel(
                    nodes::Model & model,
                    std::uint64_t sceneGeneration,
                    bool bufferNumericLiterals,
                    ParameterLookupIndices const * lookupIndices = nullptr);
                [[nodiscard]] static RenderSceneSnapshot createModel(
                    nodes::Model & model,
                    std::uint64_t sceneGeneration,
                    ResourceManager const & resourceManager,
                    bool bufferNumericLiterals,
                    ParameterLookupIndices const * lookupIndices = nullptr);
    };
}