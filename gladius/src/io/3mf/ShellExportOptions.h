#pragma once

#include "FilamentOpticalProperties.h"
#include "FrontlitThicknessSolver.h"
#include "ShellThicknessPartition.h"
#include "SurfaceExtractionOptions.h"

#include <vector>

namespace gladius::io
{
    /// Backend-neutral configuration for material shell 3MF export.
    struct ShellExportConfig
    {
        ShellGenerationMode generationMode = ShellGenerationMode::LegacyManifoldDualContouring;
        FilamentStack filamentStack;                     ///< Ordered materials (bottom to top)
        std::vector<std::vector<float>> precomputedLuts; ///< Thickness LUTs per layer
        int lutResolution = 16;                         ///< LUT grid resolution
        ThicknessConstraints thicknessConstraints;      ///< Thickness limits
        ManifoldDualContouringOptions mdcOptions;       ///< Mesh extraction options
        bool useSurfaceColorSampling = false;            ///< Sample colors at the visible surface
    };
} // namespace gladius::io
