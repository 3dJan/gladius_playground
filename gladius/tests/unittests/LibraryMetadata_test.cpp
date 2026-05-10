#include <gtest/gtest.h>

#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/LibraryMetadata.h"

#include <filesystem>
#include <lib3mf_implicit.hpp>

namespace gladius_tests
{
    using namespace gladius::io;

    /// RAII guard that removes a temp file on destruction (even if a test assertion fails).
    struct TempFileGuard
    {
        std::filesystem::path path;
        ~TempFileGuard()
        {
            std::filesystem::remove(path);
        }
    };

    // =========================================================================
    // parseResourceIds tests
    // =========================================================================

    TEST(LibraryMetadata, ParseResourceIds_WithSingleId_ReturnsOneElement)
    {
        auto const ids = parseResourceIds("5");
        ASSERT_EQ(ids.size(), 1u);
        EXPECT_EQ(ids[0], 5u);
    }

    TEST(LibraryMetadata, ParseResourceIds_WithMultipleIds_ReturnsAll)
    {
        auto const ids = parseResourceIds("5;12;3");
        ASSERT_EQ(ids.size(), 3u);
        EXPECT_EQ(ids[0], 5u);
        EXPECT_EQ(ids[1], 12u);
        EXPECT_EQ(ids[2], 3u);
    }

    TEST(LibraryMetadata, ParseResourceIds_WithWhitespace_TrimsCorrectly)
    {
        auto const ids = parseResourceIds(" 5 ; 12 ; 3 ");
        ASSERT_EQ(ids.size(), 3u);
        EXPECT_EQ(ids[0], 5u);
        EXPECT_EQ(ids[1], 12u);
        EXPECT_EQ(ids[2], 3u);
    }

    TEST(LibraryMetadata, ParseResourceIds_WithEmptyString_ReturnsEmpty)
    {
        auto const ids = parseResourceIds("");
        EXPECT_TRUE(ids.empty());
    }

    TEST(LibraryMetadata, ParseResourceIds_WithTrailingSemicolon_IgnoresEmpty)
    {
        auto const ids = parseResourceIds("5;12;");
        ASSERT_EQ(ids.size(), 2u);
        EXPECT_EQ(ids[0], 5u);
        EXPECT_EQ(ids[1], 12u);
    }

    TEST(LibraryMetadata, ParseResourceIds_WithNonNumeric_SkipsInvalid)
    {
        auto const ids = parseResourceIds("5;abc;12");
        ASSERT_EQ(ids.size(), 2u);
        EXPECT_EQ(ids[0], 5u);
        EXPECT_EQ(ids[1], 12u);
    }

    // =========================================================================
    // serializeResourceIds tests
    // =========================================================================

    TEST(LibraryMetadata, SerializeResourceIds_WithMultipleIds_RoundTrips)
    {
        std::vector<Lib3MF_uint32> const original = {5, 12, 3};
        auto const serialized = serializeResourceIds(original);
        EXPECT_EQ(serialized, "5;12;3");

        auto const parsed = parseResourceIds(serialized);
        ASSERT_EQ(parsed.size(), original.size());
        for (size_t i = 0; i < original.size(); ++i)
        {
            EXPECT_EQ(parsed[i], original[i]);
        }
    }

    TEST(LibraryMetadata, SerializeResourceIds_WithEmpty_ReturnsEmptyString)
    {
        auto const serialized = serializeResourceIds({});
        EXPECT_TRUE(serialized.empty());
    }

    TEST(LibraryMetadata, SerializeResourceIds_WithSingleId_NoSemicolon)
    {
        auto const serialized = serializeResourceIds({42});
        EXPECT_EQ(serialized, "42");
    }

    // =========================================================================
    // readLibraryMetadata / writeLibraryMetadata tests
    // =========================================================================

