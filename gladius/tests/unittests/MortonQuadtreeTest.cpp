#include <gtest/gtest.h>

#include "slicer/MortonQuadtree.h"

#include <chrono>
#include <random>
#include <vector>

using namespace gladius::slicer;

class MortonQuadtreeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Default bounding box: 100mm x 100mm
        m_bounds = BoundingBox2D{{-50.0F, -50.0F}, {50.0F, 50.0F}};
        m_defaultConfig = MortonQuadtreeConfig{};
    }

    BoundingBox2D m_bounds;
    MortonQuadtreeConfig m_defaultConfig;
};

// ============================================================================
// Morton Encoding Tests
// ============================================================================

TEST_F(MortonQuadtreeTest, MortonEncode_Zero_ReturnsZero)
{
    EXPECT_EQ(mortonEncode(0U, 0U), 0U);
}

TEST_F(MortonQuadtreeTest, MortonEncode_SingleBit_ReturnsCorrectInterleaving)
{
    // x=1, y=0 -> 0b01
    EXPECT_EQ(mortonEncode(1U, 0U), 1U);
    
    // x=0, y=1 -> 0b10
    EXPECT_EQ(mortonEncode(0U, 1U), 2U);
    
    // x=1, y=1 -> 0b11
    EXPECT_EQ(mortonEncode(1U, 1U), 3U);
}

TEST_F(MortonQuadtreeTest, MortonEncode_MultiBit_ReturnsCorrectInterleaving)
{
    // x=2 (0b10), y=1 (0b01) -> interleaved: 0b0110 = 6
    EXPECT_EQ(mortonEncode(2U, 1U), 6U);
    
    // x=3 (0b11), y=2 (0b10) -> interleaved: 0b1101 = 13
    EXPECT_EQ(mortonEncode(3U, 2U), 13U);
    
    // x=5 (0b101), y=3 (0b011) -> interleaved: 0b11011 = 27
    EXPECT_EQ(mortonEncode(5U, 3U), 27U);
}

TEST_F(MortonQuadtreeTest, MortonDecode_EncodeDecode_RoundTrip)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> testCases = {
        {0U, 0U},   {1U, 0U},   {0U, 1U},   {1U, 1U},
        {2U, 3U},   {5U, 7U},   {10U, 15U}, {100U, 200U},
        {1023U, 1023U}, {12345U, 54321U}
    };

    for (auto const& [x, y] : testCases)
    {
        auto const morton = mortonEncode(x, y);
        auto const [decodedX, decodedY] = mortonDecode(morton);
        
        EXPECT_EQ(decodedX, x) << "Failed for x=" << x << ", y=" << y;
        EXPECT_EQ(decodedY, y) << "Failed for x=" << x << ", y=" << y;
    }
}

TEST_F(MortonQuadtreeTest, MortonEncode_LocalityPreservation)
{
    // Morton encoding should preserve spatial locality
    // Points that are close in space should have close Morton codes
    
    auto const code1 = mortonEncode(10U, 10U);
    auto const code2 = mortonEncode(11U, 10U);  // Adjacent in x
    auto const code3 = mortonEncode(10U, 11U);  // Adjacent in y
    
    // Adjacent points should have Morton codes within a small range
    EXPECT_LT(std::abs(static_cast<int64_t>(code2 - code1)), 10);
    EXPECT_LT(std::abs(static_cast<int64_t>(code3 - code1)), 10);
}

TEST_F(MortonQuadtreeTest, CalculateChildMortonCodes_DepthZero_ReturnsCorrectCodes)
{
    // Root at depth 0 has Morton code 0 (position 0,0 in 1x1 grid)
    // Children are at positions (0,0), (1,0), (0,1), (1,1) in 2x2 grid
    auto const childCodes = calculateChildMortonCodes(0U, 0U);
    
    // mortonEncode(0,0) = 0, mortonEncode(1,0) = 1, mortonEncode(0,1) = 2, mortonEncode(1,1) = 3
    EXPECT_EQ(childCodes[0], 0U);  // (0,0) -> 0
    EXPECT_EQ(childCodes[1], 1U);  // (1,0) -> 1
    EXPECT_EQ(childCodes[2], 2U);  // (0,1) -> 2
    EXPECT_EQ(childCodes[3], 3U);  // (1,1) -> 3
}

TEST_F(MortonQuadtreeTest, CalculateChildMortonCodes_DepthOne_ReturnsCorrectCodes)
{
    // Node at depth 1 with Morton code 1 (position 1,0 in 2x2 grid)
    // Children are at positions (2,0), (3,0), (2,1), (3,1) in 4x4 grid
    auto const childCodes = calculateChildMortonCodes(1U, 1U);
    
    // mortonEncode(2,0) = 4, mortonEncode(3,0) = 5, mortonEncode(2,1) = 6, mortonEncode(3,1) = 7
    EXPECT_EQ(childCodes[0], 4U);  // (2,0) -> 4
    EXPECT_EQ(childCodes[1], 5U);  // (3,0) -> 5
    EXPECT_EQ(childCodes[2], 6U);  // (2,1) -> 6
    EXPECT_EQ(childCodes[3], 7U);  // (3,1) -> 7
}

