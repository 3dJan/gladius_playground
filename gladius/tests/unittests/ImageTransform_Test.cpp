#include <gtest/gtest.h>
#include "io/3mf/ImageStack.h"

namespace gladius::io::tests
{
    class ImageTransformTest : public ::testing::Test
    {
      protected:
        /// Create a simple 4x3 RGBA test image with known pixel values
        /// Layout (row-major, each cell is RGBA):
        /// Row 0: [R,0,0,A] [G,0,0,A] [B,0,0,A] [W,0,0,A]
        /// Row 1: [0,R,0,A] [0,G,0,A] [0,B,0,A] [0,W,0,A]
        /// Row 2: [0,0,R,A] [0,0,G,A] [0,0,B,A] [0,0,W,A]
        Image createTestImage()
        {
            unsigned int const width = 4;
            unsigned int const height = 3;
            unsigned int const channels = 4;  // RGBA
            ImageData data(width * height * channels, 0);

            // Row 0
            data[0] = 255; data[3] = 255;   // R
            data[4] = 0; data[5] = 255; data[7] = 255;  // G at (1,0)
            data[8] = 0; data[10] = 255; data[11] = 255; // B at (2,0)
            data[12] = 255; data[13] = 255; data[14] = 255; data[15] = 255; // W at (3,0)

            // Row 1 (offset 16)
            data[17] = 255; data[19] = 255;  // (0,1) green channel
            data[21] = 255; data[23] = 255;  // (1,1) green channel
            data[26] = 255; data[27] = 255;  // (2,1) blue channel
            data[29] = 255; data[30] = 255; data[31] = 255; // (3,1) white-ish

            // Row 2 (offset 32)
            data[34] = 255; data[35] = 255;  // (0,2) blue channel
            data[38] = 255; data[39] = 255;  // (1,2) blue channel
            data[42] = 255; data[43] = 255;  // (2,2) blue channel
            data[44] = 255; data[45] = 255; data[46] = 255; data[47] = 255; // (3,2) white

            Image img(data, width, height);
            img.setFormat(PixelFormat::RGBA_8BIT);
            img.setBitDepth(8);
            return img;
        }

        /// Create a simple 2x2 grayscale image for easier verification
        /// [1, 2]
        /// [3, 4]
        Image createSimpleGrayscaleImage()
        {
            ImageData data = {1, 2, 3, 4};
            Image img(data, 2, 2);
            img.setFormat(PixelFormat::GRAYSCALE_8BIT);
            img.setBitDepth(8);
            return img;
        }

        /// Create a 3x2 grayscale image (non-square)
        /// [1, 2, 3]
        /// [4, 5, 6]
        Image createRectangularImage()
        {
            ImageData data = {1, 2, 3, 4, 5, 6};
            Image img(data, 3, 2);
            img.setFormat(PixelFormat::GRAYSCALE_8BIT);
            img.setBitDepth(8);
            return img;
        }
    };

    TEST_F(ImageTransformTest, FlipHorizontal_SimpleGrayscale_MirrorsCorrectly)
    {
        // Arrange
        Image img = createSimpleGrayscaleImage();
        // [1, 2]
        // [3, 4]

        // Act
        img.flipHorizontal();

        // Assert - should become:
        // [2, 1]
        // [4, 3]
        auto const & data = img.getData();
        EXPECT_EQ(data[0], 2);
        EXPECT_EQ(data[1], 1);
        EXPECT_EQ(data[2], 4);
        EXPECT_EQ(data[3], 3);
    }

    TEST_F(ImageTransformTest, FlipVertical_SimpleGrayscale_MirrorsCorrectly)
    {
        // Arrange
        Image img = createSimpleGrayscaleImage();
        // [1, 2]
        // [3, 4]

        // Act
        img.flipVertical();

        // Assert - should become:
        // [3, 4]
        // [1, 2]
        auto const & data = img.getData();
        EXPECT_EQ(data[0], 3);
        EXPECT_EQ(data[1], 4);
        EXPECT_EQ(data[2], 1);
        EXPECT_EQ(data[3], 2);
    }

