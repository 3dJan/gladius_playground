#include "ShellGenerator.h"

#include "FrontlitThicknessSolver.h"
#include "SurfaceExtractionOptions.h"
#include "compute/ComputeCore.h"
#include "compute/ManifoldDualContouringGpu.h"
#include "HierarchicalDualContouring.h"
#include "kernel/types.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

namespace
{
    using gladius::io::FrontlitThicknessSolver;
    using gladius::io::ThicknessSolution;

    [[nodiscard]] std::vector<float> buildCumulativeThicknessLutInternal(
        FrontlitThicknessSolver const& solver,
        std::size_t startLayer,
        int lutResolution)
    {
        if (lutResolution <= 1 || startLayer >= solver.getFilamentStack().size())
        {
            return {};
        }

        std::size_t const numLayers = solver.getFilamentStack().size();
        std::size_t const lutSize = static_cast<std::size_t>(lutResolution) *
                                   static_cast<std::size_t>(lutResolution) *
                                   static_cast<std::size_t>(lutResolution);
        std::vector<float> lut(lutSize, 0.0f);

        auto lutIndex = [lutResolution](int r, int g, int b) -> std::size_t
        {
            return (static_cast<std::size_t>(r) * static_cast<std::size_t>(lutResolution) +
                    static_cast<std::size_t>(g)) * static_cast<std::size_t>(lutResolution) +
                   static_cast<std::size_t>(b);
        };

        float const denom = static_cast<float>(lutResolution - 1);

        for (int r = 0; r < lutResolution; ++r)
        {
            for (int g = 0; g < lutResolution; ++g)
            {
                for (int b = 0; b < lutResolution; ++b)
                {
                    Eigen::Vector3f const color{
                        static_cast<float>(r) / denom,
                        static_cast<float>(g) / denom,
                        static_cast<float>(b) / denom};

                    ThicknessSolution const solution = solver.solve(color);

                    float cumulative = 0.0f;
                    for (std::size_t layer = startLayer;
                         layer < numLayers && layer < solution.thicknesses.size();
                         ++layer)
                    {
                        cumulative += solution.thicknesses[layer];
                    }

                    lut[lutIndex(r, g, b)] = cumulative;
                }
            }
        }

        return lut;
    }
}

namespace gladius::io
{
    ShellGenerator::ShellGenerator(ComputeCore& core, Document& document)
        : m_core(core)
        , m_document(document)
    {
    }

    std::vector<float> ShellGenerator::buildCumulativeThicknessLut(
        FilamentStack const& stack,
        ThicknessConstraints const& constraints,
        std::size_t startLayer,
        int lutResolution)
    {
        FrontlitThicknessSolver solver(stack, constraints);
        return buildCumulativeThicknessLutInternal(solver, startLayer, lutResolution);
    }

    namespace
    {
                [[nodiscard]] float sampleCumulativeThickness(std::vector<float> const & lut,
                                                                                                            int lutResolution,
                                                                                                            Eigen::Vector3f const & representativeColor)
        {
            if (lut.empty() || lutResolution <= 1)
            {
                return 0.0F;
            }

                        // The LUT maps target RGB to cumulative thickness. Sampling the brightest corner
                        // often yields the maximum thickness (e.g., when the stack cannot reproduce white),
                        // which can push iso-values outside the implicit field range and produce empty meshes.
                        // Instead, sample at a representative color for the current layer.
                        float const denom = static_cast<float>(lutResolution - 1);
                        auto toIndex = [lutResolution, denom](float v) -> int
                        {
                                v = std::clamp(v, 0.0F, 1.0F);
                                return std::clamp(static_cast<int>(std::lround(v * denom)), 0, lutResolution - 1);
                        };

                        int const r = toIndex(representativeColor.x());
                        int const g = toIndex(representativeColor.y());
                        int const b = toIndex(representativeColor.z());

                        std::size_t const idx =
                            (static_cast<std::size_t>(r) * static_cast<std::size_t>(lutResolution) +
                             static_cast<std::size_t>(g)) * static_cast<std::size_t>(lutResolution) +
                            static_cast<std::size_t>(b);

                        return idx < lut.size() ? lut[idx] : 0.0F;
        }

