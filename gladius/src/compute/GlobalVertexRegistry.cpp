#include "GlobalVertexRegistry.h"

#include <algorithm>
#include <stdexcept>

namespace gladius::compute
{
    namespace
    {
        /**
         * @brief Spreads bits of a 21-bit integer for Morton encoding.
         * 
         * Inserts two zero bits between each bit of the input.
         * Example: 0b111 → 0b001001001
         */
        [[nodiscard]] std::uint64_t spreadBits3D(std::uint32_t v)
        {
            // Limit to 21 bits (max for 64-bit Morton code with 3 dimensions)
            std::uint64_t x = v & 0x1FFFFFU;

            // Magic bit spreading: insert 2 zeros between each bit
            x = (x | (x << 32U)) & 0x1F00000000FFFFULL;
            x = (x | (x << 16U)) & 0x1F0000FF0000FFULL;
            x = (x | (x << 8U)) & 0x100F00F00F00F00FULL;
            x = (x | (x << 4U)) & 0x10C30C30C30C30C3ULL;
            x = (x | (x << 2U)) & 0x1249249249249249ULL;

            return x;
        }

        /**
         * @brief Compacts bits of a Morton code back to a 21-bit integer.
         * 
         * Extracts every third bit from the input.
         * Inverse of spreadBits3D.
         */
        [[nodiscard]] std::uint32_t compactBits3D(std::uint64_t v)
        {
            v &= 0x1249249249249249ULL;
            v = (v | (v >> 2U)) & 0x10C30C30C30C30C3ULL;
            v = (v | (v >> 4U)) & 0x100F00F00F00F00FULL;
            v = (v | (v >> 8U)) & 0x1F0000FF0000FFULL;
            v = (v | (v >> 16U)) & 0x1F00000000FFFFULL;
            v = (v | (v >> 32U)) & 0x1FFFFFULL;

            return static_cast<std::uint32_t>(v);
        }
    }