    TEST_F(ImageTransformTest, Rotate90CW_RectangularImage_RotatesAndSwapsDimensions)
    {
        // Arrange
        Image img = createRectangularImage();
        // [1, 2, 3]  (3x2)
        // [4, 5, 6]
        ASSERT_EQ(img.getWidth(), 3u);
        ASSERT_EQ(img.getHeight(), 2u);

        // Act
        img.rotate90CW();

        // Assert - should become (2x3):
        // [4, 1]
        // [5, 2]
        // [6, 3]
        EXPECT_EQ(img.getWidth(), 2u);
        EXPECT_EQ(img.getHeight(), 3u);

        auto const & data = img.getData();
        EXPECT_EQ(data[0], 4);
        EXPECT_EQ(data[1], 1);
        EXPECT_EQ(data[2], 5);
        EXPECT_EQ(data[3], 2);
        EXPECT_EQ(data[4], 6);
        EXPECT_EQ(data[5], 3);
    }

    TEST_F(ImageTransformTest, Rotate90CCW_RectangularImage_RotatesAndSwapsDimensions)
    {
        // Arrange
        Image img = createRectangularImage();
        // [1, 2, 3]  (3x2)
        // [4, 5, 6]
        ASSERT_EQ(img.getWidth(), 3u);
        ASSERT_EQ(img.getHeight(), 2u);

        // Act
        img.rotate90CCW();

        // Assert - should become (2x3):
        // [3, 6]
        // [2, 5]
        // [1, 4]
        EXPECT_EQ(img.getWidth(), 2u);
        EXPECT_EQ(img.getHeight(), 3u);

        auto const & data = img.getData();
        EXPECT_EQ(data[0], 3);
        EXPECT_EQ(data[1], 6);
        EXPECT_EQ(data[2], 2);
        EXPECT_EQ(data[3], 5);
        EXPECT_EQ(data[4], 1);
        EXPECT_EQ(data[5], 4);
    }

    TEST_F(ImageTransformTest, PadTo_SmallerImage_PadsWithZeros)
    {
        // Arrange
        Image img = createSimpleGrayscaleImage();
        // [1, 2]
        // [3, 4]
        ASSERT_EQ(img.getWidth(), 2u);
        ASSERT_EQ(img.getHeight(), 2u);

        // Act
        img.padTo(4, 3);

        // Assert - should become (4x3):
        // [1, 2, 0, 0]
        // [3, 4, 0, 0]
        // [0, 0, 0, 0]
        EXPECT_EQ(img.getWidth(), 4u);
        EXPECT_EQ(img.getHeight(), 3u);

        auto const & data = img.getData();
        EXPECT_EQ(data.size(), 12u);
        // Row 0
        EXPECT_EQ(data[0], 1);
        EXPECT_EQ(data[1], 2);
        EXPECT_EQ(data[2], 0);
        EXPECT_EQ(data[3], 0);
        // Row 1
        EXPECT_EQ(data[4], 3);
        EXPECT_EQ(data[5], 4);
        EXPECT_EQ(data[6], 0);
        EXPECT_EQ(data[7], 0);
        // Row 2
        EXPECT_EQ(data[8], 0);
        EXPECT_EQ(data[9], 0);
        EXPECT_EQ(data[10], 0);
        EXPECT_EQ(data[11], 0);
    }

    TEST_F(ImageTransformTest, PadTo_SameDimensions_NoChange)
    {
        // Arrange
        Image img = createSimpleGrayscaleImage();
        auto originalData = img.getData();

        // Act
        img.padTo(2, 2);

        // Assert
        EXPECT_EQ(img.getData(), originalData);
    }

    TEST_F(ImageTransformTest, PadTo_SmallerDimensions_Throws)
    {
        // Arrange
        Image img = createSimpleGrayscaleImage();

        // Act & Assert
        EXPECT_THROW(img.padTo(1, 2), std::runtime_error);
        EXPECT_THROW(img.padTo(2, 1), std::runtime_error);
    }

