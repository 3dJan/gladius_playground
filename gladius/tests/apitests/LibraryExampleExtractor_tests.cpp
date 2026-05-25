#include "io/3mf/LibraryExampleExtractor.h"
#include "io/3mf/LibraryMetadata.h"

#include <lib3mf_implicit.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace gladius::io::tests
{
    namespace
    {
        /// @brief Creates a minimal library .3mf with a tagged function and an example caller.
        ///
        /// The tagged function has two scalar inputs (radius, height).
        /// The example function wires ConstantScalar nodes (5.0, 10.0) to those inputs via a
        /// FunctionCallNode.
        std::filesystem::path createScalarConstantTestLibrary(
          std::filesystem::path const & dir,
          std::string const & taggedFuncName = "testFunc",
          bool includeMetadata = true,
          bool includeFunctionCall = true)
        {
            auto wrapper = Lib3MF::CWrapper::loadLibrary();
            auto model = wrapper->CreateModel();

            auto taggedFunc = model->AddImplicitFunction();
            taggedFunc->SetDisplayName(taggedFuncName);
            taggedFunc->AddInput("radius", "radius", Lib3MF::eImplicitPortType::Scalar);
            taggedFunc->AddInput("height", "height", Lib3MF::eImplicitPortType::Scalar);
            taggedFunc->AddOutput("shape", "shape", Lib3MF::eImplicitPortType::Scalar);

            auto const taggedId = taggedFunc->GetModelResourceID();

            auto exampleFunc = model->AddImplicitFunction();
            exampleFunc->SetDisplayName("main");
            exampleFunc->AddInput("pos", "pos", Lib3MF::eImplicitPortType::Vector);
            exampleFunc->AddOutput("shape", "shape", Lib3MF::eImplicitPortType::Scalar);

            if (includeFunctionCall)
            {
                auto constRadius = exampleFunc->AddConstantNode("CR", "CR", "");
                constRadius->SetConstant(5.0);

                auto constHeight = exampleFunc->AddConstantNode("CH", "CH", "");
                constHeight->SetConstant(10.0);

                auto resNode = exampleFunc->AddResourceIdNode("Res", "Res", "");
                resNode->SetResource(std::static_pointer_cast<Lib3MF::CResource>(taggedFunc));

                auto fcNode = exampleFunc->AddFunctionCallNode("FC", "FC", "");
                fcNode->AddInput("radius", "radius");
                fcNode->AddInput("height", "height");

                exampleFunc->AddLink(resNode->GetOutputValue(), fcNode->GetInputFunctionID());
                exampleFunc->AddLinkByNames("CR.value", "FC.radius");
                exampleFunc->AddLinkByNames("CH.value", "FC.height");
            }

            if (includeMetadata)
            {
                writeLibraryMetadata(
                  model, LibraryMetadata{std::to_string(taggedId), "Test function", ""});
            }

            auto const path = dir / "scalar_lib.3mf";
            model->QueryWriter("3mf")->WriteToFile(path.string());
            return path;
        }

        /// @brief Creates a library .3mf where the example function passes a ConstantVector
        ///        to the tagged function's single vector input.
        std::filesystem::path createVectorConstantTestLibrary(std::filesystem::path const & dir)
        {
            auto wrapper = Lib3MF::CWrapper::loadLibrary();
            auto model = wrapper->CreateModel();

            auto taggedFunc = model->AddImplicitFunction();
            taggedFunc->SetDisplayName("vecFunc");
            taggedFunc->AddInput("offset", "offset", Lib3MF::eImplicitPortType::Vector);
            taggedFunc->AddOutput("shape", "shape", Lib3MF::eImplicitPortType::Scalar);

            auto const taggedId = taggedFunc->GetModelResourceID();

            auto exampleFunc = model->AddImplicitFunction();
            exampleFunc->SetDisplayName("main");
            exampleFunc->AddOutput("shape", "shape", Lib3MF::eImplicitPortType::Scalar);

            auto constVec = exampleFunc->AddConstVecNode("CV", "CV", "");
            Lib3MF::sVector vec{};
            vec.m_Coordinates[0] = 1.0;
            vec.m_Coordinates[1] = 2.0;
            vec.m_Coordinates[2] = 3.0;
            constVec->SetVector(vec);

            auto resNode = exampleFunc->AddResourceIdNode("Res", "Res", "");
            resNode->SetResource(std::static_pointer_cast<Lib3MF::CResource>(taggedFunc));

            auto fcNode = exampleFunc->AddFunctionCallNode("FC", "FC", "");
            fcNode->AddInput("offset", "offset");

            exampleFunc->AddLink(resNode->GetOutputValue(), fcNode->GetInputFunctionID());
            exampleFunc->AddLinkByNames("CV.vector", "FC.offset");

            writeLibraryMetadata(
              model, LibraryMetadata{std::to_string(taggedId), "Vector test function", ""});

            auto const path = dir / "vector_lib.3mf";
            model->QueryWriter("3mf")->WriteToFile(path.string());
            return path;
        }
    } // namespace

    class LibraryExampleExtractorTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_tempDir = std::filesystem::temp_directory_path() / "gladius_extractor_test";
            std::filesystem::create_directories(m_tempDir);
        }

        void TearDown() override
        {
            std::filesystem::remove_all(m_tempDir);
        }

        std::filesystem::path m_tempDir;
    };

    TEST_F(LibraryExampleExtractorTest,
           extractExampleConstants_WithScalarConstants_ReturnsExpectedValues)
    {
        // Arrange
        auto const path = createScalarConstantTestLibrary(m_tempDir);

        // Act
        auto const constants = extractExampleConstants(path, "testFunc");

        // Assert
        ASSERT_EQ(constants.size(), 2u);

        auto const findByName =
          [&](std::string const & name) -> ExampleConstantValue const *
        {
            auto it = std::find_if(
              constants.begin(), constants.end(),
              [&](auto const & c)
              {
                  return c.parameterName == name;
              });
            return it != constants.end() ? &(*it) : nullptr;
        };

        auto const * radius = findByName("radius");
        ASSERT_NE(radius, nullptr);
        EXPECT_EQ(radius->kind, ExampleConstantValue::Kind::Scalar);
        EXPECT_FLOAT_EQ(radius->scalarValue, 5.0f);

        auto const * height = findByName("height");
        ASSERT_NE(height, nullptr);
        EXPECT_EQ(height->kind, ExampleConstantValue::Kind::Scalar);
        EXPECT_FLOAT_EQ(height->scalarValue, 10.0f);
    }

    TEST_F(LibraryExampleExtractorTest,
           extractExampleConstants_WithNonExistentFile_ReturnsEmpty)
    {
        // Act
        auto const constants = extractExampleConstants("/nonexistent/path/file.3mf", "testFunc");

        // Assert
        EXPECT_TRUE(constants.empty());
    }

    TEST_F(LibraryExampleExtractorTest,
           extractExampleConstants_WithWrongFunctionName_ReturnsEmpty)
    {
        // Arrange
        auto const path = createScalarConstantTestLibrary(m_tempDir);

        // Act
        auto const constants = extractExampleConstants(path, "doesNotExist");

        // Assert
        EXPECT_TRUE(constants.empty());
    }

    TEST_F(LibraryExampleExtractorTest,
           extractExampleConstants_WithNoLibraryMetadata_ReturnsEmpty)
    {
        // Arrange
        auto const path = createScalarConstantTestLibrary(m_tempDir, "testFunc", false);

        // Act
        auto const constants = extractExampleConstants(path, "testFunc");

        // Assert
        EXPECT_TRUE(constants.empty());
    }

    TEST_F(LibraryExampleExtractorTest,
           extractExampleConstants_WithNoFunctionCallInExample_ReturnsEmpty)
    {
        // Arrange
        auto const path =
          createScalarConstantTestLibrary(m_tempDir, "testFunc", true, false);

        // Act
        auto const constants = extractExampleConstants(path, "testFunc");

        // Assert
        EXPECT_TRUE(constants.empty());
    }

    TEST_F(LibraryExampleExtractorTest,
           extractExampleConstants_WithVectorConstant_ReturnsVectorValue)
    {
        // Arrange
        auto const path = createVectorConstantTestLibrary(m_tempDir);

        // Act
        auto const constants = extractExampleConstants(path, "vecFunc");

        // Assert
        ASSERT_EQ(constants.size(), 1u);
        EXPECT_EQ(constants[0].kind, ExampleConstantValue::Kind::Vector);
        EXPECT_EQ(constants[0].parameterName, "offset");
        EXPECT_FLOAT_EQ(constants[0].vectorValue.x, 1.0f);
        EXPECT_FLOAT_EQ(constants[0].vectorValue.y, 2.0f);
        EXPECT_FLOAT_EQ(constants[0].vectorValue.z, 3.0f);
    }

} // namespace gladius::io::tests
