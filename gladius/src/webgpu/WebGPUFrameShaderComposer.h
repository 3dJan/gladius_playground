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

        /// Compose a frame shader including the beam lattice SDF module. The generated
        /// evaluator may call gladiusSignedDistanceToBeamLattice(pos, resourceId).
        [[nodiscard]] static std::string composeWithBeamSupport(std::string_view modelEvaluator);

        /// Compose a frame shader including both the mesh and beam lattice SDF modules.
        [[nodiscard]] static std::string composeWithMeshAndBeamSupport(std::string_view modelEvaluator);

        /// Compose a frame shader including the image stack sampling module.
        [[nodiscard]] static std::string composeWithImageSupport(std::string_view modelEvaluator);

        /// Compose exactly the resource modules required by an analytic scene.
        [[nodiscard]] static std::string composeWithResourceSupport(std::string_view modelEvaluator,
                                                                    bool includeMesh,
                                                                    bool includeBeam,
                                                                    bool includeImage);
    };
}