    class LibraryMetadataModel_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_wrapper = gladius::io::loadLib3mfScoped();
            ASSERT_TRUE(m_wrapper) << "Failed to load Lib3MF library";
            m_model = m_wrapper->CreateModel();
            ASSERT_TRUE(m_model) << "Failed to create 3MF model";
        }

        void TearDown() override
        {
            m_model = nullptr;
            m_wrapper = nullptr;
        }

        Lib3MF::PWrapper m_wrapper;
        Lib3MF::PModel m_model;
    };

    TEST_F(LibraryMetadataModel_Test, ReadMetadata_WithMissingKeys_ReturnsNullopt)
    {
        // Fresh model with no metadata
        auto const result = readLibraryMetadata(m_model);
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(LibraryMetadataModel_Test, ReadMetadata_WithOnlyDescription_ReturnsNullopt)
    {
        // Only description, no library-functions — not a library file
        auto metaGroup = m_model->GetMetaDataGroup();
        metaGroup->AddMetaData(
          LIBRARY_METADATA_NAMESPACE, LIBRARY_DESCRIPTION_KEY, "Some description", "xs:string", true);

        auto const result = readLibraryMetadata(m_model);
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(LibraryMetadataModel_Test, ReadMetadata_WithBothKeys_ReturnsPopulated)
    {
        auto metaGroup = m_model->GetMetaDataGroup();
        metaGroup->AddMetaData(
          LIBRARY_METADATA_NAMESPACE, LIBRARY_FUNCTIONS_KEY, "5;12", "xs:string", true);
        metaGroup->AddMetaData(LIBRARY_METADATA_NAMESPACE,
                               LIBRARY_DESCRIPTION_KEY,
                               "A nice pattern",
                               "xs:string",
                               true);

        auto const result = readLibraryMetadata(m_model);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->libraryFunctions, "5;12");
        EXPECT_EQ(result->libraryDescription, "A nice pattern");
    }

    TEST_F(LibraryMetadataModel_Test, ReadMetadata_WithFunctionsOnly_DescriptionIsEmpty)
    {
        auto metaGroup = m_model->GetMetaDataGroup();
        metaGroup->AddMetaData(
          LIBRARY_METADATA_NAMESPACE, LIBRARY_FUNCTIONS_KEY, "7", "xs:string", true);

        auto const result = readLibraryMetadata(m_model);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->libraryFunctions, "7");
        EXPECT_TRUE(result->libraryDescription.empty());
    }

    TEST_F(LibraryMetadataModel_Test, WriteMetadata_RoundTrip_PreservesValues)
    {
        LibraryMetadata const original{"5;12;3", "Triply periodic minimal surface"};

        writeLibraryMetadata(m_model, original);
        auto const result = readLibraryMetadata(m_model);

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->libraryFunctions, original.libraryFunctions);
        EXPECT_EQ(result->libraryDescription, original.libraryDescription);
    }

    TEST_F(LibraryMetadataModel_Test, WriteMetadata_CalledTwice_OverwritesPreviousValues)
    {
        LibraryMetadata const first{"1;2", "First description"};
        LibraryMetadata const second{"3;4;5", "Updated description"};

        writeLibraryMetadata(m_model, first);
        writeLibraryMetadata(m_model, second);

        auto const result = readLibraryMetadata(m_model);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->libraryFunctions, second.libraryFunctions);
        EXPECT_EQ(result->libraryDescription, second.libraryDescription);
    }

    TEST_F(LibraryMetadataModel_Test, RemoveMetadata_AfterWrite_ReturnsNullopt)
    {
        LibraryMetadata const original{"5;12", "Remove me"};
        writeLibraryMetadata(m_model, original);

        removeLibraryMetadata(m_model);

        auto const result = readLibraryMetadata(m_model);
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(LibraryMetadataModel_Test, RemoveMetadata_OnEmptyModel_DoesNotThrow)
    {
        // No metadata written — removeLibraryMetadata should be a no-op.
        EXPECT_NO_THROW(removeLibraryMetadata(m_model));
    }

    TEST_F(LibraryMetadataModel_Test, RemoveMetadata_WithNullModel_DoesNotThrow)
    {
        Lib3MF::PModel nullModel;
        EXPECT_NO_THROW(removeLibraryMetadata(nullModel));
    }

    TEST_F(LibraryMetadataModel_Test, WriteMetadata_ThenSerializeAndDeserialize_Survives)
    {
        // Write metadata, serialize to buffer, deserialize, then read back
        LibraryMetadata const original{"10;20", "Round-trip through buffer"};
        writeLibraryMetadata(m_model, original);

        // Serialize to buffer
        auto writer = m_model->QueryWriter("3mf");
        std::vector<Lib3MF_uint8> buffer;
        writer->WriteToBuffer(buffer);

        // Deserialize into a fresh model
        auto model2 = m_wrapper->CreateModel();
        auto reader = model2->QueryReader("3mf");
        reader->ReadFromBuffer(buffer);

        auto const result = readLibraryMetadata(model2);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->libraryFunctions, original.libraryFunctions);
        EXPECT_EQ(result->libraryDescription, original.libraryDescription);
    }

    TEST_F(LibraryMetadataModel_Test, ReadMetadata_WithNullModel_ReturnsNullopt)
    {
        Lib3MF::PModel nullModel;
        auto const result = readLibraryMetadata(nullModel);
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(LibraryMetadataModel_Test,
           WriteMetadata_AfterBufferRoundTripWithImplicitFunctions_Succeeds)
    {
        // Reproduce the export dialog flow: create a model with implicit
        // functions, serialize to buffer, deserialize into a working copy,
        // then write library metadata on the working copy.

        // Add implicit functions
        auto funcB = m_model->AddImplicitFunction();
        funcB->SetDisplayName("FuncB");
        funcB->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
        funcB->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);

        auto funcA = m_model->AddImplicitFunction();
        funcA->SetDisplayName("FuncA");
        funcA->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
        funcA->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);
        auto resIdNode = funcA->AddResourceIdNode("ref_b", "Reference to B", "");
        resIdNode->SetResource(funcB.get());

        // Serialize to buffer
        std::vector<Lib3MF_uint8> buffer;
        {
            auto writer = m_model->QueryWriter("3mf");
            writer->WriteToBuffer(buffer);
        }

        // Deserialize into fresh working copy
        auto workingCopy = m_wrapper->CreateModel();
        {
            auto reader = workingCopy->QueryReader("3mf");
            reader->ReadFromBuffer(buffer);
        }

        // Write metadata on the working copy — this is where the crash happens
        LibraryMetadata const metadata{
          std::to_string(funcA->GetModelResourceID()), "Test description"};
        EXPECT_NO_THROW(writeLibraryMetadata(workingCopy, metadata));

        // Verify it can be read back
        auto const result = readLibraryMetadata(workingCopy);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->libraryFunctions, metadata.libraryFunctions);
        EXPECT_EQ(result->libraryDescription, metadata.libraryDescription);
    }

    /// @brief Reproduces the exact export-to-library flow with the RadialRadiator
    /// example. The "heatexchanger" function has many ResourceIdNode references
    /// to other implicit functions. We verify that the full model (no pruning)
    /// can be exported with library metadata intact.
    /// Pruning is intentionally skipped because lib3mf's RemoveResource breaks
    /// internal state on models with cross-function ResourceIdNode references.
    TEST_F(LibraryMetadataModel_Test,
           WriteMetadata_AfterBufferRoundTripWithRadialRadiator_Succeeds)
    {
        // Locate the RadialRadiator.3mf example — skip if not found.
        auto const testFile =
          std::filesystem::path("testdata") / "RadialRadiator.3mf";
        if (!std::filesystem::exists(testFile))
        {
            GTEST_SKIP() << "RadialRadiator.3mf not found in testdata/";
        }

        // Load the original file.
        auto sourceModel = m_wrapper->CreateModel();
        {
            auto reader = sourceModel->QueryReader("3mf");
            reader->ReadFromFile(testFile.string());
        }

        // Find the "heatexchanger" function's model resource ID.
        Lib3MF_uint32 heatExchangerId = 0;
        {
            auto resources = sourceModel->GetResources();
            while (resources->MoveNext())
            {
                auto res = resources->GetCurrent();
                auto implicitFunc =
                  std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res);
                if (implicitFunc &&
                    implicitFunc->GetDisplayName() == "heatexchanger")
                {
                    heatExchangerId = implicitFunc->GetModelResourceID();
                    break;
                }
            }
        }
        ASSERT_NE(heatExchangerId, 0u)
          << "heatexchanger function not found in RadialRadiator.3mf";

        // Reproduce the new export flow: stamp metadata on source, write to
        // file (full model, no pruning), then clean up source metadata.
        LibraryMetadata const metadata{
          std::to_string(heatExchangerId), "Radial heat exchanger"};
        writeLibraryMetadata(sourceModel, metadata);

        // Write the full model with metadata to a temp file.
        auto const tempDir = std::filesystem::temp_directory_path();
        auto const outputPath = tempDir / "RadialRadiator_export_test.3mf";
        {
            auto writer = sourceModel->QueryWriter("3mf");
            EXPECT_NO_THROW(writer->WriteToFile(outputPath.string()));
        }

        // Clean up metadata on the source model (as performExport does).
        removeLibraryMetadata(sourceModel);
        auto const afterCleanup = readLibraryMetadata(sourceModel);
        EXPECT_FALSE(afterCleanup.has_value())
          << "Metadata should be removed from source after export";

        // Read back the exported file and verify metadata survived.
        auto exportedModel = m_wrapper->CreateModel();
        {
            auto reader = exportedModel->QueryReader("3mf");
            reader->ReadFromFile(outputPath.string());
        }

        auto const result = readLibraryMetadata(exportedModel);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->libraryFunctions, metadata.libraryFunctions);
        EXPECT_EQ(result->libraryDescription, metadata.libraryDescription);

        // The exported model should be writeable without errors.
        std::vector<Lib3MF_uint8> verifyBuffer;
        EXPECT_NO_THROW({
            auto writer = exportedModel->QueryWriter("3mf");
            writer->WriteToBuffer(verifyBuffer);
        });
        EXPECT_FALSE(verifyBuffer.empty());

        // Clean up temp file.
        std::filesystem::remove(outputPath);
    }

    // =========================================================================
    // pruneSourceForImport tests
    // =========================================================================

    class PruneSourceForImport_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_wrapper = Lib3MF::CWrapper::loadLibrary();
            m_model = m_wrapper->CreateModel();

            // Create two independent functions: FuncA (tagged) and FuncUnrelated.
            m_funcUnrelated = m_model->AddImplicitFunction();
            m_funcUnrelated->SetDisplayName("Unrelated");
            m_funcUnrelated->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            m_funcUnrelated->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);

            m_funcHelper = m_model->AddImplicitFunction();
            m_funcHelper->SetDisplayName("Helper");
            m_funcHelper->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            m_funcHelper->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);

            m_funcA = m_model->AddImplicitFunction();
            m_funcA->SetDisplayName("FuncA");
            m_funcA->AddInput("pos", "position", Lib3MF::eImplicitPortType::Vector);
            m_funcA->AddOutput("shape", "output", Lib3MF::eImplicitPortType::Scalar);
            // FuncA depends on Helper via a ResourceIdNode.
            auto resIdNode = m_funcA->AddResourceIdNode("ref_helper", "Reference to Helper", "");
            resIdNode->SetResource(m_funcHelper.get());

            m_tempDir = std::filesystem::temp_directory_path();
        }

        /// Write the model with metadata to a temp file and return the path.
        std::filesystem::path writeLibraryFile(std::string const & name,
                                               Lib3MF_uint32 taggedId)
        {
            auto const path = m_tempDir / name;

            LibraryMetadata metadata;
            metadata.libraryFunctions = serializeResourceIds({taggedId});
            metadata.libraryDescription = "test";
            writeLibraryMetadata(m_model, metadata);

            auto writer = m_model->QueryWriter("3mf");
            writer->WriteToFile(path.string());

            removeLibraryMetadata(m_model);
            return path;
        }

        Lib3MF::PWrapper m_wrapper;
        Lib3MF::PModel m_model;
        Lib3MF::PImplicitFunction m_funcUnrelated;
        Lib3MF::PImplicitFunction m_funcHelper;
        Lib3MF::PImplicitFunction m_funcA;
        std::filesystem::path m_tempDir;
    };

    TEST_F(PruneSourceForImport_Test,
           PruneSourceForImport_WithTaggedFunction_RemovesUnrelated)
    {
        auto const path = writeLibraryFile(
          "prune_test_removes_unrelated.3mf", m_funcA->GetModelResourceID());
        TempFileGuard guard{path};

        auto result = pruneSourceForImport(path, nullptr);
        ASSERT_TRUE(result.has_value());

        // The pruned model should contain FuncA and Helper but NOT Unrelated.
        auto prunedModel = *result;
        auto funcIter = prunedModel->GetFunctions();

        std::set<std::string> functionNames;
        while (funcIter->MoveNext())
        {
            functionNames.insert(funcIter->GetCurrentFunction()->GetDisplayName());
        }

        EXPECT_TRUE(functionNames.count("FuncA") > 0) << "Tagged function should be kept";
        EXPECT_TRUE(functionNames.count("Helper") > 0) << "Dependency should be kept";
        EXPECT_TRUE(functionNames.count("Unrelated") == 0)
          << "Unrelated function should be pruned";
    }

    TEST_F(PruneSourceForImport_Test,
           PruneSourceForImport_WithNoMetadata_ReturnsNullopt)
    {
        // Write a file without library metadata.
        auto const path = m_tempDir / "prune_test_no_metadata.3mf";
        TempFileGuard guard{path};
        {
            auto writer = m_model->QueryWriter("3mf");
            writer->WriteToFile(path.string());
        }

        auto result = pruneSourceForImport(path, nullptr);
        EXPECT_FALSE(result.has_value()) << "Should fall back when no metadata";
    }

    TEST_F(PruneSourceForImport_Test,
           PruneSourceForImport_WithNonexistentFile_ReturnsNullopt)
    {
        auto result = pruneSourceForImport("/tmp/nonexistent_library_file.3mf", nullptr);
        EXPECT_FALSE(result.has_value()) << "Should fall back on read error";
    }

    TEST_F(PruneSourceForImport_Test,
           PruneSourceForImport_PrunedModelCanBeMergedIntoFreshModel)
    {
        auto const path = writeLibraryFile(
          "prune_test_mergeable.3mf", m_funcA->GetModelResourceID());
        TempFileGuard guard{path};

        auto result = pruneSourceForImport(path, nullptr);
        ASSERT_TRUE(result.has_value());

        // Create a fresh target model and merge the pruned model into it.
        auto targetModel = m_wrapper->CreateModel();
        EXPECT_NO_THROW(targetModel->MergeFromModel(result->get()));

        // Verify the merged model has the expected functions.
        auto funcIter = targetModel->GetFunctions();
        std::set<std::string> functionNames;
        while (funcIter->MoveNext())
        {
            functionNames.insert(funcIter->GetCurrentFunction()->GetDisplayName());
        }

        EXPECT_TRUE(functionNames.count("FuncA") > 0);
        EXPECT_TRUE(functionNames.count("Helper") > 0);
        EXPECT_TRUE(functionNames.count("Unrelated") == 0);
    }

} // namespace gladius_tests
