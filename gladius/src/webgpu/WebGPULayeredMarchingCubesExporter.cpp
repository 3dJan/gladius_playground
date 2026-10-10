#include "webgpu/WebGPULayeredMarchingCubesExporter.h"

#include "io/MeshExportJob.h"
#include "webgpu/WebGPUSdfEvaluationRequest.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gladius::webgpu
{
    namespace
    {
        constexpr std::size_t kMaximumGridSamples = 20'000'000u;
        constexpr std::array<std::size_t, 4> kGridResolutions{65u, 129u, 193u, 257u};
        constexpr std::array<std::array<std::uint8_t, 4>, 6> kCubeTetrahedra{{
          {{0u, 1u, 3u, 7u}},
          {{0u, 1u, 5u, 7u}},
          {{0u, 2u, 3u, 7u}},
          {{0u, 2u, 6u, 7u}},
          {{0u, 4u, 5u, 7u}},
          {{0u, 4u, 6u, 7u}}}};

        [[nodiscard]] bool checkedCube(std::size_t const resolution,
                                       std::size_t & sampleCount) noexcept
        {
            if (resolution == 0u || resolution > std::numeric_limits<std::size_t>::max() / resolution)
            {
                return false;
            }
            auto const plane = resolution * resolution;
            if (plane > std::numeric_limits<std::size_t>::max() / resolution)
            {
                return false;
            }
            sampleCount = plane * resolution;
            return sampleCount <= kMaximumGridSamples;
        }

        [[nodiscard]] bool isCancelled(io::MeshExportCancellationPredicate const & shouldCancel)
        {
            return shouldCancel && shouldCancel();
        }

        [[nodiscard]] Eigen::Vector3f interpolate(Eigen::Vector3f const & first,
                                                   Eigen::Vector3f const & second,
                                                   float const firstValue,
                                                   float const secondValue)
        {
            float const denominator = firstValue - secondValue;
            float amount = std::abs(denominator) > 1.0e-20F ? firstValue / denominator : 0.5F;
            amount = std::clamp(amount, 0.0F, 1.0F);
            return first + amount * (second - first);
        }

        class GridSurfaceBuilder final
        {
          public:
            GridSurfaceBuilder(std::size_t const resolution,
                               Eigen::Vector3f const & minimum,
                               Eigen::Vector3f const & spacing,
                               std::vector<float> const & values)
                : m_resolution(resolution)
                , m_planeSize(resolution * resolution)
                , m_minimum(minimum)
                , m_spacing(spacing)
                , m_values(values)
            {
            }

            [[nodiscard]] io::IndexedTriangleMesh build(
              io::MeshExportCancellationPredicate const & shouldCancel,
              WebGPULayeredMarchingCubesExporter::ProgressCallback const & reportProgress)
            {
                io::IndexedTriangleMesh mesh;
                std::unordered_map<std::uint64_t, std::uint32_t> edgeVertices;
                auto const cellCount = m_resolution - 1u;
                for (std::size_t z = 0u; z < cellCount; ++z)
                {
                    if (isCancelled(shouldCancel))
                    {
                        throw io::MeshExportCancelled{};
                    }
                    for (std::size_t y = 0u; y < cellCount; ++y)
                    {
                        for (std::size_t x = 0u; x < cellCount; ++x)
                        {
                            polygonizeCube(x, y, z, mesh, edgeVertices);
                        }
                    }
                    if (reportProgress)
                    {
                        reportProgress(0.55 + 0.4 * static_cast<double>(z + 1u) /
                                                   static_cast<double>(cellCount),
                                       "Extracting uniform-grid isosurface");
                    }
                }
                if (mesh.indices.empty())
                {
                    throw std::runtime_error("WebGPU layered marching-cubes produced an empty mesh");
                }
                if (auto const error = mesh.validationError())
                {
                    throw std::runtime_error("WebGPU layered marching-cubes produced an invalid mesh: " +
                                             *error);
                }
                return mesh;
            }

          private:
            [[nodiscard]] std::size_t gridIndex(std::size_t const x,
                                                std::size_t const y,
                                                std::size_t const z) const noexcept
            {
                return z * m_planeSize + y * m_resolution + x;
            }

            [[nodiscard]] Eigen::Vector3f gridPosition(std::size_t const index) const
            {
                auto const z = index / m_planeSize;
                auto const y = (index / m_resolution) % m_resolution;
                auto const x = index % m_resolution;
                return m_minimum + m_spacing.cwiseProduct(Eigen::Vector3f{
                  static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
            }

            [[nodiscard]] std::uint32_t edgeVertex(
              std::size_t const first,
              std::size_t const second,
              std::unordered_map<std::uint64_t, std::uint32_t> & edgeVertices,
              io::IndexedTriangleMesh & mesh)
            {
                                auto zeroEndpoint = std::numeric_limits<std::size_t>::max();
                                if (m_values[first] == 0.0F)
                                {
                                    zeroEndpoint = first;
                                }
                                else if (m_values[second] == 0.0F)
                                {
                                    zeroEndpoint = second;
                                }
                                if (zeroEndpoint != std::numeric_limits<std::size_t>::max())
                                {
                                        std::uint64_t const key = (std::uint64_t{1} << 62u) |
                                                                                            static_cast<std::uint64_t>(zeroEndpoint);
                                        auto const found = edgeVertices.find(key);
                                        if (found != edgeVertices.end())
                                        {
                                                return found->second;
                                        }
                                        if (mesh.positions.size() >= std::numeric_limits<std::uint32_t>::max())
                                        {
                                                throw std::length_error(
                                                    "WebGPU layered marching-cubes mesh has too many vertices");
                                        }
                                        auto const vertexIndex = static_cast<std::uint32_t>(mesh.positions.size());
                                        mesh.positions.push_back(gridPosition(zeroEndpoint));
                                        edgeVertices.emplace(key, vertexIndex);
                                        return vertexIndex;
                                }

                auto const low = std::min(first, second);
                auto const high = std::max(first, second);
                                std::uint64_t const key = (std::uint64_t{1} << 63u) |
                                                                                    (static_cast<std::uint64_t>(low) << 32u) |
                                                                                    static_cast<std::uint64_t>(high);
                auto const found = edgeVertices.find(key);
                if (found != edgeVertices.end())
                {
                    return found->second;
                }

                Eigen::Vector3f const position = interpolate(gridPosition(first),
                                                              gridPosition(second),
                                                              m_values[first],
                                                              m_values[second]);
                if (mesh.positions.size() >= std::numeric_limits<std::uint32_t>::max())
                {
                    throw std::length_error("WebGPU layered marching-cubes mesh has too many vertices");
                }
                auto const vertexIndex = static_cast<std::uint32_t>(mesh.positions.size());
                mesh.positions.push_back(position);
                edgeVertices.emplace(key, vertexIndex);
                return vertexIndex;
            }

            void emitTriangle(std::array<std::size_t, 3> const & edgeStarts,
                              std::array<std::size_t, 3> const & edgeEnds,
                              Eigen::Vector3f const & outward,
                              std::unordered_map<std::uint64_t, std::uint32_t> & edgeVertices,
                              io::IndexedTriangleMesh & mesh)
            {
                std::array<std::uint32_t, 3> triangle{};
                for (std::size_t corner = 0u; corner < 3u; ++corner)
                {
                    triangle[corner] = edgeVertex(
                      edgeStarts[corner], edgeEnds[corner], edgeVertices, mesh);
                }
                auto const & first = mesh.positions[triangle[0]];
                auto const & second = mesh.positions[triangle[1]];
                auto const & third = mesh.positions[triangle[2]];
                Eigen::Vector3f const firstEdge = second - first;
                Eigen::Vector3f const secondEdge = third - first;
                Eigen::Vector3f const normal = firstEdge.cross(secondEdge);
                if (normal.squaredNorm() <= 1.0e-20F)
                {
                    return;
                }
                if (normal.dot(outward) < 0.0F)
                {
                    std::swap(triangle[1], triangle[2]);
                }
                mesh.indices.insert(mesh.indices.end(), triangle.begin(), triangle.end());
            }

            void polygonizeTetrahedron(
              std::array<std::size_t, 4> const & tetra,
              std::unordered_map<std::uint64_t, std::uint32_t> & edgeVertices,
              io::IndexedTriangleMesh & mesh)
            {
                std::array<std::size_t, 4> inside{};
                std::array<std::size_t, 4> outside{};
                std::size_t insideCount = 0u;
                std::size_t outsideCount = 0u;
                for (auto const vertex : tetra)
                {
                    if (m_values[vertex] < 0.0F)
                    {
                        inside[insideCount++] = vertex;
                    }
                    else
                    {
                        outside[outsideCount++] = vertex;
                    }
                }
                if (insideCount == 0u || insideCount == 4u)
                {
                    return;
                }

                Eigen::Vector3f insideCenter = Eigen::Vector3f::Zero();
                Eigen::Vector3f outsideCenter = Eigen::Vector3f::Zero();
                for (std::size_t index = 0u; index < insideCount; ++index)
                {
                    insideCenter += gridPosition(inside[index]);
                }
                for (std::size_t index = 0u; index < outsideCount; ++index)
                {
                    outsideCenter += gridPosition(outside[index]);
                }
                insideCenter /= static_cast<float>(insideCount);
                outsideCenter /= static_cast<float>(outsideCount);
                Eigen::Vector3f const outward = outsideCenter - insideCenter;

                if (insideCount == 1u || outsideCount == 1u)
                {
                    bool const oneInside = insideCount == 1u;
                    auto const pivot = oneInside ? inside[0] : outside[0];
                    auto const & other = oneInside ? outside : inside;
                    std::array<std::size_t, 3> starts{};
                    std::array<std::size_t, 3> ends{};
                    for (std::size_t index = 0u; index < 3u; ++index)
                    {
                        starts[index] = pivot;
                        ends[index] = other[index];
                    }
                    emitTriangle(starts, ends, outward, edgeVertices, mesh);
                    return;
                }

                std::array<std::size_t, 3> const startsFirst{
                  inside[0], inside[0], inside[1]};
                std::array<std::size_t, 3> const endsFirst{
                  outside[0], outside[1], outside[1]};
                std::array<std::size_t, 3> const startsSecond{
                  inside[0], inside[1], inside[1]};
                std::array<std::size_t, 3> const endsSecond{
                  outside[0], outside[0], outside[1]};
                emitTriangle(startsFirst, endsFirst, outward, edgeVertices, mesh);
                emitTriangle(startsSecond, endsSecond, outward, edgeVertices, mesh);
            }

            void polygonizeCube(std::size_t const x,
                                std::size_t const y,
                                std::size_t const z,
                                io::IndexedTriangleMesh & mesh,
                                std::unordered_map<std::uint64_t, std::uint32_t> & edgeVertices)
            {
                std::array<std::size_t, 8> corners{};
                for (std::size_t corner = 0u; corner < corners.size(); ++corner)
                {
                    corners[corner] = gridIndex(x + (corner & 1u),
                                                y + ((corner >> 1u) & 1u),
                                                z + ((corner >> 2u) & 1u));
                }
                for (auto const & tetraIndices : kCubeTetrahedra)
                {
                    std::array<std::size_t, 4> tetra{};
                    for (std::size_t index = 0u; index < tetra.size(); ++index)
                    {
                        tetra[index] = corners[tetraIndices[index]];
                    }
                    polygonizeTetrahedron(tetra, edgeVertices, mesh);
                }
            }

            std::size_t m_resolution;
            std::size_t m_planeSize;
            Eigen::Vector3f m_minimum;
            Eigen::Vector3f m_spacing;
            std::vector<float> const & m_values;
        };
    } // namespace

    io::IndexedTriangleMesh WebGPULayeredMarchingCubesExporter::generateMesh(
      std::shared_ptr<WebGPUComputeContext> context,
      compute::RenderSceneSnapshot const & snapshot,
      compute::BoundsResult const & bounds,
      std::size_t const qualityLevel,
      io::MeshExportCancellationPredicate shouldCancel,
      ProgressCallback reportProgress)
    {
        auto pumpContext = context;
        return compute::runBlocking(generateMeshAsync(std::move(context),
                                                      snapshot,
                                                      bounds,
                                                      qualityLevel,
                                                      std::move(shouldCancel),
                                                      std::move(reportProgress)),
                                    [pumpContext] { pumpContext->processEvents(); });
    }

    compute::AsyncTask<io::IndexedTriangleMesh> WebGPULayeredMarchingCubesExporter::generateMeshAsync(
      std::shared_ptr<WebGPUComputeContext> context,
      compute::RenderSceneSnapshot const & snapshot,
      compute::BoundsResult bounds,
      std::size_t const qualityLevel,
      io::MeshExportCancellationPredicate shouldCancel,
      ProgressCallback reportProgress)
    {
        if (!context || !context->isValid())
        {
            throw std::runtime_error("WebGPU compute context is unavailable");
        }
        if (!snapshot.isValid() || !bounds.isUsable() || !bounds.modelBounds.has_value())
        {
            throw std::invalid_argument(
              "WebGPU layered marching-cubes requires a valid scene and authoritative model bounds");
        }
        if (isCancelled(shouldCancel))
        {
            throw io::MeshExportCancelled{};
        }

        auto const resolution = kGridResolutions[std::min(qualityLevel, kGridResolutions.size() - 1u)];
        std::size_t sampleCount = 0u;
        if (!checkedCube(resolution, sampleCount))
        {
            throw std::length_error("WebGPU layered marching-cubes resolution exceeds the memory budget");
        }

        auto const & modelBounds = *bounds.modelBounds;
        Eigen::Vector3f const modelMin{modelBounds.min[0], modelBounds.min[1], modelBounds.min[2]};
        Eigen::Vector3f const modelMax{modelBounds.max[0], modelBounds.max[1], modelBounds.max[2]};
        Eigen::Vector3f const extent = modelMax - modelMin;
        float const maxExtent = extent.maxCoeff();
        if (!modelMin.allFinite() || !modelMax.allFinite() || !std::isfinite(maxExtent) ||
            maxExtent <= 0.0F)
        {
            throw std::invalid_argument("WebGPU layered marching-cubes received invalid model bounds");
        }
        float const padding = bounds.errorBound +
                              2.0F * maxExtent / static_cast<float>(resolution - 1u);
        Eigen::Vector3f const minimum = modelMin - Eigen::Vector3f::Constant(padding);
        Eigen::Vector3f const maximum = modelMax + Eigen::Vector3f::Constant(padding);
        Eigen::Vector3f const spacing = (maximum - minimum) / static_cast<float>(resolution - 1u);
        if (!minimum.allFinite() || !maximum.allFinite() || !spacing.allFinite() ||
            (spacing.array() <= 0.0F).any())
        {
            throw std::runtime_error("WebGPU layered marching-cubes produced invalid grid coordinates");
        }

        if (reportProgress)
        {
            reportProgress(0.05, "Sampling uniform SDF grid");
        }
        std::vector<float> values(sampleCount);
        auto request = makeSdfEvaluationRequest(snapshot);
        WebGPUSdfEvaluator evaluator{std::move(context)};
        auto const planeSize = resolution * resolution;
        bool const completed = co_await evaluator.evaluateGeneratedAsync(
          request,
          sampleCount,
          [=](std::size_t const offset,
              std::size_t const count,
              std::vector<std::array<float, 3>> & positions)
          {
              positions.reserve(count);
              for (std::size_t index = offset; index < offset + count; ++index)
              {
                  auto const z = index / planeSize;
                  auto const y = (index / resolution) % resolution;
                  auto const x = index % resolution;
                  Eigen::Vector3f const position = minimum + spacing.cwiseProduct(Eigen::Vector3f{
                    static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
                  positions.push_back({position.x(), position.y(), position.z()});
              }
          },
          [&values, sampleCount, &reportProgress](std::size_t const offset,
                                                  std::vector<float> && batch)
          {
              std::copy(batch.begin(), batch.end(), values.begin() + static_cast<std::ptrdiff_t>(offset));
              if (reportProgress)
              {
                  double const fraction = static_cast<double>(offset + batch.size()) /
                                          static_cast<double>(sampleCount);
                  reportProgress(0.05 + 0.5 * fraction, "Sampling uniform SDF grid");
              }
          },
          shouldCancel);
        if (!completed)
        {
            throw io::MeshExportCancelled{};
        }
        if (isCancelled(shouldCancel))
        {
            throw io::MeshExportCancelled{};
        }

        GridSurfaceBuilder builder{resolution, minimum, spacing, values};
        co_return builder.build(shouldCancel, reportProgress);
    }
} // namespace gladius::webgpu
