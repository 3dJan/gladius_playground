#pragma once

#include "../CLProgram.h"
#include "../ComputeContext.h"
#include "../types.h"
#include "ComputeCore.h"
#include "ManifoldDualContouringProgram.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace gladius::compute
{
    struct ManifoldDualContouringConfig
    {
        std::size_t initialDepth{5U};            ///< Initial octree depth (determines starting cell size: rootSize / 2^initialDepth)
        std::size_t maxDepth{7U};                ///< Maximum subdivision depth
        bool enableGpu{true};
        bool enableCpuFallback{true};
        bool enableCaching{true};
        float isoValue{0.0F};
        float minFeatureSize{0.0F};              ///< Minimum feature size to preserve (world units); 0 = disabled. Controls subdivision threshold
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
                ManifoldDualContouringProgram * m_program{nullptr}; // Not owned - managed by ProgramManager

                // Cached bounds for current octree build
                Eigen::Vector3f m_cachedBboxMin{Eigen::Vector3f::Zero()};
                Eigen::Vector3f m_cachedBboxMax{Eigen::Vector3f::Zero()};
                Eigen::Vector3f m_cachedBboxSize{Eigen::Vector3f::Zero()};
                std::uint32_t m_octreeDepth{0U};
                std::uint32_t m_gridResolution{1U};

        // Buffers
        std::unique_ptr<cl::Buffer> m_octreeBuffer;
        std::unique_ptr<cl::Buffer> m_vertexBuffer;
        std::unique_ptr<cl::Buffer> m_indexBuffer;
        std::unique_ptr<cl::Buffer> m_countBuffer;
        std::unique_ptr<cl::Buffer> m_offsetBuffer;

                // CPU copies for topology reconstruction
                std::vector<OctreeNode> m_cpuOctreeNodes;
                std::unordered_map<std::uint64_t, std::size_t> m_mortonToIndex;
                std::vector<int> m_cpuVertexOffsets;
                std::vector<int> m_cpuVertexCounts;

        ManifoldDualContouringConfig m_config{};
        ManifoldDualContouringMesh m_mesh{};
        std::size_t m_lastVertexCount{0U};
        std::size_t m_octreeNodeCount{0U};

        void loadKernels();
        void constructOctree();
        void generateVertices();
        void generateIndices();

                // Helper utilities
                void refreshCpuOctreeCache();
                [[nodiscard]] std::size_t findNeighborIndex(std::size_t nodeIdx, int dx, int dy, int dz) const;
                [[nodiscard]] std::optional<std::uint32_t> vertexIndexForNode(std::size_t nodeIdx) const;
                void emitTrianglesForPolygon(std::vector<std::uint32_t> const & polygon);
    };
}
