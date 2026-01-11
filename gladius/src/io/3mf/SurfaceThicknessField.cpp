#include "SurfaceThicknessField.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <stdexcept>
#include <tuple>

#include <eigen3/Eigen/Eigen>

namespace gladius::io
{
    namespace
    {
        /// @brief Convert BoundingBox (float4-based) to Eigen min/max vectors
        void extractBounds(BoundingBox const& bounds, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax)
        {
            outMin = Eigen::Vector3f(bounds.min.x, bounds.min.y, bounds.min.z);
            outMax = Eigen::Vector3f(bounds.max.x, bounds.max.y, bounds.max.z);
        }
    } // namespace

    void SurfaceThicknessField::build(std::vector<Eigen::Vector3f> const& vertices,
                                      std::vector<Eigen::Vector3f> const& colors,
                                      std::vector<float> const& thicknessLut,
                                      int lutResolution,
                                      BoundingBox const& modelBounds,
                                      SurfaceThicknessFieldConfig const& config)
    {
        // Validate inputs
        if (vertices.size() != colors.size())
        {
            throw std::runtime_error("SurfaceThicknessField::build: vertices and colors arrays must have same size");
        }

        if (vertices.empty())
        {
            m_isBuilt = false;
            return;
        }

        auto const expectedLutSize = static_cast<std::size_t>(lutResolution) * lutResolution * lutResolution;
        if (thicknessLut.size() != expectedLutSize)
        {
            throw std::runtime_error("SurfaceThicknessField::build: thicknessLut size does not match lutResolution³");
        }

        m_resolution = config.gridResolution;

        // Convert BoundingBox to internal BBox representation
        Eigen::Vector3f boundsMin;
        Eigen::Vector3f boundsMax;
        extractBounds(modelBounds, boundsMin, boundsMax);

        // Add small padding to bounds to avoid edge issues
        float const padding = 0.01f * (boundsMax - boundsMin).norm();
        
        m_bounds = BBox();
        m_bounds.extend(boundsMin - Eigen::Vector3f::Constant(padding));
        m_bounds.extend(boundsMax + Eigen::Vector3f::Constant(padding));

        // Compute world-to-grid transform
        Eigen::Vector3f const size = m_bounds.getMax() - m_bounds.getMin();
        Eigen::Vector3f const scale(static_cast<float>(m_resolution - 1) / size.x(),
                                    static_cast<float>(m_resolution - 1) / size.y(),
                                    static_cast<float>(m_resolution - 1) / size.z());

        m_worldToGrid = Eigen::Matrix4f::Identity();
        m_worldToGrid(0, 0) = scale.x();
        m_worldToGrid(1, 1) = scale.y();
        m_worldToGrid(2, 2) = scale.z();
        m_worldToGrid(0, 3) = -m_bounds.getMin().x() * scale.x();
        m_worldToGrid(1, 3) = -m_bounds.getMin().y() * scale.y();
        m_worldToGrid(2, 3) = -m_bounds.getMin().z() * scale.z();

        m_gridToWorld = m_worldToGrid.inverse();

        // Allocate field buffer
        auto const totalVoxels = static_cast<std::size_t>(m_resolution) * m_resolution * m_resolution;
        m_fieldBuffer.assign(totalVoxels, config.defaultThickness);
        m_assignedMask.assign(totalVoxels, false);

        // Convert vertex colors to thicknesses using LUT
        std::vector<float> vertexThicknesses(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            vertexThicknesses[i] = lookupThickness(colors[i], thicknessLut, lutResolution);
        }

        // Phase 1: Rasterize surface vertices into grid
        rasterizeSurfaceVertices(vertices, vertexThicknesses);

        // Phase 2: Propagate inward
        int const maxPropDist = config.maxPropagationDistance > 0 ? config.maxPropagationDistance : m_resolution / 4;
        propagateInward(maxPropDist);

        m_isBuilt = true;
    }

