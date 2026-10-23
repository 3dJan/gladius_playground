#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/VolumeDataTranslucency.h"
#include "nodes/Builder.h"
#include "nodes/Model.h"
#include "nodes/ModelUtils.h"
#include "nodes/nodesfwd.h"

#include <gtest/gtest.h>
#include <lib3mf_implicit.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace gladius::io::tests
{
    namespace
    {
        /// @brief Build an implicit function with a pos input plus the given scalar outputs.
        /// @param model The 3MF model to add the function to
        /// @param scalarOutputNames Names of the scalar output channels to declare
        /// @return the new function
        Lib3MF::PImplicitFunction addScalarFunction(Lib3MF::PModel const & model,
                                                    std::vector<std::string> const & scalarOutputNames)
        {
            auto function = model->AddImplicitFunction();
            function->SetDisplayName("TranslucentPart");
            function->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            for (auto const & outputName : scalarOutputNames)
            {
                function->AddOutput(outputName, "output", Lib3MF::eImplicitPortType::Scalar);
            }
            return function;
        }

        /// @brief Collect the names of every property attached to a volume data.
        std::vector<std::string> collectPropertyNames(Lib3MF::PVolumeData const & volumeData)
        {
            std::vector<std::string> names;
            for (auto propertyIndex = 0u; propertyIndex < volumeData->GetPropertyCount();
                 ++propertyIndex)
            {
                names.push_back(volumeData->GetProperty(propertyIndex)->GetName());
            }
            return names;
        }
    } // namespace

    class VolumeDataTranslucencyPropertyTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_wrapper = gladius::io::loadLib3mfScoped();
            ASSERT_TRUE(m_wrapper) << "Failed to load the lib3mf library";
            m_model = m_wrapper->CreateModel();
            ASSERT_TRUE(m_model) << "Failed to create the 3MF model";
        }

        void TearDown() override
        {
            m_model = nullptr;
            m_wrapper = nullptr;
        }

        Lib3MF::PWrapper m_wrapper;
        Lib3MF::PModel m_model;
    };

    // ========================================================================
    // io::VolumeDataTranslucency helpers
    // ========================================================================

    TEST_F(VolumeDataTranslucencyPropertyTest,
           FindTranslucencyPropertyIndex_WithoutProperties_ReturnsSentinel)
    {
        // Arrange
        auto volumeData = m_model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);

        // Act & Assert
        EXPECT_EQ(findTranslucencyPropertyIndex(volumeData), NO_TRANSLUCENCY_PROPERTY);
        EXPECT_EQ(findTranslucencyProperty(volumeData), nullptr);
    }

    TEST_F(VolumeDataTranslucencyPropertyTest,
           FindTranslucencyPropertyIndex_NullVolumeData_ReturnsSentinel)
    {
        // Arrange & Act & Assert
        EXPECT_EQ(findTranslucencyPropertyIndex(nullptr), NO_TRANSLUCENCY_PROPERTY);
        EXPECT_EQ(findTranslucencyProperty(nullptr), nullptr);
    }

    TEST_F(VolumeDataTranslucencyPropertyTest,
           AddTranslucencyProperty_ValidFunction_StoresNameAndChannel)
    {
        // Arrange
        auto function = addScalarFunction(m_model, {"translucency"});
        auto volumeData = m_model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);

        // Act
        auto property = addTranslucencyProperty(volumeData, function, "translucency");

        // Assert
        ASSERT_NE(property, nullptr);
        EXPECT_EQ(property->GetName(), nodes::FieldNames::Translucency);
        EXPECT_EQ(property->GetChannelName(), "translucency");
        EXPECT_FALSE(property->IsRequired());
        EXPECT_EQ(property->GetFunctionResourceID(), function->GetResourceID());
        EXPECT_EQ(volumeData->GetPropertyCount(), 1u);
        EXPECT_EQ(findTranslucencyPropertyIndex(volumeData), 0u);
    }

    TEST_F(VolumeDataTranslucencyPropertyTest,
           AddTranslucencyProperty_SecondCall_ReturnsNull)
    {
        // Arrange
        auto function = addScalarFunction(m_model, {"translucency"});
        auto volumeData = m_model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);
        ASSERT_NE(addTranslucencyProperty(volumeData, function, "translucency"), nullptr);

        // Act
        auto const second = addTranslucencyProperty(volumeData, function, "other");

        // Assert
        EXPECT_EQ(second, nullptr);
        EXPECT_EQ(volumeData->GetPropertyCount(), 1u);
    }

    TEST_F(VolumeDataTranslucencyPropertyTest,
           AddTranslucencyProperty_NullFunction_ReturnsNull)
    {
        // Arrange
        auto volumeData = m_model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);

        // Act & Assert
        EXPECT_EQ(addTranslucencyProperty(volumeData, nullptr, "translucency"), nullptr);
        EXPECT_EQ(volumeData->GetPropertyCount(), 0u);
    }

    TEST_F(VolumeDataTranslucencyPropertyTest,
           FindTranslucencyProperty_OtherPropertyFirst_ReturnsTranslucency)
    {
        // Arrange
        auto function = addScalarFunction(m_model, {"translucency", "temperature"});
        auto volumeData = m_model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);
        auto const other = volumeData->AddPropertyFromFunction("temperature", function.get());
        ASSERT_NE(other, nullptr);
        ASSERT_NE(addTranslucencyProperty(volumeData, function, "translucency"), nullptr);

        // Act
        auto const property = findTranslucencyProperty(volumeData);

        // Assert
        ASSERT_NE(property, nullptr);
        EXPECT_EQ(property->GetName(), nodes::FieldNames::Translucency);
        EXPECT_EQ(findTranslucencyPropertyIndex(volumeData), 1u);
    }

    TEST_F(VolumeDataTranslucencyPropertyTest,
           RemoveTranslucencyProperty_TwoProperties_KeepsTheOtherOne)
    {
        // Arrange
        auto function = addScalarFunction(m_model, {"translucency", "temperature"});
        auto volumeData = m_model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);
        ASSERT_NE(volumeData->AddPropertyFromFunction("temperature", function.get()), nullptr);
        ASSERT_NE(addTranslucencyProperty(volumeData, function, "translucency"), nullptr);

        // Act
        ASSERT_TRUE(removeTranslucencyProperty(volumeData));

        // Assert
        EXPECT_EQ(volumeData->GetPropertyCount(), 1u);
        EXPECT_EQ(collectPropertyNames(volumeData), std::vector<std::string>{"temperature"});
        EXPECT_EQ(findTranslucencyPropertyIndex(volumeData), NO_TRANSLUCENCY_PROPERTY);
    }

    TEST_F(VolumeDataTranslucencyPropertyTest,
           RemoveTranslucencyProperty_WithoutProperty_ReturnsFalse)
    {
        // Arrange
        auto volumeData = m_model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);

        // Act & Assert
        EXPECT_FALSE(removeTranslucencyProperty(volumeData));
        EXPECT_EQ(volumeData->GetPropertyCount(), 0u);
    }

    TEST_F(VolumeDataTranslucencyPropertyTest,
           TranslucencyProperty_WrittenAndReadBack_KeepsNameFunctionAndChannel)
    {
        // Arrange
        auto function = addScalarFunction(m_model, {"translucency"});
        auto volumeData = m_model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);
        ASSERT_NE(addTranslucencyProperty(volumeData, function, "translucency"), nullptr);

        auto meshObject = m_model->AddMeshObject();
        ASSERT_NE(meshObject, nullptr);
        meshObject->SetVolumeData(volumeData.get());

        // Act - serialize to a buffer and read it back into a fresh model
        std::vector<Lib3MF_uint8> buffer;
        m_model->QueryWriter("3mf")->WriteToBuffer(buffer);
        ASSERT_FALSE(buffer.empty());
        auto const roundTripped = m_wrapper->CreateModel();
        ASSERT_NE(roundTripped, nullptr);
        roundTripped->QueryReader("3mf")->ReadFromBuffer(buffer);

        // Assert
        Lib3MF::PVolumeData importedVolumeData;
        auto resourceIterator = roundTripped->GetResources();
        while (resourceIterator->MoveNext())
        {
            importedVolumeData = std::dynamic_pointer_cast<Lib3MF::CVolumeData>(
              resourceIterator->GetCurrent());
            if (importedVolumeData)
            {
                break;
            }
        }
        ASSERT_NE(importedVolumeData, nullptr) << "The volume data did not survive the round trip";
        ASSERT_EQ(importedVolumeData->GetPropertyCount(), 1u);

        auto const property = findTranslucencyProperty(importedVolumeData);
        ASSERT_NE(property, nullptr) << "The translucency property did not survive the round trip";
        EXPECT_EQ(property->GetName(), nodes::FieldNames::Translucency);
        EXPECT_EQ(property->GetChannelName(), "translucency");

        auto const referenced = roundTripped->GetResourceByID(property->GetFunctionResourceID());
        ASSERT_NE(referenced, nullptr);
        EXPECT_EQ(referenced->GetModelResourceID(), function->GetModelResourceID());
    }
} // namespace gladius::io::tests

