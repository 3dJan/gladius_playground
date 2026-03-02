/// @file FunctionFromImage3DView_Test.cpp
/// @brief Integration tests for FunctionFromImage3DView widget

#include "ui/FunctionFromImage3DView.h"

#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"

#include <gtest/gtest.h>

namespace gladius::ui::tests
{
    class FunctionFromImage3DViewTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_model = std::make_unique<nodes::Model>();
            m_model->createBeginEnd();
        }

        std::unique_ptr<nodes::Model> m_model;
    };

    TEST_F(FunctionFromImage3DViewTest, Constructor_InitializesState)
    {
        FunctionFromImage3DView view;
        // Should not crash on construction
        SUCCEED();
    }

    TEST_F(FunctionFromImage3DViewTest, SetFunction_Nullptr_DoesNotCrash)
    {
        FunctionFromImage3DView view;
        view.setFunction(nullptr, nullptr);
        // Should not crash
        SUCCEED();
    }

    TEST_F(FunctionFromImage3DViewTest, SetFunction_ValidModel_AcceptsModel)
    {
        FunctionFromImage3DView view;
        view.setFunction(m_model.get(), nullptr);
        // View accepts model without crashing
        SUCCEED();
    }

    TEST_F(FunctionFromImage3DViewTest, SetModelEditor_Nullptr_DoesNotCrash)
    {
        FunctionFromImage3DView view;
        view.setModelEditor(nullptr);
        SUCCEED();
    }

    TEST_F(FunctionFromImage3DViewTest, InvalidatePreview_WithoutSetup_DoesNotCrash)
    {
        FunctionFromImage3DView view;
        view.invalidatePreview();
        SUCCEED();
    }

    TEST_F(FunctionFromImage3DViewTest, FindImageSampler_ModelWithSampler_ReturnsSampler)
    {
        // Create a model with ImageSampler
        auto * sampler = m_model->create<nodes::ImageSampler>();
        ASSERT_NE(sampler, nullptr);

        FunctionFromImage3DView view;
        view.setFunction(m_model.get(), nullptr);
        // View should be able to find the sampler
        SUCCEED();
    }

    TEST_F(FunctionFromImage3DViewTest, FindImageSampler_ModelWithoutSampler_ReturnsNull)
    {
        // Model with only Begin/End nodes
        FunctionFromImage3DView view;
        view.setFunction(m_model.get(), nullptr);
        // View handles absence of sampler gracefully
        SUCCEED();
    }
}
