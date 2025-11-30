#pragma once

#include "../BBox.h"
#include "../ProgramBase.h"
#include "../types.h"

#include <Eigen/Core>
#include <array>
#include <memory>
#include <vector>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::compute
{
    struct ManifoldDualContouringConfig;
    
    /// Octree node structure matching OpenCL kernel
    struct OctreeNode
    {
        cl_ulong mortonCode;      ///< Z-order space-filling curve position
        cl_uint edgeMask;         ///< Bit mask indicating which of 12 edges cross the surface
        cl_uint internalMask;     ///< Bit mask for which of 8 corners are inside the surface
        cl_uchar depth;           ///< Octree depth level (0 = root)
        cl_uchar padding[7];      ///< Padding for 24-byte alignment
    };
    static_assert(sizeof(OctreeNode) == 24, "OctreeNode size mismatch");
    
    /// GPU program for Manifold Dual Contouring mesh generation
    class ManifoldDualContouringProgram : public ProgramBase
    {
      public:
        ManifoldDualContouringProgram(SharedComputeContext context,
                                      SharedResources const & resources);
        
        /// Build octree by iteratively subdividing cells containing surface
        void constructOctree(
            std::unique_ptr<cl::Buffer> & octreeBuffer,
            std::size_t & nodeCount,
            Eigen::Vector3f const & bboxMin,
            Eigen::Vector3f const & bboxMax,
            std::uint32_t initialDepth,
            std::uint32_t maxDepth,
            Primitives const & primitives,
            float isoValue);
            
        /// Count vertices per octree node (1-4 per cell based on discontinuity detection)
        void countVertices(
            cl::Buffer const & octreeBuffer,
            cl::Buffer & countBuffer,
            std::size_t nodeCount,
            Eigen::Vector3f const & bboxMin,
            Eigen::Vector3f const & bboxMax,
            Primitives const & primitives,
            float isoValue,
            float gradientEpsilon);
            
        /// Generate vertices using SVD-based QEF solver (1 per cell)
        void generateVertices(
            cl::Buffer const & octreeBuffer,
            cl::Buffer const & offsetBuffer,
            cl::Buffer & vertexBuffer,
            std::size_t nodeCount,
            Eigen::Vector3f const & bboxMin,
            Eigen::Vector3f const & bboxMax,
            Primitives const & primitives,
            float isoValue,
            float gradientEpsilon);

        /// Count quads (for index allocation via prefix sum)
        /// @param disableBoundaryChecks Non-zero to disable boundary coord checks (chunked mode)
        void countQuads(
            cl::Buffer const & octreeBuffer,
            cl::Buffer & quadCountBuffer,
            std::size_t nodeCount,
            std::uint32_t maxCoord,
            std::uint32_t disableBoundaryChecks = 0U);

        /// Generate triangle indices using watertight quad generation
        /// @param disableBoundaryChecks Non-zero to disable boundary coord checks (chunked mode)
        void generateIndices(
            cl::Buffer const & octreeBuffer,
            cl::Buffer const & vertexOffsetBuffer,
            cl::Buffer const & indexOffsetBuffer,
            cl::Buffer & indexBuffer,
            std::size_t nodeCount,
            std::uint32_t maxCoord,
            std::uint32_t disableBoundaryChecks = 0U);

        /// Sort octree nodes by Morton code (required for neighbor lookup)
        void sortOctreeByMorton(
            std::unique_ptr<cl::Buffer> & octreeBuffer,
            std::size_t nodeCount);
        
        /// Add halo nodes for missing neighbors to ensure watertight mesh
        /// This ensures quads can always be formed at surface boundaries
        void addHaloNodes(
            std::unique_ptr<cl::Buffer> & octreeBuffer,
            std::size_t & nodeCount,
            std::uint32_t maxCoord,
            std::uint8_t depth);
        
        /// Diagnostic counters for boundary hole analysis (all 12 edges)
        struct DiagnosticCounters
        {
            std::array<int, 12> edgeEmitted{};   ///< Quads emitted per edge
            std::array<int, 12> edgeSkipped{};   ///< Quads skipped per edge
        };
        
        /// Run diagnostic analysis on quad emission
        /// Returns counters showing why quads are skipped at boundaries
        DiagnosticCounters runQuadDiagnostics(
            cl::Buffer const & octreeBuffer,
            std::size_t nodeCount,
            std::uint32_t maxCoord);
        
        /// Diagnostic counters for gradient discontinuity detection
        struct DiscontinuityCounters
        {
            int cells1Component{};       ///< Cells with smooth surface (1 component)
            int cells2Components{};      ///< Cells with one discontinuity (2 components)
            int cells3Components{};      ///< Cells with 3 components
            int cells4Components{};      ///< Cells with 4 components
            int totalCells{};            ///< Total cells analyzed
            float avgDiscontinuityScore{}; ///< Average discontinuity score (0-1)
            int severeDiscontinuities{}; ///< Cells with score > 0.5
        };
        
        /// Run discontinuity diagnostic to detect CSG-related gradient discontinuities
        /// Returns counters showing how many cells have multiple surface components
        DiscontinuityCounters runDiscontinuityDiagnostics(
            cl::Buffer const & octreeBuffer,
            std::size_t nodeCount,
            BBox const & paddedBbox,
            Primitives const & primitives,
            float isoValue,
            float gradientEpsilon);
            
      private:
        void ensureCompiled();
    };
}
