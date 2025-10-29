#include "DualContouringPrototype.h"

#include "DualContouringOctree.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <tuple>

#include <Eigen/Geometry>

namespace gladius::dual_contouring
{
    namespace
    {
        [[nodiscard]] float sdfUnitCube(Eigen::Vector3f const & position)
        {
            Eigen::Vector3f const halfExtents = Eigen::Vector3f::Constant(0.5F);
            Eigen::Vector3f const q = position.cwiseAbs() - halfExtents;
            Eigen::Vector3f const outside = q.cwiseMax(Eigen::Vector3f::Zero());
            float const outsideLength = outside.norm();
            float const insideDistance = std::min(std::max({q.x(), q.y(), q.z()}), 0.0F);
            return outsideLength + insideDistance;
        }

        [[nodiscard]] float sdfSphere(Eigen::Vector3f const & position)
        {
            constexpr float radius = 0.6F;
            return position.norm() - radius;
        }

        [[nodiscard]] float sdfCylinderZ(Eigen::Vector3f const & position)
        {
            constexpr float radius = 0.7F;
            constexpr float halfHeight = 0.7F;
            Eigen::Vector2f const radial{position.x(), position.y()};
            float const radialDistance = radial.norm() - radius;
            float const axialDistance = std::abs(position.z()) - halfHeight;
            float const outsideRadial = std::max(radialDistance, 0.0F);
            float const outsideAxial = std::max(axialDistance, 0.0F);
            float const outside = std::sqrt(outsideRadial * outsideRadial + outsideAxial * outsideAxial);
            float const inside = std::min(std::max(radialDistance, axialDistance), 0.0F);
            return outside + inside;
        }

        [[nodiscard]] float smoothMin(float a, float b, float k)
        {
            float const h = std::clamp(0.5F + 0.5F * (b - a) / k, 0.0F, 1.0F);
            return std::lerp(b, a, h) - k * h * (1.0F - h);
        }

        [[nodiscard]] float sdfCylinderBlend(Eigen::Vector3f const & position)
        {
            constexpr float blendFactor = 0.25F;
            float const sphere = sdfSphere(position);
            float const cylinder = sdfCylinderZ(position);
            return smoothMin(sphere, cylinder, blendFactor);
        }

        [[nodiscard]] Eigen::Vector3f clampToBounds(Eigen::Vector3f const & position,
                                                    Eigen::Vector3f const & minBounds,
                                                    Eigen::Vector3f const & maxBounds)
        {
            Eigen::Vector3f clamped = position;
            clamped.x() = std::clamp(clamped.x(), minBounds.x(), maxBounds.x());
            clamped.y() = std::clamp(clamped.y(), minBounds.y(), maxBounds.y());
            clamped.z() = std::clamp(clamped.z(), minBounds.z(), maxBounds.z());
            return clamped;
        }

        [[nodiscard]] Eigen::Vector3f approximateGradient(OctreeBuilder const & builder,
                                                          Eigen::Vector3f const & position)
        {
            Eigen::Vector3f gradient = Eigen::Vector3f::Zero();
            Eigen::Vector3f const spacing =
              builder.gridSpacing().cwiseMax(Eigen::Vector3f::Constant(1e-4F));
            Eigen::Vector3f const minBounds = builder.gridMin();
            Eigen::Vector3f const maxBounds = builder.gridMax();

            for (int axis = 0; axis < 3; ++axis)
            {
                Eigen::Vector3f offset = Eigen::Vector3f::Zero();
                offset(axis) = spacing(axis);
                Eigen::Vector3f const forward = clampToBounds(position + offset, minBounds, maxBounds);
                Eigen::Vector3f const backward = clampToBounds(position - offset, minBounds, maxBounds);
                float const forwardSample = builder.gridSample(forward);
                float const backwardSample = builder.gridSample(backward);
                if (!std::isfinite(forwardSample) || !std::isfinite(backwardSample))
                {
                    continue;
                }
                gradient(axis) = (forwardSample - backwardSample) / (2.0F * spacing(axis));
            }

            return gradient;
        }

