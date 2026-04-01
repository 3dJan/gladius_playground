/**
 * @file QuadtreeContourExtractorTest.cpp
 * @brief Tests and benchmarks for QuadtreeContourExtractor.
 *
 * Validates correctness of marching-squares contour extraction on a sparse Morton quadtree
 * and compares memory usage + timing against the dense uniform-grid approach.
 *
 * Test SDF shapes used:
 *  - Circle:    sdf(p) = |p| - radius               (positive outside, negative inside)
 *  - Rectangle: sdf(p) = max(|p.x|-hw, |p.y|-hh)
 *  - Thin wall: two closely spaced rectangles (≈200 μm gap)
 */

#include <gtest/gtest.h>

#include "slicer/MortonQuadtree.h"
#include "slicer/QuadtreeContourExtractor.h"

#include <chrono>
#include <cmath>
#include <numbers>
#include <numeric>
#include <vector>

using namespace gladius::slicer;

// ============================================================================
// SDF helpers
// ============================================================================

static float circleSdf(Eigen::Vector2f const& p, float radius)
{
    return p.norm() - radius;
}

static float rectangleSdf(Eigen::Vector2f const& p, float halfW, float halfH)
{
    float const dx = std::abs(p.x()) - halfW;
    float const dy = std::abs(p.y()) - halfH;
    return std::max(dx, dy);
}

/// Two vertical walls spaced `gap` apart, each `thickness` wide, height `h`.
static float thinWallSdf(Eigen::Vector2f const& p, float gap, float height)
{
    // Left wall: x in [-0.5*gap - thickness, -0.5*gap]
    // Right wall: x in [0.5*gap, 0.5*gap + thickness]
    // We model the gap as negative SDF (the space between walls is inside):
    float const sdfLeft  = std::abs(p.x() + gap * 0.5F) - gap * 0.25F;
    float const sdfRight = std::abs(p.x() - gap * 0.5F) - gap * 0.25F;
    float const sdfY     = std::abs(p.y()) - height * 0.5F;
    float const sdfWalls = -std::min(sdfLeft, sdfRight);  // negative inside walls
    return std::max(sdfWalls, -sdfY);  // clamp by height
}

// ============================================================================
// Test fixture
// ============================================================================

class QuadtreeContourExtractorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 100 mm × 100 mm domain, centred at origin
        m_bounds = BoundingBox2D{{-50.0F, -50.0F}, {50.0F, 50.0F}};
    }

    BoundingBox2D m_bounds;

    /// Build a deep-enough quadtree to resolve features at minFeatureSize.
    MortonQuadtree buildAndPopulate(QuadtreeContourExtractor::SdfFunction const& sdf,
                                    float minFeatureSize = 0.5F,
                                    float isoValue = 0.0F)
    {
        MortonQuadtree tree{m_bounds};

        MortonQuadtreeConfig cfg;
        // Depth to resolve minFeatureSize:  cellSize = 100 / 2^depth ≤ minFeatureSize
        float const domainSize = m_bounds.getMaxExtent();
        cfg.initialDepth = 0U;  // build coarse skeleton, refine adaptively
        cfg.maxDepth = static_cast<std::size_t>(
            std::ceil(std::log2(domainSize / minFeatureSize)));
        cfg.isoValue = isoValue;
        cfg.minFeatureSize = minFeatureSize;
        cfg.enableAdaptiveRefinement = false;  // refine purely by thin-wall criterion
        cfg.maxNodes = 500000U;
        cfg.initialDepth = cfg.maxDepth;  // full uniform build for simplicity in tests

        tree.build(cfg);
        QuadtreeContourExtractor::populateCornerValues(tree, sdf, isoValue);
        return tree;
    }
};

// ============================================================================
// Marching squares correctness – single cell tests
// ============================================================================

