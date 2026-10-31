#if defined(GLADIUS_ENABLE_OPENCL) && defined(GLADIUS_ENABLE_WEBGPU)

#include "ComputeContext.h"
#include "ComputeCore.h"
#include "Document.h"
#include "EventLogger.h"
#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#include "compute/ApplicationComputeRuntime.h"
#include "compute/BoundingBoxContracts.h"
#include "compute/GlobalMortonOctree.h"
#include "compute/ManifoldDualContouringGpu.h"
#include "compute/OpenCLManifoldSdfSampler.h"
#include "io/IndexedTriangleMesh.h"
#include "io/ManifoldDualContouringStlExporter.h"
#include "io/SurfaceExtractionOptions.h"
#include "io/WindingRepair.h"
#include "io/3mf/Lib3mfLoader.h"
#include "webgpu/WebGPUComputeContext.h"
#include "webgpu/WebGPUManifoldDualContouringExporter.h"
#include "webgpu/WebGPUManifoldSdfSampler.h"
#include "webgpu/WebGPUSdfShaderComposer.h"

#include <gtest/gtest.h>
#include <lib3mf_abi.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        [[nodiscard]] bool gpuParityTestsEnabled()
        {
            return std::getenv("GLADIUS_SKIP_GPU_TESTS") == nullptr &&
                   std::getenv("GLADIUS_RUN_GPU_TESTS") != nullptr &&
                   std::getenv("GLADIUS_RUN_WEBGPU_TESTS") != nullptr;
        }

        class TemporaryFile
        {
          public:
            explicit TemporaryFile(std::filesystem::path path)
                : m_path(std::move(path))
            {
            }

            ~TemporaryFile()
            {
                std::error_code error;
                std::filesystem::remove(m_path, error);
            }

            TemporaryFile(TemporaryFile const&) = delete;
            TemporaryFile& operator=(TemporaryFile const&) = delete;

            [[nodiscard]] std::filesystem::path const& path() const noexcept
            {
                return m_path;
            }

          private:
            std::filesystem::path m_path;
        };

        [[nodiscard]] std::filesystem::path makeTemporaryStlPath(std::string const& suffix)
        {
            auto const timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            return std::filesystem::temp_directory_path() /
                   ("gladius_mdc_parity_" + suffix + "_" + std::to_string(timestamp) + ".stl");
        }

        template <typename Bounds>
        [[nodiscard]] compute::BoundsResult makeBoundsResult(Bounds const& bounds)
        {
            compute::BoundsResult result;
            result.status = compute::BoundsResultStatus::Ready;
            result.modelBounds = compute::RenderBounds{
                .min = {bounds.min.x, bounds.min.y, bounds.min.z},
                .max = {bounds.max.x, bounds.max.y, bounds.max.z}};
            result.source = compute::BoundsSource::GpuProbe;
            result.guarantee = compute::BoundsGuarantee::NumericalOpenClParity;
            result.authoritative = true;
            result.errorBound = 0.0F;
            return result;
        }

        [[nodiscard]] io::IndexedTriangleMesh readBinaryStlAsTriangleSoup(
            std::filesystem::path const& filePath)
        {
            std::ifstream input(filePath, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error("Failed to open STL file: " + filePath.string());
            }

            std::array<char, 80> header{};
            std::uint32_t triangleCount = 0U;
            input.read(header.data(), static_cast<std::streamsize>(header.size()));
            input.read(reinterpret_cast<char*>(&triangleCount), sizeof(triangleCount));
            if (!input)
            {
                throw std::runtime_error("STL file is missing its binary header");
            }

            std::uintmax_t const expectedSize = 84U + static_cast<std::uintmax_t>(triangleCount) * 50U;
            if (std::filesystem::file_size(filePath) != expectedSize)
            {
                throw std::runtime_error("STL file has an invalid binary triangle payload size");
            }

            io::IndexedTriangleMesh mesh;
            mesh.positions.reserve(static_cast<std::size_t>(triangleCount) * 3U);
            mesh.indices.reserve(static_cast<std::size_t>(triangleCount) * 3U);

            for (std::uint32_t triangle = 0U; triangle < triangleCount; ++triangle)
            {
                std::array<float, 12> record{};
                std::uint16_t attributeByteCount = 0U;
                input.read(reinterpret_cast<char*>(record.data()),
                           static_cast<std::streamsize>(sizeof(record)));
                input.read(reinterpret_cast<char*>(&attributeByteCount), sizeof(attributeByteCount));
                if (!input)
                {
                    throw std::runtime_error("STL file ended before all triangles were read");
                }
                (void)attributeByteCount;

                for (std::size_t vertex = 0U; vertex < 3U; ++vertex)
                {
                    Eigen::Vector3f const position{
                        record[3U + vertex * 3U],
                        record[4U + vertex * 3U],
                        record[5U + vertex * 3U]};
                    if (!position.allFinite())
                    {
                        throw std::runtime_error("STL file contains a non-finite vertex");
                    }

                    std::uint32_t const index = static_cast<std::uint32_t>(mesh.positions.size());
                    mesh.positions.push_back(position);
                    mesh.indices.push_back(index);
                }
            }

            return mesh;
        }

        [[nodiscard]] std::vector<double> computeTriangleSoupComponentVolumes(
            io::IndexedTriangleMesh const& triangleSoup)
        {
            constexpr double weldScale = 1'000'000.0;
            std::map<std::array<std::int64_t, 3>, std::uint32_t> vertexIndices;
            std::vector<Eigen::Vector3f> positions;
            std::vector<std::uint32_t> indices;
            positions.reserve(triangleSoup.positions.size());
            indices.reserve(triangleSoup.indices.size());
            for (auto const sourceIndex : triangleSoup.indices)
            {
                auto const& position = triangleSoup.positions[sourceIndex];
                std::array<std::int64_t, 3> const key{
                    static_cast<std::int64_t>(std::llround(position.x() * weldScale)),
                    static_cast<std::int64_t>(std::llround(position.y() * weldScale)),
                    static_cast<std::int64_t>(std::llround(position.z() * weldScale))};
                auto found = vertexIndices.find(key);
                if (found == vertexIndices.end())
                {
                    std::uint32_t const index = static_cast<std::uint32_t>(positions.size());
                    found = vertexIndices.emplace(key, index).first;
                    positions.push_back(position);
                }
                indices.push_back(found->second);
            }

            std::vector<std::size_t> parents(positions.size());
            for (std::size_t vertex = 0U; vertex < parents.size(); ++vertex)
            {
                parents[vertex] = vertex;
            }
            auto findRoot = [&parents](std::size_t vertex)
            {
                while (parents[vertex] != vertex)
                {
                    parents[vertex] = parents[parents[vertex]];
                    vertex = parents[vertex];
                }
                return vertex;
            };
            auto join = [&parents, &findRoot](std::size_t first, std::size_t second)
            {
                std::size_t const firstRoot = findRoot(first);
                std::size_t const secondRoot = findRoot(second);
                if (firstRoot != secondRoot)
                {
                    parents[secondRoot] = firstRoot;
                }
            };
            for (std::size_t offset = 0U; offset + 2U < indices.size(); offset += 3U)
            {
                join(indices[offset], indices[offset + 1U]);
                join(indices[offset], indices[offset + 2U]);
            }

            std::map<std::size_t, double> volumesByRoot;
            Eigen::Vector3d origin = Eigen::Vector3d::Zero();
            if (!positions.empty())
            {
                origin = positions.front().cast<double>();
            }
            for (std::size_t offset = 0U; offset + 2U < indices.size(); offset += 3U)
            {
                std::size_t const first = indices[offset];
                std::size_t const second = indices[offset + 1U];
                std::size_t const third = indices[offset + 2U];
                Eigen::Vector3d const a = positions[first].cast<double>() - origin;
                Eigen::Vector3d const b = positions[second].cast<double>() - origin;
                Eigen::Vector3d const c = positions[third].cast<double>() - origin;
                volumesByRoot[findRoot(first)] += a.dot(b.cross(c)) / 6.0;
            }

            std::vector<double> volumes;
            volumes.reserve(volumesByRoot.size());
            for (auto const& [root, volume] : volumesByRoot)
            {
                (void)root;
                volumes.push_back(volume);
            }
            std::sort(volumes.begin(), volumes.end(),
                      [](double first, double second)
                      { return std::abs(first) < std::abs(second); });
            return volumes;
        }

        struct AdaptiveOctreeCornerSamples
        {
            std::uint64_t mortonCode{};
            std::uint8_t depth{};
            std::uint8_t internalMask{};
            std::uint16_t edgeMask{};
            std::array<float, 8> cornerValues{};
            std::array<std::uint8_t, 12> edgeComponents{};
            std::size_t vertexCount{};
            std::vector<Eigen::Vector3f> vertexPositions;
        };

        [[nodiscard]] std::vector<AdaptiveOctreeCornerSamples> captureAdaptiveOctreeCornerSamples(
            compute::GlobalMortonOctree const& octree)
        {
            auto const& nodes = octree.getNodes();
            auto const& vertexPositions = octree.getVertexRegistry().getPositions();
            std::vector<AdaptiveOctreeCornerSamples> samples;
            samples.reserve(nodes.size());
            for (auto const& node : nodes)
            {
                AdaptiveOctreeCornerSamples sample{node.mortonCode,
                                                   node.depth,
                                                   node.internalMask,
                                                   node.edgeMask,
                                                   node.cornerValues,
                                                   node.edgeComponents,
                                                   node.vertexIndices.size(),
                                                   {}};
                sample.vertexPositions.reserve(node.vertexIndices.size());
                for (std::uint32_t const vertexIndex : node.vertexIndices)
                {
                    if (vertexIndex < vertexPositions.size())
                    {
                        sample.vertexPositions.push_back(vertexPositions[vertexIndex]);
                    }
                }
                samples.push_back(std::move(sample));
            }
            return samples;
        }

        struct SurfaceTriangle
        {
            Eigen::Vector3f a;
            Eigen::Vector3f b;
            Eigen::Vector3f c;
            Eigen::Vector3f minimum;
            Eigen::Vector3f maximum;
            Eigen::Vector3f centroid;
        };

        struct TriangleBvhNode
        {
            Eigen::Vector3f minimum{Eigen::Vector3f::Zero()};
            Eigen::Vector3f maximum{Eigen::Vector3f::Zero()};
            std::size_t begin{};
            std::size_t count{};
            std::int32_t left{-1};
            std::int32_t right{-1};
        };

        class TriangleBvh final
        {
          public:
            explicit TriangleBvh(io::IndexedTriangleMesh const& mesh)
            {
                std::size_t const triangleCount = mesh.indices.size() / 3U;
                m_triangles.reserve(triangleCount);
                m_triangleOrder.reserve(triangleCount);
                double const floatEpsilon = std::numeric_limits<float>::epsilon();
                double const relativeAreaTolerance = floatEpsilon * floatEpsilon;
                for (std::size_t offset = 0U; offset < mesh.indices.size(); offset += 3U)
                {
                    SurfaceTriangle triangle;
                    triangle.a = mesh.positions[mesh.indices[offset]];
                    triangle.b = mesh.positions[mesh.indices[offset + 1U]];
                    triangle.c = mesh.positions[mesh.indices[offset + 2U]];

                    Eigen::Vector3d const ab = (triangle.b - triangle.a).cast<double>();
                    Eigen::Vector3d const ac = (triangle.c - triangle.a).cast<double>();
                    Eigen::Vector3d const bc = (triangle.c - triangle.b).cast<double>();
                    double const maxEdgeSquared = std::max(
                        {ab.squaredNorm(), ac.squaredNorm(), bc.squaredNorm()});
                    double const crossSquared = ab.cross(ac).squaredNorm();
                    if (maxEdgeSquared == 0.0 ||
                        crossSquared <= maxEdgeSquared * maxEdgeSquared * relativeAreaTolerance)
                    {
                        continue;
                    }

                    triangle.minimum = triangle.a.cwiseMin(triangle.b).cwiseMin(triangle.c);
                    triangle.maximum = triangle.a.cwiseMax(triangle.b).cwiseMax(triangle.c);
                    triangle.centroid = (triangle.a + triangle.b + triangle.c) / 3.0F;
                    m_triangleOrder.push_back(m_triangles.size());
                    m_triangles.push_back(triangle);
                }
                if (!m_triangles.empty())
                {
                    auto const rootNode = buildNode(0U, m_triangles.size());
                    (void)rootNode;
                }
            }

            [[nodiscard]] double distanceSquared(Eigen::Vector3f const& point) const
            {
                if (m_nodes.empty())
                {
                    return std::numeric_limits<double>::infinity();
                }
                return closestDistanceSquared(0U, point, std::numeric_limits<float>::infinity());
            }

          private:
            static constexpr std::size_t kLeafTriangleCount = 8U;

            [[nodiscard]] std::uint32_t buildNode(std::size_t begin, std::size_t end)
            {
                std::uint32_t const nodeIndex = static_cast<std::uint32_t>(m_nodes.size());
                m_nodes.emplace_back();
                Eigen::Vector3f minimum = Eigen::Vector3f::Constant(
                    std::numeric_limits<float>::infinity());
                Eigen::Vector3f maximum = Eigen::Vector3f::Constant(
                    -std::numeric_limits<float>::infinity());
                Eigen::Vector3f centroidMinimum = minimum;
                Eigen::Vector3f centroidMaximum = maximum;
                for (std::size_t index = begin; index < end; ++index)
                {
                    auto const& triangle = m_triangles[m_triangleOrder[index]];
                    minimum = minimum.cwiseMin(triangle.minimum);
                    maximum = maximum.cwiseMax(triangle.maximum);
                    centroidMinimum = centroidMinimum.cwiseMin(triangle.centroid);
                    centroidMaximum = centroidMaximum.cwiseMax(triangle.centroid);
                }
                m_nodes[nodeIndex].minimum = minimum;
                m_nodes[nodeIndex].maximum = maximum;
                if (end - begin <= kLeafTriangleCount)
                {
                    m_nodes[nodeIndex].begin = begin;
                    m_nodes[nodeIndex].count = end - begin;
                    return nodeIndex;
                }

                Eigen::Vector3f const centroidExtent = centroidMaximum - centroidMinimum;
                Eigen::Index axis = 0;
                centroidExtent.maxCoeff(&axis);
                std::size_t const middle = begin + (end - begin) / 2U;
                auto const first = m_triangleOrder.begin() + static_cast<std::ptrdiff_t>(begin);
                auto const median = m_triangleOrder.begin() + static_cast<std::ptrdiff_t>(middle);
                auto const last = m_triangleOrder.begin() + static_cast<std::ptrdiff_t>(end);
                std::nth_element(first, median, last, [this, axis](std::size_t lhs, std::size_t rhs)
                {
                    return m_triangles[lhs].centroid[axis] < m_triangles[rhs].centroid[axis];
                });
                auto const left = buildNode(begin, middle);
                auto const right = buildNode(middle, end);
                m_nodes[nodeIndex].left = static_cast<std::int32_t>(left);
                m_nodes[nodeIndex].right = static_cast<std::int32_t>(right);
                return nodeIndex;
            }

            [[nodiscard]] static float boxDistanceSquared(
                Eigen::Vector3f const& point,
                Eigen::Vector3f const& minimum,
                Eigen::Vector3f const& maximum)
            {
                Eigen::Vector3f const closest = point.cwiseMax(minimum).cwiseMin(maximum);
                return (point - closest).squaredNorm();
            }

            [[nodiscard]] static float pointTriangleDistanceSquared(
                Eigen::Vector3f const& point,
                SurfaceTriangle const& triangle)
            {
                Eigen::Vector3f const ab = triangle.b - triangle.a;
                Eigen::Vector3f const ac = triangle.c - triangle.a;
                Eigen::Vector3f const ap = point - triangle.a;
                float const d1 = ab.dot(ap);
                float const d2 = ac.dot(ap);
                if (d1 <= 0.0F && d2 <= 0.0F)
                {
                    return ap.squaredNorm();
                }

                Eigen::Vector3f const bp = point - triangle.b;
                float const d3 = ab.dot(bp);
                float const d4 = ac.dot(bp);
                if (d3 >= 0.0F && d4 <= d3)
                {
                    return bp.squaredNorm();
                }

                float const vc = d1 * d4 - d3 * d2;
                if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F)
                {
                    float const v = d1 / (d1 - d3);
                    return (point - (triangle.a + v * ab)).squaredNorm();
                }

                Eigen::Vector3f const cp = point - triangle.c;
                float const d5 = ab.dot(cp);
                float const d6 = ac.dot(cp);
                if (d6 >= 0.0F && d5 <= d6)
                {
                    return cp.squaredNorm();
                }

                float const vb = d5 * d2 - d1 * d6;
                if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F)
                {
                    float const w = d2 / (d2 - d6);
                    return (point - (triangle.a + w * ac)).squaredNorm();
                }

                float const va = d3 * d6 - d5 * d4;
                if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F)
                {
                    float const w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                    return (point - (triangle.b + w * (triangle.c - triangle.b))).squaredNorm();
                }

                float const inverseSum = 1.0F / (va + vb + vc);
                float const v = vb * inverseSum;
                float const w = vc * inverseSum;
                return (point - (triangle.a + ab * v + ac * w)).squaredNorm();
            }

            [[nodiscard]] float closestDistanceSquared(
                std::uint32_t nodeIndex,
                Eigen::Vector3f const& point,
                float bestDistanceSquared) const
            {
                auto const& node = m_nodes[nodeIndex];
                if (boxDistanceSquared(point, node.minimum, node.maximum) >= bestDistanceSquared)
                {
                    return bestDistanceSquared;
                }

                if (node.left < 0)
                {
                    for (std::size_t index = node.begin; index < node.begin + node.count; ++index)
                    {
                        bestDistanceSquared = std::min(
                            bestDistanceSquared,
                            pointTriangleDistanceSquared(point, m_triangles[m_triangleOrder[index]]));
                    }
                    return bestDistanceSquared;
                }

                auto const left = static_cast<std::uint32_t>(node.left);
                auto const right = static_cast<std::uint32_t>(node.right);
                float const leftDistance = boxDistanceSquared(
                    point, m_nodes[left].minimum, m_nodes[left].maximum);
                float const rightDistance = boxDistanceSquared(
                    point, m_nodes[right].minimum, m_nodes[right].maximum);
                if (leftDistance < rightDistance)
                {
                    bestDistanceSquared = closestDistanceSquared(left, point, bestDistanceSquared);
                    return closestDistanceSquared(right, point, bestDistanceSquared);
                }
                bestDistanceSquared = closestDistanceSquared(right, point, bestDistanceSquared);
                return closestDistanceSquared(left, point, bestDistanceSquared);
            }

            std::vector<SurfaceTriangle> m_triangles;
            std::vector<std::size_t> m_triangleOrder;
            std::vector<TriangleBvhNode> m_nodes;
        };

        [[nodiscard]] double maximumDirectedSampledSurfaceDistance(
            io::IndexedTriangleMesh const& source,
            TriangleBvh const& target)
        {
            double maximumDistance = 0.0;
            for (std::size_t offset = 0U; offset < source.indices.size(); offset += 3U)
            {
                auto const& a = source.positions[source.indices[offset]];
                auto const& b = source.positions[source.indices[offset + 1U]];
                auto const& c = source.positions[source.indices[offset + 2U]];
                std::array<Eigen::Vector3f, 7> const samples{
                    a, b, c, (a + b) * 0.5F, (b + c) * 0.5F, (c + a) * 0.5F, (a + b + c) / 3.0F};
                for (auto const& sample : samples)
                {
                    maximumDistance = std::max(
                        maximumDistance, std::sqrt(target.distanceSquared(sample)));
                }
            }
            return maximumDistance;
        }

        [[nodiscard]] double maximumSymmetricSampledSurfaceDistance(
            io::IndexedTriangleMesh const& lhs,
            io::IndexedTriangleMesh const& rhs)
        {
            TriangleBvh const lhsBvh{lhs};
            TriangleBvh const rhsBvh{rhs};
            return std::max(
                maximumDirectedSampledSurfaceDistance(lhs, rhsBvh),
                maximumDirectedSampledSurfaceDistance(rhs, lhsBvh));
        }
    }

    struct ParitySettings
    {
        char const* name;
        char const* fixture;
        std::size_t initialDepth;
        std::size_t maxDepth;
        std::size_t expectedComponentCount;
        bool useDialogDefaults;
        float isoValue{0.0F};
        bool useBoundedFastQem{true};
        bool useFinePreset{false};
    };

    class ManifoldDualContouringBackendParity
        : public ::testing::TestWithParam<ParitySettings>
    {
    };

    TEST_P(ManifoldDualContouringBackendParity, StlExportsMatch)
    {
        if (!gpuParityTestsEnabled())
        {
            GTEST_SKIP() << "GPU parity is opt-in; set GLADIUS_RUN_GPU_TESTS=1 and "
                            "GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        ParitySettings const settings = GetParam();
        auto openClContext = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        if (!openClContext->isValid())
        {
            GTEST_SKIP() << "OpenCL context unavailable";
        }
        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        auto document = std::make_shared<Document>(std::make_shared<ComputeCore>(
            openClContext, RequiredCapabilities::ComputeOnly, logger));
        std::filesystem::path const fixtureName{settings.fixture};
        std::filesystem::path fixturePath = std::filesystem::path{"testdata"} / fixtureName;
        if (!std::filesystem::exists(fixturePath))
        {
            fixturePath = std::filesystem::path(__FILE__).parent_path() / "testdata" / fixtureName;
        }
        ASSERT_TRUE(std::filesystem::exists(fixturePath)) << fixturePath;
        ASSERT_NO_THROW(document->load(fixturePath));
        ASSERT_NO_THROW(document->refreshModelBlocking());

        auto const assembly = document->getAssembly();
        auto const flatAssembly = document->getFlatAssembly();
        auto const core = document->getCore();
        ASSERT_NE(assembly, nullptr);
        ASSERT_NE(flatAssembly, nullptr);
        ASSERT_NE(core, nullptr);
        ASSERT_NE(assembly->assemblyModel(), nullptr);
        ASSERT_NE(flatAssembly->assemblyModel(), nullptr);
        ASSERT_TRUE(core->updateParameterBlocking(*assembly));
        ASSERT_TRUE(core->updateBBox());

        auto const modelBounds = core->getBoundingBox();
        ASSERT_TRUE(modelBounds.has_value());
        auto const snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(*flatAssembly, 1U);
        ASSERT_TRUE(snapshot.isValid());

        auto webGpuContext = std::make_shared<webgpu::WebGPUComputeContext>();
        if (!webGpuContext->isValid())
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << webGpuContext->getErrorMessage();
        }

        io::ManifoldDualContouringOptions options{};
        if (settings.useFinePreset)
        {
            options.qualityPreset = io::ManifoldDualContouringQuality::Fine;
            options.applyPreset();
            ASSERT_EQ(options.initialDepth, 6U);
            ASSERT_EQ(options.maxDepth, 8U);
        }
        else if (settings.useDialogDefaults)
        {
            options.qualityPreset = io::ManifoldDualContouringQuality::UltraFine;
            options.applyPreset();
        }
        else
        {
            options.qualityPreset = io::ManifoldDualContouringQuality::Custom;
            options.initialDepth = settings.initialDepth;
            options.maxDepth = settings.maxDepth;
        }
        options.enableGpu = true;
        options.enableCpuFallback = true;
        options.enableCaching = true;
        options.isoValue = settings.isoValue;
        options.minFeatureSize = 0.0F;
        options.enableChunking = true;
        options.enableHierarchicalOctree = true;
        options.enableSharpFeaturePostProcess = false;
        options.projectToSurface = true;
                options.simplificationMethod =
                    settings.useBoundedFastQem ? io::SimplificationMethod::QemFast
                                                                         : io::SimplificationMethod::None;
        options.enableSimplification = settings.useBoundedFastQem;
        options.simplificationTerminationMode = io::SimplificationTerminationMode::ErrorBounded;
        options.simplificationMaxError = 0.05F * 0.05F;

        auto const bounds = makeBoundsResult(*modelBounds);
        ASSERT_TRUE(bounds.isUsable());

        auto const openClProgram = core->getProgramManager().getManifoldDualContouringProgram();
        auto const primitives = core->getPrimitives();
        ASSERT_NE(openClProgram, nullptr);
        ASSERT_NE(primitives, nullptr);

        Eigen::Vector3f const modelMin{
            bounds.modelBounds->min[0], bounds.modelBounds->min[1], bounds.modelBounds->min[2]};
        Eigen::Vector3f const modelMax{
            bounds.modelBounds->max[0], bounds.modelBounds->max[1], bounds.modelBounds->max[2]};
        Eigen::Vector3f const modelSize = modelMax - modelMin;
        float const cellWidth = modelSize.maxCoeff() / static_cast<float>(1U << options.maxDepth);
        float const padding = 2.0F * cellWidth;
        Eigen::Vector3f const domainMin = modelMin - Eigen::Vector3f::Constant(padding);
        Eigen::Vector3f const domainSize = modelSize + Eigen::Vector3f::Constant(2.0F * padding);
        std::size_t const branchSampleDepth = std::min(options.maxDepth, std::size_t{6U});
        std::size_t const subdivisions = std::size_t{1U} << branchSampleDepth;
        std::size_t const samplesPerAxis = subdivisions + 1U;
        std::vector<Eigen::Vector3f> branchSamples;
        branchSamples.reserve(samplesPerAxis * samplesPerAxis * samplesPerAxis);
        for (std::size_t z = 0U; z < samplesPerAxis; ++z)
        {
            for (std::size_t y = 0U; y < samplesPerAxis; ++y)
            {
                for (std::size_t x = 0U; x < samplesPerAxis; ++x)
                {
                    Eigen::Vector3f const fraction{
                        static_cast<float>(x) / static_cast<float>(subdivisions),
                        static_cast<float>(y) / static_cast<float>(subdivisions),
                        static_cast<float>(z) / static_cast<float>(subdivisions)};
                    branchSamples.push_back(domainMin + domainSize.cwiseProduct(fraction));
                }
            }
        }

        auto const openClValues = openClProgram->evaluateSdfBatch(
            branchSamples, *primitives, options.isoValue);
        auto request = compute::SdfEvaluationRequest{
            .positions = {},
            .isoValue = options.isoValue,
            .shaderSource = webgpu::WebGPUSdfShaderComposer::compose(snapshot.analyticEvaluatorWgsl),
            .parameterValues = snapshot.parameterValues};
        webgpu::WebGPUManifoldSdfSampler webGpuSampler{webGpuContext, std::move(request)};
        std::vector<float> webGpuValues;
        compute::ManifoldSdfSamplingCallbacks samplingCallbacks;
        ASSERT_EQ(webGpuSampler.evaluateSdfBatch(
                      branchSamples, options.isoValue, webGpuValues, samplingCallbacks),
                  compute::ManifoldSdfSamplingStatus::Completed);
        ASSERT_EQ(openClValues.size(), branchSamples.size());
        ASSERT_EQ(webGpuValues.size(), branchSamples.size());

        std::size_t signDisagreements = 0U;
        float maximumSdfError = 0.0F;
        for (std::size_t index = 0U; index < branchSamples.size(); ++index)
        {
            ASSERT_TRUE(std::isfinite(openClValues[index]));
            ASSERT_TRUE(std::isfinite(webGpuValues[index]));
            if ((openClValues[index] < 0.0F) != (webGpuValues[index] < 0.0F))
            {
                ++signDisagreements;
            }
            float const error = std::abs(openClValues[index] - webGpuValues[index]);
            maximumSdfError = std::max(maximumSdfError, error);
            float const tolerance = 2.0e-4F + 1.0e-5F * std::abs(openClValues[index]);
            EXPECT_LE(error, tolerance) << "SDF error at branch sample " << index;
        }
        EXPECT_EQ(signDisagreements, 0U)
            << "Sampled octree lattice corners must select the same inside/outside branch";

        compute::GlobalMortonOctreeConfig octreeConfig;
        octreeConfig.initialDepth = options.initialDepth;
        octreeConfig.maxDepth = options.maxDepth;
        octreeConfig.isoValue = options.isoValue;
        octreeConfig.minFeatureSize = options.minFeatureSize;
        octreeConfig.enableAdaptiveRefinement = false;
        octreeConfig.boundingBoxOverride.emplace();
        auto& paddedBounds = *octreeConfig.boundingBoxOverride;
        paddedBounds.min.s[0] = domainMin.x();
        paddedBounds.min.s[1] = domainMin.y();
        paddedBounds.min.s[2] = domainMin.z();
        paddedBounds.min.s[3] = 0.0F;
        Eigen::Vector3f const domainMax = domainMin + domainSize;
        paddedBounds.max.s[0] = domainMax.x();
        paddedBounds.max.s[1] = domainMax.y();
        paddedBounds.max.s[2] = domainMax.z();
        paddedBounds.max.s[3] = 0.0F;

        compute::OpenCLManifoldSdfSampler openClSampler{*core, *openClProgram};
        std::vector<AdaptiveOctreeCornerSamples> openClCornerSamples;
        std::vector<Eigen::Vector3f> openClRegistryPositions;
        io::IndexedTriangleMesh openClOctreeMesh;
        compute::OctreeStats openClOctreeStats;
        {
            compute::GlobalMortonOctree openClOctree{openClSampler};
            openClOctree.build(octreeConfig);
            ASSERT_FALSE(openClOctree.hasSamplingError());
            openClCornerSamples = captureAdaptiveOctreeCornerSamples(openClOctree);
            openClRegistryPositions = openClOctree.getVertexRegistry().getPositions();
            openClOctree.extractMesh(
                openClOctreeMesh.positions,
                openClOctreeMesh.normals,
                openClOctreeMesh.indices);
            openClOctreeStats = openClOctree.getStats();
        }

        auto nodeKeyLess = [](auto const& lhs, auto const& rhs)
        {
            if (lhs.depth != rhs.depth)
            {
                return lhs.depth < rhs.depth;
            }
            return lhs.mortonCode < rhs.mortonCode;
        };
        std::sort(openClCornerSamples.begin(), openClCornerSamples.end(), nodeKeyLess);

        // Permit backend-only roundoff at the iso-surface, below 10% of the absolute SDF budget.
        constexpr float branchSignTieTolerance = 2.0e-5F;
        std::size_t adaptiveNodeKeyDisagreements = 0U;
        std::size_t adaptiveBranchDecisionDisagreements = 0U;
        std::size_t adaptiveComponentDecisionDisagreements = 0U;
        std::size_t adaptiveTieComponentDecisionDisagreements = 0U;
        std::size_t adaptiveVertexPositionDisagreements = 0U;
        std::size_t adaptiveTieVertexPositionDisagreements = 0U;
        std::size_t adaptiveSignDisagreements = 0U;
        std::size_t adaptiveNumericalTieSignDisagreements = 0U;
        std::size_t adaptiveMaterialSignDisagreements = 0U;
        std::size_t adaptiveSdfToleranceViolations = 0U;
        std::size_t adaptiveCornerSampleCount = 0U;
        float maximumAdaptiveSdfError = 0.0F;
        float maximumAdaptiveVertexPositionError = 0.0F;
        float maximumSignDisagreementMagnitude = 0.0F;
        std::vector<Eigen::Vector3f> webGpuRegistryPositions;
        io::IndexedTriangleMesh webGpuOctreeMesh;
        compute::OctreeStats webGpuOctreeStats;
        {
            compute::GlobalMortonOctree webGpuOctree{webGpuSampler};
            webGpuOctree.build(octreeConfig);
            ASSERT_FALSE(webGpuOctree.hasSamplingError());
            webGpuRegistryPositions = webGpuOctree.getVertexRegistry().getPositions();
            webGpuOctree.extractMesh(
                webGpuOctreeMesh.positions,
                webGpuOctreeMesh.normals,
                webGpuOctreeMesh.indices);
            webGpuOctreeStats = webGpuOctree.getStats();
            auto const& webGpuNodes = webGpuOctree.getNodes();
            std::vector<compute::GlobalOctreeNode const*> webGpuNodesByKey;
            webGpuNodesByKey.reserve(webGpuNodes.size());
            for (auto const& node : webGpuNodes)
            {
                webGpuNodesByKey.push_back(&node);
            }
            std::sort(webGpuNodesByKey.begin(), webGpuNodesByKey.end(),
                      [&nodeKeyLess](auto const* lhs, auto const* rhs)
                      { return nodeKeyLess(*lhs, *rhs); });

            std::size_t openClNodeIndex = 0U;
            std::size_t webGpuNodeIndex = 0U;
            while (openClNodeIndex < openClCornerSamples.size() &&
                   webGpuNodeIndex < webGpuNodesByKey.size())
            {
                auto const& openClSample = openClCornerSamples[openClNodeIndex];
                auto const& webGpuNode = *webGpuNodesByKey[webGpuNodeIndex];
                if (nodeKeyLess(openClSample, webGpuNode))
                {
                    ++adaptiveNodeKeyDisagreements;
                    ++openClNodeIndex;
                    continue;
                }
                if (nodeKeyLess(webGpuNode, openClSample))
                {
                    ++adaptiveNodeKeyDisagreements;
                    ++webGpuNodeIndex;
                    continue;
                }

                if (openClSample.internalMask != webGpuNode.internalMask ||
                    openClSample.edgeMask != webGpuNode.edgeMask)
                {
                    ++adaptiveBranchDecisionDisagreements;
                }
                bool hasNearZeroCorner = false;
                for (std::size_t corner = 0U; corner < openClSample.cornerValues.size(); ++corner)
                {
                    hasNearZeroCorner = hasNearZeroCorner ||
                                        std::abs(openClSample.cornerValues[corner]) <=
                                            branchSignTieTolerance ||
                                        std::abs(webGpuNode.cornerValues[corner]) <=
                                            branchSignTieTolerance;
                }
                if (openClSample.edgeComponents != webGpuNode.edgeComponents ||
                    openClSample.vertexCount != webGpuNode.vertexIndices.size())
                {
                    if (hasNearZeroCorner)
                    {
                        ++adaptiveTieComponentDecisionDisagreements;
                    }
                    else
                    {
                        ++adaptiveComponentDecisionDisagreements;
                    }
                }
                float nodeVertexPositionError = 0.0F;
                std::size_t const sharedVertexCount = std::min(
                    openClSample.vertexPositions.size(), webGpuNode.vertexIndices.size());
                auto const& webGpuVertexPositions =
                    webGpuOctree.getVertexRegistry().getPositions();
                for (std::size_t vertex = 0U; vertex < sharedVertexCount; ++vertex)
                {
                    std::uint32_t const webGpuVertexIndex = webGpuNode.vertexIndices[vertex];
                    if (webGpuVertexIndex >= webGpuVertexPositions.size())
                    {
                        continue;
                    }
                    nodeVertexPositionError = std::max(
                        nodeVertexPositionError,
                        (openClSample.vertexPositions[vertex] -
                         webGpuVertexPositions[webGpuVertexIndex]).norm());
                }
                maximumAdaptiveVertexPositionError = std::max(
                    maximumAdaptiveVertexPositionError, nodeVertexPositionError);
                if (nodeVertexPositionError > cellWidth)
                {
                    if (hasNearZeroCorner)
                    {
                        ++adaptiveTieVertexPositionDisagreements;
                    }
                    else
                    {
                        ++adaptiveVertexPositionDisagreements;
                    }
                }
                for (std::size_t corner = 0U; corner < openClSample.cornerValues.size(); ++corner)
                {
                    float const openClValue = openClSample.cornerValues[corner];
                    float const webGpuValue = webGpuNode.cornerValues[corner];
                    ASSERT_TRUE(std::isfinite(openClValue));
                    ASSERT_TRUE(std::isfinite(webGpuValue));
                    ++adaptiveCornerSampleCount;
                    if ((openClValue < 0.0F) != (webGpuValue < 0.0F))
                    {
                        ++adaptiveSignDisagreements;
                        float const disagreementMagnitude =
                            std::max(std::abs(openClValue), std::abs(webGpuValue));
                        maximumSignDisagreementMagnitude =
                            std::max(maximumSignDisagreementMagnitude, disagreementMagnitude);
                        if (disagreementMagnitude <= branchSignTieTolerance)
                        {
                            ++adaptiveNumericalTieSignDisagreements;
                        }
                        else
                        {
                            ++adaptiveMaterialSignDisagreements;
                        }
                    }
                    float const error = std::abs(openClValue - webGpuValue);
                    maximumAdaptiveSdfError = std::max(maximumAdaptiveSdfError, error);
                    float const tolerance = 2.0e-4F + 1.0e-5F * std::abs(openClValue);
                    if (error > tolerance)
                    {
                        ++adaptiveSdfToleranceViolations;
                    }
                }
                ++openClNodeIndex;
                ++webGpuNodeIndex;
            }
            adaptiveNodeKeyDisagreements += openClCornerSamples.size() - openClNodeIndex;
            adaptiveNodeKeyDisagreements += webGpuNodesByKey.size() - webGpuNodeIndex;
        }
        EXPECT_LE(adaptiveNodeKeyDisagreements, adaptiveNumericalTieSignDisagreements)
            << "Adaptive node-set differences must be explained by near-zero sign ties";
        EXPECT_LE(adaptiveBranchDecisionDisagreements, adaptiveNumericalTieSignDisagreements)
            << "Adaptive branch-mask differences must be explained by near-zero sign ties";
        EXPECT_EQ(adaptiveComponentDecisionDisagreements, 0U)
            << "Per-cell manifold components must match away from near-zero SDF corners";
        EXPECT_EQ(adaptiveVertexPositionDisagreements, 0U)
            << "Per-cell QEF positions must remain within one cell away from near-zero SDF corners";
        EXPECT_LE(maximumAdaptiveVertexPositionError, cellWidth)
            << "Corresponding per-cell QEF vertices must remain within one cell";
        EXPECT_EQ(adaptiveMaterialSignDisagreements, 0U)
            << "Stable nonzero SDF signs must match across backends";
        EXPECT_EQ(adaptiveSignDisagreements, adaptiveNumericalTieSignDisagreements);
        EXPECT_LE(maximumSignDisagreementMagnitude, branchSignTieTolerance);
        EXPECT_EQ(adaptiveSdfToleranceViolations, 0U);

        auto const octreeValidationPolicy =
            io::IndexedTriangleMeshValidationPolicy::AllowGeometricDegenerates;
        [[maybe_unused]] auto const openClOctreeWindingRepair =
            io::repairTriangleWindingConsistency(openClOctreeMesh.positions, openClOctreeMesh.indices);
        [[maybe_unused]] auto const webGpuOctreeWindingRepair =
            io::repairTriangleWindingConsistency(webGpuOctreeMesh.positions, webGpuOctreeMesh.indices);
        auto const openClOctreeValidationError = openClOctreeMesh.validationError(octreeValidationPolicy);
        auto const webGpuOctreeValidationError = webGpuOctreeMesh.validationError(octreeValidationPolicy);
        if (settings.useFinePreset)
        {
            EXPECT_FALSE(openClOctreeValidationError.has_value())
                << openClOctreeValidationError.value_or("OpenCL octree mesh validation failed");
            EXPECT_FALSE(webGpuOctreeValidationError.has_value())
                << webGpuOctreeValidationError.value_or("WebGPU octree mesh validation failed");
        }
        else
        {
            ASSERT_FALSE(openClOctreeValidationError.has_value())
                << openClOctreeValidationError.value_or("OpenCL octree mesh validation failed");
            ASSERT_FALSE(webGpuOctreeValidationError.has_value())
                << webGpuOctreeValidationError.value_or("WebGPU octree mesh validation failed");
        }
        auto const openClOctreeTopologyError = openClOctreeValidationError.has_value()
            ? openClOctreeValidationError
            : openClOctreeMesh.closedOrientedTopologyError(octreeValidationPolicy);
        auto const webGpuOctreeTopologyError = webGpuOctreeValidationError.has_value()
            ? webGpuOctreeValidationError
            : webGpuOctreeMesh.closedOrientedTopologyError(octreeValidationPolicy);
        auto printRawOctreeDiagnostics = [](char const* backend,
                                            compute::OctreeStats const& stats,
                                            std::optional<std::string> const& topologyError)
        {
            std::cout << "MDC_WEBGPU_PARITY_RAW backend=" << backend
                      << " totalNodes=" << stats.totalNodes
                      << " intersectingLeaves=" << stats.intersectingLeaves
                      << " vertices=" << stats.vertexCount
                      << " triangles=" << stats.triangleCount
                      << " neighborMisses=" << stats.neighborMisses
                      << " missingVertexQuads=" << stats.missingVertexQuads
                      << " halfDegenerateQuads=" << stats.halfDegenerateQuads
                      << " duplicateOwnedEdgesSkipped=" << stats.duplicateOwnedEdgesSkipped
                      << " perimeterEdgesOverused=" << stats.perimeterEdgesOverused
                      << " zeroDiagonalKeys=" << stats.zeroDiagonalKeys
                      << " boundaryEdges=" << stats.boundaryEdges
                      << " nonManifoldEdges=" << stats.nonManifoldEdges
                      << " topologyError=" << topologyError.value_or("none") << std::endl;
        };
        printRawOctreeDiagnostics("opencl", openClOctreeStats, openClOctreeTopologyError);
        printRawOctreeDiagnostics("webgpu", webGpuOctreeStats, webGpuOctreeTopologyError);
        if (settings.useFinePreset)
        {
            EXPECT_FALSE(openClOctreeTopologyError.has_value())
                << openClOctreeTopologyError.value_or("OpenCL octree mesh is not closed");
            EXPECT_FALSE(webGpuOctreeTopologyError.has_value())
                << webGpuOctreeTopologyError.value_or("WebGPU octree mesh is not closed");
        }
        else
        {
            ASSERT_FALSE(openClOctreeTopologyError.has_value())
                << openClOctreeTopologyError.value_or("OpenCL octree mesh is not closed");
            ASSERT_FALSE(webGpuOctreeTopologyError.has_value())
                << webGpuOctreeTopologyError.value_or("WebGPU octree mesh is not closed");
        }
        std::size_t openClOctreeComponentCount = 0U;
        if (!openClOctreeValidationError.has_value() && !webGpuOctreeValidationError.has_value())
        {
            auto const openClOctreeDiagnostics =
                io::computeIndexedTriangleMeshDiagnostics(openClOctreeMesh);
            auto const webGpuOctreeDiagnostics =
                io::computeIndexedTriangleMeshDiagnostics(webGpuOctreeMesh);
            openClOctreeComponentCount = openClOctreeDiagnostics.connectedComponentCount;
            EXPECT_EQ(openClOctreeDiagnostics.connectedComponentCount,
                      webGpuOctreeDiagnostics.connectedComponentCount);
            if (settings.expectedComponentCount > 0U)
            {
                EXPECT_EQ(openClOctreeDiagnostics.connectedComponentCount,
                          settings.expectedComponentCount);
            }
            else
            {
                EXPECT_GT(openClOctreeDiagnostics.connectedComponentCount, 0U);
            }
        }

        TemporaryFile openClStl{makeTemporaryStlPath(std::string{settings.name} + "_opencl")};
        TemporaryFile webGpuStl{makeTemporaryStlPath(std::string{settings.name} + "_webgpu")};
        io::ManifoldDualContouringStlExporter openClExporter{logger};
        openClExporter.setOptions(options);
        openClExporter.setOutputFormat(io::MeshOutputFileFormat::STL);
        openClExporter.setDocument(document.get());
        openClExporter.setExportWithColors(false);
        openClExporter.beginExport(openClStl.path(), *core);
        while (openClExporter.advanceExport(*core))
        {
        }
        openClExporter.finalize();
        ASSERT_FALSE(openClExporter.hasError()) << openClExporter.errorMessage();
        io::IndexedTriangleMesh const openClMesh = readBinaryStlAsTriangleSoup(openClStl.path());
        ASSERT_NO_THROW(webgpu::WebGPUManifoldDualContouringExporter::exportStl(
            webGpuStl.path(), webGpuContext, snapshot, bounds, options));
        ASSERT_TRUE(std::filesystem::exists(openClStl.path()));
        ASSERT_TRUE(std::filesystem::exists(webGpuStl.path()));

        io::IndexedTriangleMesh const webGpuMesh = readBinaryStlAsTriangleSoup(webGpuStl.path());
        if (settings.isoValue != 0.0F && !settings.useBoundedFastQem)
        {
            auto const openClVertexSdf = openClProgram->evaluateSdfBatch(
                openClMesh.positions, *primitives, options.isoValue);
            ASSERT_EQ(openClVertexSdf.size(), openClMesh.positions.size());
            for (float const sdf : openClVertexSdf)
            {
                EXPECT_NEAR(sdf, 0.0F, 2.0e-4F)
                    << "OpenCL projected vertex must lie on the requested iso-surface";
            }

            std::vector<float> webGpuVertexSdf;
            ASSERT_EQ(webGpuSampler.evaluateSdfBatch(
                          webGpuMesh.positions, options.isoValue, webGpuVertexSdf, samplingCallbacks),
                      compute::ManifoldSdfSamplingStatus::Completed);
            ASSERT_EQ(webGpuVertexSdf.size(), webGpuMesh.positions.size());
            for (float const sdf : webGpuVertexSdf)
            {
                EXPECT_NEAR(sdf, 0.0F, 2.0e-4F)
                    << "WebGPU projected vertex must lie on the requested iso-surface";
            }
        }

        auto const validationPolicy =
            io::IndexedTriangleMeshValidationPolicy::AllowGeometricDegenerates;
        ASSERT_FALSE(openClMesh.validationError(validationPolicy).has_value())
            << openClMesh.validationError(validationPolicy).value_or(
                   "unknown OpenCL mesh validation error");
        ASSERT_FALSE(webGpuMesh.validationError(validationPolicy).has_value())
            << webGpuMesh.validationError(validationPolicy).value_or(
                   "unknown WebGPU mesh validation error");

        auto const openClDiagnostics = io::computeIndexedTriangleMeshDiagnostics(openClMesh);
        auto const webGpuDiagnostics = io::computeIndexedTriangleMeshDiagnostics(webGpuMesh);
        if (settings.expectedComponentCount > 0U)
        {
            auto const openClComponentVolumes = computeTriangleSoupComponentVolumes(openClMesh);
            auto const webGpuComponentVolumes = computeTriangleSoupComponentVolumes(webGpuMesh);
            ASSERT_EQ(openClComponentVolumes.size(), settings.expectedComponentCount);
            ASSERT_EQ(webGpuComponentVolumes.size(), settings.expectedComponentCount);
            for (std::size_t component = 0U; component < openClComponentVolumes.size(); ++component)
            {
                double const openClVolume = openClComponentVolumes[component];
                double const webGpuVolume = webGpuComponentVolumes[component];
                double const componentVolumeTolerance =
                    std::max(1.0e-5, std::abs(openClVolume) * 1.0e-3);
                EXPECT_GT(openClVolume, 0.0);
                EXPECT_GT(webGpuVolume, 0.0);
                EXPECT_LE(std::abs(openClVolume - webGpuVolume), componentVolumeTolerance);
            }
        }
        std::size_t const degeneracyDelta =
            openClDiagnostics.degenerateTriangleCount > webGpuDiagnostics.degenerateTriangleCount
                ? openClDiagnostics.degenerateTriangleCount - webGpuDiagnostics.degenerateTriangleCount
                : webGpuDiagnostics.degenerateTriangleCount - openClDiagnostics.degenerateTriangleCount;
        EXPECT_LE(degeneracyDelta, 2U)
            << "Backends may differ by at most two near-degenerate facets on this fixture";

        // Backend-specific float32 deltas can select different valid QemFast collapses.
        // Bound the comparison by both the finest-cell resolution and configured QEM distance.
        float const qemDistanceTolerance =
            settings.useBoundedFastQem && options.simplificationMaxError > 0.0F
                ? std::sqrt(options.simplificationMaxError)
                : 0.0F;
        float const surfaceDistanceTolerance =
            std::max({0.001F, 0.1F * cellWidth, qemDistanceTolerance});
        double const maximumSurfaceDistance = maximumSymmetricSampledSurfaceDistance(
            openClMesh, webGpuMesh);
        EXPECT_LE(maximumSurfaceDistance, surfaceDistanceTolerance);
        EXPECT_LE((openClDiagnostics.boundsMin - webGpuDiagnostics.boundsMin).cwiseAbs().maxCoeff(),
                  surfaceDistanceTolerance);
        EXPECT_LE((openClDiagnostics.boundsMax - webGpuDiagnostics.boundsMax).cwiseAbs().maxCoeff(),
                  surfaceDistanceTolerance);
        double const volumeTolerance = std::max(1.0e-5, std::abs(openClDiagnostics.signedVolume) * 1.0e-3);
        EXPECT_LE(std::abs(openClDiagnostics.signedVolume - webGpuDiagnostics.signedVolume),
                  volumeTolerance);
        EXPECT_GT(openClDiagnostics.signedVolume, 0.0);
        EXPECT_GT(webGpuDiagnostics.signedVolume, 0.0);

        std::cout << "MDC_WEBGPU_PARITY fixture=" << fixtureName.stem().string()
                  << " setting=" << settings.name
                  << " initialDepth=" << options.initialDepth
                  << " maxDepth=" << options.maxDepth
                  << " branchSampleDepth=" << branchSampleDepth
                  << " simplification="
                  << (settings.useBoundedFastQem ? "QemFast_ErrorBounded" : "None")
                  << " branchSamples=" << branchSamples.size()
                  << " signDisagreements=" << signDisagreements
                  << " maxSdfError=" << maximumSdfError
                  << " adaptiveCornerSamples=" << adaptiveCornerSampleCount
                  << " adaptiveNodeKeyDisagreements=" << adaptiveNodeKeyDisagreements
                  << " adaptiveBranchDecisionDisagreements=" << adaptiveBranchDecisionDisagreements
                  << " adaptiveComponentDecisionDisagreements="
                  << adaptiveComponentDecisionDisagreements
                  << " adaptiveTieComponentDecisionDisagreements="
                  << adaptiveTieComponentDecisionDisagreements
                  << " adaptiveVertexPositionDisagreements="
                  << adaptiveVertexPositionDisagreements
                  << " adaptiveTieVertexPositionDisagreements="
                  << adaptiveTieVertexPositionDisagreements
                  << " maxAdaptiveVertexPositionError="
                  << maximumAdaptiveVertexPositionError
                  << " registryVerticesOpenCL=" << openClRegistryPositions.size()
                  << " registryVerticesWebGPU=" << webGpuRegistryPositions.size()
                  << " adaptiveSignDisagreements=" << adaptiveSignDisagreements
                  << " adaptiveNumericalTieSignDisagreements="
                  << adaptiveNumericalTieSignDisagreements
                  << " adaptiveMaterialSignDisagreements=" << adaptiveMaterialSignDisagreements
                  << " maxAdaptiveSdfError=" << maximumAdaptiveSdfError
                  << " maxSignDisagreementMagnitude=" << maximumSignDisagreementMagnitude
                  << " verticesOpenCL=" << openClDiagnostics.vertexCount
                  << " verticesWebGPU=" << webGpuDiagnostics.vertexCount
                  << " trianglesOpenCL=" << openClDiagnostics.triangleCount
                  << " trianglesWebGPU=" << webGpuDiagnostics.triangleCount
                  << " degenerateTrianglesOpenCL=" << openClDiagnostics.degenerateTriangleCount
                  << " degenerateTrianglesWebGPU=" << webGpuDiagnostics.degenerateTriangleCount
                  << " octreeComponents=" << openClOctreeComponentCount
                  << " maxSampledSurfaceDistance=" << maximumSurfaceDistance
                  << " surfaceTolerance=" << surfaceDistanceTolerance
                  << " signedVolumeOpenCL=" << openClDiagnostics.signedVolume
                  << " signedVolumeWebGPU=" << webGpuDiagnostics.signedVolume << std::endl;
    }

    TEST(WebGPUManifoldDocumentExport, ExportMesh_KeycapsFine_ProducesReadableStlAndClosed3mf)
    {
        if (!gpuParityTestsEnabled())
        {
            GTEST_SKIP() << "GPU export regression is opt-in";
        }
        auto runtime = compute::ApplicationComputeRuntime::createWebGPU();
        ASSERT_TRUE(runtime->isAvailable()) << runtime->getErrorMessage();
        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document document{logger};
        document.setBackendRuntime(runtime.get());
        auto const fixture = std::filesystem::path{"testdata"} / "keycaps.3mf";
        ASSERT_TRUE(std::filesystem::exists(fixture));
        ASSERT_NO_THROW(document.load(fixture));

        io::MeshExportRequest request;
        request.extraction.method = io::SurfaceExtractionMethod::ManifoldDualContouring;
        auto& options = request.extraction.manifoldDualContouring;
        options.qualityPreset = io::ManifoldDualContouringQuality::Fine;
        options.applyPreset();
        options.simplificationMethod = io::SimplificationMethod::QemFast;
        options.enableSimplification = true;
        options.simplificationTerminationMode = io::SimplificationTerminationMode::ErrorBounded;
        options.simplificationMaxError = 0.05F * 0.05F;
        request.color.exportWithColors = false;

        auto openClContext = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        ASSERT_TRUE(openClContext->isValid());
        Document referenceDocument{std::make_shared<ComputeCore>(
            openClContext, RequiredCapabilities::ComputeOnly, logger)};
        ASSERT_NO_THROW(referenceDocument.load(fixture));
        TemporaryFile referenceOutput{makeTemporaryStlPath("keycaps_document_opencl")};
        request.filename = referenceOutput.path();
        request.format = io::MeshExportFormat::Stl;
        ASSERT_NO_THROW(referenceDocument.exportMesh(request));
        auto const referenceMesh = readBinaryStlAsTriangleSoup(referenceOutput.path());
        auto const referenceDiagnostics = io::computeIndexedTriangleMeshDiagnostics(referenceMesh);
        auto const referenceVolumes = computeTriangleSoupComponentVolumes(referenceMesh);
        ASSERT_EQ(referenceVolumes.size(), 2U);

        for (auto const format : {io::MeshExportFormat::Stl, io::MeshExportFormat::ThreeMf})
        {
            SCOPED_TRACE(format == io::MeshExportFormat::Stl ? "STL" : "3MF");
            auto path = makeTemporaryStlPath("keycaps_document");
            if (format == io::MeshExportFormat::ThreeMf)
            {
                path.replace_extension(".3mf");
            }
            TemporaryFile output{path};
            request.filename = path;
            request.format = format;
            ASSERT_NO_THROW(document.exportMesh(request));
            ASSERT_TRUE(std::filesystem::exists(path));
            io::IndexedTriangleMesh mesh;
            if (format == io::MeshExportFormat::Stl)
            {
                mesh = readBinaryStlAsTriangleSoup(path);
                EXPECT_GT(mesh.indices.size(), 0U);
            }
            else
            {
                auto const wrapper = io::loadLib3mfScoped();
                auto const model = wrapper->CreateModel();
                ASSERT_NO_THROW(model->QueryReader("3mf")->ReadFromFile(path.string()));
                EXPECT_TRUE(model->HasPackageThumbnailAttachment());
                auto const objects = model->GetMeshObjects();
                ASSERT_TRUE(objects->MoveNext());
                auto const object = objects->GetCurrentMeshObject();
                for (Lib3MF_uint32 index = 0U; index < object->GetVertexCount(); ++index)
                {
                    auto const vertex = object->GetVertex(index);
                    mesh.positions.emplace_back(vertex.m_Coordinates[0], vertex.m_Coordinates[1], vertex.m_Coordinates[2]);
                }
                for (Lib3MF_uint32 index = 0U; index < object->GetTriangleCount(); ++index)
                {
                    auto const triangle = object->GetTriangle(index);
                    mesh.indices.insert(mesh.indices.end(), {triangle.m_Indices[0], triangle.m_Indices[1], triangle.m_Indices[2]});
                }
                auto const error = mesh.closedOrientedTopologyError();
                EXPECT_FALSE(error.has_value()) << error.value_or("");
            }
            auto const diagnostics = io::computeIndexedTriangleMeshDiagnostics(mesh);
            EXPECT_EQ(diagnostics.degenerateTriangleCount, 0U);
            EXPECT_GT(diagnostics.signedVolume, 0.0);
            EXPECT_NEAR(diagnostics.signedVolume, referenceDiagnostics.signedVolume,
                        referenceDiagnostics.signedVolume * 1.0e-3);
            double const surfaceDistance = maximumSymmetricSampledSurfaceDistance(referenceMesh, mesh);
            EXPECT_LE(surfaceDistance, 0.05);
            EXPECT_LE((diagnostics.boundsMin - referenceDiagnostics.boundsMin).cwiseAbs().maxCoeff(), 0.05F);
            EXPECT_LE((diagnostics.boundsMax - referenceDiagnostics.boundsMax).cwiseAbs().maxCoeff(), 0.05F);
            auto const volumes = computeTriangleSoupComponentVolumes(mesh);
            ASSERT_EQ(volumes.size(), referenceVolumes.size());
            for (std::size_t component = 0U; component < volumes.size(); ++component)
            {
                EXPECT_GT(volumes[component], 0.0);
                EXPECT_NEAR(volumes[component], referenceVolumes[component], referenceVolumes[component] * 1.0e-3);
            }
            std::cout << "MDC_DOCUMENT_PARITY format=" << (format == io::MeshExportFormat::Stl ? "STL" : "3MF")
                      << " surfaceDistance=" << surfaceDistance
                      << " volumeOpenCL=" << referenceDiagnostics.signedVolume
                      << " volumeWebGPU=" << diagnostics.signedVolume << std::endl;
        }
    }

    TEST(WebGPUManifoldDocumentExport, GenerateMesh_UncertainBounds_PreservesEntireSphereWithoutSimplification)
    {
        if (!gpuParityTestsEnabled())
        {
            GTEST_SKIP() << "GPU export regression is opt-in";
        }
        auto context = std::make_shared<webgpu::WebGPUComputeContext>();
        ASSERT_TRUE(context->isValid());
        compute::RenderSceneSnapshot snapshot{
            .sceneGeneration = 1U,
            .requiredCapabilities = compute::RendererCapability::AnalyticRendering,
            .analyticEvaluatorWgsl =
                "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { "
                "return vec4<f32>(vec3<f32>(1.0f), length(position) - 1.0f); }"};
        compute::BoundsResult bounds;
        bounds.status = compute::BoundsResultStatus::Ready;
        bounds.modelBounds = compute::RenderBounds{.min = {-0.8F, -0.8F, -0.8F}, .max = {0.8F, 0.8F, 0.8F}};
        bounds.source = compute::BoundsSource::GpuProbe;
        bounds.guarantee = compute::BoundsGuarantee::NumericalOpenClParity;
        bounds.authoritative = true;
        bounds.errorBound = 0.3F;
        io::ManifoldDualContouringOptions options;
        options.initialDepth = 4U;
        options.maxDepth = 5U;
        options.enableHierarchicalOctree = true;
        options.enableSimplification = false;
        options.simplificationMethod = io::SimplificationMethod::None;
        options.projectToSurface = false;

        auto const mesh = webgpu::WebGPUManifoldDualContouringExporter::generateMesh(
            context, snapshot, bounds, options);

        auto const error = mesh.closedOrientedTopologyError(io::IndexedTriangleMeshValidationPolicy::AllowGeometricDegenerates);
        EXPECT_FALSE(error.has_value()) << error.value_or("");
        auto const diagnostics = io::computeIndexedTriangleMeshDiagnostics(mesh);
        EXPECT_EQ(diagnostics.connectedComponentCount, 1U);
        for (Eigen::Index axis = 0; axis < 3; ++axis)
        {
            EXPECT_NEAR(diagnostics.boundsMin[axis], -1.0F, 0.02F);
            EXPECT_NEAR(diagnostics.boundsMax[axis], 1.0F, 0.02F);
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        QualityPresets,
        ManifoldDualContouringBackendParity,
        ::testing::Values(ParitySettings{"SphereCoarse", "SphereInACage.3mf", 4U, 6U, 2U, false},
                          ParitySettings{"SphereDialogDefaults", "SphereInACage.3mf", 7U, 9U, 2U, true},
                          ParitySettings{"SimpleGyroidCoarse", "SimpleGyroid.3mf", 4U, 6U, 0U, false},
                          ParitySettings{"SphereNonzeroIsoProjection",
                                         "SphereInACage.3mf",
                                         4U,
                                         6U,
                                         2U,
                                         false,
                                         0.05F,
                                         false},
                          ParitySettings{"KeycapsFine", "keycaps.3mf", 6U, 8U, 0U, false, 0.0F, true, true}),
        [](auto const& info) { return info.param.name; });
}

#endif
