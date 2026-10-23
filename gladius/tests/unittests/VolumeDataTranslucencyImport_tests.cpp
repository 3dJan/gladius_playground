#include "Document.h"
#include "EventLogger.h"
#include "MeshSdfMethod.h"
#include "io/3mf/Importer3mf.h"
#include "io/3mf/Lib3mfLoader.h"
#include "nodes/Assembly.h"
#include "nodes/Model.h"
#include "nodes/NodeBase.h"
#include "nodes/nodesfwd.h"

#include <gtest/gtest.h>
#include <lib3mf_implicit.hpp>

#include <string>
#include <vector>

namespace gladius::io::tests
{
    namespace
    {
        Lib3MF::sTransform identityTransform()
        {
            Lib3MF::sTransform transform{};
            transform.m_Fields[0][0] = 1.0f;
            transform.m_Fields[1][1] = 1.0f;
            transform.m_Fields[2][2] = 1.0f;
            return transform;
        }

        /// @brief Add a mesh object carrying the given volume data to the build.
        /// @return the mesh object
        Lib3MF::PMeshObject addVolumeMesh(Lib3MF::PModel const & model,
                                          Lib3MF::PVolumeData const & volumeData)
        {
            auto meshObject = model->AddMeshObject();
            std::vector<Lib3MF::sPosition> const vertices = {
              {0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, {10.f, 10.f, 0.f}, {0.f, 10.f, 0.f}
            };
            std::vector<Lib3MF::sTriangle> const triangles = {{{0, 1, 2}, {0, 2, 3}}};
            meshObject->SetGeometry(vertices, triangles);
            if (volumeData)
            {
                meshObject->SetVolumeData(volumeData.get());
            }
            model->AddBuildItem(meshObject.get(), identityTransform());
            return meshObject;
        }
    } // namespace

    /// @brief Fixture covering how the importer wires a translucency property into the assembly.
    class VolumeDataTranslucencyImportTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
            m_wrapper = loadLib3mfScoped();
            ASSERT_TRUE(m_wrapper) << "Failed to load the lib3mf library";
            m_document = std::make_shared<Document>(m_logger);
            m_document->newEmptyModel();
        }

