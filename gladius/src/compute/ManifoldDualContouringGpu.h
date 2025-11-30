#pragma once

#include "../CLProgram.h"
#include "../ComputeContext.h"
#include "../types.h"
#include "ComputeCore.h"
#include "GlobalMortonOctree.h"
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
        float minFeatureSize{0.0F};              ///< Minimum feature size to preserve (world units); 0 = disabled
        
        // Global hierarchical octree (watertight mesh generation)
        bool enableHierarchicalOctree{false};   ///< DISABLED: Global Morton octree is experimental and broken
        bool enableAdaptiveRefinement{false};    ///< TEMPORARILY DISABLED: Enable curvature-based adaptive refinement
        float curvatureThreshold{0.3F};          ///< Gradient variance threshold for subdivision
        std::size_t refinementPasses{2U};        ///< Number of adaptive refinement passes
        
        // Chunking for large models with fine features (fallback if hierarchical disabled)
        bool enableChunking{true};               ///< Auto-enable chunking when minFeatureSize requires higher depth than maxDepth
        float chunkWeldTolerance{0.0F};          ///< Tolerance for welding vertices at chunk boundaries; 0 = auto (based on voxel size)
        
        // Sharp feature post-processing
        bool enableSharpFeaturePostProcess{false};  ///< Enable subdivision and projection at sharp features
        float sharpFeatureAngleThreshold{0.5F};     ///< Cosine of angle threshold (0.5 = ~60°, lower = more sensitive)
        std::size_t subdivisionIterations{1U};      ///< Number of subdivision passes on sharp triangles
        bool projectToSurface{true};                ///< Project vertices to SDF surface after subdivision
        
        // Mesh simplification
        bool enableSimplification{false};           ///< Enable edge-collapse simplification in flat regions
        float simplificationMaxError{0.01F};        ///< Maximum SDF deviation allowed for edge collapse (world units)
        float simplificationMinEdgeLength{0.0F};    ///< Minimum edge length to preserve (0 = auto based on voxel size)
        float simplificationFlatThreshold{0.95F};   ///< Cosine threshold for coplanar normals (0.95 ≈ 18°)
        std::size_t simplificationPasses{3U};       ///< Number of simplification passes (more = more reduction)
        float simplificationAggressiveness{1.0F};   ///< Edge length multiplier (higher = more aggressive)
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
                std::optional<BoundingBox> m_cachedBoundingBox;
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
            
            // Flag to indicate chunked processing mode (disables maxCoord boundary check)
            bool m_isChunkedMode{false};

        ManifoldDualContouringConfig m_config{};
        ManifoldDualContouringMesh m_mesh{};
        std::size_t m_lastVertexCount{0U};
        std::size_t m_octreeNodeCount{0U};

        void loadKernels();
        void constructOctree();
        void generateVertices();
        void generateIndices();
        void refreshCpuOctreeCache();
        
        // Sharp feature post-processing
        void postProcessSharpFeatures();
        std::vector<std::size_t> detectSharpTriangles();
        void subdivideTriangles(std::vector<std::size_t> const & triangleIndices);
        void projectVerticesToSurface();
        
        // Mesh simplification
        void simplifyMesh();
        [[nodiscard]] std::size_t simplifyMeshPass(float minEdgeLength, float flatThreshold);
        [[nodiscard]] float evaluateSdf(Eigen::Vector3f const & pos) const;
        
        // Chunked processing for large models with fine features
        struct ChunkInfo
        {
            Eigen::Vector3f min;        ///< Processing region min (with overlap)
            Eigen::Vector3f max;        ///< Processing region max (with overlap)
            Eigen::Vector3f coreMin;    ///< Core region min (without overlap, used for clipping)
            Eigen::Vector3f coreMax;    ///< Core region max (without overlap, used for clipping)
            std::size_t indexX{0U};
            std::size_t indexY{0U};
            std::size_t indexZ{0U};
        };
        
        [[nodiscard]] std::size_t calculateRequiredDepth(float bboxExtent, float minFeatureSize) const;
        [[nodiscard]] std::size_t calculateChunkDivisor() const;
        [[nodiscard]] std::vector<ChunkInfo> generateChunkGrid() const;
        [[nodiscard]] bool isChunkNonEmpty(ChunkInfo const & chunk) const;
        void generateMeshForChunk(ChunkInfo const & chunk, ManifoldDualContouringMesh & chunkMesh);
        void clipMeshToCore(ManifoldDualContouringMesh & mesh, ChunkInfo const & chunk);
        void mergeMeshes(ManifoldDualContouringMesh & target, ManifoldDualContouringMesh const & source);
        void weldBoundaryVertices(float tolerance);
        void fillBoundaryGaps(float searchRadius);
        
        // Hierarchical octree approach (watertight mesh generation)
        void generateMeshHierarchical();
        std::unique_ptr<GlobalMortonOctree> m_hierarchicalOctree;
    };
    }