    float SurfaceThicknessField::lookupThickness(Eigen::Vector3f const& color,
                                                 std::vector<float> const& lut,
                                                 int lutResolution) const
    {
        // Clamp color to [0, 1] and scale to LUT coordinates
        Eigen::Vector3f const uvw =
            color.cwiseMax(0.0f).cwiseMin(1.0f) * static_cast<float>(lutResolution - 1);

        Eigen::Vector3i const idx = uvw.cast<int>().cwiseMax(0).cwiseMin(lutResolution - 2);
        Eigen::Vector3f const frac = uvw - idx.cast<float>();

        auto lutIdx = [lutResolution](int r, int g, int b) -> std::size_t {
            return (static_cast<std::size_t>(r) * lutResolution + g) * lutResolution + b;
        };

        // Sample 8 corners for trilinear interpolation
        float const c000 = lut[lutIdx(idx.x(), idx.y(), idx.z())];
        float const c001 = lut[lutIdx(idx.x(), idx.y(), idx.z() + 1)];
        float const c010 = lut[lutIdx(idx.x(), idx.y() + 1, idx.z())];
        float const c011 = lut[lutIdx(idx.x(), idx.y() + 1, idx.z() + 1)];
        float const c100 = lut[lutIdx(idx.x() + 1, idx.y(), idx.z())];
        float const c101 = lut[lutIdx(idx.x() + 1, idx.y(), idx.z() + 1)];
        float const c110 = lut[lutIdx(idx.x() + 1, idx.y() + 1, idx.z())];
        float const c111 = lut[lutIdx(idx.x() + 1, idx.y() + 1, idx.z() + 1)];

        // Trilinear interpolation
        float const c00 = c000 + (c001 - c000) * frac.z();
        float const c01 = c010 + (c011 - c010) * frac.z();
        float const c10 = c100 + (c101 - c100) * frac.z();
        float const c11 = c110 + (c111 - c110) * frac.z();

        float const c0 = c00 + (c01 - c00) * frac.y();
        float const c1 = c10 + (c11 - c10) * frac.y();

        return c0 + (c1 - c0) * frac.x();
    }

