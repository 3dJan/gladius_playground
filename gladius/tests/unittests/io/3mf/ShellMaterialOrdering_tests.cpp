#include "io/3mf/ShellMaterialOrdering.h"

#include <gtest/gtest.h>

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
} // namespace gladius::io::tests