        [[nodiscard]] AxisAlignedBoundingBox defaultBounds()
        {
            AxisAlignedBoundingBox bounds{};
            bounds.min = Eigen::Vector3f::Constant(-1.0F);
            bounds.max = Eigen::Vector3f::Constant(1.0F);
            return bounds;
        }

        [[nodiscard]] std::function<float(Eigen::Vector3f const &)> selectField(PrototypeShape shape)
        {
            switch (shape)
            {
            case PrototypeShape::UnitCube:
                return &sdfUnitCube;
            case PrototypeShape::Sphere:
                return &sdfSphere;
            case PrototypeShape::CylinderBlend:
                return &sdfCylinderBlend;
            }
            throw std::invalid_argument("Unsupported prototype shape");
        }

        [[nodiscard]] std::vector<float>
          sampleScalarField(std::function<float(Eigen::Vector3f const &)> const & field,
                            AxisAlignedBoundingBox const & bounds,
                            std::uint32_t resolution)
        {
            if (resolution < 2U)
            {
                throw std::invalid_argument("Resolution must be at least 2");
            }

            std::vector<float> samples;
            samples.resize(static_cast<size_t>(resolution) * static_cast<size_t>(resolution) *
                           static_cast<size_t>(resolution));

            Eigen::Vector3f const spacing = (bounds.max - bounds.min) /
                                             static_cast<float>(resolution - 1U);

            size_t index = 0U;
            for (std::uint32_t z = 0U; z < resolution; ++z)
            {
                float const zPos = bounds.min.z() + spacing.z() * static_cast<float>(z);
                for (std::uint32_t y = 0U; y < resolution; ++y)
                {
                    float const yPos = bounds.min.y() + spacing.y() * static_cast<float>(y);
                    for (std::uint32_t x = 0U; x < resolution; ++x)
                    {
                        float const xPos = bounds.min.x() + spacing.x() * static_cast<float>(x);
                        samples[index++] = field({xPos, yPos, zPos});
                    }
                }
            }

            return samples;
        }

        struct CellData
        {
            int vertexIndex{-1};
            Eigen::Vector3f normal{Eigen::Vector3f::Zero()};
            float centerValue{0.0F};
            bool hasVertex{false};
        };

        [[nodiscard]] size_t flattenIndex(size_t x, size_t y, size_t z, size_t width, size_t height)
        {
            return z * width * height + y * width + x;
        }

        void collectLeafVertices(OctreeNode const & node,
                                 OctreeBuilder const & builder,
                                 float isoValue,
                                 size_t cellWidth,
                                 size_t cellHeight,
                                 size_t cellDepth,
                                 std::vector<CellData> & cells,
                                 PrototypeMesh & mesh)
        {
            if (!node.isLeaf)
            {
                for (auto const & child : node.children)
                {
                    if (child)
                    {
                        collectLeafVertices(*child, builder, isoValue, cellWidth, cellHeight, cellDepth, cells, mesh);
                    }
                }
                return;
            }

            if (!node.hasVertex)
            {
                return;
            }

            Eigen::Vector3f const spacing = builder.gridSpacing();
            Eigen::Vector3f const min = builder.gridMin();
            Eigen::Vector3f const volumeCenter = (min + builder.gridMax()) * 0.5F;
            Eigen::Vector3f const offset = (node.bounds.min - min).cwiseQuotient(spacing);
            Eigen::Vector3i const indices = offset.array().round().matrix().cast<int>();

            if (indices.x() < 0 || indices.y() < 0 || indices.z() < 0)
            {
                return;
            }

            if (indices.x() >= static_cast<int>(cellWidth) ||
                indices.y() >= static_cast<int>(cellHeight) ||
                indices.z() >= static_cast<int>(cellDepth))
            {
                return;
            }

            size_t const flat = flattenIndex(static_cast<size_t>(indices.x()),
                                              static_cast<size_t>(indices.y()),
                                              static_cast<size_t>(indices.z()),
                                              cellWidth,
                                              cellHeight);

            CellData & cell = cells[flat];
            if (cell.hasVertex)
            {
                return;
            }

            cell.vertexIndex = static_cast<int>(mesh.vertices.size());
            mesh.vertices.push_back(node.vertexPosition);

            Eigen::Vector3f normal = node.vertexNormal;
            if (normal.squaredNorm() > 1e-8F)
            {
                normal.normalize();
            }
            else
            {
                Eigen::Vector3f const gradient = approximateGradient(builder, node.vertexPosition);
                if (gradient.squaredNorm() > 1e-8F)
                {
                    normal = gradient.normalized();
                }
                else
                {
                    normal = Eigen::Vector3f{1.0F, 0.0F, 0.0F};
                }
            }

            cell.normal = normal;
            Eigen::Vector3f const toCenter = node.vertexPosition - volumeCenter;
            if (toCenter.squaredNorm() > 1e-8F && cell.normal.dot(toCenter) < 0.0F)
            {
                cell.normal = -cell.normal;
            }
            cell.centerValue = builder.gridSample(node.bounds.center()) - isoValue;
            cell.hasVertex = true;
        }

