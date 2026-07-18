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
            auto offsetPort = fcNode->AddInput("offset", "offset");
            offsetPort->SetType(Lib3MF::eImplicitPortType::Vector);

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

    TEST_F(LibraryExampleExtractorTest,
           extractExampleConstants_WithMatrixConstant_ReturnsMatrixValue)
    {
        // Arrange
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();

        auto taggedFunc = model->AddImplicitFunction();
        taggedFunc->SetDisplayName("matFunc");
        taggedFunc->AddInput("transform", "transform", Lib3MF::eImplicitPortType::Matrix);
        taggedFunc->AddOutput("shape", "shape", Lib3MF::eImplicitPortType::Scalar);

        auto const taggedId = taggedFunc->GetModelResourceID();

        auto exampleFunc = model->AddImplicitFunction();
        exampleFunc->SetDisplayName("main");
        exampleFunc->AddOutput("shape", "shape", Lib3MF::eImplicitPortType::Scalar);

        auto constMat = exampleFunc->AddConstMatNode("CM", "CM", "");
        Lib3MF::sMatrix4x4 mat{};
        // Identity matrix with a recognizable off-diagonal value
        mat.m_Field[0][0] = 1.0;
        mat.m_Field[1][1] = 2.0;
        mat.m_Field[2][2] = 3.0;
        mat.m_Field[3][3] = 4.0;
        mat.m_Field[0][1] = 5.0;
        constMat->SetMatrix(mat);

        auto resNode = exampleFunc->AddResourceIdNode("Res", "Res", "");
        resNode->SetResource(std::static_pointer_cast<Lib3MF::CResource>(taggedFunc));

        auto fcNode = exampleFunc->AddFunctionCallNode("FC", "FC", "");
        auto transformPort = fcNode->AddInput("transform", "transform");
        transformPort->SetType(Lib3MF::eImplicitPortType::Matrix);

        exampleFunc->AddLink(resNode->GetOutputValue(), fcNode->GetInputFunctionID());
        exampleFunc->AddLinkByNames("CM.matrix", "FC.transform");

        writeLibraryMetadata(
          model, LibraryMetadata{std::to_string(taggedId), "Matrix test function", ""});

        auto const path = m_tempDir / "matrix_lib.3mf";
        model->QueryWriter("3mf")->WriteToFile(path.string());

        // Act
        auto const constants = extractExampleConstants(path, "matFunc");

        // Assert
        ASSERT_EQ(constants.size(), 1u);
        EXPECT_EQ(constants[0].kind, ExampleConstantValue::Kind::Matrix);
        EXPECT_EQ(constants[0].parameterName, "transform");
        EXPECT_FLOAT_EQ(constants[0].matrixValue[0][0], 1.0f);
        EXPECT_FLOAT_EQ(constants[0].matrixValue[1][1], 2.0f);
        EXPECT_FLOAT_EQ(constants[0].matrixValue[2][2], 3.0f);
        EXPECT_FLOAT_EQ(constants[0].matrixValue[3][3], 4.0f);
        EXPECT_FLOAT_EQ(constants[0].matrixValue[0][1], 5.0f);
    }

} // namespace gladius::io::tests

namespace gladius::io::integration_tests
{
    /// @brief Integration tests that load real 3MF library files shipped with Gladius.
    ///
    /// These tests verify end-to-end extraction from actual library files as
    /// they would be encountered during a library drag-drop in the node editor.
    class LibraryExampleExtractorIntegrationTest : public ::testing::Test
    {
    };

    /// Verifies that @c extractExampleConstants works with the real involute_gear.3mf
    /// library file. The test dynamically discovers the tagged function display name
    /// from metadata so it stays correct even if the file is regenerated.
    TEST_F(LibraryExampleExtractorIntegrationTest,
           extractExampleConstants_WithRealInvoluteGear_ReturnsNonEmptyScalars)
    {
        // Arrange
        std::filesystem::path const filePath{"testdata/involute_gear.3mf"};
        ASSERT_TRUE(std::filesystem::exists(filePath))
          << "involute_gear.3mf not found in testdata/. "
             "Check that the CMakeLists.txt copy step is present.";

        // Discover the tagged function display name from the file's library metadata.
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        model->QueryReader("3mf")->ReadFromFile(filePath.string());

        auto const libMeta = readLibraryMetadata(model);
        ASSERT_TRUE(libMeta.has_value()) << "involute_gear.3mf has no library metadata";

        auto const taggedIds = parseResourceIds(libMeta->libraryFunctions);
        ASSERT_FALSE(taggedIds.empty()) << "No tagged function IDs in metadata";

        // Find the display name of the first tagged function.
        std::string displayName;
        auto resIter = model->GetResources();
        while (resIter->MoveNext())
        {
            auto res = resIter->GetCurrent();
            auto const modelId = res->GetModelResourceID();
            if (std::find(taggedIds.begin(), taggedIds.end(), modelId) == taggedIds.end())
            {
                continue;
            }
            auto implicitFunc = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res);
            if (implicitFunc)
            {
                displayName = implicitFunc->GetDisplayName();
                break;
            }
        }
        ASSERT_FALSE(displayName.empty()) << "Could not find tagged implicit function in file";

        // Act
        auto const constants = extractExampleConstants(filePath, displayName);

        // Assert — the gear example must supply at least one constant to be useful.
        ASSERT_FALSE(constants.empty())
          << "Expected example constants for function '" << displayName << "'";

        // All gear parameters are positive scalars (module, teeth count, height, angles).
        for (auto const & c : constants)
        {
            EXPECT_FALSE(c.parameterName.empty())
              << "Every extracted constant must have a parameter name";
            if (c.kind == ExampleConstantValue::Kind::Scalar)
            {
                EXPECT_GT(c.scalarValue, 0.0f)
                  << "Scalar parameter '" << c.parameterName
                  << "' should be positive for a gear";
            }
        }
    }

} // namespace gladius::io::integration_tests