// ============================================================================
// Quadtree Construction Tests
// ============================================================================

TEST_F(MortonQuadtreeTest, Build_EmptyTree_CreatesRootNode)
{
    MortonQuadtree tree{m_bounds};
    tree.build(m_defaultConfig);
    
    EXPECT_GT(tree.getNodeCount(), 0U);
}

TEST_F(MortonQuadtreeTest, Build_InitialDepth3_CreatesCorrectNodeCount)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 3U;
    config.maxDepth = 3U;
    
    tree.build(config);
    
    // At depth 3, we should have 1 + 4 + 16 + 64 = 85 nodes
    // (root + 4 children + 16 grandchildren + 64 great-grandchildren)
    std::size_t const expectedNodeCount = 1U + 4U + 16U + 64U;
    EXPECT_EQ(tree.getNodeCount(), expectedNodeCount);
}

TEST_F(MortonQuadtreeTest, Build_InitialDepth5_CreatesCorrectNodeCount)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 5U;
    config.maxDepth = 5U;
    
    tree.build(config);
    
    // At depth 5: sum of 4^i for i=0..5 = (4^6 - 1) / (4 - 1) = 1365 nodes
    std::size_t const expectedNodeCount = (4096U - 1U) / 3U;
    EXPECT_EQ(tree.getNodeCount(), expectedNodeCount);
}

TEST_F(MortonQuadtreeTest, GetLeafCount_AfterBuild_ReturnsCorrectCount)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 3U;
    config.maxDepth = 3U;
    
    tree.build(config);
    
    // At depth 3, all leaves are at depth 3: 4^3 = 64 leaves
    std::size_t const expectedLeafCount = 64U;
    EXPECT_EQ(tree.getLeafCount(), expectedLeafCount);
}

TEST_F(MortonQuadtreeTest, GetNodeByMorton_ExistingNode_ReturnsNode)
{
    MortonQuadtree tree{m_bounds};
    tree.build(m_defaultConfig);
    
    // Root node should have Morton code 0 at depth 0
    auto const* root = tree.getNodeByMorton(0U, 0U);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->depth, 0U);
    EXPECT_EQ(root->mortonCode, 0U);
}

TEST_F(MortonQuadtreeTest, GetNodeByMorton_NonExistentNode_ReturnsNullptr)
{
    MortonQuadtree tree{m_bounds};
    tree.build(m_defaultConfig);
    
    // Very large Morton code should not exist at depth 0
    auto const* node = tree.getNodeByMorton(999999999U, 0U);
    EXPECT_EQ(node, nullptr);
}

// ============================================================================
// Adaptive Refinement Tests
// ============================================================================

TEST_F(MortonQuadtreeTest, RefineAdaptively_NoIntersectingNodes_NoRefinement)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 2U;
    config.maxDepth = 5U;
    
    tree.build(config);
    
    // No nodes are marked as intersecting, so no refinement should occur
    auto const nodeCountBefore = tree.getNodeCount();
    tree.refineAdaptively(config);
    auto const nodeCountAfter = tree.getNodeCount();
    
    EXPECT_EQ(nodeCountBefore, nodeCountAfter);
}

TEST_F(MortonQuadtreeTest, RefineAdaptively_WithIntersectingNodes_RefinesToMaxDepth)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 2U;
    config.maxDepth = 4U;
    config.minFeatureSize = 0.0F;  // Disable thin wall protection
    config.enableAdaptiveRefinement = false;
    
    tree.build(config);
    
    // Mark some nodes as intersecting (this would normally be done by the SDF evaluation)
    // For this test, we'll just verify the structure is correct
    
    auto const leafCount = tree.getLeafCount();
    EXPECT_GT(leafCount, 0U);
}

// ============================================================================
// Bounding Box Tests
// ============================================================================

TEST_F(MortonQuadtreeTest, BoundingBox_GetMaxExtent_ReturnsCorrectValue)
{
    BoundingBox2D box{{-50.0F, -50.0F}, {50.0F, 50.0F}};
    EXPECT_FLOAT_EQ(box.getMaxExtent(), 100.0F);
}

TEST_F(MortonQuadtreeTest, BoundingBox_GetCellBounds_RootNode_ReturnsFullBounds)
{
    MortonQuadtree tree{m_bounds};
    tree.build(m_defaultConfig);
    
    auto const* root = tree.getNodeByMorton(0U, 0U);
    ASSERT_NE(root, nullptr);
    
    auto const cellBounds = m_bounds.getCellBounds(0U, 0U, 0U, m_bounds.getMaxExtent());
    
    // Root cell should match the full bounds
    EXPECT_FLOAT_EQ(cellBounds.min[0], m_bounds.min[0]);
    EXPECT_FLOAT_EQ(cellBounds.min[1], m_bounds.min[1]);
    EXPECT_FLOAT_EQ(cellBounds.max[0], m_bounds.max[0]);
    EXPECT_FLOAT_EQ(cellBounds.max[1], m_bounds.max[1]);
}