TEST_F(QuadtreeContourExtractorTest, ExtractCellSegments_AllBelow_NoSegments)
{
    BoundingBox2D cell{{0.0F, 0.0F}, {1.0F, 1.0F}};
    std::array<float, 4> corners = {-1.0F, -2.0F, -0.5F, -1.5F};
    std::vector<ContourSegment> segs;
    QuadtreeContourExtractor::extractCellSegments(cell, corners, 0.0F, segs);
    EXPECT_TRUE(segs.empty());
}

TEST_F(QuadtreeContourExtractorTest, ExtractCellSegments_AllAbove_NoSegments)
{
    BoundingBox2D cell{{0.0F, 0.0F}, {1.0F, 1.0F}};
    std::array<float, 4> corners = {1.0F, 2.0F, 0.5F, 1.5F};
    std::vector<ContourSegment> segs;
    QuadtreeContourExtractor::extractCellSegments(cell, corners, 0.0F, segs);
    EXPECT_TRUE(segs.empty());
}

TEST_F(QuadtreeContourExtractorTest, ExtractCellSegments_BLAbove_OneSegment)
{
    // Case 1: only BL above iso → segment from bottom to left edge
    BoundingBox2D cell{{0.0F, 0.0F}, {1.0F, 1.0F}};
    // BL = 1 (above), BR = TL = TR = -1 (below)
    std::array<float, 4> corners = {1.0F, -1.0F, -1.0F, -1.0F};
    std::vector<ContourSegment> segs;
    QuadtreeContourExtractor::extractCellSegments(cell, corners, 0.0F, segs);

    ASSERT_EQ(segs.size(), 1U);
    // Start on bottom edge (y = 0)
    EXPECT_FLOAT_EQ(segs[0].start.y(), 0.0F);
    // End on left edge (x = 0)
    EXPECT_FLOAT_EQ(segs[0].end.x(), 0.0F);
}

TEST_F(QuadtreeContourExtractorTest, ExtractCellSegments_BRAbove_BottomToRight)
{
    // Case 2: only BR above iso
    BoundingBox2D cell{{0.0F, 0.0F}, {1.0F, 1.0F}};
    std::array<float, 4> corners = {-1.0F, 1.0F, -1.0F, -1.0F};
    std::vector<ContourSegment> segs;
    QuadtreeContourExtractor::extractCellSegments(cell, corners, 0.0F, segs);

    ASSERT_EQ(segs.size(), 1U);
    EXPECT_FLOAT_EQ(segs[0].start.y(), 0.0F);  // bottom edge
    EXPECT_FLOAT_EQ(segs[0].end.x(), 1.0F);    // right edge
}

TEST_F(QuadtreeContourExtractorTest, ExtractCellSegments_Interpolation_ExactHalfway)
{
    // BL = -1, BL is outside; BR = +1, BR is inside.
    // Iso at 0 → crossing at t=0.5 along bottom edge → x = 0.5, y = 0
    BoundingBox2D cell{{0.0F, 0.0F}, {2.0F, 2.0F}};
    std::array<float, 4> corners = {-1.0F, 1.0F, -1.0F, -1.0F};
    std::vector<ContourSegment> segs;
    QuadtreeContourExtractor::extractCellSegments(cell, corners, 0.0F, segs);

    ASSERT_EQ(segs.size(), 1U);
    // Bottom edge crossing at x = 0 + 0.5 * 2 = 1.0
    EXPECT_NEAR(segs[0].start.x(), 1.0F, 1e-5F);
    EXPECT_NEAR(segs[0].start.y(), 0.0F, 1e-5F);
}

TEST_F(QuadtreeContourExtractorTest, ExtractCellSegments_SaddleCase6_TwoSegments)
{
    // Case 6: BR + TL above → saddle, should produce 2 segments
    BoundingBox2D cell{{0.0F, 0.0F}, {1.0F, 1.0F}};
    //                BL      BR      TL      TR
    std::array<float, 4> corners = {-1.0F, 1.0F, 1.0F, -1.0F};
    std::vector<ContourSegment> segs;
    QuadtreeContourExtractor::extractCellSegments(cell, corners, 0.0F, segs);

    EXPECT_EQ(segs.size(), 2U);
}

