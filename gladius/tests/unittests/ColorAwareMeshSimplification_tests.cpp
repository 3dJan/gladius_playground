#include "compute/FastQemSimplification.h"
#include "io/IndexedTriangleMesh.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace gladius::compute::tests
{
    namespace
    {
        struct ColoredMesh : io::IndexedTriangleMesh
        {
            std::vector<std::uint32_t> labels;
        };

        /// Subdivided octahedron with shared indices, closed and consistently oriented.
        ColoredMesh makeSphere()
        {
            ColoredMesh mesh;
            mesh.positions = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            mesh.indices = {0, 2, 4, 4, 2, 1, 1, 2, 5, 5, 2, 0,
                            4, 3, 0, 1, 3, 4, 5, 3, 1, 0, 3, 5};
            for (int level = 0; level < 3; ++level)
            {
                std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> midpoints;
                auto midpoint = [&](std::uint32_t a, std::uint32_t b)
                {
                    auto const key = std::minmax(a, b);
                    auto const found = midpoints.find(key);
                    if (found != midpoints.end()) return found->second;
                    auto const index = static_cast<std::uint32_t>(mesh.positions.size());
                    Eigen::Vector3f const position = (mesh.positions[a] + mesh.positions[b]).normalized();
                    mesh.positions.push_back(position);
                    midpoints.emplace(key, index);
                    return index;
                };
                std::vector<std::uint32_t> subdivided;
                for (std::size_t t = 0; t < mesh.indices.size(); t += 3U)
                {
                    auto const a = mesh.indices[t];
                    auto const b = mesh.indices[t + 1U];
                    auto const c = mesh.indices[t + 2U];
                    auto const ab = midpoint(a, b);
                    auto const bc = midpoint(b, c);
                    auto const ca = midpoint(c, a);
                    subdivided.insert(subdivided.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
                }
                mesh.indices = std::move(subdivided);
            }
            for (std::size_t t = 0; t < mesh.indices.size(); t += 3U)
            {
                auto const y = mesh.positions[mesh.indices[t]].y() +
                               mesh.positions[mesh.indices[t + 1U]].y() +
                               mesh.positions[mesh.indices[t + 2U]].y();
                mesh.labels.push_back(y > 0.0F ? 0U : 1U);
            }
            return mesh;
        }

        using Point = std::array<float, 3>;
        using Seam = std::tuple<Point, Point, std::uint32_t, std::uint32_t>;

        /// Compare seam geometry and neighboring material IDs independently of vertex remapping.
        std::set<Seam> collectSeams(ColoredMesh const & mesh)
        {
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> edgeLabels;
            for (std::size_t t = 0; t < mesh.labels.size(); ++t)
            {
                for (std::size_t e = 0; e < 3U; ++e)
                {
                    auto const a = mesh.indices[t * 3U + e];
                    auto const b = mesh.indices[t * 3U + (e + 1U) % 3U];
                    edgeLabels[std::minmax(a, b)].push_back(mesh.labels[t]);
                }
            }
            std::set<Seam> seams;
            for (auto const & [edge, labels] : edgeLabels)
            {
                if (labels.size() != 2U || labels[0] == labels[1]) continue;
                auto const & a = mesh.positions[edge.first];
                auto const & b = mesh.positions[edge.second];
                Point first{a.x(), a.y(), a.z()};
                Point second{b.x(), b.y(), b.z()};
                if (second < first) std::swap(first, second);
                seams.emplace(first, second, std::min(labels[0], labels[1]), std::max(labels[0], labels[1]));
            }
            return seams;
        }
    }

    TEST(ColorAwareMeshSimplification, Simplify_TwoMaterials_ReducesInteriorsAndPreservesSeams)
    {
        auto mesh = makeSphere();
        auto const originalSeams = collectSeams(mesh);
        auto const originalCount = mesh.labels.size();
        ASSERT_FALSE(mesh.closedOrientedTopologyError().has_value());
        ASSERT_FALSE(originalSeams.empty());

        auto const collapsed = fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, FastQemConfig{});

        EXPECT_GT(collapsed, 0U);
        EXPECT_LT(mesh.labels.size(), originalCount);
        EXPECT_EQ(mesh.labels.size(), mesh.indices.size() / 3U);
        EXPECT_EQ(collectSeams(mesh), originalSeams);
        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());
        for (std::size_t t = 0; t < mesh.labels.size(); ++t)
        {
            float const y = mesh.positions[mesh.indices[t * 3U]].y() +
                            mesh.positions[mesh.indices[t * 3U + 1U]].y() +
                            mesh.positions[mesh.indices[t * 3U + 2U]].y();
            EXPECT_EQ(mesh.labels[t], y > 0.0F ? 0U : 1U);
        }
    }

    TEST(ColorAwareMeshSimplification, Simplify_ThinStripeIslandAndJunction_PreservesAllSeams)
    {
        auto mesh = makeSphere();
        for (std::size_t t = 0; t < mesh.labels.size(); ++t)
        {
            Eigen::Vector3f const center = (mesh.positions[mesh.indices[t * 3U]] +
                                            mesh.positions[mesh.indices[t * 3U + 1U]] +
                                            mesh.positions[mesh.indices[t * 3U + 2U]]) / 3.0F;
            mesh.labels[t] = std::abs(center.y()) < 0.15F ? 2U : (center.x() > 0.0F ? 0U : 1U);
        }
        mesh.labels[0] = 3U;
        auto const seams = collectSeams(mesh);
        FastQemConfig config;
        config.targetReductionPercent = 90.0F;

        fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, config);

        EXPECT_EQ(collectSeams(mesh), seams);
        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());
        EXPECT_NE(std::find(mesh.labels.begin(), mesh.labels.end(), 3U), mesh.labels.end());
    }

    TEST(ColorAwareMeshSimplification, Simplify_DegenerateFaces_CompactsLabelsInFaceOrder)
    {
        std::vector<Eigen::Vector3f> positions{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}};
        std::vector<std::uint32_t> indices{0, 0, 1, 0, 1, 2, 0, 1, 3};
        std::vector<std::uint32_t> labels{7U, 11U, 9U};

        fastQemSimplify(positions, indices, labels, FastQemConfig{});

        EXPECT_EQ(indices.size(), 3U);
        EXPECT_EQ(labels, (std::vector<std::uint32_t>{11U}));
    }

    TEST(ColorAwareMeshSimplification, Simplify_InvalidInputs_ThrowsBeforeMutation)
    {
        auto mesh = makeSphere();
        auto const indices = mesh.indices;
        mesh.labels.pop_back();
        EXPECT_THROW(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, FastQemConfig{}), std::invalid_argument);
        EXPECT_EQ(mesh.indices, indices);
        mesh.labels.push_back(0U);
        mesh.indices[0] = static_cast<std::uint32_t>(mesh.positions.size());
        EXPECT_THROW(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, FastQemConfig{}), std::invalid_argument);
        mesh.indices.pop_back();
        EXPECT_THROW(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, FastQemConfig{}), std::invalid_argument);
    }

    TEST(ColorAwareMeshSimplification, Simplify_NonfinitePositionOrInvalidConfiguration_ThrowsBeforeMutation)
    {
        auto mesh = makeSphere();
        auto const indices = mesh.indices;
        auto const labels = mesh.labels;
        for (auto const value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
        {
            mesh.positions[0].x() = value;
            EXPECT_THROW(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, FastQemConfig{}), std::invalid_argument);
            EXPECT_EQ(mesh.indices, indices);
            EXPECT_EQ(mesh.labels, labels);
        }
        mesh = makeSphere();
        FastQemConfig config;
        config.maxError = std::numeric_limits<float>::quiet_NaN();
        EXPECT_THROW(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, config), std::invalid_argument);
        config.maxError = -1.0F;
        EXPECT_THROW(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, config), std::invalid_argument);
        config.maxError = 1.0F;
        config.cancelCheckPeriod = 0U;
        EXPECT_THROW(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, config), std::invalid_argument);
        EXPECT_EQ(mesh.indices, indices);
        EXPECT_EQ(mesh.labels, labels);
    }

    TEST(ColorAwareMeshSimplification, Simplify_EmptyAndNoReduction_KeepsLabels)
    {
        ColoredMesh empty;
        EXPECT_EQ(fastQemSimplify(empty.positions, empty.indices, empty.labels, FastQemConfig{}), 0U);
        auto mesh = makeSphere();
        auto const labels = mesh.labels;
        auto const indices = mesh.indices;
        FastQemConfig config;
        config.targetReductionPercent = 0.0F;
        EXPECT_EQ(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, config), 0U);
        EXPECT_EQ(mesh.labels, labels);
        EXPECT_EQ(mesh.indices, indices);
    }

    TEST(ColorAwareMeshSimplification, Simplify_AllFacesDistinct_StopsAboveImpossibleTarget)
    {
        auto mesh = makeSphere();
        for (std::size_t t = 0; t < mesh.labels.size(); ++t) mesh.labels[t] = static_cast<std::uint32_t>(t);
        auto const indices = mesh.indices;
        FastQemConfig config;
        config.targetReductionPercent = 100.0F;

        EXPECT_EQ(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, config), 0U);
        EXPECT_EQ(mesh.indices.size(), indices.size());
        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());
    }

    TEST(ColorAwareMeshSimplification, Simplify_CancellationDuringSetup_Throws)
    {
        auto mesh = makeSphere();
        int calls = 0;
        auto cancel = [&]()
        {
            if (++calls == 3) throw std::runtime_error("cancelled");
        };
        EXPECT_THROW(fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, FastQemConfig{}, cancel), std::runtime_error);
        EXPECT_EQ(calls, 3);
    }

    TEST(ColorAwareMeshSimplification, Simplify_CoincidentSheets_DoesNotWeld)
    {
        ColoredMesh mesh;
        mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                          {0, 0, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
        mesh.indices = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3,
                        4, 6, 5, 4, 5, 7, 5, 6, 7, 6, 4, 7};
        mesh.labels = {0, 0, 0, 0, 1, 1, 1, 1};

        fastQemSimplify(mesh.positions, mesh.indices, mesh.labels, FastQemConfig{});

        EXPECT_EQ(mesh.positions.size(), 8U);
        EXPECT_EQ(io::computeIndexedTriangleMeshDiagnostics(mesh).connectedComponentCount, 2U);
        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());
    }

    TEST(ColorAwareMeshSimplification, Simplify_SingleMaterial_MatchesUnlabeledAndIsDeterministic)
    {
        auto labeled = makeSphere();
        std::fill(labeled.labels.begin(), labeled.labels.end(), 42U);
        auto unlabeled = labeled;
        auto repeated = labeled;

        fastQemSimplify(labeled.positions, labeled.indices, labeled.labels, FastQemConfig{});
        fastQemSimplify(unlabeled.positions, unlabeled.indices, FastQemConfig{});
        fastQemSimplify(repeated.positions, repeated.indices, repeated.labels, FastQemConfig{});

        EXPECT_EQ(labeled.indices, unlabeled.indices);
        EXPECT_EQ(labeled.positions, unlabeled.positions);
        EXPECT_EQ(labeled.labels, repeated.labels);
        EXPECT_EQ(labeled.indices, repeated.indices);
        EXPECT_EQ(labeled.positions, repeated.positions);
        EXPECT_TRUE(std::all_of(labeled.labels.begin(), labeled.labels.end(), [](auto label) { return label == 42U; }));
    }
}