namespace gladius::nodes::tests
{
    class ModelUtilsScalarChannelTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_model.createBeginEndWithDefaultInAndOuts();
        }

        Model m_model;
    };

    TEST_F(ModelUtilsScalarChannelTest,
           IsQualifiedForScalarPropertyChannel_DefaultModel_IsQualifiedByShapeChannel)
    {
        // Arrange - the default End node exposes shape (float) and color (float3).
        // Act & Assert
        EXPECT_TRUE(isQualifiedForScalarPropertyChannel(m_model));
    }

    TEST_F(ModelUtilsScalarChannelTest,
           IsQualifiedForScalarPropertyChannel_WithoutBeginNode_ReturnsFalse)
    {
        // Arrange
        Model model;

        // Act & Assert
        EXPECT_FALSE(isQualifiedForScalarPropertyChannel(model));
        EXPECT_TRUE(collectScalarOutputChannelNames(model).empty());
    }

    TEST_F(ModelUtilsScalarChannelTest,
           CollectScalarOutputChannelNames_MixedChannels_ReturnsSortedScalarsOnly)
    {
        // Arrange
        m_model.addFunctionOutput("translucency", VariantParameter(float{0.25f}));
        m_model.addFunctionOutput("opacity", VariantParameter(float{0.75f}));

        // Act
        auto const channelNames = collectScalarOutputChannelNames(m_model);

        // Assert
        EXPECT_EQ(channelNames,
                  (std::vector<std::string>{"opacity", "shape", "translucency"}));
    }

    TEST_F(ModelUtilsScalarChannelTest,
           CollectScalarOutputChannelNames_VectorChannels_AreExcluded)
    {
        // Arrange
        m_model.addFunctionOutput("temperature", VariantParameter(float3{1.f, 2.f, 3.f}));

        // Act
        auto const channelNames = collectScalarOutputChannelNames(m_model);

        // Assert
        EXPECT_TRUE(std::ranges::find(channelNames, "temperature") == channelNames.end());
        EXPECT_TRUE(std::ranges::find(channelNames, "color") == channelNames.end());
    }

    class BuilderTranslucencyOutputTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_target.createBeginEndWithDefaultInAndOuts();
            m_referenced.createBeginEndWithDefaultInAndOuts();
            m_referenced.setResourceId(ResourceId{42});
            m_referenced.addFunctionOutput("translucency", VariantParameter(float{0.4f}));
            m_referenced.addFunctionOutput("opacity", VariantParameter(float{0.6f}));
        }

        Model m_target;
        Model m_referenced;
    };

    TEST_F(BuilderTranslucencyOutputTest,
           AppendFunctionForTranslucencyOutput_CustomChannel_CreatesAndWiresSink)
    {
        // Arrange
        nodes::Builder builder;

        // Act
        builder.appendFunctionForTranslucencyOutput(
          m_target, m_referenced, m_target.getBeginNode()->getOutputs().at(FieldNames::Pos),
          nodes::Matrix4x4{}, "opacity");

        // Assert
        auto * endNode = m_target.getEndNode();
        ASSERT_NE(endNode, nullptr);
        auto * sink = endNode->getParameter(FieldNames::Translucency);
        ASSERT_NE(sink, nullptr) << "The translucency output channel must be created on demand";
        EXPECT_TRUE(sink->getSource().has_value());
    }

    TEST_F(BuilderTranslucencyOutputTest,
           AppendFunctionForTranslucencyOutput_UnknownChannel_Throws)
    {
        // Arrange
        nodes::Builder builder;

        // Act & Assert
        EXPECT_THROW(builder.appendFunctionForTranslucencyOutput(
                       m_target, m_referenced,
                       m_target.getBeginNode()->getOutputs().at(FieldNames::Pos),
                       nodes::Matrix4x4{}, "does_not_exist"),
                     std::exception);
    }

    TEST_F(BuilderTranslucencyOutputTest,
           AppendFunctionForTranslucencyOutput_VectorChannel_Throws)
    {
        // Arrange
        nodes::Builder builder;

        // Act & Assert - "color" is a float3 channel, so it cannot drive translucency.
        EXPECT_THROW(builder.appendFunctionForTranslucencyOutput(
                       m_target, m_referenced,
                       m_target.getBeginNode()->getOutputs().at(FieldNames::Pos),
                       nodes::Matrix4x4{}, "color"),
                     std::exception);
    }

    TEST_F(BuilderTranslucencyOutputTest,
           AppendFunctionForTranslucencyOutput_FunctionWithoutPos_Throws)
    {
        // Arrange
        nodes::Builder builder;
        Model withoutPos;
        withoutPos.createBeginEnd();
        withoutPos.addFunctionOutput(FieldNames::Translucency, VariantParameter(float{0.4f}));

        // Act & Assert
        EXPECT_THROW(builder.appendFunctionForTranslucencyOutput(
                       m_target, withoutPos,
                       m_target.getBeginNode()->getOutputs().at(FieldNames::Pos),
                       nodes::Matrix4x4{}),
                     std::exception);
    }
} // namespace gladius::nodes::tests