    TEST_F(ImageTransformTest, FlipHorizontal_EmptyImage_NoOp)
    {
        // Arrange
        Image img(ImageData{}, 0, 0);

        // Act & Assert - should not throw
        EXPECT_NO_THROW(img.flipHorizontal());
    }

    TEST_F(ImageTransformTest, Rotate90CW_ThenRotate90CCW_ReturnsOriginal)
    {
        // Arrange
        Image img = createRectangularImage();
        auto originalData = img.getData();
        auto originalWidth = img.getWidth();
        auto originalHeight = img.getHeight();

        // Act
        img.rotate90CW();
        img.rotate90CCW();

        // Assert
        EXPECT_EQ(img.getWidth(), originalWidth);
        EXPECT_EQ(img.getHeight(), originalHeight);
        EXPECT_EQ(img.getData(), originalData);
    }

    TEST_F(ImageTransformTest, FlipHorizontal_Twice_ReturnsOriginal)
    {
        // Arrange
        Image img = createSimpleGrayscaleImage();
        auto originalData = img.getData();

        // Act
        img.flipHorizontal();
        img.flipHorizontal();

        // Assert
        EXPECT_EQ(img.getData(), originalData);
    }

    TEST_F(ImageTransformTest, FlipVertical_Twice_ReturnsOriginal)
    {
        // Arrange
        Image img = createSimpleGrayscaleImage();
        auto originalData = img.getData();

        // Act
        img.flipVertical();
        img.flipVertical();

        // Assert
        EXPECT_EQ(img.getData(), originalData);
    }

    // ImageStack transform tests
    class ImageStackTransformTest : public ::testing::Test
    {
      protected:
        ImageStack createTestStack()
        {
            ImageStack stack;
            // Add 3 layers, each 2x2 grayscale
            for (int i = 0; i < 3; ++i)
            {
                ImageData data = {
                  static_cast<unsigned char>(i * 4 + 1),
                  static_cast<unsigned char>(i * 4 + 2),
                  static_cast<unsigned char>(i * 4 + 3),
                  static_cast<unsigned char>(i * 4 + 4)};
                Image img(data, 2, 2);
                img.setFormat(PixelFormat::GRAYSCALE_8BIT);
                stack.push_back(img);
            }
            return stack;
        }
    };

    TEST_F(ImageStackTransformTest, At_ValidIndex_ReturnsLayer)
    {
        // Arrange
        ImageStack stack = createTestStack();

        // Act & Assert
        EXPECT_EQ(stack.at(0).getData()[0], 1);
        EXPECT_EQ(stack.at(1).getData()[0], 5);
        EXPECT_EQ(stack.at(2).getData()[0], 9);
    }

    TEST_F(ImageStackTransformTest, At_InvalidIndex_Throws)
    {
        // Arrange
        ImageStack stack = createTestStack();

        // Act & Assert
        EXPECT_THROW(stack.at(3), std::out_of_range);
    }

    TEST_F(ImageStackTransformTest, FlipHorizontal_AllLayers_FlipsEach)
    {
        // Arrange
        ImageStack stack = createTestStack();
        // Layer 0: [1, 2, 3, 4] as 2x2

        // Act
        stack.flipHorizontal();

        // Assert - each layer should be flipped
        // Layer 0 [1,2,3,4] -> [2,1,4,3]
        EXPECT_EQ(stack.at(0).getData()[0], 2);
        EXPECT_EQ(stack.at(0).getData()[1], 1);
    }

    TEST_F(ImageStackTransformTest, Rotate90CW_AllLayers_RotatesEach)
    {
        // Arrange
        ImageStack stack = createTestStack();

        // Act
        stack.rotate90CW();

        // Assert - dimensions should swap for all layers
        EXPECT_EQ(stack.at(0).getWidth(), 2u);
        EXPECT_EQ(stack.at(0).getHeight(), 2u);  // Square, so same
    }
}
