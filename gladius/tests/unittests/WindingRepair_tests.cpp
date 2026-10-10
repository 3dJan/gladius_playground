#include "io/IndexedTriangleMesh.h"
#include "io/WindingRepair.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        [[nodiscard]] io::IndexedTriangleMesh makeTwoTetrahedra()
        {
            io::IndexedTriangleMesh mesh;
            mesh.positions = {
                {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F},
                {3.0F, 0.0F, 0.0F}, {4.0F, 0.0F, 0.0F}, {3.0F, 1.0F, 0.0F}, {3.0F, 0.0F, 1.0F}};
            mesh.indices = {
                1U, 2U, 3U, 0U, 3U, 2U, 0U, 1U, 3U, 0U, 2U, 1U,
                5U, 6U, 7U, 4U, 7U, 6U, 4U, 5U, 7U, 4U, 6U, 5U};
            for (std::size_t triangle = 4U; triangle < 8U; ++triangle)
            {
                std::swap(mesh.indices[triangle * 3U + 1U], mesh.indices[triangle * 3U + 2U]);
            }
            return mesh;
        }
    } // namespace

    TEST(WindingRepair_ComponentOrientation, SeparateShells_UsesIndependentSdfVotes)
    {
        io::IndexedTriangleMesh mesh = makeTwoTetrahedra();
        std::vector<std::uint32_t> triangleComponents;

        auto const repairStats = io::repairTriangleWindingConsistency(
            mesh.positions, mesh.indices, &triangleComponents);

        ASSERT_EQ(repairStats.components, 2U);
        ASSERT_EQ(triangleComponents.size(), mesh.indices.size() / 3U);
        for (std::size_t triangle = 1U; triangle < 4U; ++triangle)
        {
            EXPECT_EQ(triangleComponents[triangle], triangleComponents[0U]);
        }
        for (std::size_t triangle = 5U; triangle < 8U; ++triangle)
        {
            EXPECT_EQ(triangleComponents[triangle], triangleComponents[4U]);
        }
        EXPECT_NE(triangleComponents[0U], triangleComponents[4U]);

        std::vector<double> componentVotes(repairStats.components, 0.0);
        componentVotes[triangleComponents[0U]] = 1.0;
        componentVotes[triangleComponents[4U]] = -1.0;
        EXPECT_EQ(io::orientTriangleComponentsByVotes(mesh.indices, triangleComponents, componentVotes), 1U);

        auto const diagnostics = io::computeIndexedTriangleMeshDiagnostics(mesh);
        EXPECT_EQ(diagnostics.connectedComponentCount, 2U);
        EXPECT_EQ(diagnostics.boundaryEdgeCount, 0U);
        EXPECT_EQ(diagnostics.inconsistentWindingEdgeCount, 0U);
        EXPECT_NEAR(diagnostics.signedVolume, 1.0 / 3.0, 1.0e-6);
    }

    TEST(WindingRepair_ComponentOrientation, Sampling_ReturnsBoundedUnitNormals)
    {
        io::IndexedTriangleMesh mesh = makeTwoTetrahedra();
        std::vector<std::uint32_t> triangleComponents;
        auto const repairStats = io::repairTriangleWindingConsistency(
            mesh.positions, mesh.indices, &triangleComponents);

        auto const samples = io::makeTriangleOrientationSamples(
            mesh.positions, mesh.indices, triangleComponents, 2U);

        ASSERT_EQ(samples.size(), 4U);
        std::vector<std::size_t> samplesPerComponent(repairStats.components, 0U);
        for (auto const& sample : samples)
        {
            ASSERT_LT(sample.component, samplesPerComponent.size());
            ++samplesPerComponent[sample.component];
            EXPECT_TRUE(sample.position.allFinite());
            EXPECT_NEAR(sample.normal.norm(), 1.0F, 1.0e-6F);
        }
        EXPECT_EQ(samplesPerComponent[0U], 2U);
        EXPECT_EQ(samplesPerComponent[1U], 2U);
    }

    TEST(WindingRepair_ComponentOrientation, InvalidIndexBuffer_ClearsComponentOutput)
    {
        std::vector<Eigen::Vector3f> const positions{
            {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
        std::vector<std::uint32_t> indices{0U, 1U};
        std::vector<std::uint32_t> triangleComponents{7U};

        auto const repairStats = io::repairTriangleWindingConsistency(
            positions, indices, &triangleComponents);

        EXPECT_EQ(repairStats.triangleCount, 0U);
        EXPECT_TRUE(triangleComponents.empty());
    }
} // namespace gladius::tests
