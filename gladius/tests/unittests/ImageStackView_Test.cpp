#include <gtest/gtest.h>
#include "ui/ImageStackView.h"
#include "io/3mf/ImageStack.h"

namespace gladius::ui::tests
{
    class ImageStackViewTest : public ::testing::Test
    {
      protected:
        /// Create a simple test ImageStack with 3 layers
        io::ImageStack createTestStack()
        {
            io::ImageStack stack;
            stack.setResourceId(42);
            
            // Add 3 layers of 4x4 RGBA images
            for (int i = 0; i < 3; ++i)
            {
                io::ImageData data(4 * 4 * 4, static_cast<unsigned char>(i * 50));
                io::Image img(data, 4, 4);
                img.setFormat(io::PixelFormat::RGBA_8BIT);
                img.setBitDepth(8);
                stack.push_back(img);
            }
            return stack;
        }
    };

    TEST_F(ImageStackViewTest, Constructor_InitializesState)
    {
        // Act
        ImageStackView view;

        // Assert
        EXPECT_EQ(view.getCurrentLayerIndex(), 0);
        EXPECT_FALSE(view.isHovered());
    }

    TEST_F(ImageStackViewTest, SetImageStack_UpdatesState)
    {
        // Arrange
        ImageStackView view;
        auto stack = createTestStack();

        // Act
        view.setImageStack(&stack);

        // Assert - should start at layer 0
        EXPECT_EQ(view.getCurrentLayerIndex(), 0);
    }

    TEST_F(ImageStackViewTest, SetImageStack_Nullptr_ClearsState)
    {
        // Arrange
        ImageStackView view;
        auto stack = createTestStack();
        view.setImageStack(&stack);

        // Act
        view.setImageStack(nullptr);

        // Assert
        EXPECT_EQ(view.getCurrentLayerIndex(), 0);
    }

    TEST_F(ImageStackViewTest, SetCurrentLayerIndex_ValidIndex_Updates)
    {
        // Arrange
        ImageStackView view;
        auto stack = createTestStack();
        view.setImageStack(&stack);

        // Act
        view.setCurrentLayerIndex(2);

        // Assert
        EXPECT_EQ(view.getCurrentLayerIndex(), 2);
    }

    TEST_F(ImageStackViewTest, SetCurrentLayerIndex_NegativeIndex_ClampsToZero)
    {
        // Arrange
        ImageStackView view;
        auto stack = createTestStack();
        view.setImageStack(&stack);

        // Act
        view.setCurrentLayerIndex(-5);

        // Assert
        EXPECT_EQ(view.getCurrentLayerIndex(), 0);
    }

    TEST_F(ImageStackViewTest, SetCurrentLayerIndex_TooLarge_ClampsToMax)
    {
        // Arrange
        ImageStackView view;
        auto stack = createTestStack();  // 3 layers
        view.setImageStack(&stack);

        // Act
        view.setCurrentLayerIndex(10);

        // Assert - should clamp to max valid index (2)
        EXPECT_EQ(view.getCurrentLayerIndex(), 2);
    }

    TEST_F(ImageStackViewTest, SetCurrentLayerIndex_NoImageStack_StaysZero)
    {
        // Arrange
        ImageStackView view;

        // Act
        view.setCurrentLayerIndex(5);

        // Assert - should stay at 0 when no stack
        EXPECT_EQ(view.getCurrentLayerIndex(), 0);
    }
}
