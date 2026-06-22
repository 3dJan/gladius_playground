#pragma once

#include "ShellGenerator.h"
#include "ShellThicknessPartition.h"

#include "../vdb.h"

#include <functional>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::io
{
    /// @brief Generates constant-thickness shell bands using an OpenVDB level-set workflow.
    ///
    /// This is the first implementation slice of the OpenVDB backend. It supports a uniform
    /// shell stack derived from a constant thickness solution and produces one watertight shell
    /// mesh per material band. Surface-color propagation is intentionally left for a later step.
    class OpenVdbShellGenerator
    {
      public:
        explicit OpenVdbShellGenerator(ComputeCore& core);

        [[nodiscard]] std::vector<ShellGenerator::ShellMesh> generateUniformShells(
            FilamentStack const& stack,
            ThicknessSolution const& solution,
            ManifoldDualContouringOptions const& options,
            std::function<bool()> cancellationCheck = {});

        /// @brief Signed-distance for a shell band derived from a model SDF.
        ///
        /// The original model SDF is negative inside the solid. A shell occupies the inward depth
        /// interval [outerDepth, innerDepth]. The returned signed distance is negative inside the
        /// shell band and positive outside.
        [[nodiscard]] static float evaluateShellSignedDistance(
            float modelSdf,
            ShellLayerDepthInterval const& interval) noexcept;

      private:
        [[nodiscard]] openvdb::FloatGrid::Ptr createShellGrid(
            PreComputedSdf& sdf,
            BoundingBox const& bbox,
            ShellLayerDepthInterval const& interval,
            float narrowBandWidth) const;

        [[nodiscard]] ShellGenerator::ShellMesh meshToShellMesh(
            Mesh& mesh,
            std::string filamentName,
            int layerIndex) const;

        [[nodiscard]] static std::size_t selectSdfResolution(
            ManifoldDualContouringOptions const& options) noexcept;

        ComputeCore& m_core;
    };
} // namespace gladius::io