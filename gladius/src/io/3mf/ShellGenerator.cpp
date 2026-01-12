#include "ShellGenerator.h"

#include "FaceColorSampler.h"
#include "FrontlitThicknessSolver.h"
#include "SurfaceExtractionOptions.h"
#include "SurfaceThicknessField.h"
#include "compute/ComputeCore.h"
#include "compute/ManifoldDualContouringGpu.h"
#include "HierarchicalDualContouring.h"
#include "kernel/types.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_map>

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
        /// Note: For shell generation, we force a uniform grid (initialDepth == maxDepth) to avoid
        /// fragmented meshes caused by multi-level octree neighbor handling issues
        [[nodiscard]] hierarchical_dc::HierarchicalConfig toHierarchicalConfig(
            ManifoldDualContouringOptions const& options)
        {
            hierarchical_dc::HierarchicalConfig cfg{};
            // Force uniform grid by setting initialDepth = maxDepth
            // This avoids the fragmented mesh issue where cells at different octree levels
            // don't share vertices properly
            cfg.initialDepth = options.maxDepth;
            cfg.maxDepth = options.maxDepth;
            cfg.enableGpuAcceleration = options.enableGpu;
            cfg.gpuFallbackToCpu = options.enableCpuFallback;
            cfg.gpuEnableCaching = options.enableCaching;
            cfg.isoValue = options.isoValue;
            cfg.minFeatureSize = options.minFeatureSize;
            cfg.projectVerticesToSurface = options.projectToSurface;
            return cfg;
        }

        /// Weld duplicate vertices in a mesh to create proper connectivity
        /// This fixes the fragmented mesh output from HierarchicalOctreeBuilder
        void weldMeshVertices(
            std::vector<Eigen::Vector3f> & vertices,
            std::vector<std::uint32_t> & indices,
            float tolerance)
        {
            if (vertices.size() < 2U || indices.empty())
            {
                return;
            }

            float const toleranceSq = tolerance * tolerance;
            std::size_t const numVertices = vertices.size();

            // Build a simple spatial hash for faster neighbor lookup
            float const cellSize = tolerance * 2.0F;
            float const invCellSize = 1.0F / cellSize;

            auto hashPos = [invCellSize](Eigen::Vector3f const & pos) -> std::uint64_t
            {
                auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * invCellSize));
                auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * invCellSize));
                auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * invCellSize));

                std::uint64_t const hx = static_cast<std::uint64_t>(ix) & 0x1FFFFF;
                std::uint64_t const hy = static_cast<std::uint64_t>(iy) & 0x1FFFFF;
                std::uint64_t const hz = static_cast<std::uint64_t>(iz) & 0x1FFFFF;
                return (hx << 42) | (hy << 21) | hz;
            };

            // Map from cell hash to vertex indices in that cell
            std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> spatialHash;
            for (std::uint32_t i = 0U; i < numVertices; ++i)
            {
                std::uint64_t const hash = hashPos(vertices[i]);
                spatialHash[hash].push_back(i);
            }

            // Vertex remapping: vertexRemap[old] = new (canonical vertex)
            std::vector<std::uint32_t> vertexRemap(numVertices);
            std::iota(vertexRemap.begin(), vertexRemap.end(), 0U);

            // For each vertex, find nearby vertices and potentially merge
            for (std::uint32_t i = 0U; i < numVertices; ++i)
            {
                if (vertexRemap[i] != i)
                {
                    continue; // Already remapped
                }

                Eigen::Vector3f const & pos = vertices[i];
                auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * invCellSize));
                auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * invCellSize));
                auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * invCellSize));

                // Check neighboring cells
                for (int dz = -1; dz <= 1; ++dz)
                {
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            std::uint64_t const hx = static_cast<std::uint64_t>(ix + dx) & 0x1FFFFF;
                            std::uint64_t const hy = static_cast<std::uint64_t>(iy + dy) & 0x1FFFFF;
                            std::uint64_t const hz = static_cast<std::uint64_t>(iz + dz) & 0x1FFFFF;
                            std::uint64_t const neighborHash = (hx << 42) | (hy << 21) | hz;

                            auto it = spatialHash.find(neighborHash);
                            if (it == spatialHash.end())
                            {
                                continue;
                            }

                            for (std::uint32_t j : it->second)
                            {
                                if (j <= i || vertexRemap[j] != j)
                                {
                                    continue;
                                }

                                float const distSq = (vertices[j] - pos).squaredNorm();
                                if (distSq < toleranceSq)
                                {
                                    vertexRemap[j] = i;
                                }
                            }
                        }
                    }
                }
            }

            // Build compaction map and count surviving vertices
            std::vector<std::uint32_t> compactMap(numVertices, std::numeric_limits<std::uint32_t>::max());
            std::uint32_t newVertexCount = 0U;

            for (std::uint32_t i = 0U; i < numVertices; ++i)
            {
                if (vertexRemap[i] == i)
                {
                    compactMap[i] = newVertexCount++;
                }
            }

            // Update remap to point to compact indices
            for (std::uint32_t i = 0U; i < numVertices; ++i)
            {
                std::uint32_t canonical = vertexRemap[i];
                vertexRemap[i] = compactMap[canonical];
            }

            // Compact vertices
            std::vector<Eigen::Vector3f> newVertices(newVertexCount);
            for (std::uint32_t i = 0U; i < numVertices; ++i)
            {
                if (compactMap[i] != std::numeric_limits<std::uint32_t>::max())
                {
                    newVertices[compactMap[i]] = vertices[i];
                }
            }

            // Remap indices and remove degenerate triangles
            std::vector<std::uint32_t> newIndices;
            newIndices.reserve(indices.size());
            for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
            {
                std::uint32_t const a = vertexRemap[indices[i]];
                std::uint32_t const b = vertexRemap[indices[i + 1]];
                std::uint32_t const c = vertexRemap[indices[i + 2]];

                // Skip degenerate triangles
                if (a != b && b != c && a != c)
                {
                    newIndices.push_back(a);
                    newIndices.push_back(b);
                    newIndices.push_back(c);
                }
            }

            vertices = std::move(newVertices);
            indices = std::move(newIndices);
        }
    }

    std::vector<ShellGenerator::ShellMesh> ShellGenerator::generateShells(
        FilamentStack const& stack,
        ThicknessSolution const& solution,
        ManifoldDualContouringOptions const& options,
        int thicknessLutResolution,
        ThicknessConstraints thicknessConstraints,
        std::vector<std::vector<float>> const* precomputedLuts,
        bool useSurfaceColorSampling)
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

        // NEW: Use surface-aligned color sampling if requested
        if (useSurfaceColorSampling && useVariableThickness)
        {
            fmt::print("[ShellGenerator] Using surface-aligned color sampling\n");
            return generateShellsWithSurfaceSampling(
                stack, options, thicknessLutResolution, thicknessConstraints);
        }

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

                gpuPipeline.setConfig(std::move(config));
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

    std::vector<ShellGenerator::ShellMesh> ShellGenerator::generateShellsWithSurfaceSampling(
        FilamentStack const& stack,
        ManifoldDualContouringOptions const& options,
        int lutResolution,
        ThicknessConstraints const& thicknessConstraints)
    {
        std::vector<ShellMesh> shells;

        auto const boundingBox = m_core.getBoundingBox();
        if (!boundingBox.has_value())
        {
            fmt::print(stderr, "[ShellGenerator] Surface sampling: No bounding box available\n");
            return shells;
        }

        // Phase 1: Extract outer surface mesh at SDF=0
        fmt::print("[ShellGenerator] Phase 1: Extracting outer surface mesh...\n");
        
        hierarchical_dc::HierarchicalConfig outerConfig = toHierarchicalConfig(options);
        outerConfig.isoValue = 0.0F;
        outerConfig.useShellVolumeMode = false;

        hierarchical_dc::HierarchicalOctreeBuilder outerBuilder(m_core, outerConfig);
        outerBuilder.buildOctree(boundingBox.value());

        std::vector<Eigen::Vector3f> surfaceVertices;
        std::vector<std::uint32_t> surfaceIndices;
        outerBuilder.extractMesh(surfaceVertices, surfaceIndices);

        if (surfaceVertices.empty())
        {
            fmt::print(stderr, "[ShellGenerator] Surface sampling: Failed to extract outer surface mesh\n");
            return shells;
        }

        fmt::print("[ShellGenerator] Phase 1 complete: {} vertices, {} triangles\n",
                   surfaceVertices.size(), surfaceIndices.size() / 3);

        // Phase 2: Sample colors at surface vertices
        fmt::print("[ShellGenerator] Phase 2: Sampling colors at surface vertices...\n");

        auto* samplingProgram = m_core.getProgramManager().getDualContouringSamplingProgram();
        auto primitives = m_core.getPrimitives();
        
        if (samplingProgram == nullptr || primitives == nullptr)
        {
            fmt::print(stderr, "[ShellGenerator] Surface sampling: Failed to get sampling program or primitives\n");
            return shells;
        }

        // Sample colors directly at vertex positions using the GPU sampling program
        std::vector<Eigen::Vector3f> surfaceColors;
        samplingProgram->sampleColors(surfaceVertices, surfaceColors, *primitives);

        if (surfaceColors.size() != surfaceVertices.size())
        {
            fmt::print(stderr, "[ShellGenerator] Surface sampling: Color sampling size mismatch\n");
            return shells;
        }

        fmt::print("[ShellGenerator] Phase 2 complete: {} colors sampled\n", surfaceColors.size());

        // Create solver for building LUTs
        FrontlitThicknessSolver solver(stack, thicknessConstraints);
        std::size_t const numLayers = stack.size();

        // Phase 3-5: Build thickness fields and extract shells for each layer
        for (int i = static_cast<int>(numLayers) - 1; i >= 0; --i)
        {
            fmt::print("[ShellGenerator] Phase 3-5: Building thickness field for layer {} ({})...\n",
                       i, stack[i].name);

            // Build outer LUT: cumulative thickness of layers ABOVE this one
            std::vector<float> outerLut;
            if (i == static_cast<int>(numLayers) - 1)
            {
                // Outermost layer: outer boundary is the model surface (thickness = 0)
                std::size_t const lutSize = static_cast<std::size_t>(lutResolution) *
                                           static_cast<std::size_t>(lutResolution) *
                                           static_cast<std::size_t>(lutResolution);
                outerLut.resize(lutSize, 0.0F);
            }
            else
            {
                outerLut = buildCumulativeThicknessLutInternal(
                    solver,
                    static_cast<std::size_t>(i + 1),
                    lutResolution);
            }

            // Build inner LUT: cumulative thickness of layers ABOVE AND INCLUDING this one
            std::vector<float> innerLut = buildCumulativeThicknessLutInternal(
                solver,
                static_cast<std::size_t>(i),
                lutResolution);

            if (outerLut.empty() || (i != 0 && innerLut.empty()))
            {
                fmt::print("[ShellGenerator] Layer {}: Skipping - empty LUT\n", i);
                continue;
            }

            // Build SurfaceThicknessField for outer boundary
            // Use higher resolution for accurate color reproduction
            SurfaceThicknessFieldConfig fieldConfig;
            fieldConfig.gridResolution = std::max(128, 1 << (options.maxDepth + 1));
            fieldConfig.maxPropagationDistance = fieldConfig.gridResolution;  // Fill entire grid
            fieldConfig.defaultThickness = 0.0F;

            SurfaceThicknessField outerField;
            outerField.build(surfaceVertices, surfaceColors, outerLut, lutResolution,
                            boundingBox.value(), fieldConfig);

            SurfaceThicknessField innerField;
            if (i != 0)
            {
                innerField.build(surfaceVertices, surfaceColors, innerLut, lutResolution,
                                boundingBox.value(), fieldConfig);
            }

            fmt::print("[ShellGenerator] Layer {}: Thickness fields built ({}³ grid, {} MB)\n",
                       i, fieldConfig.gridResolution, outerField.getMemoryUsage() / (1024 * 1024));

            // Use ManifoldDualContouringGpu with thickness field for watertight mesh output
            compute::ManifoldDualContouringConfig shellConfig = toComputeConfig(options);
            shellConfig.useThicknessField = true;
            shellConfig.outerThicknessField = outerField.getFieldBuffer();
            shellConfig.innerThicknessField = (i != 0) ? innerField.getFieldBuffer() : std::vector<float>{};
            shellConfig.thicknessFieldResolution = fieldConfig.gridResolution;
            shellConfig.worldToThicknessField = outerField.getWorldToGridTransform();
            shellConfig.isInnermostLayer = (i == 0);
            shellConfig.isoValue = 0.0F;
            // Disable quality improvement for shell meshes - prioritize watertightness
            shellConfig.enableQualityImprovement = false;
            // Use hierarchical octree for innermost layer (solid core) - produces watertight meshes
            // Use non-hierarchical path for non-innermost layers (thin shells) - better gap filling
            // Thin shells have thickness < voxel size, causing topology issues in both paths
            shellConfig.enableHierarchicalOctree = (i == 0);

            // Use heap allocation to avoid potential stack issues with large config
            auto gpuPipeline = std::make_unique<compute::ManifoldDualContouringGpu>(m_core);
            gpuPipeline->setConfig(std::move(shellConfig));
            gpuPipeline->generateMesh();

            auto const& mesh = gpuPipeline->getMesh();

            fmt::print("[ShellGenerator] Layer {} result: {} vertices, {} triangles\n",
                       i, mesh.positions.size(), mesh.indices.size() / 3);

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

        fmt::print("[ShellGenerator] Surface sampling complete: {} shells generated\n", shells.size());

        return shells;
    }
}
