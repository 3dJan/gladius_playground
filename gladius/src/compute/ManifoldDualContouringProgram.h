#pragma once

#include "../ProgramBase.h"
#include "../types.h"

#include <Eigen/Core>
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
            
        /// Count vertices per octree node (1 per cell with surface crossing)
        void countVertices(
            cl::Buffer const & octreeBuffer,
            cl::Buffer & countBuffer,
            std::size_t nodeCount,
            Eigen::Vector3f const & bboxMin,
            Eigen::Vector3f const & bboxMax,
            Primitives const & primitives,
            float isoValue);
            
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
        void countQuads(
            cl::Buffer const & octreeBuffer,
            cl::Buffer & quadCountBuffer,
            std::size_t nodeCount,
            std::uint32_t maxCoord);

        /// Generate triangle indices using watertight quad generation
        void generateIndices(
            cl::Buffer const & octreeBuffer,
            cl::Buffer const & vertexOffsetBuffer,
            cl::Buffer const & indexOffsetBuffer,
            cl::Buffer & indexBuffer,
            std::size_t nodeCount,
            std::uint32_t maxCoord);

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
            
      private:
        void ensureCompiled();
    };
}
