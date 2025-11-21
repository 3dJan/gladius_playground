#pragma once

#include "../CLProgram.h"
#include "../ComputeContext.h"
#include "../types.h"
#include "ComputeCore.h"

#include <Eigen/Core>

#include <cstdint>
#include <memory>
#include <vector>

namespace gladius::compute
{
    struct ManifoldDualContouringConfig
    {
        std::size_t initialDepth{5U};
        std::size_t maxDepth{7U};
        bool enableGpu{true};
        bool enableCpuFallback{true};
        bool enableCaching{true};
        float isoValue{0.0F};
    };

    struct ManifoldDualContouringMesh
    {
        std::vector<Eigen::Vector3f> positions;
        std::vector<Eigen::Vector3f> normals;
        std::vector<std::uint32_t> indices;
    };

    class ManifoldDualContouringGpu
    {
      public:
        explicit ManifoldDualContouringGpu(ComputeCore & core);

        void setConfig(ManifoldDualContouringConfig config);
        void generateMesh();

        [[nodiscard]] ManifoldDualContouringMesh const & getMesh() const
        {
            return m_mesh;
        }

      private:
        ComputeCore & m_core;
        std::unique_ptr<CLProgram> m_program;

        // Buffers
        std::unique_ptr<cl::Buffer> m_octreeBuffer;
        std::unique_ptr<cl::Buffer> m_vertexBuffer;
        std::unique_ptr<cl::Buffer> m_indexBuffer;
        std::unique_ptr<cl::Buffer> m_countBuffer;
        std::unique_ptr<cl::Buffer> m_offsetBuffer;

        ManifoldDualContouringConfig m_config{};
        ManifoldDualContouringMesh m_mesh{};
        std::size_t m_lastVertexCount{0U};

        void loadKernels();
        void constructOctree();
        void generateVertices();
        void generateIndices();
    };
}
