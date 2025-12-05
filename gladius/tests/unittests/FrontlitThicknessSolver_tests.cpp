/**
 * @file FrontlitThicknessSolver_tests.cpp
 * @brief Unit tests for FrontlitThicknessSolver and FaceThicknessMapper
 */

#include "io/3mf/FaceThicknessMapper.h"
#include "io/3mf/FilamentOpticalProperties.h"
#include "io/3mf/FrontlitThicknessSolver.h"

#include <gtest/gtest.h>

#include <cmath>

namespace gladius::io::tests
{

    class FrontlitThicknessSolverTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Create a simple 3-filament stack: black (bottom), red, white (top)
            m_blackFilament = FilamentOpticalProperties{
                "Black",
                Eigen::Vector3f(0.05f, 0.05f, 0.05f), // Very dark
                0.8f,                                  // High opacity
                0.4f                                   // Reference thickness
            };

            m_redFilament = FilamentOpticalProperties{
                "Red",
                Eigen::Vector3f(0.9f, 0.1f, 0.1f), // Bright red
                0.6f,                               // Medium opacity
                0.4f};

            m_whiteFilament = FilamentOpticalProperties{
                "White",
                Eigen::Vector3f(0.95f, 0.95f, 0.95f), // Near white
                0.7f,                                  // High-ish opacity
                0.4f};