// ============================================================================
// Circle contour tests
// ============================================================================

TEST_F(QuadtreeContourExtractorTest, PopulateCornerValues_Circle_MarksIntersecting)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    MortonQuadtree tree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 5U;
    cfg.maxDepth = 5U;
    tree.build(cfg);

    std::size_t const intersecting =
        QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);

    // The circle has circumference 2π*30 ≈ 188 mm. At depth 5, cell size ≈ 100/32 ≈ 3.125 mm.
    // Expected intersecting nodes ≈ 188 / 3.125 ≈ 60.  Allow generous range.
    EXPECT_GT(intersecting, 30U);
    EXPECT_LT(intersecting, 300U);
    std::cout << "[INFO] Circle: " << intersecting << " intersecting nodes at depth 5" << std::endl;
}

TEST_F(QuadtreeContourExtractorTest, ExtractSegments_Circle_ReasonableSegmentCount)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    MortonQuadtree tree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 6U;
    cfg.maxDepth = 6U;
    tree.build(cfg);
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);

    QuadtreeContourExtractor extractor;
    auto const segments = extractor.extractSegments(tree);

    // Each intersecting cell contributes ~1 segment
    EXPECT_GT(segments.size(), 50U);
    std::cout << "[INFO] Circle depth 6: " << segments.size() << " segments" << std::endl;
}

TEST_F(QuadtreeContourExtractorTest, ExtractSegments_Circle_TotalLengthMatchesCircumference)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    // Use depth 8 for reasonable accuracy (~0.39 mm cells)
    MortonQuadtree tree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 8U;
    cfg.maxDepth = 8U;
    tree.build(cfg);
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);

    QuadtreeContourExtractor extractor;
    auto const segments = extractor.extractSegments(tree);

    float totalLength = 0.0F;
    for (auto const& seg : segments)
    {
        totalLength += (seg.end - seg.start).norm();
    }

    float const expectedCircumference = 2.0F * std::numbers::pi_v<float> * radius;
    // Allow ±5 % error at this resolution
    EXPECT_NEAR(totalLength, expectedCircumference, expectedCircumference * 0.05F);
    std::cout << "[INFO] Circle circumference: expected=" << expectedCircumference
              << " mm, got=" << totalLength << " mm" << std::endl;
}

TEST_F(QuadtreeContourExtractorTest, ExtractPolyLines_Circle_FormsSingleClosedLoop)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    MortonQuadtree tree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 7U;
    cfg.maxDepth = 7U;
    tree.build(cfg);
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);

    QuadtreeContourExtractor extractor;
    // Use a snap tolerance proportional to cell size (100/128 ≈ 0.78 mm)
    float const cellSize = m_bounds.getMaxExtent() / static_cast<float>(1 << 7);
    auto const polylines = extractor.extractPolyLines(tree, 0.0F, cellSize * 0.01F);

    // Expect exactly one polyline and it should be closed
    ASSERT_EQ(polylines.size(), 1U);
    EXPECT_TRUE(polylines[0].isClosed);
    EXPECT_GT(polylines[0].vertices.size(), 50U);
    std::cout << "[INFO] Circle polyline vertices: " << polylines[0].vertices.size() << std::endl;
}

// ============================================================================
// Thin wall detection test
// ============================================================================

TEST_F(QuadtreeContourExtractorTest, ThinWall_200um_Detected)
{
    // A 200 µm gap is the minimum feature modelled reliably.
    float const gap   = 0.2F;  // mm
    float const h     = 5.0F;  // mm wall height
    auto sdf = [gap, h](Eigen::Vector2f const& p) { return thinWallSdf(p, gap, h); };

    // Depth = ceil(log2(100 / 0.1)) = ceil(log2(1000)) = 10
    // At depth 10 cell size = 100/1024 ≈ 0.098 mm, smaller than the gap.
    MortonQuadtree tree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 10U;
    cfg.maxDepth = 10U;
    cfg.maxNodes = 2000000U;
    tree.build(cfg);

    std::size_t const intersecting =
        QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);

    // There should be intersecting nodes near the gap
    EXPECT_GT(intersecting, 0U);
    std::cout << "[INFO] Thin wall (200µm gap): " << intersecting
              << " intersecting nodes at depth 10" << std::endl;

    QuadtreeContourExtractor extractor;
    auto segments = extractor.extractSegments(tree);
    EXPECT_GT(segments.size(), 0U);
    std::cout << "[INFO] Thin wall segments: " << segments.size() << std::endl;
}

