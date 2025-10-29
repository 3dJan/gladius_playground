#pragma once

#include "DualContouringOctree.h"

#include <Eigen/Core>

#include <cstdint>
#include <vector>

namespace gladius::dual_contouring
{
    enum class PrototypeShape
    {
        UnitCube,
        Sphere,
        CylinderBlend
    };

    struct PrototypeMesh
    {
        std::vector<Eigen::Vector3f> vertices{};
        std::vector<Eigen::Vector3i> faces{};
        std::vector<Eigen::Vector3f> faceNormals{};

        [[nodiscard]] AxisAlignedBoundingBox bounds() const;
    };

    struct PrototypeDiagnostics
    {
        AxisAlignedBoundingBox bounds{};
        size_t vertexCount{0U};
        size_t faceCount{0U};
        size_t invertedFaceCount{0U};
        size_t signChangedEdgeCount{0U};
        size_t skippedFaceCount{0U};
    };

    PrototypeMesh generatePrototypeMesh(PrototypeShape shape,
                                        std::uint32_t resolution,
                                        PrototypeDiagnostics * diagnostics = nullptr);
}
