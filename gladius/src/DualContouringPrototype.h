#pragma once

#include "DualContouringMesher.h"

#include <Eigen/Core>

#include <cstdint>

namespace gladius::dual_contouring
{
    enum class PrototypeShape
    {
        UnitCube,
        Sphere,
        CylinderBlend
    };

    using PrototypeMesh = DualContouringMesh;
    using PrototypeDiagnostics = DualContouringDiagnostics;

    PrototypeMesh generatePrototypeMesh(PrototypeShape shape,
                                        std::uint32_t resolution,
                                        PrototypeDiagnostics * diagnostics = nullptr);
}
