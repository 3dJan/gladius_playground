#pragma once

#include "FilamentOpticalProperties.h"
#include "SurfaceExtractionOptions.h"
#include "SurfaceThicknessField.h"
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
         * @param precomputedLuts Optional precomputed LUTs (if null, will be built internally)
         * @param useSurfaceColorSampling If true, sample colors at surface (SDF=0) instead of 
         *        interior evaluation points. This fixes color reproduction for projected images.
         * @return One mesh per interface (outermost first)
         */
        std::vector<ShellMesh> generateShells(
            FilamentStack const& stack,
            ThicknessSolution const& solution,
            ManifoldDualContouringOptions const& options,
            int thicknessLutResolution = 0,
            ThicknessConstraints thicknessConstraints = {},
            std::vector<std::vector<float>> const* precomputedLuts = nullptr,
            bool useSurfaceColorSampling = false);

        /// Build a cumulative thickness LUT for a given layer index (layer and all above)
        static std::vector<float> buildCumulativeThicknessLut(
            FilamentStack const& stack,
            ThicknessConstraints const& constraints,
            std::size_t startLayer,
            int lutResolution);

    private:
        /**
         * @brief Generate shells using surface-aligned color sampling
         * 
         * This method extracts the outer surface mesh, samples colors at surface vertices,
         * builds a SurfaceThicknessField, and uses it for shell extraction. This ensures
         * shell thicknesses match surface colors, not interior colors.
         */
        std::vector<ShellMesh> generateShellsWithSurfaceSampling(
            FilamentStack const& stack,
            ManifoldDualContouringOptions const& options,
            int lutResolution,
            ThicknessConstraints const& thicknessConstraints);

        ComputeCore& m_core;
        Document& m_document;
    };
}
