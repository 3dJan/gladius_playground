#pragma once

#include <cstddef>
#include <optional>

namespace gladius::io
{
    enum class SurfaceExtractionMethod
    {
        LayeredMarchingCubes,
        DualContouring,
        ManifoldDualContouring
    };

    /// Simplification algorithm selection
    enum class SimplificationMethod
    {
        None,           ///< No simplification
        QemSdfAware     ///< QEM with GPU SDF error evaluation (SDF-aware)
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

    enum class ManifoldDualContouringQuality
    {
        Draft,
        Balanced,
        Fine,
        UltraFine,
        Custom
    };

    struct ManifoldDualContouringOptions
    {
        ManifoldDualContouringQuality qualityPreset{
          ManifoldDualContouringQuality::UltraFine};
                // Keep defaults consistent with the default preset (UltraFine) even if applyPreset() is not called.
                std::size_t initialDepth{7U};
                std::size_t maxDepth{9U};
        bool enableGpu{true};
        bool enableCpuFallback{true};
        bool enableCaching{true};
        float isoValue{0.0F};
        
        // Minimum feature size for thin wall preservation
        float minFeatureSize{0.0F};                 ///< Minimum feature size to preserve (world units); 0 = disabled
        bool enableChunking{true};                  ///< Enable spatial chunking when minFeatureSize requires higher depth than maxDepth
        
        // Hierarchical octree approach (recommended for watertight exports)
        bool enableHierarchicalOctree{true};        ///< Enable global Morton octree with 2:1 balancing for watertight mesh
        
        // Sharp feature post-processing options
        bool enableSharpFeaturePostProcess{false};  ///< Enable subdivision and projection at sharp features
        float sharpFeatureAngleThreshold{0.5F};     ///< Cosine of angle threshold (0.5 = ~60°)
        std::size_t subdivisionIterations{1U};      ///< Number of subdivision passes
        bool projectToSurface{true};                ///< Project vertices to SDF surface
        
        // Mesh simplification options
        SimplificationMethod simplificationMethod{SimplificationMethod::None}; ///< Which simplification algorithm to use
        bool enableSimplification{false};           ///< DEPRECATED: Use simplificationMethod instead
        
        // QEM SDF-aware options
        float simplificationMaxSdfError{0.01F};     ///< Maximum SDF deviation allowed for edge collapse (world units)
        float simplificationMaxQemError{1e-4F};     ///< Maximum QEM error allowed for edge collapse
        float simplificationMaxNormalDeviation{0.3F}; ///< Maximum normal deviation (1 - dot product, 0.3 ≈ ~45°)
        float simplificationSdfWeight{0.5F};        ///< Weight for SDF error in combined metric [0,1]
        float simplificationQemWeight{0.2F};        ///< Weight for QEM error in combined metric [0,1]
        float simplificationNormalWeight{0.3F};     ///< Weight for normal deviation in combined metric [0,1]
        float simplificationSharpEdgeThreshold{0.7F}; ///< Cosine threshold for sharp edges (0.7 ≈ 45°)
        std::size_t simplificationBatchSize{100000U}; ///< Number of edges per GPU evaluation batch
        std::size_t simplificationMaxPasses{10U};   ///< Maximum simplification passes
        std::optional<std::size_t> simplificationTargetTriangles{std::nullopt}; ///< Target triangle count (optional)
        std::optional<float> simplificationTargetReduction{std::nullopt};       ///< Target reduction percentage (optional)

        void applyPreset();
    };

    struct StlExportOptions
    {
        SurfaceExtractionMethod method{SurfaceExtractionMethod::LayeredMarchingCubes};
        std::size_t marchingCubesQualityLevel{1U};
        DualContouringOptions dualContouring{};
        ManifoldDualContouringOptions manifoldDualContouring{};
    };
}
