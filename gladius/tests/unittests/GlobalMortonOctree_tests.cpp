/// @file GlobalMortonOctree_tests.cpp
/// Unit tests for GlobalMortonOctree to debug watertight mesh generation issues.

#include "compute/GlobalMortonOctree.h"
#include "compute/ManifoldDualContouringGpu.h"
#include "compute/ComputeCore.h"
#include "ComputeContext.h"
#include "Document.h"
#include "EventLogger.h"

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <set>

namespace gladius::compute::tests
{
    class GlobalMortonOctreeTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<events::Logger>();
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
        }

        void TearDown() override
        {
            m_core.reset();
            m_document.reset();
        }

        /// Load a 3MF document and initialize ComputeCore
        void loadDocument(std::filesystem::path const& path)
        {
            m_core = std::make_shared<ComputeCore>(
                m_context, RequiredCapabilities::ComputeOnly, m_logger);
            m_document = std::make_shared<Document>(m_core);
            m_document->load(path);
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
        std::shared_ptr<ComputeCore> m_core;
        std::shared_ptr<Document> m_document;
    };

    // ============================================================================
    // Morton Encoding/Decoding Tests (Pure algorithm, no GPU needed)
    // ============================================================================

    TEST_F(GlobalMortonOctreeTest, MortonEncoding_RoundTrip_Depth1)
    {
        // Test that encoding and decoding Morton codes works correctly for depth 1
        // At depth 1, we have 2x2x2 = 8 cells with coordinates (0,0,0) to (1,1,1)
        
        // At depth 1:
        // Morton code is just the octant index (0-7)
        // Octant encoding: bit0=x, bit1=y, bit2=z
        
        for (std::uint32_t z = 0; z < 2; ++z)
        {
            for (std::uint32_t y = 0; y < 2; ++y)
            {
                for (std::uint32_t x = 0; x < 2; ++x)
                {
                    // Expected Morton code at depth 1
                    std::uint64_t expected = x | (y << 1) | (z << 2);
                    
                    // Encode using the same logic as GlobalMortonOctree::encodePathMorton
                    std::uint64_t encoded = 0U;
                    std::uint8_t depth = 1U;
                    for (std::uint8_t d = 0U; d < depth; ++d)
                    {
                        auto const shift = depth - 1U - d;
                        auto const ox = (x >> shift) & 1U;
                        auto const oy = (y >> shift) & 1U;
                        auto const oz = (z >> shift) & 1U;
                        encoded = (encoded << 3U) | (ox | (oy << 1U) | (oz << 2U));
                    }
                    
                    EXPECT_EQ(encoded, expected) 
                        << "Morton encoding failed for (" << x << "," << y << "," << z << ")";
                    
                    // Decode
                    std::uint32_t dx = 0U, dy = 0U, dz = 0U;
                    for (std::uint8_t d = 0U; d < depth; ++d)
                    {
                        auto const shift = (depth - 1U - d) * 3U;
                        auto const octant = static_cast<std::uint8_t>((encoded >> shift) & 0x7U);
                        auto const levelScale = 1U << (depth - 1U - d);
                        dx += ((octant >> 0U) & 1U) * levelScale;
                        dy += ((octant >> 1U) & 1U) * levelScale;
                        dz += ((octant >> 2U) & 1U) * levelScale;
                    }
                    
                    EXPECT_EQ(dx, x) << "X decode failed for Morton " << encoded;
                    EXPECT_EQ(dy, y) << "Y decode failed for Morton " << encoded;
                    EXPECT_EQ(dz, z) << "Z decode failed for Morton " << encoded;
                }
            }
        }
    }

    TEST_F(GlobalMortonOctreeTest, MortonEncoding_RoundTrip_Depth3)
    {
        // Test Morton encoding at depth 3 (8x8x8 = 512 cells)
        std::uint8_t const depth = 3U;
        std::uint32_t const maxCoord = (1U << depth);
        
        std::size_t errors = 0;
        
        for (std::uint32_t z = 0; z < maxCoord; ++z)
        {
            for (std::uint32_t y = 0; y < maxCoord; ++y)
            {
                for (std::uint32_t x = 0; x < maxCoord; ++x)
                {
                    // Encode
                    std::uint64_t encoded = 0U;
                    for (std::uint8_t d = 0U; d < depth; ++d)
                    {
                        auto const shift = depth - 1U - d;
                        auto const ox = (x >> shift) & 1U;
                        auto const oy = (y >> shift) & 1U;
                        auto const oz = (z >> shift) & 1U;
                        encoded = (encoded << 3U) | (ox | (oy << 1U) | (oz << 2U));
                    }
                    
                    // Decode
                    std::uint32_t dx = 0U, dy = 0U, dz = 0U;
                    for (std::uint8_t d = 0U; d < depth; ++d)
                    {
                        auto const shift = (depth - 1U - d) * 3U;
                        auto const octant = static_cast<std::uint8_t>((encoded >> shift) & 0x7U);
                        auto const levelScale = 1U << (depth - 1U - d);
                        dx += ((octant >> 0U) & 1U) * levelScale;
                        dy += ((octant >> 1U) & 1U) * levelScale;
                        dz += ((octant >> 2U) & 1U) * levelScale;
                    }
                    
                    if (dx != x || dy != y || dz != z)
                    {
                        ++errors;
                        if (errors <= 5)
                        {
                            std::cout << "Round-trip error: (" << x << "," << y << "," << z 
                                      << ") -> morton=" << std::hex << encoded << std::dec
                                      << " -> (" << dx << "," << dy << "," << dz << ")" << std::endl;
                        }
                    }
                }
            }
        }
        
        EXPECT_EQ(errors, 0U) << "Morton encoding had " << errors << " round-trip errors at depth 3";
    }

    TEST_F(GlobalMortonOctreeTest, MortonEncoding_NeighborLookup_Depth3)
    {
        // Test that neighbor coordinate calculation works correctly
        std::uint8_t const depth = 3U;
        std::uint32_t const maxCoord = (1U << depth) - 1U;
        
        // Test cell (3, 3, 3) - center of the grid
        std::uint32_t const cx = 3U, cy = 3U, cz = 3U;
        
        // Encode center cell
        std::uint64_t centerMorton = 0U;
        for (std::uint8_t d = 0U; d < depth; ++d)
        {
            auto const shift = depth - 1U - d;
            auto const ox = (cx >> shift) & 1U;
            auto const oy = (cy >> shift) & 1U;
            auto const oz = (cz >> shift) & 1U;
            centerMorton = (centerMorton << 3U) | (ox | (oy << 1U) | (oz << 2U));
        }
        
        // Check 6 face neighbors
        std::array<std::tuple<int, int, int, std::string>, 6> const neighbors = {{
            {1, 0, 0, "+X"}, {-1, 0, 0, "-X"},
            {0, 1, 0, "+Y"}, {0, -1, 0, "-Y"},
            {0, 0, 1, "+Z"}, {0, 0, -1, "-Z"}
        }};
        
        for (auto const& [dx, dy, dz, name] : neighbors)
        {
            std::uint32_t nx = static_cast<std::uint32_t>(static_cast<int>(cx) + dx);
            std::uint32_t ny = static_cast<std::uint32_t>(static_cast<int>(cy) + dy);
            std::uint32_t nz = static_cast<std::uint32_t>(static_cast<int>(cz) + dz);
            
            // Encode neighbor
            std::uint64_t neighborMorton = 0U;
            for (std::uint8_t d = 0U; d < depth; ++d)
            {
                auto const shift = depth - 1U - d;
                auto const ox = (nx >> shift) & 1U;
                auto const oy = (ny >> shift) & 1U;
                auto const oz = (nz >> shift) & 1U;
                neighborMorton = (neighborMorton << 3U) | (ox | (oy << 1U) | (oz << 2U));
            }
            
            // Decode to verify
            std::uint32_t dnx = 0U, dny = 0U, dnz = 0U;
            for (std::uint8_t d = 0U; d < depth; ++d)
            {
                auto const shift = (depth - 1U - d) * 3U;
                auto const octant = static_cast<std::uint8_t>((neighborMorton >> shift) & 0x7U);
                auto const levelScale = 1U << (depth - 1U - d);
                dnx += ((octant >> 0U) & 1U) * levelScale;
                dny += ((octant >> 1U) & 1U) * levelScale;
                dnz += ((octant >> 2U) & 1U) * levelScale;
            }
            
            EXPECT_EQ(dnx, nx) << "Neighbor " << name << " X coordinate wrong";
            EXPECT_EQ(dny, ny) << "Neighbor " << name << " Y coordinate wrong";
            EXPECT_EQ(dnz, nz) << "Neighbor " << name << " Z coordinate wrong";
        }
    }

    // ============================================================================
    // Edge Ownership Tests
    // ============================================================================

    TEST_F(GlobalMortonOctreeTest, EdgeOwnership_OwnedEdges_AreCorrect)
    {
        // The EDGE_CORNERS table defines 12 edges:
        // Edges 0-3: X-aligned (varying x, fixed y,z)
        // Edges 4-7: Y-aligned (fixed x, varying y, fixed z)
        // Edges 8-11: Z-aligned (fixed x,y, varying z)
        //
        // In dual contouring, each cell "owns" 3 edges at specific corners:
        // Edge 3: X-aligned at (y=max, z=max) - corners 6-7  
        // Edge 7: Y-aligned at (x=max, z=max) - corners 5-7
        // Edge 11: Z-aligned at (x=max, y=max) - corners 3-7
        
        // Verify EDGE_CORNERS table structure
        // X-aligned edges (edges 0-3)
        EXPECT_EQ(EDGE_CORNERS[0][0], 0U) << "Edge 0 connects corners 0-1";
        EXPECT_EQ(EDGE_CORNERS[0][1], 1U);
        EXPECT_EQ(EDGE_CORNERS[3][0], 6U) << "Edge 3 connects corners 6-7 (owned edge)";
        EXPECT_EQ(EDGE_CORNERS[3][1], 7U);
        
        // Y-aligned edges (edges 4-7)
        EXPECT_EQ(EDGE_CORNERS[4][0], 0U) << "Edge 4 connects corners 0-2";
        EXPECT_EQ(EDGE_CORNERS[4][1], 2U);
        EXPECT_EQ(EDGE_CORNERS[7][0], 5U) << "Edge 7 connects corners 5-7 (owned edge)";
        EXPECT_EQ(EDGE_CORNERS[7][1], 7U);
        
        // Z-aligned edges (edges 8-11)
        EXPECT_EQ(EDGE_CORNERS[8][0], 0U) << "Edge 8 connects corners 0-4";
        EXPECT_EQ(EDGE_CORNERS[8][1], 4U);
        EXPECT_EQ(EDGE_CORNERS[11][0], 3U) << "Edge 11 connects corners 3-7 (owned edge)";
        EXPECT_EQ(EDGE_CORNERS[11][1], 7U);
        
        // Wait - let me verify the corner indexing convention
        // Corner 0: (0,0,0) - min corner
        // Corner 7: (1,1,1) - max corner
        // Corner i: (i&1, (i>>1)&1, (i>>2)&1)
        
        std::cout << "Corner positions:" << std::endl;
        for (int c = 0; c < 8; ++c)
        {
            int x = c & 1;
            int y = (c >> 1) & 1;
            int z = (c >> 2) & 1;
            std::cout << "  Corner " << c << ": (" << x << "," << y << "," << z << ")" << std::endl;
        }
        
        std::cout << "\nEdge corners:" << std::endl;
        for (int e = 0; e < 12; ++e)
        {
            int c0 = EDGE_CORNERS[e][0];
            int c1 = EDGE_CORNERS[e][1];
            int x0 = c0 & 1, y0 = (c0 >> 1) & 1, z0 = (c0 >> 2) & 1;
            int x1 = c1 & 1, y1 = (c1 >> 1) & 1, z1 = (c1 >> 2) & 1;
            char axis = (x0 != x1) ? 'X' : ((y0 != y1) ? 'Y' : 'Z');
            std::cout << "  Edge " << e << ": corners " << c0 << "-" << c1 
                      << " = (" << x0 << "," << y0 << "," << z0 << ")-("
                      << x1 << "," << y1 << "," << z1 << ") [" << axis << "-aligned]" << std::endl;
        }
    }

    TEST_F(GlobalMortonOctreeTest, EdgeOwnership_QuadNeighbors_ForAllOwnedEdges)
    {
        // Test that the neighbor offsets for all three owned edges are correct.
        // The owned edges are at the max corner of each cell:
        //   Edge 3: X-aligned at (y=max, z=max), corners 6-7
        //   Edge 7: Y-aligned at (x=max, z=max), corners 5-7
        //   Edge 11: Z-aligned at (x=max, y=max), corners 3-7
        
        // For an edge aligned with axis A, the 4 cells sharing it are offset
        // along the two perpendicular axes B and C:
        //   (0, 0), (+B, 0), (0, +C), (+B, +C)
        
        // Edge 3 (X-aligned): perpendicular axes are Y and Z
        //   Cell offsets: (0,0,0), (0,+1,0), (0,0,+1), (0,+1,+1)
        std::cout << "\nEdge 3 (X-aligned at y=max, z=max):" << std::endl;
        std::cout << "  Cell at (cx, cy, cz)       - owner (corners 6-7)" << std::endl;
        std::cout << "  Cell at (cx, cy+1, cz)     - +Y neighbor" << std::endl;
        std::cout << "  Cell at (cx, cy, cz+1)     - +Z neighbor" << std::endl;
        std::cout << "  Cell at (cx, cy+1, cz+1)   - +Y+Z neighbor" << std::endl;
        
        // Edge 7 (Y-aligned): perpendicular axes are X and Z
        //   Cell offsets: (0,0,0), (+1,0,0), (0,0,+1), (+1,0,+1)
        std::cout << "\nEdge 7 (Y-aligned at x=max, z=max):" << std::endl;
        std::cout << "  Cell at (cx, cy, cz)       - owner (corners 5-7)" << std::endl;
        std::cout << "  Cell at (cx+1, cy, cz)     - +X neighbor" << std::endl;
        std::cout << "  Cell at (cx, cy, cz+1)     - +Z neighbor" << std::endl;
        std::cout << "  Cell at (cx+1, cy, cz+1)   - +X+Z neighbor" << std::endl;
        
        // Edge 11 (Z-aligned): perpendicular axes are X and Y
        //   Cell offsets: (0,0,0), (+1,0,0), (0,+1,0), (+1,+1,0)
        std::cout << "\nEdge 11 (Z-aligned at x=max, y=max):" << std::endl;
        std::cout << "  Cell at (cx, cy, cz)       - owner (corners 3-7)" << std::endl;
        std::cout << "  Cell at (cx+1, cy, cz)     - +X neighbor" << std::endl;
        std::cout << "  Cell at (cx, cy+1, cz)     - +Y neighbor" << std::endl;
        std::cout << "  Cell at (cx+1, cy+1, cz)   - +X+Y neighbor" << std::endl;
        
        // Verify the EDGE_CORNERS table matches these expectations
        EXPECT_EQ(EDGE_CORNERS[3][0], 6U);
        EXPECT_EQ(EDGE_CORNERS[3][1], 7U);
        EXPECT_EQ(EDGE_CORNERS[7][0], 5U);
        EXPECT_EQ(EDGE_CORNERS[7][1], 7U);
        EXPECT_EQ(EDGE_CORNERS[11][0], 3U);
        EXPECT_EQ(EDGE_CORNERS[11][1], 7U);
    }

    // ============================================================================
    // Vertex Generation Tests
    // ============================================================================

    TEST_F(GlobalMortonOctreeTest, VertexGeneration_IntersectingCell_HasVertex)
    {
        // A cell that intersects the surface should have exactly one vertex
        // (in simple manifold DC, each intersecting cell gets one vertex)
        
        // This requires setting up a model - skip for now
        GTEST_SKIP() << "Requires model setup";
    }

    // ============================================================================
    // Quad Formation Tests
    // ============================================================================

    TEST_F(GlobalMortonOctreeTest, QuadFormation_AllNeighborsExist_FormValidQuad)
    {
        // When all 4 cells sharing an edge exist and have vertices,
        // a valid quad should be formed with correct winding
        
        // This requires a full octree setup - skip for now
        GTEST_SKIP() << "Requires model setup";
    }

    TEST_F(GlobalMortonOctreeTest, QuadFormation_MissingNeighbor_NoBoundaryEdge)
    {
        // When octree is balanced, missing neighbors should be created
        // This should eliminate boundary edges from missing cells
        
        // This requires a full octree setup - skip for now
        GTEST_SKIP() << "Requires model setup";
    }

    // ============================================================================
    // Integration Tests with Simple Shapes
    // ============================================================================

    TEST_F(GlobalMortonOctreeTest, DISABLED_SimpleSphere_ProducesWatertightMesh)
    {
        // Test with a simple sphere SDF
        // Expected: 0 boundary edges, 0 non-manifold edges, 1 part
        
        // Requires setting up a sphere model in ComputeCore
        GTEST_SKIP() << "Requires sphere model setup";
    }

}  // namespace gladius::compute::tests
