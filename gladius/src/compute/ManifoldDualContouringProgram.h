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
        cl_ulong mortonCode;
        cl_uint edgeMask;
        cl_uint internalMask;
        cl_uint vertexStartIndex;
        cl_uchar vertexCount;
        cl_uchar depth;
        cl_uchar padding[2];
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
            
        /// Count vertices per octree node
        void countVertices(
            cl::Buffer const & octreeBuffer,
            cl::Buffer & countBuffer,
            std::size_t nodeCount);
            
        /// Generate vertices using QEF solver
        void generateVertices(
            cl::Buffer const & octreeBuffer,
            cl::Buffer const & offsetBuffer,
            cl::Buffer & vertexBuffer,
            std::size_t nodeCount,
            Eigen::Vector3f const & bboxMin,
            Eigen::Vector3f const & bboxMax,
            Primitives const & primitives,
            float isoValue);

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
            
      private:
        void ensureCompiled();
    };
}
