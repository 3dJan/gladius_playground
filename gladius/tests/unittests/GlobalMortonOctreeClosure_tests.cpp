#include "compute/GlobalMortonOctree.h"
#include "io/IndexedTriangleMesh.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace gladius::compute::tests
{
    namespace
    {
        /// A thin ring seeded at coarse-grid corners, requiring many neighbor-discovery waves.
        class TorusSampler final : public ManifoldSdfSampler
        {
          public:
            [[nodiscard]] ManifoldSdfSamplingStatus sampleSdf(
                Eigen::Vector3f const& position, Eigen::Vector3f const&, Eigen::Vector3f const&,
                float& value, ManifoldSdfSamplingCallbacks const& callbacks) const override
            {
                value = evaluate(position);
                return getStatus(callbacks);
            }

            [[nodiscard]] ManifoldSdfSamplingStatus evaluateSdfBatch(
                std::vector<Eigen::Vector3f> const& positions, float isoValue,
                std::vector<float>& values, ManifoldSdfSamplingCallbacks const& callbacks) const override
            {
                values.clear();
                for (auto const& position : positions)
                {
                    values.push_back(evaluate(position) - isoValue);
                }
                return getStatus(callbacks);
            }

            [[nodiscard]] ManifoldSdfSamplingStatus evaluateSdfGradientBatch(
                std::vector<Eigen::Vector3f> const& positions, float isoValue, float,
                std::vector<float>& values, std::vector<Eigen::Vector3f>& gradients,
                ManifoldSdfSamplingCallbacks const& callbacks) const override
            {
                gradients.clear();
                for (auto const& position : positions)
                {
                    gradients.push_back(gradient(position));
                }
                return evaluateSdfBatch(positions, isoValue, values, callbacks);
            }

            [[nodiscard]] ManifoldSdfSamplingStatus refineHermiteEdgesBatch(
                std::vector<Eigen::Vector3f> const& starts, std::vector<Eigen::Vector3f> const& ends,
                std::vector<float> const& startValues, std::vector<float> const&, float isoValue, float,
                std::vector<Eigen::Vector3f>& positions, std::vector<Eigen::Vector3f>& gradients,
                ManifoldSdfSamplingCallbacks const& callbacks) const override
            {
                positions.clear();
                gradients.clear();
                for (std::size_t edge = 0U; edge < starts.size(); ++edge)
                {
                    float low = 0.0F;
                    float high = 1.0F;
                    bool const startInside = startValues[edge] < 0.0F;
                    for (std::size_t iteration = 0U; iteration < 24U; ++iteration)
                    {
                        float const middle = (low + high) * 0.5F;
                        auto const position = starts[edge] + middle * (ends[edge] - starts[edge]);
                        if ((evaluate(position) < isoValue) == startInside)
                        {
                            low = middle;
                        }
                        else
                        {
                            high = middle;
                        }
                    }
                    positions.push_back(starts[edge] + (low + high) * 0.5F * (ends[edge] - starts[edge]));
                    gradients.push_back(gradient(positions.back()));
                }
                return getStatus(callbacks);
            }

            [[nodiscard]] ManifoldSdfSamplingStatus projectToSurfaceBatch(
                std::vector<Eigen::Vector3f> const& inputPositions,
                std::vector<Eigen::Vector3f> const&, std::vector<Eigen::Vector3f> const&, float, float,
                std::vector<Eigen::Vector3f>& positions, std::vector<Eigen::Vector3f>& gradients,
                ManifoldSdfSamplingCallbacks const& callbacks) const override
            {
                positions = inputPositions;
                gradients.clear();
                for (auto const& position : positions)
                {
                    gradients.push_back(gradient(position));
                }
                return getStatus(callbacks);
            }

          private:
            [[nodiscard]] float evaluate(Eigen::Vector3f const& position) const
            {
                float const radial = std::hypot(position.x(), position.y()) - 0.75F;
                return std::hypot(radial, position.z()) - 0.04F;
            }

            [[nodiscard]] Eigen::Vector3f gradient(Eigen::Vector3f const& position) const
            {
                float const radius = std::hypot(position.x(), position.y());
                if (radius == 0.0F)
                {
                    return Eigen::Vector3f::UnitX();
                }
                float const radial = radius - 0.75F;
                Eigen::Vector3f const result{radial * position.x() / radius,
                                            radial * position.y() / radius, position.z()};
                return result.squaredNorm() > 0.0F ? Eigen::Vector3f{result.normalized()}
                                                 : Eigen::Vector3f{Eigen::Vector3f::UnitZ()};
            }

            [[nodiscard]] ManifoldSdfSamplingStatus getStatus(ManifoldSdfSamplingCallbacks const& callbacks) const
            {
                return callbacks.shouldCancel && callbacks.shouldCancel()
                    ? ManifoldSdfSamplingStatus::Cancelled : ManifoldSdfSamplingStatus::Completed;
            }
        };

        [[nodiscard]] GlobalMortonOctreeConfig makeConfig()
        {
            GlobalMortonOctreeConfig config;
            config.initialDepth = 3U;
            config.maxDepth = 6U;
            config.enableAdaptiveRefinement = false;
            config.boundingBoxOverride.emplace();
            for (std::size_t axis = 0U; axis < 3U; ++axis)
            {
                config.boundingBoxOverride->min.s[axis] = -1.0F;
                config.boundingBoxOverride->max.s[axis] = 1.0F;
            }
            return config;
        }
    }

    TEST(GlobalMortonOctreeClosure, Build_ThinRingBeyondTenDiscoveryWaves_CompletesSurfaceNeighbors)
    {
        TorusSampler sampler;
        GlobalMortonOctree octree{sampler};

        octree.build(makeConfig());
        io::IndexedTriangleMesh mesh;
        octree.extractMesh(mesh.positions, mesh.normals, mesh.indices);

        ASSERT_FALSE(octree.hasSamplingError());
        ASSERT_FALSE(octree.wasCancelled());
        ASSERT_FALSE(mesh.indices.empty());
        EXPECT_EQ(octree.getStats().neighborMisses, 0U);
        EXPECT_EQ(octree.getStats().missingVertexQuads, 0U);
        EXPECT_EQ(octree.getStats().boundaryEdges, 0U);
        EXPECT_EQ(octree.getStats().nonManifoldEdges, 0U);
        EXPECT_EQ(mesh.positions.size(), mesh.normals.size());
        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());

        Eigen::Vector3f minimum = mesh.positions.front();
        Eigen::Vector3f maximum = minimum;
        for (auto const& position : mesh.positions)
        {
            minimum = minimum.cwiseMin(position);
            maximum = maximum.cwiseMax(position);
        }
        EXPECT_GT(maximum.x() - minimum.x(), 1.5F);
        EXPECT_GT(maximum.y() - minimum.y(), 1.5F);
    }

    TEST(GlobalMortonOctreeClosure, Build_CancelledDuringNeighborDiscovery_StopsWithoutSamplingFailure)
    {
        TorusSampler sampler;
        GlobalMortonOctree octree{sampler};
        auto const config = makeConfig();
        octree.setCancellationCheckCallback([&octree]
        {
            auto const initialNodeCount = octree.getStats().totalNodes;
            return initialNodeCount != 0U && octree.getNodes().size() > initialNodeCount;
        });

        octree.build(config);

        EXPECT_TRUE(octree.wasCancelled());
        EXPECT_FALSE(octree.hasSamplingError());
        EXPECT_GT(octree.getNodes().size(), octree.getStats().totalNodes);
    }
}