        [[nodiscard]] bool hasSignChange(float a, float b)
        {
            constexpr float epsilon = 1e-6F;

            auto classify = [](float value) -> int
            {
                if (value > epsilon)
                {
                    return 1;
                }
                if (value < -epsilon)
                {
                    return -1;
                }
                return 0;
            };

            int const signA = classify(a);
            int const signB = classify(b);

            if (signA == 0 && signB == 0)
            {
                return false;
            }

            if (signA == 0 || signB == 0)
            {
                return signA != 0 || signB != 0;
            }

            return signA != signB;
        }

        void emitTriangle(int idx0,
                          int idx1,
                          int idx2,
                          std::array<int, 4> const & vertexIndices,
                          std::array<CellData const *, 4> const & cellRefs,
                          Eigen::Vector3f const & volumeCenter,
                          PrototypeMesh & mesh,
                          PrototypeDiagnostics * diagnostics)
        {
            int v0 = vertexIndices[idx0];
            int v1 = vertexIndices[idx1];
            int v2 = vertexIndices[idx2];

            if (v0 < 0 || v1 < 0 || v2 < 0)
            {
                return;
            }

            Eigen::Vector3f edge1 = mesh.vertices[static_cast<size_t>(v1)] -
                                     mesh.vertices[static_cast<size_t>(v0)];
            Eigen::Vector3f edge2 = mesh.vertices[static_cast<size_t>(v2)] -
                                     mesh.vertices[static_cast<size_t>(v0)];
            auto recomputeNormal = [&]() -> Eigen::Vector3f
            {
                edge1 = mesh.vertices[static_cast<size_t>(v1)] - mesh.vertices[static_cast<size_t>(v0)];
                edge2 = mesh.vertices[static_cast<size_t>(v2)] - mesh.vertices[static_cast<size_t>(v0)];
                return edge1.cross(edge2);
            };

            Eigen::Vector3f normal = recomputeNormal();

            if (normal.squaredNorm() <= 1e-12F)
            {
                return;
            }

            Eigen::Vector3f expected = cellRefs[idx0]->normal +
                                       cellRefs[idx1]->normal +
                                       cellRefs[idx2]->normal;

            if (expected.squaredNorm() > 1e-8F && normal.dot(expected) < 0.0F)
            {
                std::swap(v1, v2);
                normal = recomputeNormal();
            }

            if (normal.squaredNorm() <= 1e-12F)
            {
                return;
            }

            Eigen::Vector3f const centroid = (mesh.vertices[static_cast<size_t>(v0)] +
                                              mesh.vertices[static_cast<size_t>(v1)] +
                                              mesh.vertices[static_cast<size_t>(v2)]) /
                                             3.0F;
            Eigen::Vector3f const outwardReference = centroid - volumeCenter;

            bool flippedByCentroid = false;
            if (outwardReference.squaredNorm() > 1e-8F && normal.dot(outwardReference) < 0.0F)
            {
                std::swap(v1, v2);
                normal = recomputeNormal();
                flippedByCentroid = true;
            }

            if (normal.squaredNorm() <= 1e-12F)
            {
                return;
            }

            if (diagnostics != nullptr && !flippedByCentroid)
            {
                float const expectedNorm = expected.norm();
                if (expectedNorm > 1e-4F)
                {
                    Eigen::Vector3f const expectedUnit = expected / expectedNorm;
                    Eigen::Vector3f const normalUnit = normal.normalized();
                    if (normalUnit.dot(expectedUnit) < 0.0F)
                    {
                        diagnostics->invertedFaceCount += 1U;
                    }
                }
            }

            mesh.faces.emplace_back(Eigen::Vector3i{v0, v1, v2});
            mesh.faceNormals.push_back(normal.normalized());
        }

