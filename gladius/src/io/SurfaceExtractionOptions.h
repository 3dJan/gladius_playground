#pragma once

#include <cstddef>
#include <optional>

namespace gladius::io
{
    enum class SurfaceExtractionMethod
    {
        LayeredMarchingCubes,
        DualContouring
    };

    /// Quality presets for dual contouring mesh generation
    enum class DualContouringQuality
    {
        Draft,      ///< Fast preview: 65³ resolution, no curvature refinement
        Balanced,   ///< Good quality/speed: 129³ resolution, basic refinement
        Fine,       ///< High detail: 257³ resolution, curvature refinement enabled
        UltraFine,  ///< Maximum quality: 513³ resolution, all features enabled
        Custom      ///< User-specified parameters
    };

    struct DualContouringOptions
    {
        DualContouringQuality qualityPreset{DualContouringQuality::Balanced};
        std::size_t sdfResolution{129U};
        float isoValue{0.0F};
        bool forceUniform{false};
        std::optional<std::size_t> maxDepth{};
        bool enableGpuSampling{true};           ///< Use GPU acceleration for SDF sampling
        bool enableCurvatureRefinement{false};
        float curvatureThreshold{0.5F};
        bool enableBalancedRefinement{false};
        
        /// Apply quality preset parameters
        void applyPreset();
    };

    struct StlExportOptions
    {
        SurfaceExtractionMethod method{SurfaceExtractionMethod::LayeredMarchingCubes};
        std::size_t marchingCubesQualityLevel{1U};
        DualContouringOptions dualContouring{};
    };
}
