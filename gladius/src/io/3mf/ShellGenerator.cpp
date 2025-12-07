#include "ShellGenerator.h"

#include "FrontlitThicknessSolver.h"
#include "HierarchicalDualContouring.h"
#include "compute/ComputeCore.h"
#include "kernel/types.h"

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

    std::vector<ShellGenerator::ShellMesh> ShellGenerator::generateShells(
        FilamentStack const& stack,
        ThicknessSolution const& solution,
        hierarchical_dc::HierarchicalConfig config,
        int thicknessLutResolution,
        ThicknessConstraints thicknessConstraints,
        std::vector<std::vector<float>> const* precomputedLuts)
    {
        std::vector<ShellMesh> shells;
        
        if (stack.size() != solution.thicknesses.size())
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
        
        BoundingBox const bounds = m_document.computeBoundingBox();

        // Iterate from top (last element) to bottom (first element)
        for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i)
        {
            if (useVariableThickness)
            {
                config.isoValue = 0.0f;
                config.lutResolution = thicknessLutResolution;
                if (precomputedLuts != nullptr &&
                    i < static_cast<int>(precomputedLuts->size()) &&
                    !precomputedLuts->at(static_cast<std::size_t>(i)).empty())
                {
                    config.thicknessLUT = precomputedLuts->at(static_cast<std::size_t>(i));
                }
                else if (lutSolver)
                {
                    config.thicknessLUT = buildCumulativeThicknessLutInternal(
                        *lutSolver,
                        static_cast<std::size_t>(i),
                        thicknessLutResolution);
                }
                else
                {
                    config.thicknessLUT.clear();
                }
            }
            else
            {
                // Configure iso value
                // We want to extract iso surface at -currentOffset.
                // The kernel computes `distance - isoValue`.
                // So we set isoValue = -currentOffset.
                config.isoValue = -currentOffset;
                config.thicknessLUT.clear();
                config.lutResolution = 0;
            }
            
            // Create builder
            hierarchical_dc::HierarchicalOctreeBuilder builder(m_core, config);
            
            // Build octree
            builder.buildOctree(bounds);
            
            // Extract mesh
            std::vector<Eigen::Vector3f> vertices;
            std::vector<std::uint32_t> indices;
            builder.extractMesh(vertices, indices);
            
            if (!vertices.empty() && !indices.empty())
            {
                ShellMesh shell;
                shell.vertices = std::move(vertices);
                shell.indices = std::move(indices);
                shell.filamentName = stack[i].name;
                shell.layerIndex = i;
                
                shells.push_back(std::move(shell));
            }
            
            // Update offset for the next inner shell (only used in constant thickness mode)
            if (!useVariableThickness)
            {
                currentOffset += solution.thicknesses[i];
            }
        }
        
        return shells;
    }
}
