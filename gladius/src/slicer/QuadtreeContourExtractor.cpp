#include "QuadtreeContourExtractor.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <unordered_map>

namespace gladius::slicer
{

    // ============================================================================
    // Marching squares lookup table
    //
    //  Corner bit mask:  bit0 = BL, bit1 = BR, bit2 = TL, bit3 = TR
    //  Edge indices:     0 = BOTTOM (BL↔BR)
    //                    1 = RIGHT  (BR↔TR)
    //                    2 = TOP    (TL↔TR)
    //                    3 = LEFT   (BL↔TL)
    //
    //  Each entry stores {seg1_e0, seg1_e1, seg2_e0, seg2_e1}.
    //  -1 means "unused".  Cases 6 and 9 are saddle points producing two segments.
    // ============================================================================

    static constexpr std::array<std::array<int8_t, 4>, 16> MS_TABLE = {{
        {-1, -1, -1, -1},  //  0: 0000  no surface
        { 0,  3, -1, -1},  //  1: 0001  BL         – bottom  → left
        { 0,  1, -1, -1},  //  2: 0010  BR         – bottom  → right
        { 3,  1, -1, -1},  //  3: 0011  BL+BR      – left    → right
        { 2,  3, -1, -1},  //  4: 0100  TL         – top     → left
        { 0,  2, -1, -1},  //  5: 0101  BL+TL      – bottom  → top
        { 0,  3,  1,  2},  //  6: 0110  BR+TL      – SADDLE: (bottom→left) + (right→top)
        { 1,  2, -1, -1},  //  7: 0111  BL+BR+TL   – right   → top
        { 1,  2, -1, -1},  //  8: 1000  TR         – right   → top
        { 0,  1,  3,  2},  //  9: 1001  BL+TR      – SADDLE: (bottom→right) + (left→top)
        { 0,  2, -1, -1},  // 10: 1010  BR+TR      – bottom  → top
        { 2,  3, -1, -1},  // 11: 1011  BL+BR+TR   – top     → left
        { 1,  3, -1, -1},  // 12: 1100  TL+TR      – right   → left
        { 0,  1, -1, -1},  // 13: 1101  BL+TL+TR   – bottom  → right
        { 0,  3, -1, -1},  // 14: 1110  BR+TL+TR   – bottom  → left
        {-1, -1, -1, -1},  // 15: 1111  no surface
    }};

    // Corner pair for each edge: {corner_a_index, corner_b_index}
    // t=0 is at corner_a, t=1 is at corner_b
    static constexpr int EDGE_CORNERS[4][2] = {
        {0, 1},  // edge 0 (bottom): BL → BR
        {1, 3},  // edge 1 (right):  BR → TR
        {2, 3},  // edge 2 (top):    TL → TR
        {0, 2},  // edge 3 (left):   BL → TL
    };

    // ============================================================================
    // QuadtreeContourExtractor – edge crossing helper
    // ============================================================================

    Eigen::Vector2f QuadtreeContourExtractor::computeEdgeCrossing(
        BoundingBox2D const& cellBounds,
        std::array<float, 4> const& corners,
        int edgeIdx,
        float isoValue)
    {
        assert(edgeIdx >= 0 && edgeIdx < 4);

        int const c0 = EDGE_CORNERS[edgeIdx][0];
        int const c1 = EDGE_CORNERS[edgeIdx][1];

        float const v0 = corners[c0];
        float const v1 = corners[c1];
        float const dv = v1 - v0;

        // Clamp t to [0,1] to avoid numerical issues when values are very close
        float const t = (std::abs(dv) > 1e-12F) ? std::clamp((isoValue - v0) / dv, 0.0F, 1.0F)
                                                 : 0.5F;

        float const minX = cellBounds.min.x();
        float const minY = cellBounds.min.y();
        float const maxX = cellBounds.max.x();
        float const maxY = cellBounds.max.y();

        switch (edgeIdx)
        {
        case 0:  // bottom: x varies, y = minY
            return {minX + t * (maxX - minX), minY};
        case 1:  // right: x = maxX, y varies
            return {maxX, minY + t * (maxY - minY)};
        case 2:  // top: x varies, y = maxY
            return {minX + t * (maxX - minX), maxY};
        case 3:  // left: x = minX, y varies
            return {minX, minY + t * (maxY - minY)};
        default:
            return {(minX + maxX) * 0.5F, (minY + maxY) * 0.5F};
        }
    }

    // ============================================================================
    // Per-cell marching squares
    // ============================================================================

