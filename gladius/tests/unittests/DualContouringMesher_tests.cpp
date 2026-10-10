#include "io/DualContouringMeshBuilder.h"

#include <Eigen/Geometry>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>

namespace gladius_tests
{
    using namespace gladius::dual_contouring;
    using gladius::io::buildIndexedDualContouringMesh;

    namespace
    {
        using Edge = std::pair<std::int32_t, std::int32_t>;

        [[nodiscard]] Edge makeEdge(std::int32_t first, std::int32_t second)
        {
            if (first > second)
            {
                std::swap(first, second);
            }
            return {first, second};
        }
    }

    TEST(DualContouringMesher_Test, UniformSphereProducesClosedOrientedMesh)
    {
        constexpr std::size_t resolution = 17U;
        AxisAlignedBoundingBox bounds{};
        bounds.min = Eigen::Vector3f::Constant(-1.5F);
        bounds.max = Eigen::Vector3f::Constant(1.5F);

        std::vector<float> values;
        values.reserve(resolution * resolution * resolution);
        for (std::size_t z = 0U; z < resolution; ++z)
        {
            for (std::size_t y = 0U; y < resolution; ++y)
            {
                for (std::size_t x = 0U; x < resolution; ++x)
                {
                    Eigen::Vector3f const point{
                      bounds.min.x() + (bounds.max.x() - bounds.min.x()) *
                                         static_cast<float>(x) / static_cast<float>(resolution - 1U),
                      bounds.min.y() + (bounds.max.y() - bounds.min.y()) *
                                         static_cast<float>(y) / static_cast<float>(resolution - 1U),
                      bounds.min.z() + (bounds.max.z() - bounds.min.z()) *
                                         static_cast<float>(z) / static_cast<float>(resolution - 1U)};
                    values.push_back(point.norm() - 1.0F);
                }
            }
        }

        OctreeBuildConfig config{};
        config.sdfResolution = resolution;
        config.maxDepth = 4U;
        config.forceUniform = true;
                auto mesh = buildIndexedDualContouringMesh(
                    bounds, config, resolution, resolution, resolution, std::move(values));
                ASSERT_FALSE(mesh.positions.empty());
                ASSERT_FALSE(mesh.indices.empty());
                EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());

        std::map<Edge, std::pair<std::size_t, std::int32_t>> edgeUse;
                for (std::size_t offset = 0u; offset < mesh.indices.size(); offset += 3u)
        {
                        std::array<std::int32_t, 3> const vertices{
                            static_cast<std::int32_t>(mesh.indices[offset]),
                            static_cast<std::int32_t>(mesh.indices[offset + 1u]),
                            static_cast<std::int32_t>(mesh.indices[offset + 2u])};
            for (std::size_t edgeIndex = 0U; edgeIndex < vertices.size(); ++edgeIndex)
            {
                auto const first = vertices[edgeIndex];
                auto const second = vertices[(edgeIndex + 1U) % vertices.size()];
                auto & use = edgeUse[makeEdge(first, second)];
                ++use.first;
                use.second += first < second ? 1 : -1;
            }
        }

        for (auto const & [edge, use] : edgeUse)
        {
            (void)edge;
            EXPECT_EQ(use.first, 2U) << "Every edge of a closed sphere must have two incident faces";
            EXPECT_EQ(use.second, 0) << "Adjacent faces must use a shared edge in opposite directions";
        }
    }

    TEST(DualContouringMesher_Test, SeparatedSpheresProduceClosedOrientedMeshes)
    {
        constexpr std::size_t resolution = 33U;
        AxisAlignedBoundingBox bounds{};
        bounds.min = Eigen::Vector3f::Constant(-3.0F);
        bounds.max = Eigen::Vector3f::Constant(3.0F);

        Eigen::Vector3f const leftCenter{-1.5F, 0.0F, 0.0F};
        Eigen::Vector3f const rightCenter{1.5F, 0.0F, 0.0F};
        std::vector<float> values;
        values.reserve(resolution * resolution * resolution);
        for (std::size_t z = 0U; z < resolution; ++z)
        {
            for (std::size_t y = 0U; y < resolution; ++y)
            {
                for (std::size_t x = 0U; x < resolution; ++x)
                {
                    Eigen::Vector3f const point{
                      bounds.min.x() + (bounds.max.x() - bounds.min.x()) *
                                         static_cast<float>(x) / static_cast<float>(resolution - 1U),
                      bounds.min.y() + (bounds.max.y() - bounds.min.y()) *
                                         static_cast<float>(y) / static_cast<float>(resolution - 1U),
                      bounds.min.z() + (bounds.max.z() - bounds.min.z()) *
                                         static_cast<float>(z) / static_cast<float>(resolution - 1U)};
                    float const leftDistance = (point - leftCenter).norm() - 0.75F;
                    float const rightDistance = (point - rightCenter).norm() - 0.75F;
                    values.push_back(leftDistance < rightDistance ? leftDistance : rightDistance);
                }
            }
        }

        OctreeBuildConfig config{};
        config.sdfResolution = resolution;
        config.maxDepth = 5U;
        config.forceUniform = true;

        auto const mesh = buildIndexedDualContouringMesh(
          bounds, config, resolution, resolution, resolution, std::move(values));
        EXPECT_FALSE(mesh.positions.empty());
        EXPECT_FALSE(mesh.indices.empty());
        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());

        double leftSignedVolume = 0.0;
        double rightSignedVolume = 0.0;
        for (std::size_t offset = 0U; offset < mesh.indices.size(); offset += 3U)
        {
            Eigen::Vector3f const & first = mesh.positions[mesh.indices[offset]];
            Eigen::Vector3f const & second = mesh.positions[mesh.indices[offset + 1U]];
            Eigen::Vector3f const & third = mesh.positions[mesh.indices[offset + 2U]];
            double const signedVolume =
              static_cast<double>(first.dot(second.cross(third))) / 6.0;
            float const centroidX = (first.x() + second.x() + third.x()) / 3.0F;
            if (centroidX < 0.0F)
            {
                leftSignedVolume += signedVolume;
            }
            else
            {
                rightSignedVolume += signedVolume;
            }
        }
        EXPECT_GT(leftSignedVolume, 0.0);
        EXPECT_GT(rightSignedVolume, 0.0);
    }
}