        /// @brief Create a function exposing shape plus the given scalar channels.
        Lib3MF::PImplicitFunction addFunction(std::string const & displayName,
                                              std::vector<std::string> const & scalarChannels)
        {
            auto function = m_document->get3mfModel()->AddImplicitFunction();
            function->SetDisplayName(displayName);
            function->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            function->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);
            for (auto const & channel : scalarChannels)
            {
                function->AddOutput(channel, "output", Lib3MF::eImplicitPortType::Scalar);
            }
            return function;
        }

        /// @brief Import functions and build items of the document's 3MF model.
        void importModel()
        {
            Importer3mf importer{m_logger};
            MeshSdfEvaluationConfig meshConfig;
            meshConfig.method = MeshSdfMethod::PureBVH;
            importer.setMeshSdfEvaluationConfig(meshConfig);
            importer.loadImplicitFunctions(m_document->get3mfModel(), *m_document);
            importer.loadBuildItems(m_document->get3mfModel(), *m_document);
        }

        /// @brief The assembly End node parameter driving translucency, if any.
        [[nodiscard]] nodes::VariantParameter * translucencySink() const
        {
            auto * assemblyModel = m_document->getAssembly()->assemblyModel().get();
            auto * endNode = assemblyModel->getEndNode();
            if (endNode == nullptr)
            {
                return nullptr;
            }
            return endNode->getParameter(nodes::FieldNames::Translucency);
        }

        events::SharedLogger m_logger;
        Lib3MF::PWrapper m_wrapper;
        std::shared_ptr<Document> m_document;
    };

    TEST_F(VolumeDataTranslucencyImportTest,
           LoadBuildItems_TranslucencyProperty_WiresAssemblyChannel)
    {
        // Arrange
        auto model = m_document->get3mfModel();
        auto function = addFunction("TranslucentPart", {nodes::FieldNames::Translucency});
        auto volumeData = model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);
        auto property =
          volumeData->AddPropertyFromFunction(nodes::FieldNames::Translucency, function.get());
        ASSERT_NE(property, nullptr);
        property->SetChannelName(nodes::FieldNames::Translucency);
        addVolumeMesh(model, volumeData);

        // Act
        importModel();

        // Assert
        auto * sink = translucencySink();
        ASSERT_NE(sink, nullptr) << "The importer must expose a translucency output channel";
        EXPECT_TRUE(sink->getSource().has_value())
          << "The translucency channel must be wired to the referenced function";
        EXPECT_EQ(m_logger->getErrorCount(), 0u);
    }

    TEST_F(VolumeDataTranslucencyImportTest,
           LoadBuildItems_CustomChannelName_WiresAssemblyChannel)
    {
        // Arrange
        auto model = m_document->get3mfModel();
        auto function = addFunction("PartWithOpacity", {"opacity"});
        auto volumeData = model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);
        auto property =
          volumeData->AddPropertyFromFunction(nodes::FieldNames::Translucency, function.get());
        ASSERT_NE(property, nullptr);
        property->SetChannelName("opacity");
        addVolumeMesh(model, volumeData);

        // Act
        importModel();

        // Assert
        auto * sink = translucencySink();
        ASSERT_NE(sink, nullptr);
        EXPECT_TRUE(sink->getSource().has_value());
    }

    TEST_F(VolumeDataTranslucencyImportTest,
           LoadBuildItems_PropertyWithUnknownChannel_LeavesChannelUnwired)
    {
        // Arrange
        auto model = m_document->get3mfModel();
        auto function = addFunction("OpaquePart", {nodes::FieldNames::Translucency});
        auto volumeData = model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);
        auto property =
          volumeData->AddPropertyFromFunction(nodes::FieldNames::Translucency, function.get());
        ASSERT_NE(property, nullptr);
        property->SetChannelName("not_an_output");
        addVolumeMesh(model, volumeData);

        // Act
        importModel();

        // Assert - the failure is reported but does not abort the import
        auto * sink = translucencySink();
        EXPECT_TRUE(sink == nullptr || !sink->getSource().has_value());
        EXPECT_EQ(m_logger->getWarningCount(), 1u);
    }

    TEST_F(VolumeDataTranslucencyImportTest,
           LoadBuildItems_TwoTranslucencyProperties_OnlyFirstOneWins)
    {
        // Arrange
        auto model = m_document->get3mfModel();
        auto firstFunction = addFunction("FirstPart", {nodes::FieldNames::Translucency});
        auto secondFunction = addFunction("SecondPart", {nodes::FieldNames::Translucency});

        auto firstVolumeData = model->AddVolumeData();
        ASSERT_NE(firstVolumeData, nullptr);
        ASSERT_NE(firstVolumeData->AddPropertyFromFunction(nodes::FieldNames::Translucency,
                                                           firstFunction.get()),
                  nullptr);
        addVolumeMesh(model, firstVolumeData);

        auto secondVolumeData = model->AddVolumeData();
        ASSERT_NE(secondVolumeData, nullptr);
        ASSERT_NE(secondVolumeData->AddPropertyFromFunction(nodes::FieldNames::Translucency,
                                                            secondFunction.get()),
                  nullptr);
        addVolumeMesh(model, secondVolumeData);

        // Act
        importModel();

        // Assert
        auto * sink = translucencySink();
        ASSERT_NE(sink, nullptr);
        EXPECT_TRUE(sink->getSource().has_value());
        EXPECT_EQ(m_logger->getWarningCount(), 1u);
    }

    TEST_F(VolumeDataTranslucencyImportTest,
           LoadBuildItems_ColorAndTranslucency_BothChannelsAreWired)
    {
        // Arrange
        auto model = m_document->get3mfModel();
        auto colorFunction = m_document->get3mfModel()->AddImplicitFunction();
        colorFunction->SetDisplayName("ColoredPart");
        colorFunction->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
        colorFunction->AddOutput("color", "output", Lib3MF::eImplicitPortType::Vector);

        auto translucencyFunction =
          addFunction("TranslucentPart", {nodes::FieldNames::Translucency});

        auto volumeData = model->AddVolumeData();
        ASSERT_NE(volumeData, nullptr);
        auto color = volumeData->CreateNewColor(colorFunction.get());
        ASSERT_NE(color, nullptr);
        color->SetChannelName("color");
        ASSERT_NE(volumeData->AddPropertyFromFunction(nodes::FieldNames::Translucency,
                                                      translucencyFunction.get()),
                  nullptr);
        addVolumeMesh(model, volumeData);

        // Act
        importModel();

        // Assert
        auto * endNode = m_document->getAssembly()->assemblyModel()->getEndNode();
        ASSERT_NE(endNode, nullptr);
        auto * colorSink = endNode->getParameter(nodes::FieldNames::Color);
        ASSERT_NE(colorSink, nullptr);
        EXPECT_TRUE(colorSink->getSource().has_value());

        auto * sink = translucencySink();
        ASSERT_NE(sink, nullptr);
        EXPECT_TRUE(sink->getSource().has_value());
        EXPECT_EQ(m_logger->getErrorCount(), 0u);
    }
} // namespace gladius::io::tests