        void emitQuad(std::array<int, 4> const & vertexIndices,
                      std::array<CellData const *, 4> const & cellRefs,
                      Eigen::Vector3f const & volumeCenter,
                      PrototypeMesh & mesh,
                      PrototypeDiagnostics * diagnostics)
        {
            emitTriangle(0, 1, 2, vertexIndices, cellRefs, volumeCenter, mesh, diagnostics);
            emitTriangle(0, 2, 3, vertexIndices, cellRefs, volumeCenter, mesh, diagnostics);
        }

        void generateFaces(OctreeBuilder const & builder,
                           float isoValue,
                           size_t width,
                           size_t height,
                           size_t depth,
                           size_t cellWidth,
                           size_t cellHeight,
                           size_t cellDepth,
                           std::vector<CellData> const & cells,
                           PrototypeMesh & mesh,
                           PrototypeDiagnostics * diagnostics)
        {
            auto const flatten = [&](int x, int y, int z) -> size_t
            {
                return flattenIndex(static_cast<size_t>(x),
                                      static_cast<size_t>(y),
                                      static_cast<size_t>(z),
                                      cellWidth,
                                      cellHeight);
            };

            Eigen::Vector3f const volumeCenter = (builder.gridMin() + builder.gridMax()) * 0.5F;

            // X-axis edges
            for (size_t i = 0U; i < width - 1U; ++i)
            {
                for (size_t j = 1U; j < height - 1U; ++j)
                {
                    for (size_t k = 1U; k < depth - 1U; ++k)
                    {
                        float const v0 = builder.gridValueAt(i, j, k) - isoValue;
                        float const v1 = builder.gridValueAt(i + 1U, j, k) - isoValue;
                        if (!std::isfinite(v0) || !std::isfinite(v1) || !hasSignChange(v0, v1))
                        {
                            continue;
                        }

                        std::array<std::tuple<int, int, int>, 4> const coords{
                          std::tuple<int, int, int>{static_cast<int>(i), static_cast<int>(j - 1U), static_cast<int>(k - 1U)},
                          std::tuple<int, int, int>{static_cast<int>(i), static_cast<int>(j), static_cast<int>(k - 1U)},
                          std::tuple<int, int, int>{static_cast<int>(i), static_cast<int>(j), static_cast<int>(k)},
                          std::tuple<int, int, int>{static_cast<int>(i), static_cast<int>(j - 1U), static_cast<int>(k)}};

                        std::array<int, 4> vertexIndices{};
                        std::array<CellData const *, 4> cellRefs{};
                        bool skip = false;
                        for (size_t idx = 0U; idx < coords.size(); ++idx)
                        {
                            auto const [cx, cy, cz] = coords[idx];
                            if (cx < 0 || cy < 0 || cz < 0 || cx >= static_cast<int>(cellWidth) ||
                                cy >= static_cast<int>(cellHeight) || cz >= static_cast<int>(cellDepth))
                            {
                                skip = true;
                                break;
                            }
                            size_t const flat = flatten(cx, cy, cz);
                            CellData const & cell = cells.at(flat);
                            if (!cell.hasVertex)
                            {
                                skip = true;
                                break;
                            }
                            vertexIndices[idx] = cell.vertexIndex;
                            cellRefs[idx] = &cell;
                        }
                        if (diagnostics != nullptr)
                        {
                            diagnostics->signChangedEdgeCount += 1U;
                        }
                        if (!skip)
                        {
                            emitQuad(vertexIndices, cellRefs, volumeCenter, mesh, diagnostics);
                        }
                        else if (diagnostics != nullptr)
                        {
                            diagnostics->skippedFaceCount += 1U;
                        }
                    }
                }
            }

            // Y-axis edges
            for (size_t j = 0U; j < height - 1U; ++j)
            {
                for (size_t i = 1U; i < width - 1U; ++i)
                {
                    for (size_t k = 1U; k < depth - 1U; ++k)
                    {
                        float const v0 = builder.gridValueAt(i, j, k) - isoValue;
                        float const v1 = builder.gridValueAt(i, j + 1U, k) - isoValue;
                        if (!std::isfinite(v0) || !std::isfinite(v1) || !hasSignChange(v0, v1))
                        {
                            continue;
                        }

                        std::array<std::tuple<int, int, int>, 4> const coords{
                          std::tuple<int, int, int>{static_cast<int>(i - 1U), static_cast<int>(j), static_cast<int>(k - 1U)},
                          std::tuple<int, int, int>{static_cast<int>(i), static_cast<int>(j), static_cast<int>(k - 1U)},
                          std::tuple<int, int, int>{static_cast<int>(i), static_cast<int>(j), static_cast<int>(k)},
                          std::tuple<int, int, int>{static_cast<int>(i - 1U), static_cast<int>(j), static_cast<int>(k)}};

                        std::array<int, 4> vertexIndices{};
                        std::array<CellData const *, 4> cellRefs{};
                        bool skip = false;
                        for (size_t idx = 0U; idx < coords.size(); ++idx)
                        {
                            auto const [cx, cy, cz] = coords[idx];
                            if (cx < 0 || cy < 0 || cz < 0 || cx >= static_cast<int>(cellWidth) ||
                                cy >= static_cast<int>(cellHeight) || cz >= static_cast<int>(cellDepth))
                            {
                                skip = true;
                                break;
                            }
                            size_t const flat = flatten(cx, cy, cz);
                            CellData const & cell = cells.at(flat);
                            if (!cell.hasVertex)
                            {
                                skip = true;
                                break;
                            }
                            vertexIndices[idx] = cell.vertexIndex;
                            cellRefs[idx] = &cell;
                        }
                        if (diagnostics != nullptr)
                        {
                            diagnostics->signChangedEdgeCount += 1U;
                        }
                        if (!skip)
                        {
                            emitQuad(vertexIndices, cellRefs, volumeCenter, mesh, diagnostics);
                        }
                        else if (diagnostics != nullptr)
                        {
                            diagnostics->skippedFaceCount += 1U;
                        }
                    }
                }
            }

            // Z-axis edges
            for (size_t k = 0U; k < depth - 1U; ++k)
            {
                for (size_t i = 1U; i < width - 1U; ++i)
                {
                    for (size_t j = 1U; j < height - 1U; ++j)
                    {
                        float const v0 = builder.gridValueAt(i, j, k) - isoValue;
                        float const v1 = builder.gridValueAt(i, j, k + 1U) - isoValue;
                        if (!std::isfinite(v0) || !std::isfinite(v1) || !hasSignChange(v0, v1))
                        {
                            continue;
                        }

                        std::array<std::tuple<int, int, int>, 4> const coords{
                          std::tuple<int, int, int>{static_cast<int>(i - 1U), static_cast<int>(j - 1U), static_cast<int>(k)},
                          std::tuple<int, int, int>{static_cast<int>(i), static_cast<int>(j - 1U), static_cast<int>(k)},
                          std::tuple<int, int, int>{static_cast<int>(i), static_cast<int>(j), static_cast<int>(k)},
                          std::tuple<int, int, int>{static_cast<int>(i - 1U), static_cast<int>(j), static_cast<int>(k)}};

                        std::array<int, 4> vertexIndices{};
                        std::array<CellData const *, 4> cellRefs{};
                        bool skip = false;
                        for (size_t idx = 0U; idx < coords.size(); ++idx)
                        {
                            auto const [cx, cy, cz] = coords[idx];
                            if (cx < 0 || cy < 0 || cz < 0 || cx >= static_cast<int>(cellWidth) ||
                                cy >= static_cast<int>(cellHeight) || cz >= static_cast<int>(cellDepth))
                            {
                                skip = true;
                                break;
                            }
                            size_t const flat = flatten(cx, cy, cz);
                            CellData const & cell = cells.at(flat);
                            if (!cell.hasVertex)
                            {
                                skip = true;
                                break;
                            }
                            vertexIndices[idx] = cell.vertexIndex;
                            cellRefs[idx] = &cell;
                        }
                        if (diagnostics != nullptr)
                        {
                            diagnostics->signChangedEdgeCount += 1U;
                        }
                        if (!skip)
                        {
                            emitQuad(vertexIndices, cellRefs, volumeCenter, mesh, diagnostics);
                        }
                        else if (diagnostics != nullptr)
                        {
                            diagnostics->skippedFaceCount += 1U;
                        }
                    }
                }
            }
        }