    std::uint64_t encodeMorton3D(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    {
        return spreadBits3D(x) | (spreadBits3D(y) << 1U) | (spreadBits3D(z) << 2U);
    }

    void decodeMorton3D(std::uint64_t code, std::uint32_t& x, std::uint32_t& y, std::uint32_t& z)
    {
        x = compactBits3D(code);
        y = compactBits3D(code >> 1U);
        z = compactBits3D(code >> 2U);
    }

    std::uint64_t computeGlobalMorton(Eigen::Vector3f const& position,
                                       Eigen::Vector3f const& globalBboxMin,
                                       Eigen::Vector3f const& globalBboxSize,
                                       std::uint32_t maxDepth)
    {
        // Number of cells per axis at maximum depth
        auto const cellsPerAxis = static_cast<float>(1U << maxDepth);

        // Normalize position to [0, 1) range within bounding box
        Eigen::Vector3f normalized = (position - globalBboxMin).cwiseQuotient(globalBboxSize);

        // Clamp to valid range to handle floating point errors at boundaries
        normalized = normalized.cwiseMax(0.0F).cwiseMin(0.9999999F);

        // Convert to integer cell coordinates
        auto ix = static_cast<std::uint32_t>(normalized.x() * cellsPerAxis);
        auto iy = static_cast<std::uint32_t>(normalized.y() * cellsPerAxis);
        auto iz = static_cast<std::uint32_t>(normalized.z() * cellsPerAxis);

        return encodeMorton3D(ix, iy, iz);
    }

    std::uint64_t computeEdgeMorton(std::uint64_t cellMorton, std::uint8_t edgeIndex, std::uint8_t depth)
    {
        // Compute canonical edge key based on edge's spatial position, not cell Morton.
        // This ensures all 4 cells sharing an edge compute the same key.
        
        // First, decode the cell's position from its path-based Morton code
        std::uint32_t cx = 0U;
        std::uint32_t cy = 0U;
        std::uint32_t cz = 0U;
        
        for (std::uint8_t d = 0U; d < depth; ++d)
        {
            auto const shift = (depth - 1U - d) * 3U;
            auto const octant = static_cast<std::uint8_t>((cellMorton >> shift) & 0x7U);
            
            auto const ox = (octant >> 0U) & 1U;
            auto const oy = (octant >> 1U) & 1U;
            auto const oz = (octant >> 2U) & 1U;
            
            auto const levelScale = 1U << (depth - 1U - d);
            cx += ox * levelScale;
            cy += oy * levelScale;
            cz += oz * levelScale;
        }
        
        // Determine edge axis and offset based on edge index
        // Edges 0-3: X-axis, 4-7: Y-axis, 8-11: Z-axis
        std::uint8_t axis = edgeIndex / 4U;
        std::uint8_t localEdge = edgeIndex % 4U;
        
        // Edge position offset within the cell (0 or 1 for each non-axis dimension)
        // For X-edges (axis=0): localEdge encodes (y,z) position
        // For Y-edges (axis=1): localEdge encodes (x,z) position (reordered)
        // For Z-edges (axis=2): localEdge encodes (x,y) position
        std::uint32_t ex = cx;
        std::uint32_t ey = cy;
        std::uint32_t ez = cz;
        
        switch (axis)
        {
            case 0: // X-edge
                ey += (localEdge & 1U);
                ez += (localEdge >> 1U) & 1U;
                break;
            case 1: // Y-edge
                ex += (localEdge & 1U);
                ez += (localEdge >> 1U) & 1U;
                break;
            case 2: // Z-edge
                ex += (localEdge & 1U);
                ey += (localEdge >> 1U) & 1U;
                break;
        }
        
        // Encode edge as: position-based Morton + axis in upper bits
        std::uint64_t edgeMorton = encodeMorton3D(ex, ey, ez);
        edgeMorton |= (static_cast<std::uint64_t>(axis) << 60U);
        edgeMorton |= (static_cast<std::uint64_t>(depth) << 56U);
        
        return edgeMorton;
    }

    void GlobalVertexRegistry::initialize(Eigen::Vector3f const& bboxMin,
                                           Eigen::Vector3f const& bboxMax,
                                           std::uint32_t maxDepth)
    {
        m_globalBboxMin = bboxMin;
        m_globalBboxMax = bboxMax;
        m_globalBboxSize = bboxMax - bboxMin;
        m_maxDepth = maxDepth;

        clear();
    }

    std::uint32_t GlobalVertexRegistry::registerCellVertex(std::uint64_t cellMorton,
                                                            Eigen::Vector3f const& position,
                                                            Eigen::Vector3f const& normal)
    {
        auto it = m_cellToVertex.find(cellMorton);
        if (it != m_cellToVertex.end())
        {
            return it->second;
        }

        auto const index = static_cast<std::uint32_t>(m_positions.size());
        m_positions.push_back(position);
        m_normals.push_back(normal);
        m_cellToVertex[cellMorton] = index;

        return index;
    }

    std::uint32_t GlobalVertexRegistry::registerEdgeVertex(std::uint64_t edgeMorton,
                                                            Eigen::Vector3f const& position,
                                                            Eigen::Vector3f const& normal)
    {
        auto it = m_edgeToVertex.find(edgeMorton);
        if (it != m_edgeToVertex.end())
        {
            return it->second;
        }

        auto const index = static_cast<std::uint32_t>(m_positions.size());
        m_positions.push_back(position);
        m_normals.push_back(normal);
        m_edgeToVertex[edgeMorton] = index;

        return index;
    }

    bool GlobalVertexRegistry::hasCellVertex(std::uint64_t cellMorton) const
    {
        return m_cellToVertex.find(cellMorton) != m_cellToVertex.end();
    }

    std::uint32_t GlobalVertexRegistry::getCellVertexIndex(std::uint64_t cellMorton) const
    {
        auto it = m_cellToVertex.find(cellMorton);
        if (it == m_cellToVertex.end())
        {
            throw std::out_of_range("Cell vertex not found for Morton code");
        }
        return it->second;
    }

    bool GlobalVertexRegistry::tryGetCellVertexIndex(std::uint64_t cellMorton, std::uint32_t& index) const
    {
        auto it = m_cellToVertex.find(cellMorton);
        if (it == m_cellToVertex.end())
        {
            return false;
        }
        index = it->second;
        return true;
    }

    void GlobalVertexRegistry::clear()
    {
        m_cellToVertex.clear();
        m_edgeToVertex.clear();
        m_positions.clear();
        m_normals.clear();
    }

    void GlobalVertexRegistry::reserve(std::size_t expectedVertexCount)
    {
        m_cellToVertex.reserve(expectedVertexCount);
        m_edgeToVertex.reserve(expectedVertexCount / 3);  // Fewer edge vertices typically
        m_positions.reserve(expectedVertexCount);
        m_normals.reserve(expectedVertexCount);
    }
}