// ============================================================================
// Memory comparison tests
// ============================================================================

TEST_F(QuadtreeContourExtractorTest, Memory_QuadtreeVsDense_SparseIsMuchSmaller)
{
    // Memory advantage of the sparse approach grows with domain size.
    // Use a large 400×400 mm domain (matching the plan's target of 400mm² build plate).
    BoundingBox2D largeBounds{{-200.0F, -200.0F}, {200.0F, 200.0F}};
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    float const resolution = 0.5F;
    std::size_t const denseMem =
        QuadtreeContourExtractor::estimateDenseGridMemoryBytes(400.0F, 400.0F, resolution);

    // Quadtree: depth = ceil(log2(400/0.5)) = ceil(log2(800)) = 10
    // At depth 10 over 400mm: cell size = 400/1024 ≈ 0.39 mm
    MortonQuadtree tree{largeBounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 10U;
    cfg.maxDepth = 10U;
    cfg.maxNodes = 5000000U;
    tree.build(cfg);
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);

    auto const leaves = tree.getIntersectingLeaves();
    std::size_t const sparseMem =
        QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(leaves.size());

    float const ratio = static_cast<float>(denseMem) / static_cast<float>(sparseMem);

    std::cout << "[MEMORY] 400×400mm domain, circle r=30mm, ~0.4mm resolution:" << std::endl;
    std::cout << "  Dense grid:          " << denseMem / (1024U) << " KB ("
              << (400.0F / resolution + 1) * (400.0F / resolution + 1) << " nodes)" << std::endl;
    std::cout << "  Sparse (intersecting): " << sparseMem / 1024U << " KB ("
              << leaves.size() << " nodes)" << std::endl;
    std::cout << "  Reduction ratio: " << ratio << "×" << std::endl;
    std::cout << "  Note: ratio grows linearly with domain area; "
              << "for 400mm² domain the savings are significant." << std::endl;

    // For a 400×400mm domain, the circle only touches a small fraction of cells
    EXPECT_GT(ratio, 5.0F);
}

TEST_F(QuadtreeContourExtractorTest, Memory_Rectangle_SparseMemoryIsBounded)
{
    // Axis-aligned rectangle occupies only its perimeter in the quadtree
    float const hw = 20.0F, hh = 15.0F;
    auto sdf = [hw, hh](Eigen::Vector2f const& p) { return rectangleSdf(p, hw, hh); };

    MortonQuadtree tree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 7U;
    cfg.maxDepth = 7U;
    tree.build(cfg);
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);

    auto const leaves = tree.getIntersectingLeaves();
    std::size_t const sparseMem = QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(leaves.size());
    std::size_t const totalMem  = QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(tree.getNodeCount());

    // Dense equivalent at the same resolution
    float const cellSize = m_bounds.getMaxExtent() / static_cast<float>(1 << 7);
    std::size_t const denseMem =
        QuadtreeContourExtractor::estimateDenseGridMemoryBytes(
            m_bounds.getWidth(), m_bounds.getHeight(), cellSize);

    std::cout << "[MEMORY] Rectangle at depth 7" << std::endl;
    std::cout << "  Dense grid:          " << denseMem / 1024U << " KB" << std::endl;
    std::cout << "  Quadtree (total):    " << totalMem / 1024U << " KB ("
              << tree.getNodeCount() << " nodes)" << std::endl;
    std::cout << "  Intersecting only:   " << sparseMem / 1024U << " KB ("
              << leaves.size() << " nodes)" << std::endl;

    EXPECT_LT(sparseMem, denseMem);
}