        PrototypeMesh buildPrototypeMesh(OctreeBuilder const & builder,
                                         OctreeNode const & root,
                                         OctreeBuildConfig const & config,
                                         PrototypeDiagnostics * diagnostics)
        {
            PrototypeMesh mesh{};

            if (diagnostics != nullptr)
            {
                diagnostics->invertedFaceCount = 0U;
            }

            size_t const width = builder.gridWidth();
            size_t const height = builder.gridHeight();
            size_t const depth = builder.gridDepth();

            if (width < 2U || height < 2U || depth < 2U)
            {
                return mesh;
            }

            size_t const cellWidth = width - 1U;
            size_t const cellHeight = height - 1U;
            size_t const cellDepth = depth - 1U;

            std::vector<CellData> cells(cellWidth * cellHeight * cellDepth);

            mesh.faces.reserve(cellWidth * cellHeight * cellDepth * 2U);
            mesh.faceNormals.reserve(cellWidth * cellHeight * cellDepth * 2U);

            collectLeafVertices(root,
                                builder,
                                config.isoValue,
                                cellWidth,
                                cellHeight,
                                cellDepth,
                                cells,
                                mesh);

            if (mesh.vertices.empty())
            {
                if (diagnostics != nullptr)
                {
                    diagnostics->vertexCount = 0U;
                    diagnostics->faceCount = 0U;
                    diagnostics->bounds = AxisAlignedBoundingBox{};
                }
                return mesh;
            }

            generateFaces(builder,
                          config.isoValue,
                          width,
                          height,
                          depth,
                          cellWidth,
                          cellHeight,
                          cellDepth,
                          cells,
                          mesh,
                          diagnostics);

            if (diagnostics != nullptr)
            {
                diagnostics->vertexCount = mesh.vertices.size();
                diagnostics->faceCount = mesh.faces.size();
                diagnostics->bounds = mesh.bounds();
            }

            return mesh;
        }
    }

