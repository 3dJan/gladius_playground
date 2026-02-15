#include <gtest/gtest.h>

#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/LibraryMetadata.h"

#include <lib3mf_implicit.hpp>

namespace gladius_tests
{
    using namespace gladius::io;

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

} // namespace gladius_tests
