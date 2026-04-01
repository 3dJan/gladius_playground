#pragma once

/**
 * @file QuadtreeContourExtractor.h
 * @brief Memory-footprint-optimized contour extraction via marching squares on sparse
 *        Morton quadtree leaf nodes.
 *
 * Key advantage over the dense MarchingSquaresStates grid:
 * - Dense grid: allocates width × height cells for the entire build area regardless of content.
 * - Sparse quadtree: only allocates cells near the iso-surface (up to 95% memory reduction).
 *
 * Marching squares convention used here:
 *   Corner indices:  [0]=BL, [1]=BR, [2]=TL, [3]=TR
 *   Edge indices:    0=BOTTOM (BL↔BR), 1=RIGHT (BR↔TR), 2=TOP (TL↔TR), 3=LEFT (BL↔TL)
 *   Case bit mask:   bit0=BL, bit1=BR, bit2=TL, bit3=TR  (set when corner value > isoValue)
 */

#include "MortonQuadtree.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace gladius::slicer
{
    /// A single 2D contour line segment.
    struct ContourSegment
    {
        Eigen::Vector2f start;
        Eigen::Vector2f end;
    };

    /// A polyline (ordered chain of vertices) extracted from the quadtree.
    struct SparsePolyLine
    {
        std::vector<Eigen::Vector2f> vertices;
        bool isClosed{false};
    };

    /**
     * @brief Extracts iso-contours from a MortonQuadtree via marching squares.
     *
     * Usage:
     *  1. Build the quadtree with MortonQuadtree::build().
     *  2. Call QuadtreeContourExtractor::populateCornerValues() to evaluate the SDF at all
     *     leaf node corners and mark intersecting nodes.
     *  3. Call extractSegments() or extractPolyLines() to get contour geometry.
     */
    class QuadtreeContourExtractor
    {
    public:
        using SdfFunction = std::function<float(Eigen::Vector2f const&)>;

        /**
         * @brief Evaluate SDF at all leaf node corners, updating cornerValues and isIntersecting.
         *
         * A node is marked intersecting when its corner values contain at least one sign change
         * relative to the isoValue (i.e. the iso-surface passes through the node).
         *
         * @param tree      Quadtree to populate (leaf corner values are written).
         * @param sdf       Function R²→R that returns the signed distance (or any scalar field).
         * @param isoValue  Iso-surface threshold. Default 0.
         * @return Number of intersecting leaf nodes found.
         */
        static std::size_t populateCornerValues(MortonQuadtree& tree,
                                                SdfFunction const& sdf,
                                                float isoValue = 0.0F);

        /**
         * @brief Extract raw line segments from all intersecting leaf nodes.
         *
         * Runs a per-cell marching-squares kernel.  Segments are unordered and unconnected.
         *
         * @param tree      Quadtree with cornerValues already populated.
         * @param isoValue  Iso-surface threshold. Default 0.
         * @return Vector of line segments.
         */
        [[nodiscard]] std::vector<ContourSegment> extractSegments(MortonQuadtree const& tree,
                                                                   float isoValue = 0.0F) const;

        /**
         * @brief Extract and chain segments into ordered polylines.
         *
         * Uses endpoint matching (within snapTolerance) to build chains.
         * Chains whose endpoints are within snapTolerance are closed.
         *
         * @param tree          Quadtree with cornerValues already populated.
         * @param isoValue      Iso-surface threshold. Default 0.
         * @param snapTolerance Maximum distance between endpoints to be considered connected.
         * @return Vector of polylines (closed or open).
         */
        [[nodiscard]] std::vector<SparsePolyLine> extractPolyLines(
            MortonQuadtree const& tree,
            float isoValue = 0.0F,
            float snapTolerance = 1e-4F) const;

        /**
         * @brief Estimate memory occupied by a quadtree with the given node count.
         * @param nodeCount Number of nodes.
         * @return Bytes.
         */
        [[nodiscard]] static std::size_t estimateQuadtreeMemoryBytes(std::size_t nodeCount);

        /**
         * @brief Estimate memory required for a dense uniform grid.
         * @param width          Domain width in mm.
         * @param height         Domain height in mm.
         * @param cellResolution Cell size in mm.
         * @return Bytes (float per grid node).
         */
        [[nodiscard]] static std::size_t estimateDenseGridMemoryBytes(float width,
                                                                       float height,
                                                                       float cellResolution);

        /// Marching squares on a single cell — also used by DenseMarchingSquares.
        static void extractCellSegments(BoundingBox2D const& cellBounds,
                                        std::array<float, 4> const& corners,
                                        float isoValue,
                                        std::vector<ContourSegment>& outSegments);

        /**
         * @brief Detect self-intersections in a set of polylines.
         *
         * Checks all non-adjacent segment pairs for geometric crossings.
         * O(n²) in total segment count — acceptable for typical contour sizes.
         *
         * @param polylines Polylines to check.
         * @return Number of self-intersecting segment pairs found.
         */
        [[nodiscard]] static std::size_t detectSelfIntersections(
            std::vector<SparsePolyLine> const& polylines);

    private:
        /// Interpolate an edge crossing position.
        static Eigen::Vector2f computeEdgeCrossing(BoundingBox2D const& cellBounds,
                                                    std::array<float, 4> const& corners,
                                                    int edgeIdx,
                                                    float isoValue);
    };

    // ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ──

    /**
     * @brief Dense uniform-grid marching squares — reference implementation for benchmarking.
     *
     * Creates a regular (width+1)×(height+1) node grid, samples the SDF at every node,
     * then applies marching squares on each grid cell.  Memory usage scales as O(W·H),
     * regardless of where the surface is located.
     */
    class DenseMarchingSquares
    {
    public:
        using SdfFunction = std::function<float(Eigen::Vector2f const&)>;

        /**
         * @brief Construct a dense grid over the given domain at the given resolution.
         * @param bounds     2D bounding box of the domain.
         * @param resolution Cell size in mm.  Determines grid dimensions.
         */
        DenseMarchingSquares(BoundingBox2D const& bounds, float resolution);

        /**
         * @brief Sample the SDF at all grid nodes.
         * @param sdf SDF function R²→R.
         */
        void populate(SdfFunction const& sdf);

        /**
         * @brief Extract contour segments from the dense grid.
         * @param isoValue Iso-surface threshold.
         * @return Vector of line segments.
         */
        [[nodiscard]] std::vector<ContourSegment> extractSegments(float isoValue = 0.0F) const;

        /// Memory used by the value grid in bytes.
        [[nodiscard]] std::size_t getMemoryBytes() const;

        [[nodiscard]] std::size_t getGridWidth() const { return m_gridWidth; }
        [[nodiscard]] std::size_t getGridHeight() const { return m_gridHeight; }

        /// Number of grid cells (each cell = one marching-squares kernel invocation).
        [[nodiscard]] std::size_t getCellCount() const
        {
            return (m_gridWidth > 0U && m_gridHeight > 0U) ? (m_gridWidth - 1U) * (m_gridHeight - 1U)
                                                           : 0U;
        }

    private:
        BoundingBox2D m_bounds;
        float m_resolution;
        std::size_t m_gridWidth{0U};   ///< Number of grid nodes in X
        std::size_t m_gridHeight{0U};  ///< Number of grid nodes in Y
        std::vector<float> m_values;   ///< Flat array, row-major: m_values[iy * m_gridWidth + ix]

        [[nodiscard]] float getValue(std::size_t ix, std::size_t iy) const
        {
            return m_values[iy * m_gridWidth + ix];
        }
    };

} // namespace gladius::slicer
