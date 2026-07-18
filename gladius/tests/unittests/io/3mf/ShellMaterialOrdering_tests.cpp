#include "io/3mf/ShellMaterialOrdering.h"

#include <gtest/gtest.h>

#include <string>

namespace gladius::io::tests
{
    TEST(ShellMaterialOrdering_Test, ReorderForShells_MovesBackgroundToInnermost)
    {
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties{"OuterCandidate", Eigen::Vector3f::Ones(), 0.5F, 0.4F, Eigen::Vector3f{1.0F, 1.0F, 1.0F}});
        stack.push_back(FilamentOpticalProperties{"Background", Eigen::Vector3f::Zero(), 0.8F, 0.4F, Eigen::Vector3f{0.5F, 0.5F, 0.5F}});
        stack.push_back(FilamentOpticalProperties{"Middle", Eigen::Vector3f::UnitX(), 0.6F, 0.4F, Eigen::Vector3f{2.0F, 2.0F, 2.0F}});

        auto ordered = ShellMaterialOrdering::reorderForShells(stack, 1U, IlluminationMode::Frontlit);

        ASSERT_EQ(ordered.stack.size(), 3U);
        EXPECT_EQ(ordered.backgroundIndex, 0U);
        EXPECT_EQ(ordered.stack[0].name, "Background");
        EXPECT_EQ(ordered.orderedToOriginal[0], 1U);
    }

    TEST(ShellMaterialOrdering_Test, ReorderForShells_FrontlitSortsRemainingByAscendingTranslucency)
    {
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties{"LeastTranslucent", Eigen::Vector3f::Ones(), 0.5F, 0.4F, Eigen::Vector3f{0.5F, 0.5F, 0.5F}});
        stack.push_back(FilamentOpticalProperties{"MostTranslucent", Eigen::Vector3f::Ones(), 0.5F, 0.4F, Eigen::Vector3f{3.0F, 3.0F, 3.0F}});
        stack.push_back(FilamentOpticalProperties{"MediumTranslucent", Eigen::Vector3f::Ones(), 0.5F, 0.4F, Eigen::Vector3f{1.5F, 1.5F, 1.5F}});

        auto ordered = ShellMaterialOrdering::reorderForShells(
            stack,
            std::numeric_limits<std::size_t>::max(),
            IlluminationMode::Frontlit);

        ASSERT_EQ(ordered.stack.size(), 3U);
        EXPECT_EQ(ordered.stack[0].name, "LeastTranslucent");
        EXPECT_EQ(ordered.stack[1].name, "MediumTranslucent");
        EXPECT_EQ(ordered.stack[2].name, "MostTranslucent");
    }

    TEST(ShellMaterialOrdering_Test, ReorderForShells_BacklitPreservesRemainingOrder)
    {
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties{"Background", Eigen::Vector3f::Zero(), 0.8F, 0.4F, Eigen::Vector3f{0.5F, 0.5F, 0.5F}});
        stack.push_back(FilamentOpticalProperties{"First", Eigen::Vector3f::UnitX(), 0.5F, 0.4F, Eigen::Vector3f{3.0F, 3.0F, 3.0F}});
        stack.push_back(FilamentOpticalProperties{"Second", Eigen::Vector3f::UnitY(), 0.5F, 0.4F, Eigen::Vector3f{1.0F, 1.0F, 1.0F}});

        auto ordered = ShellMaterialOrdering::reorderForShells(stack, 0U, IlluminationMode::Backlit);

        ASSERT_EQ(ordered.stack.size(), 3U);
        EXPECT_EQ(ordered.stack[0].name, "Background");
        EXPECT_EQ(ordered.stack[1].name, "First");
        EXPECT_EQ(ordered.stack[2].name, "Second");
    }

    TEST(ShellMaterialOrdering_Test, OptimizeGlobalOrderForShells_FindsBestPermutationForSmallFrontlitStack)
    {
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties{"Background", Eigen::Vector3f::Zero(), 0.8F, 0.4F, Eigen::Vector3f{0.5F, 0.5F, 0.5F}});
        stack.push_back(FilamentOpticalProperties{"A", Eigen::Vector3f::UnitX(), 0.5F, 0.4F, Eigen::Vector3f{3.0F, 3.0F, 3.0F}});
        stack.push_back(FilamentOpticalProperties{"B", Eigen::Vector3f::UnitY(), 0.5F, 0.4F, Eigen::Vector3f{1.0F, 1.0F, 1.0F}});
        stack.push_back(FilamentOpticalProperties{"C", Eigen::Vector3f::UnitZ(), 0.5F, 0.4F, Eigen::Vector3f{2.0F, 2.0F, 2.0F}});

        auto ordered = ShellMaterialOrdering::optimizeGlobalOrderForShells(
            stack,
            0U,
            IlluminationMode::Frontlit,
            [](FilamentStack const& candidate, std::size_t backgroundIdx) {
                std::string signature = candidate[backgroundIdx + 1U].name + candidate[backgroundIdx + 2U].name + candidate[backgroundIdx + 3U].name;
                return (signature == "CAB") ? 0.0F : 10.0F;
            });

        ASSERT_EQ(ordered.stack.size(), 4U);
        EXPECT_EQ(ordered.backgroundIndex, 0U);
        EXPECT_EQ(ordered.stack[0].name, "Background");
        EXPECT_EQ(ordered.stack[1].name, "C");
        EXPECT_EQ(ordered.stack[2].name, "A");
        EXPECT_EQ(ordered.stack[3].name, "B");
    }

    TEST(ShellMaterialOrdering_Test, OptimizeGlobalOrderForPalette_FallsBackToHeuristicForEmptyPalette)
    {
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties{"Background", Eigen::Vector3f::Zero(), 0.8F, 0.4F, Eigen::Vector3f{0.5F, 0.5F, 0.5F}});
        stack.push_back(FilamentOpticalProperties{"LeastTranslucent", Eigen::Vector3f::Ones(), 0.5F, 0.4F, Eigen::Vector3f{0.5F, 0.5F, 0.5F}});
        stack.push_back(FilamentOpticalProperties{"MostTranslucent", Eigen::Vector3f::Ones(), 0.5F, 0.4F, Eigen::Vector3f{3.0F, 3.0F, 3.0F}});

        ThicknessConstraints constraints;
        std::vector<Eigen::Vector3f> const emptyPalette;

        auto ordered = ShellMaterialOrdering::optimizeGlobalOrderForPalette(
            stack,
            0U,
            IlluminationMode::Frontlit,
            constraints,
            emptyPalette);

        ASSERT_EQ(ordered.stack.size(), 3U);
        EXPECT_EQ(ordered.backgroundIndex, 0U);
        EXPECT_EQ(ordered.stack[0].name, "Background");
        EXPECT_EQ(ordered.stack[1].name, "LeastTranslucent");
        EXPECT_EQ(ordered.stack[2].name, "MostTranslucent");
    }

    TEST(ShellMaterialOrdering_Test, OptimizeGlobalOrderForPalette_PicksLowerErrorOrderForKnownPalette)
    {
        // Two printable layers over a background. The palette is pure red.
        // Layer "Red" has a red reflectance; layer "Cyan" has a cyan reflectance.
        // The optimizer should prefer the order that lets the red layer sit closer
        // to the visible surface for the red-dominant target.
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties{"Background", Eigen::Vector3f::Zero(), 0.8F, 0.4F, Eigen::Vector3f{0.5F, 0.5F, 0.5F}});
        stack.push_back(FilamentOpticalProperties{"Cyan", Eigen::Vector3f{0.1F, 0.7F, 0.8F}, 0.5F, 0.4F, Eigen::Vector3f{2.0F, 2.0F, 2.0F}});
        stack.push_back(FilamentOpticalProperties{"Red", Eigen::Vector3f{0.9F, 0.1F, 0.1F}, 0.5F, 0.4F, Eigen::Vector3f{2.0F, 2.0F, 2.0F}});

        ThicknessConstraints constraints;
        constraints.minThickness = 0.0F;
        constraints.maxThickness = 4.0F;

        std::vector<Eigen::Vector3f> const palette = {Eigen::Vector3f{0.9F, 0.1F, 0.1F}};

        auto ordered = ShellMaterialOrdering::optimizeGlobalOrderForPalette(
            stack,
            0U,
            IlluminationMode::Frontlit,
            constraints,
            palette);

        ASSERT_EQ(ordered.stack.size(), 3U);
        EXPECT_EQ(ordered.backgroundIndex, 0U);
        EXPECT_EQ(ordered.stack[0].name, "Background");

        // The red layer should be the outermost printable layer so it can dominate
        // the visible color for a red target.
        EXPECT_EQ(ordered.stack[2].name, "Red");
    }
} // namespace gladius::io::tests