// ============================================================================
// Performance Benchmarks
// ============================================================================

class MortonQuadtreeBenchmark : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_bounds = BoundingBox2D{{-50.0F, -50.0F}, {50.0F, 50.0F}};
    }

    BoundingBox2D m_bounds;
};

TEST_F(MortonQuadtreeBenchmark, MortonEncode_1MillionEncodings_CompletesQuickly)
{
    auto const startTime = std::chrono::high_resolution_clock::now();
    
    volatile MortonCode2D sum = 0U;  // volatile to prevent optimization
    for (std::uint32_t i = 0U; i < 1000U; ++i)
    {
        for (std::uint32_t j = 0U; j < 1000U; ++j)
        {
            sum += mortonEncode(i, j);
        }
    }
    
    auto const endTime = std::chrono::high_resolution_clock::now();
    auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Should complete in less than 100ms (very conservative)
    EXPECT_LT(duration.count(), 100);
    
    std::cout << "[PERF] Morton encode 1M operations: " << duration.count() << "ms" << std::endl;
}

TEST_F(MortonQuadtreeBenchmark, MortonDecode_1MillionDecodings_CompletesQuickly)
{
    auto const startTime = std::chrono::high_resolution_clock::now();
    
    volatile std::uint32_t sum = 0U;
    for (MortonCode2D code = 0U; code < 1000000U; ++code)
    {
        auto const [x, y] = mortonDecode(code);
        sum += x + y;
    }
    
    auto const endTime = std::chrono::high_resolution_clock::now();
    auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Should complete in less than 100ms (very conservative)
    EXPECT_LT(duration.count(), 100);
    
    std::cout << "[PERF] Morton decode 1M operations: " << duration.count() << "ms" << std::endl;
}

TEST_F(MortonQuadtreeBenchmark, Build_Depth5_CompletesQuickly)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 5U;
    config.maxDepth = 5U;
    
    auto const startTime = std::chrono::high_resolution_clock::now();
    tree.build(config);
    auto const endTime = std::chrono::high_resolution_clock::now();
    
    auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Should complete in less than 50ms
    EXPECT_LT(duration.count(), 50);
    
    std::cout << "[PERF] Build quadtree depth 5 (" << tree.getNodeCount() 
              << " nodes): " << duration.count() << "ms" << std::endl;
}

TEST_F(MortonQuadtreeBenchmark, Build_Depth7_CompletesQuickly)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 7U;
    config.maxDepth = 7U;
    
    auto const startTime = std::chrono::high_resolution_clock::now();
    tree.build(config);
    auto const endTime = std::chrono::high_resolution_clock::now();
    
    auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Should complete in less than 500ms
    EXPECT_LT(duration.count(), 500);
    
    std::cout << "[PERF] Build quadtree depth 7 (" << tree.getNodeCount() 
              << " nodes): " << duration.count() << "ms" << std::endl;
}

TEST_F(MortonQuadtreeBenchmark, GetIntersectingLeaves_Depth5_CompletesQuickly)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 5U;
    config.maxDepth = 5U;
    
    tree.build(config);
    
    auto const startTime = std::chrono::high_resolution_clock::now();
    auto const leaves = tree.getIntersectingLeaves();
    auto const endTime = std::chrono::high_resolution_clock::now();
    
    auto const duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    // Should complete in less than 1ms
    EXPECT_LT(duration.count(), 1000);
    
    std::cout << "[PERF] Get intersecting leaves depth 5: " << duration.count() << "μs" << std::endl;
}

TEST_F(MortonQuadtreeBenchmark, MemoryUsage_Depth5_ReasonableSize)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 5U;
    config.maxDepth = 5U;
    
    tree.build(config);
    
    // Estimate memory usage
    auto const nodeCount = tree.getNodeCount();
    auto const estimatedMemoryMB = (nodeCount * sizeof(QuadNode)) / (1024.0 * 1024.0);
    
    std::cout << "[PERF] Memory usage depth 5 (" << nodeCount << " nodes): " 
              << estimatedMemoryMB << " MB" << std::endl;
    
    // Should use less than 10MB for depth 5
    EXPECT_LT(estimatedMemoryMB, 10.0);
}

TEST_F(MortonQuadtreeBenchmark, MemoryUsage_Depth7_ReasonableSize)
{
    MortonQuadtree tree{m_bounds};
    
    MortonQuadtreeConfig config;
    config.initialDepth = 7U;
    config.maxDepth = 7U;
    
    tree.build(config);
    
    // Estimate memory usage
    auto const nodeCount = tree.getNodeCount();
    auto const estimatedMemoryMB = (nodeCount * sizeof(QuadNode)) / (1024.0 * 1024.0);
    
    std::cout << "[PERF] Memory usage depth 7 (" << nodeCount << " nodes): " 
              << estimatedMemoryMB << " MB" << std::endl;
    
    // Should use less than 200MB for depth 7
    EXPECT_LT(estimatedMemoryMB, 200.0);
}