// ============================================================================
// Benchmark: quadtree vs dense marching squares
// ============================================================================

/// Build an adaptively refined quadtree: populate SDF → refine intersecting → repeat.
/// This is the memory-optimal approach: only cells near the surface are allocated.
static MortonQuadtree buildAdaptive(BoundingBox2D const& bounds,
                                     QuadtreeContourExtractor::SdfFunction const& sdf,
                                     std::size_t targetDepth,
                                     float isoValue = 0.0F)
{
    constexpr std::size_t INITIAL_DEPTH = 3U;

    MortonQuadtree tree{bounds};

    MortonQuadtreeConfig cfg;
    cfg.initialDepth = INITIAL_DEPTH;
    cfg.maxDepth = targetDepth;
    cfg.isoValue = isoValue;
    cfg.minFeatureSize = 0.0F;  // No extra thin-wall protection for benchmark
    cfg.enableAdaptiveRefinement = false;
    cfg.maxNodes = 2000000U;
    cfg.refinementPasses = 1U;  // One pass per refinement call

    tree.build(cfg);

    // Iterative deepening: populate corners → refine intersecting leaves → repeat
    for (std::size_t currentDepth = INITIAL_DEPTH; currentDepth < targetDepth; ++currentDepth)
    {
        QuadtreeContourExtractor::populateCornerValues(tree, sdf, isoValue);
        tree.refineAdaptively(cfg);
    }
    // Final populate for the deepest leaves
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, isoValue);

    return tree;
}

class ContourExtractionBenchmark : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_bounds = BoundingBox2D{{-50.0F, -50.0F}, {50.0F, 50.0F}};
    }

    BoundingBox2D m_bounds;
};

TEST_F(ContourExtractionBenchmark, Dense_ExtractCircle_Depth7)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    float const resolution = m_bounds.getMaxExtent() / static_cast<float>(1 << 7);  // ≈ 0.78 mm

    auto t0 = std::chrono::high_resolution_clock::now();
    DenseMarchingSquares dense{m_bounds, resolution};
    dense.populate(sdf);
    auto const segs = dense.extractSegments(0.0F);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto const ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "[BENCH] Dense MarchingSquares (depth-7 equiv): "
              << dense.getCellCount() << " cells, " << segs.size() << " segments, "
              << ms << " µs, " << dense.getMemoryBytes() / 1024U << " KB" << std::endl;

    EXPECT_GT(segs.size(), 0U);
    EXPECT_LT(ms, 5000000LL);  // must finish in under 5 s
}

TEST_F(ContourExtractionBenchmark, Sparse_ExtractCircle_Depth7)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    auto t0 = std::chrono::high_resolution_clock::now();
    MortonQuadtree tree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 7U;
    cfg.maxDepth = 7U;
    tree.build(cfg);
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);
    QuadtreeContourExtractor extractor;
    auto const segs = extractor.extractSegments(tree);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto const ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    auto const intersecting = tree.getIntersectingLeaves().size();
    std::size_t const mem = QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(tree.getNodeCount());

    std::cout << "[BENCH] Sparse Quadtree (depth 7): "
              << tree.getNodeCount() << " total nodes, "
              << intersecting << " intersecting, "
              << segs.size() << " segments, "
              << ms << " µs, " << mem / 1024U << " KB" << std::endl;

    EXPECT_GT(segs.size(), 0U);
    EXPECT_LT(ms, 5000000LL);
}

TEST_F(ContourExtractionBenchmark, Dense_ExtractCircle_Depth9)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    float const resolution = m_bounds.getMaxExtent() / static_cast<float>(1 << 9);  // ≈ 0.195 mm

    auto t0 = std::chrono::high_resolution_clock::now();
    DenseMarchingSquares dense{m_bounds, resolution};
    dense.populate(sdf);
    auto const segs = dense.extractSegments(0.0F);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto const ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "[BENCH] Dense MarchingSquares (depth-9 equiv): "
              << dense.getCellCount() << " cells, " << segs.size() << " segments, "
              << ms << " µs, " << dense.getMemoryBytes() / 1024U << " KB" << std::endl;

    EXPECT_GT(segs.size(), 0U);
}

