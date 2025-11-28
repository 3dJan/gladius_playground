#pragma once

#include <Eigen/Core>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gladius::compute
{
    /**
     * @brief Encodes 3D integer coordinates into a 64-bit Morton code (Z-order curve).
     * 
     * Morton codes preserve spatial locality and enable efficient neighbor lookups.
     * Supports coordinates up to 21 bits each (2^21 = ~2 million cells per axis).
     * 
     * @param x X coordinate
     * @param y Y coordinate 
     * @param z Z coordinate
     * @return Morton-encoded 64-bit code
     */
    [[nodiscard]] std::uint64_t encodeMorton3D(std::uint32_t x, std::uint32_t y, std::uint32_t z);
    
    /**
     * @brief Decodes a 64-bit Morton code back to 3D integer coordinates.
     * 
     * @param code Morton-encoded code
     * @param[out] x X coordinate
     * @param[out] y Y coordinate
     * @param[out] z Z coordinate
     */
    void decodeMorton3D(std::uint64_t code, std::uint32_t& x, std::uint32_t& y, std::uint32_t& z);
    
    /**
     * @brief Computes global Morton code for a position within the domain.
     * 
     * @param position 3D world position
     * @param globalBboxMin Minimum corner of global bounding box
     * @param globalBboxSize Size of global bounding box
     * @param maxDepth Maximum octree depth (determines grid resolution: 2^maxDepth cells per axis)
     * @return Morton code for the cell containing this position
     */
    [[nodiscard]] std::uint64_t computeGlobalMorton(Eigen::Vector3f const& position,
                                                     Eigen::Vector3f const& globalBboxMin,
                                                     Eigen::Vector3f const& globalBboxSize,
                                                     std::uint32_t maxDepth);

    /**
     * @brief Computes Morton code for an edge within a cell.
     * 
     * Edges are encoded by combining the cell Morton code with the edge index.
     * This ensures unique identification of each edge in the global octree.
     * 
     * @param cellMorton Morton code of the cell
     * @param edgeIndex Edge index (0-11)
     * @param depth Cell depth
     * @return Unique edge Morton code
     */
    [[nodiscard]] std::uint64_t computeEdgeMorton(std::uint64_t cellMorton,
                                                   std::uint8_t edgeIndex,
                                                   std::uint8_t depth);

    /**
     * @brief Global vertex registry for shared vertex tracking across the entire domain.
     * 
     * This class ensures that vertices at cell boundaries are shared correctly,
     * enabling watertight mesh generation. Vertices are indexed by Morton codes
     * derived from the global bounding box, not chunk-local coordinates.
     * 
     * Key insight: All octree cells use the same global Morton indexing scheme,
     * so vertices at identical positions will hash to the same Morton code
     * regardless of which chunk/pass discovered them.
     */
    class GlobalVertexRegistry
    {
      public:
        GlobalVertexRegistry() = default;

        /**
         * @brief Initialize registry with global bounding box.
         * 
         * @param bboxMin Minimum corner of global bounding box
         * @param bboxMax Maximum corner of global bounding box
         * @param maxDepth Maximum octree depth
         */
        void initialize(Eigen::Vector3f const& bboxMin,
                        Eigen::Vector3f const& bboxMax,
                        std::uint32_t maxDepth);

        /**
         * @brief Register a vertex for a cell, returning its global index.
         * 
         * If a vertex already exists for this cell, returns the existing index.
         * Otherwise, adds the vertex and returns the new index.
         * 
         * @param cellMorton Morton code of the cell
         * @param position Vertex position
         * @param normal Vertex normal (for shading)
         * @return Global vertex index
         */
        [[nodiscard]] std::uint32_t registerCellVertex(std::uint64_t cellMorton,
                                                        Eigen::Vector3f const& position,
                                                        Eigen::Vector3f const& normal);

        /**
         * @brief Register a vertex for an edge crossing.
         * 
         * Edge vertices are used in manifold dual contouring where cells can have
         * multiple vertices (up to 4 for complex topologies).
         * 
         * @param edgeMorton Morton code of the edge
         * @param position Vertex position
         * @param normal Vertex normal
         * @return Global vertex index
         */
        [[nodiscard]] std::uint32_t registerEdgeVertex(std::uint64_t edgeMorton,
                                                        Eigen::Vector3f const& position,
                                                        Eigen::Vector3f const& normal);

        /**
         * @brief Check if a cell vertex is already registered.
         * 
         * @param cellMorton Morton code of the cell
         * @return true if vertex exists
         */
        [[nodiscard]] bool hasCellVertex(std::uint64_t cellMorton) const;

        /**
         * @brief Get vertex index for a cell (must exist).
         * 
         * @param cellMorton Morton code of the cell
         * @return Global vertex index
         * @throws std::out_of_range if vertex not found
         */
        [[nodiscard]] std::uint32_t getCellVertexIndex(std::uint64_t cellMorton) const;

        /**
         * @brief Try to get vertex index for a cell.
         * 
         * @param cellMorton Morton code of the cell
         * @param[out] index Output vertex index
         * @return true if vertex found, false otherwise
         */
        [[nodiscard]] bool tryGetCellVertexIndex(std::uint64_t cellMorton, std::uint32_t& index) const;

        /**
         * @brief Get all vertex positions.
         */
        [[nodiscard]] std::vector<Eigen::Vector3f> const& getPositions() const
        {
            return m_positions;
        }

        /**
         * @brief Get all vertex normals.
         */
        [[nodiscard]] std::vector<Eigen::Vector3f> const& getNormals() const
        {
            return m_normals;
        }

        /**
         * @brief Get total vertex count.
         */
        [[nodiscard]] std::size_t getVertexCount() const
        {
            return m_positions.size();
        }

        /**
         * @brief Clear all registered vertices.
         */
        void clear();

        /**
         * @brief Reserve memory for expected vertex count.
         */
        void reserve(std::size_t expectedVertexCount);

        /**
         * @brief Get global bounding box minimum.
         */
        [[nodiscard]] Eigen::Vector3f const& getBboxMin() const
        {
            return m_globalBboxMin;
        }

        /**
         * @brief Get global bounding box maximum.
         */
        [[nodiscard]] Eigen::Vector3f const& getBboxMax() const
        {
            return m_globalBboxMax;
        }

        /**
         * @brief Get maximum octree depth.
         */
        [[nodiscard]] std::uint32_t getMaxDepth() const
        {
            return m_maxDepth;
        }

      private:
        Eigen::Vector3f m_globalBboxMin{Eigen::Vector3f::Zero()};
        Eigen::Vector3f m_globalBboxMax{Eigen::Vector3f::Zero()};
        Eigen::Vector3f m_globalBboxSize{Eigen::Vector3f::Zero()};
        std::uint32_t m_maxDepth{0U};

        /// Cell Morton code → global vertex index
        std::unordered_map<std::uint64_t, std::uint32_t> m_cellToVertex;

        /// Edge Morton code → global vertex index
        std::unordered_map<std::uint64_t, std::uint32_t> m_edgeToVertex;

        /// All vertex positions (indexed by global vertex index)
        std::vector<Eigen::Vector3f> m_positions;

        /// All vertex normals (indexed by global vertex index)
        std::vector<Eigen::Vector3f> m_normals;
    };
}
