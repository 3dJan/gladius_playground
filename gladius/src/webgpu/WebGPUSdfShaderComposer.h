#pragma once

#include <string>
#include <string_view>

namespace gladius::webgpu
{
    /**
     * @brief Inserts a generated model evaluator into the raw SDF evaluation shader.
     */
    class WebGPUSdfShaderComposer final
    {
      public:
        [[nodiscard]] static std::string compose(std::string_view modelEvaluator);

        /// Compose an evaluation shader including the mesh SDF module. The
        /// generated evaluator may call gladiusSignedDistanceToMesh(pos, resourceId).
        [[nodiscard]] static std::string composeWithMeshSupport(std::string_view modelEvaluator);
    };
}