    void QuadtreeContourExtractor::extractCellSegments(BoundingBox2D const& cellBounds,
                                                        std::array<float, 4> const& corners,
                                                        float isoValue,
                                                        std::vector<ContourSegment>& outSegments)
    {
        // Build case index: bit i set when corners[i] > isoValue
        // Corner mapping: 0=BL(bit0), 1=BR(bit1), 2=TL(bit2), 3=TR(bit3)
        std::uint8_t caseIdx = 0U;
        for (int i = 0; i < 4; ++i)
        {
            if (corners[i] > isoValue)
            {
                caseIdx |= static_cast<std::uint8_t>(1U << i);
            }
        }

        auto const& entry = MS_TABLE[caseIdx];

        // First segment
        if (entry[0] >= 0 && entry[1] >= 0)
        {
            ContourSegment seg;
            seg.start = computeEdgeCrossing(cellBounds, corners, entry[0], isoValue);
            seg.end = computeEdgeCrossing(cellBounds, corners, entry[1], isoValue);
            if ((seg.end - seg.start).squaredNorm() > 1.0e-18F)
            {
                outSegments.push_back(seg);
            }
        }

        // Optional second segment (saddle cases 6 and 9)
        if (entry[2] >= 0 && entry[3] >= 0)
        {
            ContourSegment seg;
            seg.start = computeEdgeCrossing(cellBounds, corners, entry[2], isoValue);
            seg.end = computeEdgeCrossing(cellBounds, corners, entry[3], isoValue);
            if ((seg.end - seg.start).squaredNorm() > 1.0e-18F)
            {
                outSegments.push_back(seg);
            }
        }
    }

    // ============================================================================
    // populateCornerValues
    // ============================================================================

    std::size_t QuadtreeContourExtractor::populateCornerValues(MortonQuadtree& tree,
                                                                SdfFunction const& sdf,
                                                                float isoValue)
    {
        auto& nodes = tree.getNodesMutable();
        auto const& bounds = tree.getBounds();
        float const rootSize = bounds.getMaxExtent();

        std::size_t intersectingCount = 0U;

        for (auto& node : nodes)
        {
            if (!node.isLeaf)
            {
                continue;
            }

            auto const [gx, gy] = mortonDecode(node.mortonCode);
            auto const cellBounds =
                bounds.getCellBounds(gx, gy, node.depth, rootSize);

            // Sample at 4 corners: BL, BR, TL, TR
            node.cornerValues[0] = sdf({cellBounds.min.x(), cellBounds.min.y()});  // BL
            node.cornerValues[1] = sdf({cellBounds.max.x(), cellBounds.min.y()});  // BR
            node.cornerValues[2] = sdf({cellBounds.min.x(), cellBounds.max.y()});  // TL
            node.cornerValues[3] = sdf({cellBounds.max.x(), cellBounds.max.y()});  // TR

            // Update sign mask and intersection flag
            node.cornerSignMask = 0U;
            bool hasAbove = false;
            bool hasBelow = false;

            for (int i = 0; i < 4; ++i)
            {
                if (node.cornerValues[i] > isoValue)
                {
                    node.cornerSignMask |= static_cast<std::uint8_t>(1U << i);
                    hasAbove = true;
                }
                else
                {
                    hasBelow = true;
                }
            }

            node.isIntersecting = hasAbove && hasBelow;
            if (node.isIntersecting)
            {
                ++intersectingCount;
            }
        }

        return intersectingCount;
    }

    // ============================================================================
    // extractSegments
    // ============================================================================

    std::vector<ContourSegment> QuadtreeContourExtractor::extractSegments(
        MortonQuadtree const& tree, float isoValue) const
    {
        std::vector<ContourSegment> segments;
        // Estimate capacity: each intersecting node produces ~1 segment on average
        segments.reserve(tree.getLeafCount());

        auto const& nodes = tree.getNodes();
        auto const& bounds = tree.getBounds();
        float const rootSize = bounds.getMaxExtent();

        for (auto const& node : nodes)
        {
            if (!node.isLeaf || !node.isIntersecting)
            {
                continue;
            }

            auto const [gx, gy] = mortonDecode(node.mortonCode);
            auto const cellBounds = bounds.getCellBounds(gx, gy, node.depth, rootSize);

            extractCellSegments(cellBounds, node.cornerValues, isoValue, segments);
        }

        return segments;
    }

    // ============================================================================
    // extractPolyLines – greedy chain builder using endpoint hash map
    // ============================================================================

    namespace
    {
        /// Snap a coordinate to a grid at the given tolerance to use as hash key.
        inline std::pair<int64_t, int64_t> snapPoint(Eigen::Vector2f const& p, float tol)
        {
            float const inv = 1.0F / tol;
            return {static_cast<int64_t>(std::round(p.x() * inv)),
                    static_cast<int64_t>(std::round(p.y() * inv))};
        }