        [[nodiscard]] compute::ManifoldDualContouringConfig toComputeConfig(
          ManifoldDualContouringOptions const & options)
        {
            compute::ManifoldDualContouringConfig cfg{};
            cfg.initialDepth = options.initialDepth;
            cfg.maxDepth = options.maxDepth;
            cfg.enableGpu = options.enableGpu;
            cfg.enableCpuFallback = options.enableCpuFallback;
            cfg.enableCaching = options.enableCaching;
            cfg.isoValue = options.isoValue;
            cfg.minFeatureSize = options.minFeatureSize;
            cfg.enableChunking = options.enableChunking;
            cfg.enableHierarchicalOctree = options.enableHierarchicalOctree;
            cfg.enableSharpFeaturePostProcess = options.enableSharpFeaturePostProcess;
            cfg.sharpFeatureAngleThreshold = options.sharpFeatureAngleThreshold;
            cfg.subdivisionIterations = options.subdivisionIterations;
            cfg.projectToSurface = options.projectToSurface;
            cfg.simplificationMethod = static_cast<compute::SimplificationMethod>(
              static_cast<int>(options.simplificationMethod));
            cfg.enableSimplification = options.enableSimplification;
            cfg.simplificationMaxSdfError = options.simplificationMaxSdfError;
            cfg.simplificationMaxQemError = options.simplificationMaxQemError;
            cfg.simplificationMaxNormalDeviation = options.simplificationMaxNormalDeviation;
            cfg.simplificationSdfWeight = options.simplificationSdfWeight;
            cfg.simplificationQemWeight = options.simplificationQemWeight;
            cfg.simplificationNormalWeight = options.simplificationNormalWeight;
            cfg.simplificationSharpEdgeThreshold = options.simplificationSharpEdgeThreshold;
            cfg.simplificationBatchSize = options.simplificationBatchSize;
            cfg.simplificationMaxPasses = options.simplificationMaxPasses;
            cfg.simplificationTargetTriangles = options.simplificationTargetTriangles;
            cfg.simplificationTargetReduction = options.simplificationTargetReduction;
            cfg.enableQualityImprovement = true;
            cfg.qualityImprovementPasses = 3U;
            cfg.qualityMinAngleThreshold = 15.0F;
            return cfg;
        }

        /// Convert ManifoldDualContouringOptions to HierarchicalConfig for LUT-based shell generation
        [[nodiscard]] hierarchical_dc::HierarchicalConfig toHierarchicalConfig(
            ManifoldDualContouringOptions const& options)
        {
            hierarchical_dc::HierarchicalConfig cfg{};
            cfg.initialDepth = options.initialDepth;
            cfg.maxDepth = options.maxDepth;
            cfg.enableGpuAcceleration = options.enableGpu;
            cfg.gpuFallbackToCpu = options.enableCpuFallback;
            cfg.gpuEnableCaching = options.enableCaching;
            cfg.isoValue = options.isoValue;
            cfg.minFeatureSize = options.minFeatureSize;
            cfg.projectVerticesToSurface = options.projectToSurface;
            return cfg;
        }
    }

