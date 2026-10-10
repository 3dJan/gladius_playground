#include "DualContouringMeshBuilder.h"

#include "WindingRepair.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gladius::io
{
    IndexedTriangleMesh buildIndexedDualContouringMesh(
      dual_contouring::AxisAlignedBoundingBox const & bounds,
      dual_contouring::OctreeBuildConfig const & config,
      std::size_t width,
      std::size_t height,
      std::size_t depth,
      std::vector<float> values,
      dual_contouring::DualContouringDiagnostics * diagnostics)
    {
        dual_contouring::OctreeBuilder builder{bounds,
                                                config,
                                                width,
                                                height,
                                                depth,
                                                std::move(values)};
        dual_contouring::OctreeMetrics metrics{};
        auto root = builder.build(metrics);
        if (!root)
        {
            throw std::runtime_error("Failed to build dual-contouring octree");
        }

        auto const extracted =
          dual_contouring::buildDualContouringMesh(builder, *root, config, diagnostics);
        if (extracted.faces.empty())
        {
            throw std::runtime_error("Dual contouring produced an empty mesh");
        }
        if (extracted.vertices.size() > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::length_error("Dual-contouring mesh exceeds the 32-bit vertex index limit");
        }
        if (extracted.faces.size() > std::numeric_limits<std::size_t>::max() / 3U)
        {
            throw std::length_error("Dual-contouring mesh exceeds the supported triangle count");
        }

        IndexedTriangleMesh mesh;
        mesh.positions = extracted.vertices;
        mesh.indices.reserve(extracted.faces.size() * 3U);
        for (auto const & face : extracted.faces)
        {
            for (int corner = 0; corner < 3; ++corner)
            {
                auto const index = face[corner];
                if (index < 0 || static_cast<std::size_t>(index) >= mesh.positions.size())
                {
                    throw std::runtime_error(
                      "Dual contouring produced an out-of-range face index");
                }
                mesh.indices.push_back(static_cast<std::uint32_t>(index));
            }
        }

        // Reconcile local face choices, then orient each disconnected shell from the SDF gradient.
        std::vector<std::uint32_t> triangleComponents;
        auto const windingStats = repairTriangleWindingConsistency(
          mesh.positions, mesh.indices, &triangleComponents);
        std::vector<double> componentVotes(windingStats.components, 0.0);
        auto const orientationSamples = makeTriangleOrientationSamples(
          mesh.positions, mesh.indices, triangleComponents, 128U);
        Eigen::Vector3f const sampleOffset =
          builder.gridSpacing().cwiseMax(Eigen::Vector3f::Constant(1e-4F)) * 0.5F;
        for (auto const & sample : orientationSamples)
        {
            if (sample.component >= componentVotes.size())
            {
                continue;
            }

            Eigen::Vector3f gradient = Eigen::Vector3f::Zero();
            for (int axis = 0; axis < 3; ++axis)
            {
                Eigen::Vector3f offset = Eigen::Vector3f::Zero();
                offset(axis) = sampleOffset(axis);
                float const outside = builder.gridSample(sample.position + offset);
                float const inside = builder.gridSample(sample.position - offset);
                gradient(axis) = (outside - inside) / (2.0F * sampleOffset(axis));
            }

            if (gradient.allFinite() && gradient.squaredNorm() > 1e-8F)
            {
                componentVotes[sample.component] +=
                  static_cast<double>(sample.normal.dot(gradient));
            }
        }
        [[maybe_unused]] auto const flippedComponentCount =
          orientTriangleComponentsByVotes(mesh.indices, triangleComponents, componentVotes);

        if (auto const error = mesh.validationError())
        {
            throw std::runtime_error("Dual contouring produced an invalid mesh: " + *error);
        }
        if (auto const error = mesh.closedOrientedTopologyError())
        {
            throw std::runtime_error("Dual contouring produced invalid topology: " + *error);
        }
        return mesh;
    }
}
