#include "ShellGenerator.h"

#include "FrontlitThicknessSolver.h"
#include "SurfaceExtractionOptions.h"
#include "compute/ComputeCore.h"
#include "compute/ManifoldDualContouringGpu.h"
#include "kernel/types.h"

#include <algorithm>
#include <cmath>
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

        float currentOffset = 0.0f;
        bool const useVariableThickness = thicknessLutResolution > 1;
        std::unique_ptr<FrontlitThicknessSolver> lutSolver;

        if (useVariableThickness && precomputedLuts == nullptr)
        {
            lutSolver = std::make_unique<FrontlitThicknessSolver>(stack, thicknessConstraints);
        }

        compute::ManifoldDualContouringGpu gpuPipeline(m_core);
        compute::ManifoldDualContouringConfig baseConfig = toComputeConfig(options);
        baseConfig.enableQualityImprovement = true;
        baseConfig.qualityImprovementPasses = 3U;
        baseConfig.qualityMinAngleThreshold = 15.0F;

        // Iterate from top (last element) to bottom (first element)
        for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i)
        {
            compute::ManifoldDualContouringConfig config = baseConfig;

            if (useVariableThickness)
            {
                Eigen::Vector3f const representativeColor =
                  stack[static_cast<std::size_t>(i)].reflectanceColor.cwiseMax(0.0F).cwiseMin(1.0F);

                // LUT values are cumulative thicknesses (sum from current layer to top).
                // Convert to per-layer thickness by subtracting the thickness already
                // accumulated from layers above (currentOffset).
                float cumulativeThickness = 0.0F;
                if (precomputedLuts != nullptr &&
                    i < static_cast<int>(precomputedLuts->size()) &&
                    !precomputedLuts->at(static_cast<std::size_t>(i)).empty())
                {
                    cumulativeThickness = sampleCumulativeThickness(
                        precomputedLuts->at(static_cast<std::size_t>(i)), thicknessLutResolution, representativeColor);
                }
                else if (lutSolver)
                {
                    auto lut = buildCumulativeThicknessLutInternal(
                        *lutSolver,
                        static_cast<std::size_t>(i),
                        thicknessLutResolution);
                    cumulativeThickness = sampleCumulativeThickness(lut, thicknessLutResolution, representativeColor);
                }

                float thickness = 0.0F;
                if (cumulativeThickness > 0.0F)
                {
                    thickness = std::max(0.0F, cumulativeThickness - currentOffset);
                }

                if (thickness <= 0.0F)
                {
                    thickness = solution.thicknesses[i];
                }

                config.isoValue = -currentOffset;
                currentOffset += thickness;
            }
            else
            {
                // Configure iso value
                // We want to extract iso surface at -currentOffset.
                // The kernel computes `distance - isoValue`.
                // So we set isoValue = -currentOffset.
                config.isoValue = -currentOffset;
                currentOffset += solution.thicknesses[i];
            }

            gpuPipeline.setConfig(config);
            gpuPipeline.generateMesh();

            auto const & mesh = gpuPipeline.getMesh();

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
        
        return shells;
    }
}