    std::vector<ShellGenerator::ShellMesh> ShellGenerator::generateShells(
        FilamentStack const& stack,
        ThicknessSolution const& solution,
        ManifoldDualContouringOptions const& options,
        int thicknessLutResolution,
        ThicknessConstraints thicknessConstraints,
        std::vector<std::vector<float>> const* precomputedLuts)
    {
        std::vector<ShellMesh> shells;
        
        if (stack.size() != solution.thicknesses.size())
        {
            return shells;
        }

        // Ensure bounding box is available for the GPU pipeline
        if (!m_core.updateBBox())
        {
            return shells;
        }

        auto const boundingBox = m_core.getBoundingBox();
        if (!boundingBox.has_value())
        {
            return shells;
        }

        bool const useVariableThickness = thicknessLutResolution > 1;
        std::unique_ptr<FrontlitThicknessSolver> lutSolver;

        // Always create the solver for shell volume mode - we need to build LUTs dynamically
        if (useVariableThickness)
        {
            lutSolver = std::make_unique<FrontlitThicknessSolver>(stack, thicknessConstraints);
        }

        // For variable thickness, we use HierarchicalOctreeBuilder with the thickness LUT.
        // The LUT is passed to the GPU kernel, which samples the local color at each point
        // and looks up the cumulative thickness from the LUT.
        // 
        // For constant thickness, we use ManifoldDualContouringGpu with explicit iso-values.

        if (useVariableThickness)
        {
            // Shell volume mode: Use HierarchicalOctreeBuilder with two LUTs per layer
            // Each shell is a material band between outer and inner depth boundaries
            hierarchical_dc::HierarchicalConfig baseConfig = toHierarchicalConfig(options);

            std::size_t const numLayers = stack.size();

            // Iterate from top (last element) to bottom (first element)
            for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i)
            {
                hierarchical_dc::HierarchicalConfig config = baseConfig;

                // Build outer LUT: cumulative thickness of layers ABOVE this one
                // For outermost layer (i = numLayers-1), outer = 0 (model surface)
                std::vector<float> outerLut;
                if (i == static_cast<int>(numLayers) - 1)
                {
                    // Outermost layer: outer boundary is the model surface (thickness = 0)
                    std::size_t const lutSize = static_cast<std::size_t>(thicknessLutResolution) *
                                               static_cast<std::size_t>(thicknessLutResolution) *
                                               static_cast<std::size_t>(thicknessLutResolution);
                    outerLut.resize(lutSize, 0.0F);
                }
                else if (lutSolver)
                {
                    // Outer boundary = cumulative of layers above this one (from i+1 to top)
                    outerLut = buildCumulativeThicknessLutInternal(
                        *lutSolver,
                        static_cast<std::size_t>(i + 1),
                        thicknessLutResolution);
                }

                // Build inner LUT: cumulative thickness of layers ABOVE AND INCLUDING this one
                std::vector<float> innerLut;
                if (lutSolver)
                {
                    // Inner boundary = cumulative from layer i to top
                    innerLut = buildCumulativeThicknessLutInternal(
                        *lutSolver,
                        static_cast<std::size_t>(i),
                        thicknessLutResolution);
                }

                if (outerLut.empty())
                {
                    std::cout << "[ShellGenerator] Layer " << i << " (" << stack[i].name 
                              << "): Skipping - empty outer LUT" << std::endl;
                    continue;
                }

                // For non-innermost layers, we need a valid inner LUT
                if (i != 0 && innerLut.empty())
                {
                    std::cout << "[ShellGenerator] Layer " << i << " (" << stack[i].name 
                              << "): Skipping - empty inner LUT" << std::endl;
                    continue;
                }

                // Diagnostic: show LUT thickness ranges (before move)
                auto lutStats = [](std::vector<float> const& lut) {
                    if (lut.empty()) return std::make_tuple(0.0F, 0.0F, 0.0F);
                    float minVal = std::numeric_limits<float>::max();
                    float maxVal = std::numeric_limits<float>::lowest();
                    float sum = 0.0F;
                    for (float t : lut) {
                        minVal = std::min(minVal, t);
                        maxVal = std::max(maxVal, t);
                        sum += t;
                    }
                    return std::make_tuple(minVal, maxVal, sum / static_cast<float>(lut.size()));
                };

                auto [outerMin, outerMax, outerAvg] = lutStats(outerLut);
                auto [innerMin, innerMax, innerAvg] = lutStats(innerLut);

                // Check for degenerate shell (outer and inner are identical)
                // This can happen when a layer has zero thickness everywhere
                if (!innerLut.empty())
                {
                    bool const isDegenerate = (innerMax - outerMax) < 0.001F && 
                                              (innerMin - outerMin) < 0.001F;
                    if (isDegenerate)
                    {
                        std::cout << "[ShellGenerator] Layer " << i << " (" << stack[i].name 
                                  << "): Skipping - degenerate shell (zero thickness)" << std::endl;
                        continue;
                    }
                }

                std::cout << "[ShellGenerator] Layer " << i << " (" << stack[i].name << "): "
                          << "Shell volume mode, outer=[" << outerMin << ", " << outerMax 
                          << "], inner=[" << innerMin << ", " << innerMax << "]"
                          << (i == 0 ? " (innermost)" : "") << std::endl;

                // Configure for shell volume mode
                config.useShellVolumeMode = true;
                config.outerLUT = std::move(outerLut);
                config.innerLUT = std::move(innerLut);
                config.lutResolution = thicknessLutResolution;
                config.isInnermostLayer = (i == 0);  // Innermost layer has no inner boundary
                config.isoValue = 0.0F;

                // Build octree and extract mesh using shell volume path
                hierarchical_dc::HierarchicalOctreeBuilder builder(m_core, config);
                builder.buildOctree(boundingBox.value());

                // Diagnostic: print octree stats
                auto const& stats = builder.getStats();
                std::cout << "[ShellGenerator] Layer " << i << " octree: "
                          << stats.totalNodes << " nodes, "
                          << stats.leafNodes << " leaves, "
                          << stats.intersectingLeaves << " intersecting" << std::endl;

                std::vector<Eigen::Vector3f> vertices;
                std::vector<std::uint32_t> indices;
                builder.extractMesh(vertices, indices);

                // Count boundary edges to assess mesh quality
                std::size_t boundaryEdges = 0;
                if (!indices.empty())
                {
                    std::unordered_map<std::uint64_t, int> edgeCounts;
                    auto makeEdgeKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t {
                        auto lo = std::min(a, b);
                        auto hi = std::max(a, b);
                        return (static_cast<std::uint64_t>(lo) << 32) | hi;
                    };
                    for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
                    {
                        edgeCounts[makeEdgeKey(indices[t], indices[t + 1])]++;
                        edgeCounts[makeEdgeKey(indices[t + 1], indices[t + 2])]++;
                        edgeCounts[makeEdgeKey(indices[t + 2], indices[t])]++;
                    }
                    for (auto const& [key, count] : edgeCounts)
                    {
                        if (count == 1) ++boundaryEdges;
                    }
                }

                std::cout << "[ShellGenerator] Layer " << i << " result: "
                          << vertices.size() << " vertices, "
                          << indices.size() / 3 << " triangles, "
                          << boundaryEdges << " boundary edges" << std::endl;

                if (!vertices.empty() && !indices.empty())
                {
                    ShellMesh shell;
                    shell.vertices = std::move(vertices);
                    shell.indices = std::move(indices);
                    shell.filamentName = stack[i].name;
                    shell.layerIndex = i;
                    
                    shells.push_back(std::move(shell));
                }
            }
        }
        else
        {
            // Constant thickness: Use ManifoldDualContouringGpu with explicit iso-values
            compute::ManifoldDualContouringGpu gpuPipeline(m_core);
            compute::ManifoldDualContouringConfig baseConfig = toComputeConfig(options);
            baseConfig.enableQualityImprovement = true;
            baseConfig.qualityImprovementPasses = 3U;
            baseConfig.qualityMinAngleThreshold = 15.0F;

            float currentOffset = 0.0F;

            // Iterate from top (last element) to bottom (first element)
            for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i)
            {
                compute::ManifoldDualContouringConfig config = baseConfig;

                std::cout << "[ShellGenerator] Layer " << i << " (" << stack[i].name << "): "
                          << "isoValue=" << -currentOffset 
                          << ", thickness=" << solution.thicknesses[i]
                          << ", cumulativeOffset after=" << (currentOffset + solution.thicknesses[i])
                          << " (constant thickness)" << std::endl;

                config.isoValue = -currentOffset;
                currentOffset += solution.thicknesses[i];

                gpuPipeline.setConfig(config);
                gpuPipeline.generateMesh();

                auto const& mesh = gpuPipeline.getMesh();

                std::cout << "[ShellGenerator] Layer " << i << " result: "
                          << mesh.positions.size() << " vertices, "
                          << mesh.indices.size() / 3 << " triangles" << std::endl;

                if (!mesh.positions.empty() && !mesh.indices.empty())
                {
                    ShellMesh shell;
                    shell.vertices = mesh.positions;
                    shell.indices = mesh.indices;
                    shell.filamentName = stack[i].name;
                    shell.layerIndex = i;
                    
                    shells.push_back(std::move(shell));
                }
            }
        }
        
        return shells;
    }
}