TEST_F(ContourExtractionBenchmark, Sparse_ExtractCircle_Depth9)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    auto t0 = std::chrono::high_resolution_clock::now();
    MortonQuadtree tree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 9U;
    cfg.maxDepth = 9U;
    tree.build(cfg);
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);
    QuadtreeContourExtractor extractor;
    auto const segs = extractor.extractSegments(tree);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto const ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    auto const intersecting = tree.getIntersectingLeaves().size();
    std::size_t const mem = QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(tree.getNodeCount());

    std::cout << "[BENCH] Sparse Quadtree (depth 9): "
              << tree.getNodeCount() << " total nodes, "
              << intersecting << " intersecting, "
              << segs.size() << " segments, "
              << ms << " µs, " << mem / 1024U << " KB" << std::endl;

    EXPECT_GT(segs.size(), 0U);
}

TEST_F(ContourExtractionBenchmark, MemoryComparison_200um_Resolution)
{
    // Compare memory for a 200 µm (~ minimum feature size) resolution over a 100×100 mm domain
    float const resolution = 0.2F;  // mm
    float const width = 100.0F;
    float const height = 100.0F;

    std::size_t const denseMem =
        QuadtreeContourExtractor::estimateDenseGridMemoryBytes(width, height, resolution);

    // For a circle of radius 30 mm at depth 10 (cell size ≈ 0.098 mm):
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };
    BoundingBox2D bounds{{-50.0F, -50.0F}, {50.0F, 50.0F}};

    // Depth for 0.2 mm resolution: depth = ceil(log2(100/0.2)) = ceil(log2(500)) = 9
    MortonQuadtree tree{bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = 9U;
    cfg.maxDepth = 9U;
    cfg.maxNodes = 2000000U;
    tree.build(cfg);
    QuadtreeContourExtractor::populateCornerValues(tree, sdf, 0.0F);

    auto const leaves = tree.getIntersectingLeaves();
    std::size_t const sparseMem =
        QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(leaves.size());

    float const reductionPct =
        100.0F * (1.0F - static_cast<float>(sparseMem) / static_cast<float>(denseMem));

    std::cout << "[MEMORY] At 200µm resolution over 100×100mm domain:" << std::endl;
    std::cout << "  Dense grid:          " << denseMem / (1024U * 1024U) << " MB" << std::endl;
    std::cout << "  Sparse (circle ∅60): " << sparseMem / 1024U << " KB ("
              << leaves.size() << " intersecting nodes)" << std::endl;
    std::cout << "  Reduction:           " << reductionPct << "%" << std::endl;

    // The sparse approach should be dramatically more memory-efficient for sparse surfaces
    EXPECT_GT(reductionPct, 80.0F);
}