        struct PairHash
        {
            std::size_t operator()(std::pair<int64_t, int64_t> const& k) const noexcept
            {
                std::size_t const h0 = std::hash<int64_t>{}(k.first);
                std::size_t const h1 = std::hash<int64_t>{}(k.second);
                return h0 ^ (h1 * 0x9e3779b97f4a7c15ULL);
            }
        };
    }  // namespace

    std::vector<SparsePolyLine> QuadtreeContourExtractor::extractPolyLines(
        MortonQuadtree const& tree, float isoValue, float snapTolerance) const
    {
        auto const segments = extractSegments(tree, isoValue);
        if (segments.empty())
        {
            return {};
        }

        // Build adjacency: snap endpoint → list of (segmentIndex, isStartEnd)
        // isStartEnd: 0 = start endpoint, 1 = end endpoint
        using Key = std::pair<int64_t, int64_t>;
        std::unordered_multimap<Key, std::pair<std::size_t, int>, PairHash> endpointMap;

        for (std::size_t i = 0U; i < segments.size(); ++i)
        {
            endpointMap.emplace(snapPoint(segments[i].start, snapTolerance),
                                std::make_pair(i, 0));
            endpointMap.emplace(snapPoint(segments[i].end, snapTolerance),
                                std::make_pair(i, 1));
        }

        std::vector<bool> used(segments.size(), false);
        std::vector<SparsePolyLine> polylines;

        for (std::size_t startSeg = 0U; startSeg < segments.size(); ++startSeg)
        {
            if (used[startSeg])
            {
                continue;
            }

            SparsePolyLine poly;
            poly.vertices.push_back(segments[startSeg].start);
            poly.vertices.push_back(segments[startSeg].end);
            used[startSeg] = true;

            // Grow chain from the last vertex
            bool extended = true;
            while (extended)
            {
                extended = false;
                auto const tailKey = snapPoint(poly.vertices.back(), snapTolerance);
                auto range = endpointMap.equal_range(tailKey);

                for (auto it = range.first; it != range.second; ++it)
                {
                    auto const [segIdx, endFlag] = it->second;
                    if (used[segIdx])
                    {
                        continue;
                    }

                    used[segIdx] = true;
                    extended = true;

                    if (endFlag == 0)  // tail matched start of next segment
                    {
                        poly.vertices.push_back(segments[segIdx].end);
                    }
                    else  // tail matched end → append reversed
                    {
                        poly.vertices.push_back(segments[segIdx].start);
                    }
                    break;
                }
            }

            // Check if closed
            auto const headKey = snapPoint(poly.vertices.front(), snapTolerance);
            auto const tailKey = snapPoint(poly.vertices.back(), snapTolerance);
            poly.isClosed = (headKey == tailKey);

            polylines.push_back(std::move(poly));
        }

        return polylines;
    }

    // ============================================================================
    // Self-intersection detection
    // ============================================================================

    namespace
    {
        /// 2D cross product of (b-a) × (c-a).
        inline float cross2d(Eigen::Vector2f const& a,
                             Eigen::Vector2f const& b,
                             Eigen::Vector2f const& c)
        {
            return (b.x() - a.x()) * (c.y() - a.y()) -
                   (b.y() - a.y()) * (c.x() - a.x());
        }

        /// Test whether segment (p1,p2) crosses segment (p3,p4) (proper intersection only).
        inline bool segmentsCross(Eigen::Vector2f const& p1, Eigen::Vector2f const& p2,
                                  Eigen::Vector2f const& p3, Eigen::Vector2f const& p4)
        {
            float const d1 = cross2d(p3, p4, p1);
            float const d2 = cross2d(p3, p4, p2);
            float const d3 = cross2d(p1, p2, p3);
            float const d4 = cross2d(p1, p2, p4);

            if (((d1 > 0.0F && d2 < 0.0F) || (d1 < 0.0F && d2 > 0.0F)) &&
                ((d3 > 0.0F && d4 < 0.0F) || (d3 < 0.0F && d4 > 0.0F)))
            {
                return true;
            }
            return false;
        }
    } // namespace

