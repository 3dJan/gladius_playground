#pragma once

#include "DualContouringOctree.h"

#include <Eigen/Core>

#include <cstddef>
#include <vector>

namespace gladius::dual_contouring
{
    struct DualContouringMesh
    {
        std::vector<Eigen::Vector3f> vertices{};
        std::vector<Eigen::Vector3i> faces{};
        std::vector<Eigen::Vector3f> faceNormals{};

        [[nodiscard]] AxisAlignedBoundingBox bounds() const;
    };

    struct DualContouringDiagnostics
    {
        AxisAlignedBoundingBox bounds{};
        size_t vertexCount{0U};
        size_t faceCount{0U};
        size_t invertedFaceCount{0U};
        size_t signChangedEdgeCount{0U};
        size_t skippedFaceCount{0U};
    };

    [[nodiscard]] DualContouringMesh buildDualContouringMesh(OctreeBuilder const & builder,
                                                             OctreeNode const & root,
                                                             OctreeBuildConfig const & config,
                                                             DualContouringDiagnostics * diagnostics = nullptr);
}
