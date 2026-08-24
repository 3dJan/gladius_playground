#pragma once

#include <string>
#include <string_view>

namespace gladius::webgpu
{
    /**
     * @brief Combines the headless frame ray-march template with a generated WGSL model evaluator.
     */
    class WebGPUFrameShaderComposer
    {
      public:
        /// Compose a frame shader without mesh support (analytic only).
        [[nodiscard]] static std::string compose(std::string_view modelEvaluator);

        /// Compose a frame shader including the mesh SDF module. The generated
        /// evaluator may call gladiusSignedDistanceToMesh(pos, resourceId).
        [[nodiscard]] static std::string composeWithMeshSupport(std::string_view modelEvaluator);
    };
}