    AxisAlignedBoundingBox PrototypeMesh::bounds() const
    {
        AxisAlignedBoundingBox bbox{};
        if (vertices.empty())
        {
            bbox.min = Eigen::Vector3f::Zero();
            bbox.max = Eigen::Vector3f::Zero();
            return bbox;
        }

        Eigen::Vector3f min = vertices.front();
        Eigen::Vector3f max = vertices.front();
        for (auto const & vertex : vertices)
        {
            min = min.cwiseMin(vertex);
            max = max.cwiseMax(vertex);
        }
        bbox.min = min;
        bbox.max = max;
        return bbox;
    }

    PrototypeMesh generatePrototypeMesh(PrototypeShape shape,
                                        std::uint32_t resolution,
                                        PrototypeDiagnostics * diagnostics)
    {
        if (resolution < 2U)
        {
            throw std::invalid_argument("Prototype resolution must be at least 2");
        }

        std::uint32_t const cellCount = resolution - 1U;
        if (cellCount < 1U || !std::has_single_bit(cellCount))
        {
            throw std::invalid_argument("Resolution - 1 must be a power of two for uniform octree meshing");
        }

        OctreeBuildConfig config{};
        config.sdfResolution = resolution;
        config.maxDepth = static_cast<size_t>(std::countr_zero(cellCount));
        config.isoValue = 0.0F;
        config.forceUniform = true;

        AxisAlignedBoundingBox const bounds = defaultBounds();
        auto const field = selectField(shape);
        std::vector<float> samples = sampleScalarField(field, bounds, resolution);

        OctreeBuilder builder(bounds, config, resolution, resolution, resolution, std::move(samples));
        OctreeMetrics metrics{};
        auto root = builder.build(metrics);

        return buildPrototypeMesh(builder, *root, config, diagnostics);
    }
}
