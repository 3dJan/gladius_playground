#pragma once

/// @file SurfaceThicknessField.h
/// @brief Precomputed 3D thickness field for surface-aligned shell generation

#include "BBox.h"
#include "kernel/types.h"

#include <eigen3/Eigen/Core>

#include <vector>

namespace gladius::io
{
    /// @brief Configuration for surface-based thickness field generation
    struct SurfaceThicknessFieldConfig
    {
        /// Grid resolution per axis (e.g., 128 → 128³ = 2M voxels)
        /// Higher = more accurate, more memory
        /// Default chosen to balance accuracy and memory (~8MB for floats)
        int gridResolution = 128;

        /// Narrow band width for surface seeding (in voxels)
        /// Vertices are rasterized within this band around their position
        float seedBandWidth = 1.5f;

        /// Maximum propagation distance (in voxels)
        /// If 0, auto-compute from maximum expected shell thickness
        int maxPropagationDistance = 0;

        /// Use OpenVDB morphological dilation vs simple flood fill
        /// OpenVDB is faster for large grids, flood fill is simpler
        bool useOpenVdbDilation = false;

        /// Default (unassigned) thickness value
        /// Used for voxels that don't receive propagated values
        float defaultThickness = 0.0f;
    };

    /// @brief Precomputed 3D thickness field for GPU shell extraction
    ///
    /// This class builds a dense 3D grid where each voxel contains the
    /// cumulative thickness value derived from the surface color at that location.
    /// Surface vertices seed the field, and values propagate inward via dilation.
    class SurfaceThicknessField
    {
    public:
        SurfaceThicknessField() = default;

        /// @brief Build the thickness field from surface mesh data
        ///
        /// @param vertices Surface mesh vertex positions (on SDF=0)
        /// @param colors Volumetric colors sampled at each vertex (linear RGB, [0,1])
        /// @param thicknessLut Precomputed LUT mapping RGB → cumulative thickness
        /// @param lutResolution Resolution of the thickness LUT per axis
        /// @param modelBounds Bounding box of the model
        /// @param config Field generation configuration
        ///
        /// @throws std::runtime_error if vertices/colors size mismatch
        void build(std::vector<Eigen::Vector3f> const& vertices,
                   std::vector<Eigen::Vector3f> const& colors,
                   std::vector<float> const& thicknessLut,
                   int lutResolution,
                   BoundingBox const& modelBounds,
                   SurfaceThicknessFieldConfig const& config = {});

        /// @brief Check if field has been built
        [[nodiscard]] bool isBuilt() const noexcept
        {
            return m_isBuilt;
        }

        /// @brief Get the thickness field as a flat buffer for GPU upload
        ///
        /// Layout: fieldBuffer[z * res² + y * res + x]
        /// Size: resolution³ floats
        [[nodiscard]] std::vector<float> const& getFieldBuffer() const noexcept
        {
            return m_fieldBuffer;
        }

        /// @brief Get the grid resolution (same for all axes)
        [[nodiscard]] int getResolution() const noexcept
        {
            return m_resolution;
        }

        /// @brief Get the world-to-grid transformation matrix
        ///
        /// Transform a world position to grid coordinates:
        ///   gridPos = worldToGrid * worldPos
        /// Grid coordinates are in [0, resolution-1] for points inside the bbox.
        [[nodiscard]] Eigen::Matrix4f const& getWorldToGridTransform() const noexcept
        {
            return m_worldToGrid;
        }

        /// @brief Get the grid-to-world transformation matrix (inverse)
        [[nodiscard]] Eigen::Matrix4f const& getGridToWorldTransform() const noexcept
        {
            return m_gridToWorld;
        }

        /// @brief Get memory usage in bytes
        [[nodiscard]] std::size_t getMemoryUsage() const noexcept;

        /// @brief Sample thickness at a world position (CPU, for testing)
        [[nodiscard]] float sampleAt(Eigen::Vector3f const& worldPos) const;

    private:
        /// Rasterize vertex thicknesses into the grid (seeds the field)
        void rasterizeSurfaceVertices(std::vector<Eigen::Vector3f> const& vertices,
                                      std::vector<float> const& vertexThicknesses);

        /// Propagate thickness values inward using dilation
        void propagateInward(int maxIterations);

        /// Look up thickness from LUT given a color
        [[nodiscard]] float lookupThickness(Eigen::Vector3f const& color,
                                            std::vector<float> const& lut,
                                            int lutResolution) const;

        std::vector<float> m_fieldBuffer; ///< Dense 3D grid (res³ floats)
        std::vector<bool> m_assignedMask; ///< Track which voxels have values
        int m_resolution = 0;
        BBox m_bounds;
        Eigen::Matrix4f m_worldToGrid = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f m_gridToWorld = Eigen::Matrix4f::Identity();
        bool m_isBuilt = false;
    };
} // namespace gladius::io