    void SurfaceThicknessField::rasterizeSurfaceVertices(std::vector<Eigen::Vector3f> const& vertices,
                                                         std::vector<float> const& vertexThicknesses)
    {
        auto voxelIdx = [this](int x, int y, int z) -> std::size_t {
            return (static_cast<std::size_t>(z) * m_resolution + y) * m_resolution + x;
        };

        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            // Transform to grid coordinates
            Eigen::Vector4f const homogeneous(vertices[i].x(), vertices[i].y(), vertices[i].z(), 1.0f);
            Eigen::Vector4f const gridCoord = m_worldToGrid * homogeneous;

            // Round to nearest voxel
            int const x = std::clamp(static_cast<int>(std::round(gridCoord.x())), 0, m_resolution - 1);
            int const y = std::clamp(static_cast<int>(std::round(gridCoord.y())), 0, m_resolution - 1);
            int const z = std::clamp(static_cast<int>(std::round(gridCoord.z())), 0, m_resolution - 1);

            std::size_t const idx = voxelIdx(x, y, z);

            // If already assigned, average (for overlapping vertices)
            if (m_assignedMask[idx])
            {
                m_fieldBuffer[idx] = (m_fieldBuffer[idx] + vertexThicknesses[i]) * 0.5f;
            }
            else
            {
                m_fieldBuffer[idx] = vertexThicknesses[i];
                m_assignedMask[idx] = true;
            }
        }
    }

    void SurfaceThicknessField::propagateInward(int maxIterations)
    {
        auto voxelIdx = [this](int x, int y, int z) -> std::size_t {
            return (static_cast<std::size_t>(z) * m_resolution + y) * m_resolution + x;
        };

        auto isValid = [this](int x, int y, int z) -> bool {
            return x >= 0 && x < m_resolution && y >= 0 && y < m_resolution && z >= 0 && z < m_resolution;
        };

        // 6-connected neighbors
        std::array<std::array<int, 3>, 6> const neighbors = {
            {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};

        // Initialize queue with all assigned voxels
        std::queue<std::tuple<int, int, int, int>> queue; // x, y, z, distance

        for (int z = 0; z < m_resolution; ++z)
        {
            for (int y = 0; y < m_resolution; ++y)
            {
                for (int x = 0; x < m_resolution; ++x)
                {
                    if (m_assignedMask[voxelIdx(x, y, z)])
                    {
                        queue.emplace(x, y, z, 0);
                    }
                }
            }
        }

        // BFS propagation
        while (!queue.empty())
        {
            auto const [x, y, z, dist] = queue.front();
            queue.pop();

            if (dist >= maxIterations)
            {
                continue;
            }

            std::size_t const currentIdx = voxelIdx(x, y, z);
            float const currentThickness = m_fieldBuffer[currentIdx];

            for (auto const& neighbor : neighbors)
            {
                int const nx = x + neighbor[0];
                int const ny = y + neighbor[1];
                int const nz = z + neighbor[2];

                if (!isValid(nx, ny, nz))
                {
                    continue;
                }

                std::size_t const neighborIdx = voxelIdx(nx, ny, nz);

                if (!m_assignedMask[neighborIdx])
                {
                    m_fieldBuffer[neighborIdx] = currentThickness;
                    m_assignedMask[neighborIdx] = true;
                    queue.emplace(nx, ny, nz, dist + 1);
                }
            }
        }
    }

    float SurfaceThicknessField::sampleAt(Eigen::Vector3f const& worldPos) const
    {
        if (!m_isBuilt)
        {
            return 0.0f;
        }

        Eigen::Vector4f const homogeneous(worldPos.x(), worldPos.y(), worldPos.z(), 1.0f);
        Eigen::Vector4f const gridCoord = m_worldToGrid * homogeneous;

        // Clamp to valid range
        Eigen::Vector3f const gridPos(
            std::clamp(gridCoord.x(), 0.0f, static_cast<float>(m_resolution - 1)),
            std::clamp(gridCoord.y(), 0.0f, static_cast<float>(m_resolution - 1)),
            std::clamp(gridCoord.z(), 0.0f, static_cast<float>(m_resolution - 1)));

        // Trilinear interpolation
        Eigen::Vector3i const idx = gridPos.cast<int>().cwiseMax(0).cwiseMin(m_resolution - 2);
        Eigen::Vector3f const frac = gridPos - idx.cast<float>();

        auto voxelIdx = [this](int x, int y, int z) -> std::size_t {
            return (static_cast<std::size_t>(z) * m_resolution + y) * m_resolution + x;
        };

        float const c000 = m_fieldBuffer[voxelIdx(idx.x(), idx.y(), idx.z())];
        float const c001 = m_fieldBuffer[voxelIdx(idx.x(), idx.y(), idx.z() + 1)];
        float const c010 = m_fieldBuffer[voxelIdx(idx.x(), idx.y() + 1, idx.z())];
        float const c011 = m_fieldBuffer[voxelIdx(idx.x(), idx.y() + 1, idx.z() + 1)];
        float const c100 = m_fieldBuffer[voxelIdx(idx.x() + 1, idx.y(), idx.z())];
        float const c101 = m_fieldBuffer[voxelIdx(idx.x() + 1, idx.y(), idx.z() + 1)];
        float const c110 = m_fieldBuffer[voxelIdx(idx.x() + 1, idx.y() + 1, idx.z())];
        float const c111 = m_fieldBuffer[voxelIdx(idx.x() + 1, idx.y() + 1, idx.z() + 1)];

        float const c00 = c000 + (c001 - c000) * frac.z();
        float const c01 = c010 + (c011 - c010) * frac.z();
        float const c10 = c100 + (c101 - c100) * frac.z();
        float const c11 = c110 + (c111 - c110) * frac.z();

        float const c0 = c00 + (c01 - c00) * frac.y();
        float const c1 = c10 + (c11 - c10) * frac.y();

        return c0 + (c1 - c0) * frac.x();
    }

    std::size_t SurfaceThicknessField::getMemoryUsage() const noexcept
    {
        return m_fieldBuffer.size() * sizeof(float) + m_assignedMask.size() * sizeof(bool);
    }

} // namespace gladius::io