    std::size_t QuadtreeContourExtractor::detectSelfIntersections(
        std::vector<SparsePolyLine> const& polylines)
    {
        // Flatten all segments from all polylines into a single list with polyline ID.
        struct IndexedSegment
        {
            Eigen::Vector2f a;
            Eigen::Vector2f b;
            std::size_t polyIdx;
            std::size_t segIdx;   ///< segment index within polyline
        };

        std::vector<IndexedSegment> allSegs;
        for (std::size_t pi = 0U; pi < polylines.size(); ++pi)
        {
            auto const& verts = polylines[pi].vertices;
            if (verts.size() < 2U)
            {
                continue;
            }
            for (std::size_t si = 0U; si + 1U < verts.size(); ++si)
            {
                allSegs.push_back({verts[si], verts[si + 1U], pi, si});
            }
        }

        std::size_t count = 0U;

        for (std::size_t i = 0U; i < allSegs.size(); ++i)
        {
            for (std::size_t j = i + 1U; j < allSegs.size(); ++j)
            {
                auto const& si = allSegs[i];
                auto const& sj = allSegs[j];

                // Skip adjacent segments within the same polyline (they share an endpoint)
                if (si.polyIdx == sj.polyIdx)
                {
                    auto const diff = (si.segIdx > sj.segIdx)
                                        ? (si.segIdx - sj.segIdx)
                                        : (sj.segIdx - si.segIdx);
                    if (diff <= 1U)
                    {
                        continue;
                    }
                    // Also skip first-last adjacency for closed polylines
                    auto const& poly = polylines[si.polyIdx];
                    if (poly.isClosed && poly.vertices.size() >= 3U)
                    {
                        std::size_t const lastSeg = poly.vertices.size() - 2U;
                        if ((si.segIdx == 0U && sj.segIdx == lastSeg) ||
                            (sj.segIdx == 0U && si.segIdx == lastSeg))
                        {
                            continue;
                        }
                    }
                }

                if (segmentsCross(si.a, si.b, sj.a, sj.b))
                {
                    ++count;
                }
            }
        }

        return count;
    }

    // ============================================================================
    // Memory estimation helpers
    // ============================================================================

    std::size_t QuadtreeContourExtractor::estimateQuadtreeMemoryBytes(std::size_t nodeCount)
    {
        return nodeCount * sizeof(QuadNode);
    }

    std::size_t QuadtreeContourExtractor::estimateDenseGridMemoryBytes(float width,
                                                                        float height,
                                                                        float cellResolution)
    {
        if (cellResolution <= 0.0F)
        {
            return 0U;
        }
        auto const nodesX = static_cast<std::size_t>(std::ceil(width / cellResolution)) + 1U;
        auto const nodesY = static_cast<std::size_t>(std::ceil(height / cellResolution)) + 1U;
        return nodesX * nodesY * sizeof(float);
    }

    // ============================================================================
    // DenseMarchingSquares
    // ============================================================================

    DenseMarchingSquares::DenseMarchingSquares(BoundingBox2D const& bounds, float resolution)
        : m_bounds(bounds)
        , m_resolution(resolution)
    {
        assert(resolution > 0.0F);
        m_gridWidth  = static_cast<std::size_t>(
                           std::ceil(bounds.getWidth() / resolution)) + 1U;
        m_gridHeight = static_cast<std::size_t>(
                           std::ceil(bounds.getHeight() / resolution)) + 1U;
    }

    void DenseMarchingSquares::populate(SdfFunction const& sdf)
    {
        m_values.resize(m_gridWidth * m_gridHeight);

        for (std::size_t iy = 0U; iy < m_gridHeight; ++iy)
        {
            float const y = m_bounds.min.y() + static_cast<float>(iy) * m_resolution;
            for (std::size_t ix = 0U; ix < m_gridWidth; ++ix)
            {
                float const x = m_bounds.min.x() + static_cast<float>(ix) * m_resolution;
                m_values[iy * m_gridWidth + ix] = sdf({x, y});
            }
        }
    }

    std::vector<ContourSegment> DenseMarchingSquares::extractSegments(float isoValue) const
    {
        std::vector<ContourSegment> segments;
        segments.reserve(m_gridWidth * m_gridHeight / 8U);  // rough estimate

        if (m_gridWidth < 2U || m_gridHeight < 2U)
        {
            return segments;
        }

        for (std::size_t iy = 0U; iy < m_gridHeight - 1U; ++iy)
        {
            float const y0 = m_bounds.min.y() + static_cast<float>(iy) * m_resolution;
            float const y1 = y0 + m_resolution;

            for (std::size_t ix = 0U; ix < m_gridWidth - 1U; ++ix)
            {
                float const x0 = m_bounds.min.x() + static_cast<float>(ix) * m_resolution;
                float const x1 = x0 + m_resolution;

                // Corner order: BL=0, BR=1, TL=2, TR=3
                std::array<float, 4> const corners = {
                    getValue(ix,     iy    ),  // BL
                    getValue(ix + 1, iy    ),  // BR
                    getValue(ix,     iy + 1),  // TL
                    getValue(ix + 1, iy + 1),  // TR
                };

                BoundingBox2D const cellBounds{{x0, y0}, {x1, y1}};
                QuadtreeContourExtractor::extractCellSegments(cellBounds, corners, isoValue, segments);
            }
        }

        return segments;
    }

    std::size_t DenseMarchingSquares::getMemoryBytes() const
    {
        return m_values.size() * sizeof(float);
    }

} // namespace gladius::slicer
