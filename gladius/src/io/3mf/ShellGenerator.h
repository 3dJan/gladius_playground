#pragma once

#include "FilamentOpticalProperties.h"
#include "HierarchicalDualContouring.h"
#include "Mesh.h"
#include "Document.h"

#include <vector>
#include <memory>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::io
{
    class ShellGenerator
    {
    public:
        ShellGenerator(ComputeCore& core, Document& document);

        struct ShellMesh
        {
            std::vector<Eigen::Vector3f> vertices;
            std::vector<std::uint32_t> indices;
            std::string filamentName;
            int layerIndex;
        };

        /**
         * @brief Generate nested shell meshes (top to bottom)
         *
         * @param stack Filament stack ordered from bottom to top
         * @param solution Thickness solution (used for constant-thickness fallback)
         * @param config Hierarchical DC configuration (isoValue will be overridden per pass)
         * @param thicknessLutResolution If > 1, build a cumulative RGB->thickness LUT and use the
         *        variable-thickness GPU kernel. Resolution is per axis (e.g., 16 => 16^3 entries).
         * @param thicknessConstraints Constraints used when solving per-color thickness for the LUT
         * @return One mesh per interface (outermost first)
         */
        std::vector<ShellMesh> generateShells(
            FilamentStack const& stack,
            ThicknessSolution const& solution,
            hierarchical_dc::HierarchicalConfig config,
            int thicknessLutResolution = 0,
            ThicknessConstraints thicknessConstraints = {},
            std::vector<std::vector<float>> const* precomputedLuts = nullptr);

        /// Build a cumulative thickness LUT for a given layer index (layer and all above)
        static std::vector<float> buildCumulativeThicknessLut(
            FilamentStack const& stack,
            ThicknessConstraints const& constraints,
            std::size_t startLayer,
            int lutResolution);

    private:
        ComputeCore& m_core;
        Document& m_document;
    };
}
