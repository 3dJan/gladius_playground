#pragma once

#include "compute/RenderContracts.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gladius::compute
{
    /**
     * @brief Backend-independent mesh resource payload for a render scene.
     *
     * The data is a flat float array using the same layout as the OpenCL primitives
     * payload (header + BVH nodes + triangles + normals + indices), with local
     * offsets. It is baked at snapshot creation time and never mutated afterwards.
     */
    struct MeshResourcePayload
    {
        std::vector<float> data;

        [[nodiscard]] bool isValid() const noexcept
        {
            return !data.empty();
        }
    };

    /**
     * @brief Immutable CPU payload for a supported analytic render scene.
     *
     * The initial payload deliberately covers only resource-independent analytic graphs. The
     * evaluator is WGSL because it is produced by the current analytic graph lowering path; mesh,
     * image, VDB, beam, command-stream, and precomputed-SDF payloads will extend this contract
     * with backend-independent resource descriptors rather than backend handles.
     */
    struct RenderSceneSnapshot
    {
        std::uint64_t sceneGeneration{};
        RendererCapability requiredCapabilities{RendererCapability::AnalyticRendering};
        std::string analyticEvaluatorWgsl;
        std::vector<float> parameterValues;
        /// Mesh payloads indexed by mesh resource id (sparse; empty entries are invalid).
        std::vector<MeshResourcePayload> meshResources;

        [[nodiscard]] bool isValid() const noexcept
        {
            if (sceneGeneration == 0u || analyticEvaluatorWgsl.empty() ||
                !hasCapability(requiredCapabilities, RendererCapability::AnalyticRendering))
            {
                return false;
            }

            for (auto const value : parameterValues)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }

            bool const declaresMesh = hasCapability(requiredCapabilities, RendererCapability::MeshSdf);
            bool const hasMeshes = !meshResources.empty();
            if (declaresMesh != hasMeshes)
            {
                return false;
            }
            // Slots are indexed by resource id and may be sparse (holes are empty).
            // At least one slot must carry a real payload.
            bool hasValidPayload = false;
            for (auto const & mesh : meshResources)
            {
                if (!mesh.isValid())
                {
                    continue;
                }
                hasValidPayload = true;
                break;
            }
            return !declaresMesh || hasValidPayload;
        }
    };
}