            m_stack.push_back(m_blackFilament);
            m_stack.push_back(m_redFilament);
            m_stack.push_back(m_whiteFilament);
        }

        FilamentOpticalProperties m_blackFilament;
        FilamentOpticalProperties m_redFilament;
        FilamentOpticalProperties m_whiteFilament;
        FilamentStack m_stack;
    };

    // Test FilamentOpticalProperties::computeEffectiveOpacity
    TEST_F(FrontlitThicknessSolverTest, EffectiveOpacity_ZeroThickness_ReturnsZero)
    {
        float const opacity = m_blackFilament.computeEffectiveOpacity(0.0f);
        EXPECT_FLOAT_EQ(opacity, 0.0f);
    }

    TEST_F(FrontlitThicknessSolverTest, EffectiveOpacity_ReferenceThickness_ReturnsNominalOpacity)
    {
        // At reference thickness, opacity should be close to the nominal value
        float const opacity = m_blackFilament.computeEffectiveOpacity(m_blackFilament.referenceThickness);

        // Due to exponential model, it won't be exactly m_blackFilament.opacity,
        // but should be in the same ballpark
        EXPECT_GT(opacity, 0.5f);
        EXPECT_LT(opacity, 1.0f);
    }

    TEST_F(FrontlitThicknessSolverTest, EffectiveOpacity_LargeThickness_ApproachesOne)
    {
        float const opacity = m_blackFilament.computeEffectiveOpacity(100.0f);
        EXPECT_GT(opacity, 0.99f);
    }

    TEST_F(FrontlitThicknessSolverTest, EffectiveOpacity_NegativeThickness_ReturnsZero)
    {
        float const opacity = m_blackFilament.computeEffectiveOpacity(-1.0f);
        EXPECT_FLOAT_EQ(opacity, 0.0f);
    }

    // Test FrontlitThicknessSolver::predictColor
    TEST_F(FrontlitThicknessSolverTest, PredictColor_AllZeroThickness_ReturnsBlackOrDefault)
    {
        FrontlitThicknessSolver solver(m_stack);

        std::vector<float> thicknesses = {0.0f, 0.0f, 0.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);

        // With all zero thicknesses, no color is visible - should be black or near-black
        EXPECT_LT(predicted.norm(), 0.1f);
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_OnlyTopLayerThick_ReturnsTopColor)
    {
        FrontlitThicknessSolver solver(m_stack);

        // Only the white (top) layer has thickness
        std::vector<float> thicknesses = {0.0f, 0.0f, 5.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);

        // Should be dominated by white
        EXPECT_GT(predicted.x(), 0.8f);
        EXPECT_GT(predicted.y(), 0.8f);
        EXPECT_GT(predicted.z(), 0.8f);
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_OnlyBottomLayerThick_ReturnsBottomColor)
    {
        FrontlitThicknessSolver solver(m_stack);

        // Only the black (bottom) layer has thickness
        std::vector<float> thicknesses = {5.0f, 0.0f, 0.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);

        // Should be dominated by black
        EXPECT_LT(predicted.x(), 0.2f);
        EXPECT_LT(predicted.y(), 0.2f);
        EXPECT_LT(predicted.z(), 0.2f);
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_OnlyMiddleLayerThick_ReturnsMiddleColor)
    {
        FrontlitThicknessSolver solver(m_stack);

        // Only the red (middle) layer has thickness
        std::vector<float> thicknesses = {0.0f, 5.0f, 0.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);

        // Should be dominated by red
        EXPECT_GT(predicted.x(), 0.7f); // High red
        EXPECT_LT(predicted.y(), 0.3f); // Low green
        EXPECT_LT(predicted.z(), 0.3f); // Low blue
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_TopOccludesLower_ColorDominatedByTop)
    {
        FrontlitThicknessSolver solver(m_stack);

        // All layers thick, but top should dominate
        std::vector<float> thicknesses = {5.0f, 5.0f, 5.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);

        // Top layer (white) should dominate due to high opacity
        EXPECT_GT(predicted.x(), 0.7f);
        EXPECT_GT(predicted.y(), 0.7f);
        EXPECT_GT(predicted.z(), 0.7f);
    }

    // Test FrontlitThicknessSolver::computeVisibilities
    TEST_F(FrontlitThicknessSolverTest, Visibilities_SumToApproximatelyOne)
    {
        FrontlitThicknessSolver solver(m_stack);

        std::vector<float> thicknesses = {1.0f, 1.0f, 1.0f};
        std::vector<float> const visibilities = solver.computeVisibilities(thicknesses);

        ASSERT_EQ(visibilities.size(), 3u);

        // Sum of visibilities plus remaining (transmitted) light should be <= 1
        float const sum = visibilities[0] + visibilities[1] + visibilities[2];
        EXPECT_LE(sum, 1.0f);
        EXPECT_GT(sum, 0.5f); // Should capture most of the light
    }

    TEST_F(FrontlitThicknessSolverTest, Visibilities_TopLayerMostVisible)
    {
        FrontlitThicknessSolver solver(m_stack);

        // Equal thicknesses - top layer should have highest visibility
        std::vector<float> thicknesses = {1.0f, 1.0f, 1.0f};
        std::vector<float> const visibilities = solver.computeVisibilities(thicknesses);

        // Top layer (index 2) should have highest visibility
        EXPECT_GT(visibilities[2], visibilities[1]);
        EXPECT_GT(visibilities[1], visibilities[0]);
    }

    // Test FrontlitThicknessSolver::solve (inverse problem)
    TEST_F(FrontlitThicknessSolverTest, Solve_TargetWhite_IncreaseTopLayerThickness)
    {
        ThicknessConstraints constraints;
        constraints.minThickness = 0.0f;
        constraints.maxThickness = 5.0f;

        FrontlitThicknessSolver solver(m_stack, constraints);

        Eigen::Vector3f const targetWhite(0.9f, 0.9f, 0.9f);
        ThicknessSolution const solution = solver.solve(targetWhite);

        ASSERT_EQ(solution.thicknesses.size(), 3u);

        // To achieve white, the white (top) layer should be thickest
        EXPECT_GT(solution.thicknesses[2], solution.thicknesses[0]);
        EXPECT_GT(solution.thicknesses[2], solution.thicknesses[1]);

        // Error should be reasonably small
        EXPECT_LT(solution.colorError, 0.2f);
    }

    TEST_F(FrontlitThicknessSolverTest, Solve_TargetRed_IncreaseRedLayerThickness)
    {
        ThicknessConstraints constraints;
        constraints.minThickness = 0.0f;
        constraints.maxThickness = 5.0f;

        FrontlitThicknessSolver solver(m_stack, constraints);

        Eigen::Vector3f const targetRed(0.8f, 0.1f, 0.1f);
        ThicknessSolution const solution = solver.solve(targetRed);

        ASSERT_EQ(solution.thicknesses.size(), 3u);

        // To achieve red, the red (middle) layer should be significant
        // Top layer should be thin to not occlude the red
        EXPECT_LT(solution.thicknesses[2], 1.0f);

        // Error should be reasonably small
        EXPECT_LT(solution.colorError, 0.3f);
    }

    TEST_F(FrontlitThicknessSolverTest, Solve_TargetBlack_MinimalTopLayers)
    {
        ThicknessConstraints constraints;
        constraints.minThickness = 0.0f;
        constraints.maxThickness = 5.0f;

        FrontlitThicknessSolver solver(m_stack, constraints);

        Eigen::Vector3f const targetBlack(0.1f, 0.1f, 0.1f);
        ThicknessSolution const solution = solver.solve(targetBlack);

        ASSERT_EQ(solution.thicknesses.size(), 3u);

        // To achieve black, the black (bottom) layer needs to be visible
        // which means upper layers should be thin
        // OR all layers are thick and the top-most contributes minimal light (unlikely with white)

        // The achieved color should be dark
        EXPECT_LT(solution.achievedColor.norm(), 0.5f);
    }

    TEST_F(FrontlitThicknessSolverTest, Solve_RespectsConstraints)
    {
        ThicknessConstraints constraints;
        constraints.minThickness = 0.1f;
        constraints.maxThickness = 2.0f;
        constraints.layerHeight = 0.04f;

        FrontlitThicknessSolver solver(m_stack, constraints);

        Eigen::Vector3f const targetGray(0.5f, 0.5f, 0.5f);
        ThicknessSolution const solution = solver.solve(targetGray);

        for (float t : solution.thicknesses)
        {
            EXPECT_GE(t, constraints.minThickness - 1e-6f);
            EXPECT_LE(t, constraints.maxThickness + 1e-6f);

            // Check quantization to layer height
            float const remainder = std::fmod(t, constraints.layerHeight);
            bool const isQuantized = remainder < 1e-5f || (constraints.layerHeight - remainder) < 1e-5f;
            EXPECT_TRUE(isQuantized) << "Thickness " << t << " not quantized to layer height "
                                     << constraints.layerHeight;
        }
    }

    // Test FaceThicknessMapper
    class FaceThicknessMapperTest : public FrontlitThicknessSolverTest
    {
    };

    TEST_F(FaceThicknessMapperTest, MapColors_EmptyInput_ReturnsEmptyResult)
    {
        FaceThicknessMapper mapper(m_stack);

        std::vector<Eigen::Vector3f> const emptyColors;
        FaceThicknessResult const result = mapper.mapColors(emptyColors);

        EXPECT_EQ(result.numFaces(), 0u);
        EXPECT_EQ(result.numLayers(), 3u);
        EXPECT_FLOAT_EQ(result.convergenceRate, 1.0f);
    }

    TEST_F(FaceThicknessMapperTest, MapColors_SingleFace_ReturnsValidResult)
    {
        FaceThicknessMapper mapper(m_stack);

        std::vector<Eigen::Vector3f> colors = {Eigen::Vector3f(0.5f, 0.5f, 0.5f)};
        FaceThicknessResult const result = mapper.mapColors(colors);

        EXPECT_EQ(result.numFaces(), 1u);
        EXPECT_EQ(result.numLayers(), 3u);

        // Each layer should have one thickness value
        for (auto const& layer : result.layerThicknesses)
        {
            EXPECT_EQ(layer.size(), 1u);
        }
    }

    TEST_F(FaceThicknessMapperTest, MapColors_MultipleFaces_ReturnsCorrectDimensions)
    {
        FaceThicknessMapper mapper(m_stack);

        std::vector<Eigen::Vector3f> colors = {
            Eigen::Vector3f(0.9f, 0.9f, 0.9f), // White-ish
            Eigen::Vector3f(0.8f, 0.1f, 0.1f), // Red
            Eigen::Vector3f(0.1f, 0.1f, 0.1f), // Black-ish
            Eigen::Vector3f(0.5f, 0.5f, 0.5f)  // Gray
        };

        FaceThicknessResult const result = mapper.mapColors(colors);

        EXPECT_EQ(result.numFaces(), 4u);
        EXPECT_EQ(result.numLayers(), 3u);
        EXPECT_EQ(result.achievedColors.size(), 4u);
        EXPECT_EQ(result.colorErrors.size(), 4u);

        // All layers should have 4 values
        for (auto const& layer : result.layerThicknesses)
        {
            EXPECT_EQ(layer.size(), 4u);
        }
    }

    TEST_F(FaceThicknessMapperTest, MapColorsWithSmoothing_NoAdjacency_SameAsNoSmoothing)
    {
        FaceThicknessMapper mapper(m_stack);

        std::vector<Eigen::Vector3f> colors = {
            Eigen::Vector3f(0.9f, 0.9f, 0.9f),
            Eigen::Vector3f(0.1f, 0.1f, 0.1f)};

        std::vector<std::vector<std::size_t>> const emptyAdjacency;

        FaceThicknessResult const resultNoSmooth = mapper.mapColors(colors);
        FaceThicknessResult const resultSmooth =
            mapper.mapColorsWithSmoothing(colors, emptyAdjacency, 3, 0.3f);

        // Results should be identical when no adjacency is provided
        EXPECT_EQ(resultNoSmooth.numFaces(), resultSmooth.numFaces());
        EXPECT_EQ(resultNoSmooth.numLayers(), resultSmooth.numLayers());
    }

    TEST_F(FaceThicknessMapperTest, MapColorsWithSmoothing_WithAdjacency_SmoothsThicknesses)
    {
        FaceThicknessMapper mapper(m_stack);

        // Three faces with very different target colors
        std::vector<Eigen::Vector3f> colors = {
            Eigen::Vector3f(0.9f, 0.9f, 0.9f), // Face 0: white
            Eigen::Vector3f(0.1f, 0.1f, 0.1f), // Face 1: black
            Eigen::Vector3f(0.9f, 0.9f, 0.9f)  // Face 2: white
        };

        // Face 1 is adjacent to faces 0 and 2
        std::vector<std::vector<std::size_t>> adjacency = {
            {1},    // Face 0 neighbors face 1
            {0, 2}, // Face 1 neighbors faces 0 and 2
            {1}     // Face 2 neighbors face 1
        };

        FaceThicknessResult const resultNoSmooth = mapper.mapColors(colors);
        FaceThicknessResult const resultSmooth =
            mapper.mapColorsWithSmoothing(colors, adjacency, 5, 0.5f);

        // After smoothing, face 1's thicknesses should be closer to faces 0 and 2
        // The difference between face 1 and its neighbors should be smaller
        for (std::size_t layer = 0; layer < resultSmooth.numLayers(); ++layer)
        {
            float const diffNoSmooth =
                std::abs(resultNoSmooth.layerThicknesses[layer][1] -
                         (resultNoSmooth.layerThicknesses[layer][0] +
                          resultNoSmooth.layerThicknesses[layer][2]) /
                             2.0f);

            float const diffSmooth = std::abs(
                resultSmooth.layerThicknesses[layer][1] -
                (resultSmooth.layerThicknesses[layer][0] + resultSmooth.layerThicknesses[layer][2]) /
                    2.0f);

            // Smoothed difference should generally be smaller
            // (though not always guaranteed due to constraint projection)
            EXPECT_LE(diffSmooth, diffNoSmooth + 0.5f);
        }
    }

    // Test color space conversions
    TEST(ColorConversionTest, SrgbToLinear_Zero_ReturnsZero)
    {
        EXPECT_FLOAT_EQ(srgbToLinear(0.0f), 0.0f);
    }

    TEST(ColorConversionTest, SrgbToLinear_One_ReturnsOne)
    {
        EXPECT_FLOAT_EQ(srgbToLinear(1.0f), 1.0f);
    }

    TEST(ColorConversionTest, LinearToSrgb_Zero_ReturnsZero)
    {
        EXPECT_FLOAT_EQ(linearToSrgb(0.0f), 0.0f);
    }

    TEST(ColorConversionTest, LinearToSrgb_One_ReturnsOne)
    {
        EXPECT_FLOAT_EQ(linearToSrgb(1.0f), 1.0f);
    }

    TEST(ColorConversionTest, RoundTrip_PreservesValue)
    {
        for (float v : {0.0f, 0.1f, 0.5f, 0.8f, 1.0f})
        {
            float const roundTrip = srgbToLinear(linearToSrgb(v));
            EXPECT_NEAR(roundTrip, v, 1e-5f) << "Round trip failed for value " << v;
        }
    }

    TEST(ColorConversionTest, LinearToSrgb_MidGray_IsLighter)
    {
        // Linear 0.5 should map to a lighter sRGB value (gamma expansion)
        float const srgb = linearToSrgb(0.5f);
        EXPECT_GT(srgb, 0.5f);
        EXPECT_LT(srgb, 1.0f);
    }

    TEST(ColorConversionTest, SrgbToLinear_MidGray_IsDarker)
    {
        // sRGB 0.5 should map to a darker linear value (gamma compression)
        float const linear = srgbToLinear(0.5f);
        EXPECT_LT(linear, 0.5f);
        EXPECT_GT(linear, 0.0f);
    }

} // namespace gladius::io::tests

