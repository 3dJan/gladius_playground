/**
 * @file ColorQuantizer_tests.cpp
 * @brief Unit tests for deterministic adaptive color quantization
 */

#include "io/3mf/ColorQuantizer.h"
#include "io/3mf/FaceColors.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

namespace gladius_tests
{
    using namespace gladius::io;

    class ColorQuantizerTest : public ::testing::Test
    {
    };

    // =========================================================================
    // Unique color counting
    // =========================================================================

    TEST_F(ColorQuantizerTest, CountUniqueOpaqueColors_Empty_ReturnsZero)
    {
        FaceColors empty;
        EXPECT_EQ(ColorQuantizer::countUniqueOpaqueColors(empty), 0U);
    }

    TEST_F(ColorQuantizerTest, CountUniqueOpaqueColors_AllSame_ReturnsOne)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0),
            Color8(255, 0, 0),
            Color8(255, 0, 0),
        });

        EXPECT_EQ(ColorQuantizer::countUniqueOpaqueColors(colors), 1U);
    }

    TEST_F(ColorQuantizerTest, CountUniqueOpaqueColors_DifferentAlphaSameRgb_CountsAsOne)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0, 255),
            Color8(255, 0, 0, 128),
        });

        EXPECT_EQ(ColorQuantizer::countUniqueOpaqueColors(colors), 1U);
    }

    TEST_F(ColorQuantizerTest, CountUniqueOpaqueColors_ThreeDistinct_ReturnsThree)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0),
            Color8(0, 255, 0),
            Color8(0, 0, 255),
        });

        EXPECT_EQ(ColorQuantizer::countUniqueOpaqueColors(colors), 3U);
    }

    // =========================================================================
    // Transparency detection
    // =========================================================================

    TEST_F(ColorQuantizerTest, HasTransparency_AllOpaque_ReturnsFalse)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0, 255),
            Color8(0, 255, 0, 255),
        });

        EXPECT_FALSE(ColorQuantizer::hasTransparency(colors));
    }

    TEST_F(ColorQuantizerTest, HasTransparency_SomeTransparent_ReturnsTrue)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0, 255),
            Color8(0, 255, 0, 128),
        });

        EXPECT_TRUE(ColorQuantizer::hasTransparency(colors));
    }

    // =========================================================================
    // Quantization
    // =========================================================================

    TEST_F(ColorQuantizerTest, Quantize_EmptyInput_ReturnsEmpty)
    {
        FaceColors empty;
        auto const result = ColorQuantizer::quantize(empty, 4);

        EXPECT_TRUE(result.colors.empty());
        EXPECT_TRUE(result.sourceToPaletteMap.empty());
    }

    TEST_F(ColorQuantizerTest, Quantize_SingleColor_ReturnsSinglePalette)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(128, 64, 32),
            Color8(128, 64, 32),
            Color8(128, 64, 32),
        });

        auto const result = ColorQuantizer::quantize(colors, 4);

        EXPECT_EQ(result.colors.size(), 1U);
        EXPECT_EQ(result.sourceToPaletteMap.size(), 3U);
        EXPECT_FLOAT_EQ(result.maxApproximationError, 0.0f);
    }

    TEST_F(ColorQuantizerTest, Quantize_TwoColors_MaxTwoPalette_PreservesBoth)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0),
            Color8(0, 0, 255),
        });

        auto const result = ColorQuantizer::quantize(colors, 2);

        EXPECT_EQ(result.colors.size(), 2U);
        EXPECT_EQ(result.sourceToPaletteMap.size(), 2U);
        // Each face should have a different palette index
        EXPECT_NE(result.sourceToPaletteMap[0], result.sourceToPaletteMap[1]);
    }

    TEST_F(ColorQuantizerTest, Quantize_ManyColors_ReducesToMaxPalette)
    {
        // Create 16 distinct colors
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0),   Color8(0, 255, 0),   Color8(0, 0, 255),   Color8(255, 255, 0),
            Color8(255, 0, 255), Color8(0, 255, 255),  Color8(128, 0, 0),   Color8(0, 128, 0),
            Color8(0, 0, 128),   Color8(128, 128, 0),  Color8(128, 0, 128), Color8(0, 128, 128),
            Color8(64, 0, 0),    Color8(0, 64, 0),     Color8(0, 0, 64),    Color8(64, 64, 64),
        });

        auto const result = ColorQuantizer::quantize(colors, 4);

        EXPECT_LE(result.colors.size(), 4U);
        EXPECT_EQ(result.sourceToPaletteMap.size(), 16U);

        // Every palette index must be valid
        for (auto idx : result.sourceToPaletteMap)
        {
            EXPECT_LT(idx, static_cast<std::uint32_t>(result.colors.size()));
        }
    }

    TEST_F(ColorQuantizerTest, Quantize_Deterministic_SameInputSameOutput)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0),
            Color8(0, 255, 0),
            Color8(0, 0, 255),
            Color8(128, 128, 128),
            Color8(200, 100, 50),
            Color8(50, 100, 200),
        });

        auto const r1 = ColorQuantizer::quantize(colors, 3);
        auto const r2 = ColorQuantizer::quantize(colors, 3);

        ASSERT_EQ(r1.colors.size(), r2.colors.size());
        for (std::size_t i = 0; i < r1.colors.size(); ++i)
        {
            EXPECT_EQ(r1.colors[i], r2.colors[i]);
        }

        ASSERT_EQ(r1.sourceToPaletteMap.size(), r2.sourceToPaletteMap.size());
        for (std::size_t i = 0; i < r1.sourceToPaletteMap.size(); ++i)
        {
            EXPECT_EQ(r1.sourceToPaletteMap[i], r2.sourceToPaletteMap[i]);
        }

        EXPECT_FLOAT_EQ(r1.maxApproximationError, r2.maxApproximationError);
    }

    TEST_F(ColorQuantizerTest, Quantize_RepeatedColors_GroupedTogether)
    {
        FaceColors colors(std::vector<Color8>{
            Color8(255, 0, 0),
            Color8(255, 0, 0),
            Color8(0, 0, 255),
            Color8(0, 0, 255),
        });

        auto const result = ColorQuantizer::quantize(colors, 2);

        EXPECT_EQ(result.colors.size(), 2U);
        // The two reds should map to the same palette entry
        EXPECT_EQ(result.sourceToPaletteMap[0], result.sourceToPaletteMap[1]);
        // The two blues should map to the same palette entry
        EXPECT_EQ(result.sourceToPaletteMap[2], result.sourceToPaletteMap[3]);
    }

} // namespace gladius_tests