// ─────────────────────────────────────────────────────────────────────────────
// Adaptive refinement benchmarks — true memory-footprint-optimized approach
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ContourExtractionBenchmark, Adaptive_ExtractCircle_Depth9_MemoryAndTiming)
{
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    // Dense reference at same resolution
    float const domainSize = m_bounds.getMaxExtent();   // 100 mm
    float const cellSize = domainSize / static_cast<float>(1 << 9);  // depth-9 ≈ 0.195 mm
    DenseMarchingSquares dense{m_bounds, cellSize};

    auto t0 = std::chrono::high_resolution_clock::now();
    dense.populate(sdf);
    auto const denseSegs = dense.extractSegments(0.0F);
    auto t1 = std::chrono::high_resolution_clock::now();
    auto const denseUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // Adaptive quadtree: start at depth 3, refine to depth 9
    auto t2 = std::chrono::high_resolution_clock::now();
    auto tree = buildAdaptive(m_bounds, sdf, 9U, 0.0F);
    QuadtreeContourExtractor extractor;
    auto const sparseSegs = extractor.extractSegments(tree);
    auto t3 = std::chrono::high_resolution_clock::now();
    auto const sparseUs = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

    auto const intersecting = tree.getIntersectingLeaves();
    std::size_t const totalMem    = QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(tree.getNodeCount());
    std::size_t const intersectMem= QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(intersecting.size());
    std::size_t const denseMem    = dense.getMemoryBytes();

    std::cout << "[BENCH] Dense (depth-9 equiv):" << std::endl;
    std::cout << "  Cells=" << dense.getCellCount() << ", segs=" << denseSegs.size()
              << ", time=" << denseUs << " µs, mem=" << denseMem / 1024U << " KB" << std::endl;
    std::cout << "[BENCH] Adaptive Quadtree (depth 3→9):" << std::endl;
    std::cout << "  Total nodes=" << tree.getNodeCount()
              << ", intersecting=" << intersecting.size()
              << ", segs=" << sparseSegs.size()
              << ", time=" << sparseUs << " µs" << std::endl;
    std::cout << "  Total mem=" << totalMem / 1024U << " KB"
              << ", intersecting-only=" << intersectMem / 1024U << " KB" << std::endl;
    std::cout << "  Memory reduction (intersecting vs dense): "
              << static_cast<float>(denseMem) / static_cast<float>(intersectMem) << "×" << std::endl;

    EXPECT_GT(sparseSegs.size(), 0U);
    // Adaptive and dense should agree on segment count (both at depth-9 resolution)
    EXPECT_NEAR(static_cast<long>(sparseSegs.size()), static_cast<long>(denseSegs.size()), 5);
    // fullBuildNodes = sum of 4^k for k=0..9 = (4^10 - 1)/3 = 349525
    std::size_t const fullBuildNodes = ((std::size_t{1} << (2U * 10U)) - 1U) / 3U;
    EXPECT_LT(tree.getNodeCount(), fullBuildNodes / 50U);  // at least 50× fewer nodes
}

TEST_F(ContourExtractionBenchmark, Adaptive_vs_Uniform_NodeCountComparison)
{
    // Compare node count between adaptive and uniform builds to illustrate memory advantage
    float const radius = 30.0F;
    auto sdf = [radius](Eigen::Vector2f const& p) { return circleSdf(p, radius); };

    constexpr std::size_t TARGET_DEPTH = 8U;

    // Uniform build
    MortonQuadtree uniformTree{m_bounds};
    MortonQuadtreeConfig cfg;
    cfg.initialDepth = TARGET_DEPTH;
    cfg.maxDepth = TARGET_DEPTH;
    uniformTree.build(cfg);
    std::size_t const uniformNodes = uniformTree.getNodeCount();

    // Adaptive build
    auto adaptiveTree = buildAdaptive(m_bounds, sdf, TARGET_DEPTH, 0.0F);
    std::size_t const adaptiveNodes = adaptiveTree.getNodeCount();
    std::size_t const adaptiveIntersecting = adaptiveTree.getIntersectingLeaves().size();

    float const nodeReduction =
        100.0F * (1.0F - static_cast<float>(adaptiveNodes) / static_cast<float>(uniformNodes));

    std::cout << "[COMPARE] Depth " << TARGET_DEPTH << " over 100×100mm, circle r=30mm:" << std::endl;
    std::cout << "  Uniform build:  " << uniformNodes << " nodes, "
              << QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(uniformNodes) / 1024U
              << " KB" << std::endl;
    std::cout << "  Adaptive build: " << adaptiveNodes << " nodes ("
              << adaptiveIntersecting << " intersecting), "
              << QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(adaptiveNodes) / 1024U
              << " KB" << std::endl;
    std::cout << "  Node count reduction: " << nodeReduction << "%" << std::endl;

    // Adaptive should have dramatically fewer nodes than uniform
    EXPECT_LT(adaptiveNodes, uniformNodes / 10U);
    EXPECT_GT(nodeReduction, 90.0F);
}
