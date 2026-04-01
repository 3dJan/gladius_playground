/**
 * @file MmuSegmentation_tests.cpp
 * @brief Unit tests for MMU segmentation encoding, slicer config, and 3MF post-processing
 */

#include "io/3mf/MmuSegmentationWriter.h"
#include "io/3mf/SlicerConfigWriter.h"
#include "io/3mf/ThreeMfPostProcessor.h"
#include "io/3mf/ColorQuantizer.h"
#include "io/3mf/MeshWriter3mf.h"

#include "ComputeContext.h"
#include "Mesh.h"

#include <gmock/gmock.h>

#include <lib3mf_abi.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#endif

namespace gladius_tests
{
    using namespace gladius;
    using namespace gladius::io;

    // ========================================================================
    // MmuSegmentationWriter encoding tests
    // ========================================================================

    class MmuSegmentationWriterTest : public ::testing::Test
    {
    };

    TEST_F(MmuSegmentationWriterTest, EncodeExtruderState_Zero_ReturnsEmpty)
    {
        EXPECT_EQ(MmuSegmentationWriter::encodeExtruderState(0), "");
    }

    TEST_F(MmuSegmentationWriterTest, EncodeExtruderState_One_ReturnsFour)
    {
        // State 1: bits = [0,0, 1,0] → nibble = 4 → "4"
        EXPECT_EQ(MmuSegmentationWriter::encodeExtruderState(1), "4");
    }

    TEST_F(MmuSegmentationWriterTest, EncodeExtruderState_Two_ReturnsEight)
    {
        // State 2: bits = [0,0, 0,1] → nibble = 8 → "8"
        EXPECT_EQ(MmuSegmentationWriter::encodeExtruderState(2), "8");
    }

    TEST_F(MmuSegmentationWriterTest, EncodeExtruderState_Three_ReturnsZeroC)
    {
        // State 3: bits = [0,0,1,1, 0,0,0,0] → nibbles prepended → "0C"
        EXPECT_EQ(MmuSegmentationWriter::encodeExtruderState(3), "0C");
    }

    TEST_F(MmuSegmentationWriterTest, EncodeExtruderState_Four_ReturnsOneC)
    {
        // State 4: extended, (4-3)=1 → nibbles prepended → "1C"
        EXPECT_EQ(MmuSegmentationWriter::encodeExtruderState(4), "1C");
    }

    TEST_F(MmuSegmentationWriterTest, EncodeExtruderState_Sixteen_IsValid)
    {
        // State 16: extended, (16-3)=13 → nibbles prepended → "DC"
        auto const result = MmuSegmentationWriter::encodeExtruderState(16);
        EXPECT_FALSE(result.empty());
        EXPECT_EQ(result, "DC");
    }

    TEST_F(MmuSegmentationWriterTest, EncodeExtruderState_AboveMax_Throws)
    {
        EXPECT_THROW(MmuSegmentationWriter::encodeExtruderState(17), std::runtime_error);
    }

    TEST_F(MmuSegmentationWriterTest, EncodeFaceExtruders_MapsPaletteToExtruders)
    {
        QuantizedPalette palette;
        palette.colors = {{255, 0, 0, 255}, {0, 255, 0, 255}};
        // Face 0 → palette 0 → extruder 1 → "4"
        // Face 1 → palette 1 → extruder 2 → "8"
        // Face 2 → palette 0 → extruder 1 → "4"
        palette.sourceToPaletteMap = {0, 1, 0};

        auto result = MmuSegmentationWriter::encodeFaceExtruders(palette);
        ASSERT_EQ(result.size(), 3u);
        EXPECT_EQ(result[0], "4");
        EXPECT_EQ(result[1], "8");
        EXPECT_EQ(result[2], "4");
    }

    // ========================================================================
    // SlicerConfigWriter tests
    // ========================================================================

    class SlicerConfigWriterTest : public ::testing::Test
    {
    };

    TEST_F(SlicerConfigWriterTest, Generate_ProducesValidXml)
    {
        SlicerVolumeInfo volume;
        volume.name = "TestMesh";
        volume.firstTriangleId = 0;
        volume.lastTriangleId = 99;
        volume.defaultExtruder = 1;

        auto xml = SlicerConfigWriter::generate(42, volume);

        EXPECT_THAT(xml, ::testing::HasSubstr("<?xml version"));
        EXPECT_THAT(xml, ::testing::HasSubstr("<config>"));
        EXPECT_THAT(xml, ::testing::HasSubstr("id=\"42\""));
        EXPECT_THAT(xml, ::testing::HasSubstr("firstid=\"0\""));
        EXPECT_THAT(xml, ::testing::HasSubstr("lastid=\"99\""));
        EXPECT_THAT(xml, ::testing::HasSubstr("key=\"extruder\" value=\"1\""));
        EXPECT_THAT(xml, ::testing::HasSubstr("key=\"volume_type\" value=\"ModelPart\""));
        EXPECT_THAT(xml, ::testing::HasSubstr("</config>"));
    }

    // ========================================================================
    // ThreeMfPostProcessor tests
    // ========================================================================

    class ThreeMfPostProcessorTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_context = std::make_unique<ComputeContext>();
            m_mesh = std::make_unique<Mesh>(*m_context);

            // Simple tetrahedron (4 faces)
            Vector3 const v0(0.0f, 0.0f, 0.0f);
            Vector3 const v1(10.0f, 0.0f, 0.0f);
            Vector3 const v2(5.0f, 10.0f, 0.0f);
            Vector3 const v3(5.0f, 5.0f, 10.0f);

