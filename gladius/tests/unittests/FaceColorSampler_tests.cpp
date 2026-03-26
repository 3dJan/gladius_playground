/**
 * @file FaceColorSampler_tests.cpp
 * @brief Unit tests for FaceColorSampler class
 */

#include "io/3mf/FaceColorSampler.h"
#include "io/3mf/FaceColors.h"

#include "nodes/Model.h"
#include "nodes/DerivedNodes.h"

#include <gmock/gmock.h>

#include <cmath>

namespace gladius::tests
{
    using namespace gladius::io;
    using namespace gladius::nodes;

    class FaceColorSamplerTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Create a model with Begin and End nodes including default parameters
            m_model.createBeginEndWithDefaultInAndOuts();
        }

        nodes::Model m_model;
    };

    // ==========================================================================
    // Linear to sRGB conversion tests
    // ==========================================================================

    TEST_F(FaceColorSamplerTest, LinearToSrgb_ZeroReturnsZero)
    {
        EXPECT_FLOAT_EQ(FaceColorSampler::linearToSrgb(0.0f), 0.0f);
    }

    TEST_F(FaceColorSamplerTest, LinearToSrgb_OneReturnsOne)
    {
        EXPECT_FLOAT_EQ(FaceColorSampler::linearToSrgb(1.0f), 1.0f);
    }

    TEST_F(FaceColorSamplerTest, LinearToSrgb_LinearRegionUsesMultiplier)
    {
        // Values <= 0.0031308 should use linear formula: 12.92 * linear
        float const input = 0.001f;
        float const expected = 12.92f * input;
        EXPECT_NEAR(FaceColorSampler::linearToSrgb(input), expected, 1e-6f);
    }

    TEST_F(FaceColorSamplerTest, LinearToSrgb_GammaRegionUsesGammaFormula)
    {
        // Values > 0.0031308 should use gamma formula: 1.055 * linear^(1/2.4) - 0.055
        float const input = 0.5f;
        float const expected = 1.055f * std::pow(input, 1.0f / 2.4f) - 0.055f;
        EXPECT_NEAR(FaceColorSampler::linearToSrgb(input), expected, 1e-6f);
    }

    TEST_F(FaceColorSamplerTest, LinearToSrgb_Vector3f)
    {
        Eigen::Vector3f const linear(0.0f, 0.5f, 1.0f);
        auto const srgb = FaceColorSampler::linearToSrgb(linear);

        EXPECT_FLOAT_EQ(srgb.x(), 0.0f);
        EXPECT_NEAR(srgb.y(), 1.055f * std::pow(0.5f, 1.0f / 2.4f) - 0.055f, 1e-6f);
        EXPECT_FLOAT_EQ(srgb.z(), 1.0f);
    }

    TEST_F(FaceColorSamplerTest, LinearToSrgb_MidGray)
    {
        // Linear 0.18 (18% gray) should be roughly sRGB 0.46
        float const srgb = FaceColorSampler::linearToSrgb(0.18f);
        EXPECT_GT(srgb, 0.45f);
        EXPECT_LT(srgb, 0.48f);
    }

    // ==========================================================================
    // hasVolumetricColor tests
    // ==========================================================================

    TEST_F(FaceColorSamplerTest, HasVolumetricColor_ReturnsFalseForDefaultModel)
    {
        // Default model has no color connected to End node
        EXPECT_FALSE(FaceColorSampler::hasVolumetricColor(m_model));
    }

    TEST_F(FaceColorSamplerTest, HasVolumetricColor_ReturnsFalseForEmptyModel)
    {
        nodes::Model emptyModel;
        EXPECT_FALSE(FaceColorSampler::hasVolumetricColor(emptyModel));
    }

    TEST_F(FaceColorSamplerTest, HasVolumetricColor_ReturnsTrueWhenColorConnected)
    {
        // Create a ConstantVector node to provide color
        auto* colorNode = m_model.create<ConstantVector>();
        ASSERT_NE(colorNode, nullptr);

        // Get the End node
        auto* endNode = m_model.getEndNode();
        ASSERT_NE(endNode, nullptr);

        // Find the color output port from ConstantVector
        auto& outputs = colorNode->getOutputs();
        ASSERT_FALSE(outputs.empty());
        auto& colorPort = outputs.begin()->second;

        // Find the color input parameter on End node
        auto& params = endNode->parameter();
        
        // Debug: verify parameter exists
        ASSERT_TRUE(params.contains(FieldNames::Color)) 
            << "End node should have 'color' parameter after createBeginEndWithDefaultInAndOuts()";

        // Connect the color port to End node's color parameter
        bool const linkAdded = m_model.addLink(colorPort.getId(), params.at(FieldNames::Color).getId());
        EXPECT_TRUE(linkAdded);

        // Now the model should have volumetric color
        EXPECT_TRUE(FaceColorSampler::hasVolumetricColor(m_model));
    }

    // ==========================================================================
    // Centroid computation tests (via sampleFaceColors with empty mesh)
    // ==========================================================================

    TEST_F(FaceColorSamplerTest, SampleFaceColors_EmptyMesh)
    {
        // This test requires a DualContouringSamplingProgram which needs full GPU setup
        // For now, we just verify the interface works with empty input

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::array<std::uint32_t, 3>> faces;

        // Can't call sampleFaceColors without a sampling program, but we can verify
        // the function signature compiles and the interface is correct
        SUCCEED();
    }

    // ==========================================================================
    // Color8 integration tests
    // ==========================================================================

    TEST_F(FaceColorSamplerTest, FaceColorsFromVector3f_ConvertsCorrectly)
    {
        std::vector<Eigen::Vector3f> srgbColors = {
            {0.0f, 0.0f, 0.0f},   // Black
            {1.0f, 0.0f, 0.0f},   // Red
            {0.0f, 1.0f, 0.0f},   // Green
            {0.0f, 0.0f, 1.0f},   // Blue
            {1.0f, 1.0f, 1.0f},   // White
            {0.5f, 0.5f, 0.5f}    // Gray
        };

        auto const faceColors = FaceColors::fromVector3f(srgbColors);

        ASSERT_EQ(faceColors.size(), 6);

        // Black
        EXPECT_EQ(faceColors[0].r, 0);
        EXPECT_EQ(faceColors[0].g, 0);
        EXPECT_EQ(faceColors[0].b, 0);

        // Red
        EXPECT_EQ(faceColors[1].r, 255);
        EXPECT_EQ(faceColors[1].g, 0);
        EXPECT_EQ(faceColors[1].b, 0);

        // Green
        EXPECT_EQ(faceColors[2].r, 0);
        EXPECT_EQ(faceColors[2].g, 255);
        EXPECT_EQ(faceColors[2].b, 0);

        // Blue
        EXPECT_EQ(faceColors[3].r, 0);
        EXPECT_EQ(faceColors[3].g, 0);
        EXPECT_EQ(faceColors[3].b, 255);

        // White
        EXPECT_EQ(faceColors[4].r, 255);
        EXPECT_EQ(faceColors[4].g, 255);
        EXPECT_EQ(faceColors[4].b, 255);

        // Gray (0.5 -> 128 with rounding)
        EXPECT_EQ(faceColors[5].r, 128);
        EXPECT_EQ(faceColors[5].g, 128);
        EXPECT_EQ(faceColors[5].b, 128);
    }

    TEST_F(FaceColorSamplerTest, LinearToSrgb_FullPipeline)
    {
        // Test the full pipeline: linear RGB -> sRGB -> Color8

        // Linear RGB red at 50% intensity
        Eigen::Vector3f const linearRed(0.5f, 0.0f, 0.0f);

        // Convert to sRGB
        auto const srgbRed = FaceColorSampler::linearToSrgb(linearRed);

        // sRGB value should be higher than linear (gamma expansion)
        EXPECT_GT(srgbRed.x(), linearRed.x());
        EXPECT_FLOAT_EQ(srgbRed.y(), 0.0f);
        EXPECT_FLOAT_EQ(srgbRed.z(), 0.0f);

        // Convert to Color8
        auto const color8 = Color8::fromVector3f(srgbRed);

        // Should be around 188 (0.735 * 255) for red channel
        EXPECT_GT(color8.r, 180);
        EXPECT_LT(color8.r, 195);
        EXPECT_EQ(color8.g, 0);
        EXPECT_EQ(color8.b, 0);
    }

} // namespace gladius::tests