            m_mesh->addFace(v0, v2, v1);
            m_mesh->addFace(v0, v1, v3);
            m_mesh->addFace(v1, v2, v3);
            m_mesh->addFace(v2, v0, v3);

            auto const * testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
            std::string const suffix =
                testInfo ? (std::string(testInfo->test_suite_name()) + "_" + testInfo->name())
                         : "unknown";
            m_outputDir = std::filesystem::temp_directory_path() /
                          ("gladius_mmu_tests_" + std::to_string(static_cast<long>(
#ifdef _WIN32
                               ::_getpid()
#else
                               ::getpid()
#endif
                           )) +
                           "_" + suffix);
            std::filesystem::create_directories(m_outputDir);
        }

        void TearDown() override
        {
            std::filesystem::remove_all(m_outputDir);
        }

        std::unique_ptr<ComputeContext> m_context;
        std::unique_ptr<Mesh> m_mesh;
        std::filesystem::path m_outputDir;
    };

    TEST_F(ThreeMfPostProcessorTest, InjectMmuSegmentation_ProducesReadable3mf)
    {
        // First write a plain 3MF
        auto const outputPath = m_outputDir / "mmu_test.3mf";
        MeshWriter3mf writer(nullptr);
        writer.exportMesh(outputPath, *m_mesh, "TestMesh");

        // Create test MMU attributes (4 faces)
        std::vector<std::string> mmuAttrs = {"4", "8", "4", "C0"};

        // Generate config
        SlicerVolumeInfo volume;
        volume.name = "TestMesh";
        volume.firstTriangleId = 0;
        volume.lastTriangleId = 3;
        volume.defaultExtruder = 1;
        auto configXml = SlicerConfigWriter::generate(2, volume);

        // Post-process
        ThreeMfPostProcessor::injectMmuSegmentation(outputPath, mmuAttrs, configXml);

        // The post-processed file should still be a valid ZIP
        EXPECT_TRUE(std::filesystem::exists(outputPath));
        EXPECT_GT(std::filesystem::file_size(outputPath), 0u);

        // Verify it's still readable by lib3mf (it ignores unknown attributes)
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        // lib3mf strict mode might reject unknown namespaces, so we allow lax reading
        reader->SetStrictModeActive(false);
        EXPECT_NO_THROW(reader->ReadFromFile(outputPath.string()));

        // Verify mesh is still intact
        auto meshIterator = model->GetMeshObjects();
        ASSERT_TRUE(meshIterator->MoveNext());
        auto meshObj = meshIterator->GetCurrentMeshObject();
        EXPECT_EQ(meshObj->GetTriangleCount(), 4u);
    }

    TEST_F(ThreeMfPostProcessorTest, InjectTriangleAttributes_EmitsBothPaintColorAndSlic3rpe)
    {
        // Minimal model XML with two triangles
        std::string const modelXml =
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<model unit="millimeter">)"
            R"(<resources><object id="2" type="model"><mesh>)"
            R"(<vertices/>)"
            R"(<triangles>)"
            R"(<triangle v1="0" v2="1" v3="2"/>)"
            R"(<triangle v1="1" v2="2" v3="3"/>)"
            R"(</triangles>)"
            R"(</mesh></object></resources></model>)";

        std::vector<std::string> mmuAttrs = {"4", "8"};

        auto result = ThreeMfPostProcessor::injectTriangleAttributes(modelXml, mmuAttrs);

        // OrcaSlicer reads paint_color
        EXPECT_THAT(result, ::testing::HasSubstr(R"(paint_color="4")"));
        EXPECT_THAT(result, ::testing::HasSubstr(R"(paint_color="8")"));

        // PrusaSlicer reads slic3rpe:mmu_segmentation
        EXPECT_THAT(result, ::testing::HasSubstr(R"(slic3rpe:mmu_segmentation="4")"));
        EXPECT_THAT(result, ::testing::HasSubstr(R"(slic3rpe:mmu_segmentation="8")"));

        // Namespace must be present for PrusaSlicer
        EXPECT_THAT(result, ::testing::HasSubstr("xmlns:slic3rpe"));
    }

    // ========================================================================
    // Full pipeline integration test
    // ========================================================================

    TEST_F(ThreeMfPostProcessorTest, ExportMeshWithMmuSegmentation_EndToEnd)
    {
        auto const outputPath = m_outputDir / "mmu_full.3mf";

        // Create a 2-color palette mapped to 4 faces
        QuantizedPalette palette;
        palette.colors = {{255, 0, 0, 255}, {0, 0, 255, 255}};
        palette.sourceToPaletteMap = {0, 1, 0, 1}; // alternating colors

        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithMmuSegmentation(outputPath, *m_mesh, "MmuMesh", palette);

        // File should exist and be non-empty
        EXPECT_TRUE(std::filesystem::exists(outputPath));
        EXPECT_GT(std::filesystem::file_size(outputPath), 0u);

        // Verify the file is still valid 3MF
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->SetStrictModeActive(false);
        EXPECT_NO_THROW(reader->ReadFromFile(outputPath.string()));

        // Mesh should be intact with 4 triangles
        auto meshIterator = model->GetMeshObjects();
        ASSERT_TRUE(meshIterator->MoveNext());
        auto meshObj = meshIterator->GetCurrentMeshObject();
        EXPECT_EQ(meshObj->GetTriangleCount(), 4u);
    }

} // namespace gladius_tests
