/**
 * @file ColorExport_Integration_tests.cpp
 * @brief Integration tests for color-aware 3MF mesh export
 *
 * Tests the full pipeline from implicit model with volumetric color to
 * mesh-based 3MF with per-face colors.
 */

#include "Document.h"
// #include "ManifoldDualContouring.h"  // TODO: File doesn't exist - needs update
#include "EventLogger.h"
#include "ComputeContext.h"
#include "DualContouringSamplingProgram.h"

#include "compute/ComputeCore.h"
#include "compute/ProgramManager.h"

#include "io/3mf/FaceColorSampler.h"
#include "io/3mf/FaceColors.h"
#include "io/3mf/MeshWriter3mf.h"

#include "nodes/Assembly.h"
#include "nodes/Model.h"

#include <gtest/gtest.h>

#include <lib3mf_abi.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <vector>

namespace gladius_tests::color_export
{
    using namespace gladius;
    using namespace gladius::io;

    class ColorExport_Integration_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<events::Logger>();
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }

            // Create output directory for test files
            m_outputDir = std::filesystem::temp_directory_path() / "gladius_color_export_tests";
            std::filesystem::create_directories(m_outputDir);
        }

        void TearDown() override
        {
            // Clean up test files
            std::filesystem::remove_all(m_outputDir);
        }

        struct DocumentBundle
        {
            std::shared_ptr<ComputeCore> core;
            std::shared_ptr<Document> document;
        };

        DocumentBundle loadDocument(std::filesystem::path const& path)
        {
            auto core = std::make_shared<ComputeCore>(
                m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto document = std::make_shared<Document>(core);
            document->load(path);

            return DocumentBundle{std::move(core), std::move(document)};
        }

        /// Extract mesh using ManifoldDualContouring
        /// Returns vertices and face indices (3 per face)
        std::pair<std::vector<Eigen::Vector3f>, std::vector<std::array<std::uint32_t, 3>>>
        extractMesh([[maybe_unused]] ComputeCore& core)
        {
            // TODO: Restore after manifold_dc::ManifoldDualContouring API is available
            // GTEST_SKIP() << "manifold_dc::ManifoldDualContouring API not available";
            return {{}, {}}; // Return empty data - tests will be skipped above this call
            
            /* ORIGINAL CODE - COMMENTED OUT UNTIL API EXISTS
            EXPECT_TRUE(core.updateBBox());

            auto const bbox = core.getBoundingBox();
            EXPECT_TRUE(bbox.has_value());

            manifold_dc::ManifoldDualContouring extractor(core.getContext(), &core);
            auto result = extractor.extractMeshSynchronous(
                bbox.value(),
                5U,  // initialDepth (Draft quality)
                7U,  // maxDepth
                0.0F // isoValue
            );

            std::vector<Eigen::Vector3f> vertices = std::move(result.vertices);
            std::vector<std::uint32_t> indices = std::move(result.indices);

            // Convert flat indices to face triples
            EXPECT_EQ(indices.size() % 3, 0U);
            std::vector<std::array<std::uint32_t, 3>> faces;
            faces.reserve(indices.size() / 3);
            for (std::size_t i = 0; i < indices.size(); i += 3)
            {
                faces.push_back({indices[i], indices[i + 1], indices[i + 2]});
            }

            return {std::move(vertices), std::move(faces)};
            */
            
            return {{}, {}}; // Return empty data for now
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
        std::filesystem::path m_outputDir;
    };

    // =========================================================================
    // Test: Model has volumetric color defined
    // =========================================================================

    TEST_F(ColorExport_Integration_Test, WebcamMountColor_HasVolumetricColor)
    {
        auto bundle = loadDocument("testdata/webcam_mount_color.3mf");
        ASSERT_NE(bundle.document, nullptr);

        auto assembly = bundle.document->getAssembly();
        ASSERT_NE(assembly, nullptr);

        // Get the assembly model (root model)
        auto& model = assembly->assemblyModel();
        ASSERT_NE(model, nullptr);

        // Check that the model has volumetric color defined
        bool const hasColor = FaceColorSampler::hasVolumetricColor(*model);
        EXPECT_TRUE(hasColor) << "webcam_mount_color.3mf should have volumetric color defined";
    }

    // =========================================================================
    // Test: Mesh can be generated from the model
    // =========================================================================

    TEST_F(ColorExport_Integration_Test, WebcamMountColor_GeneratesMesh)
    {
        auto bundle = loadDocument("testdata/webcam_mount_color.3mf");

        auto [vertices, faces] = extractMesh(*bundle.core);

        EXPECT_GT(vertices.size(), 100U) << "Should generate substantial mesh";
        EXPECT_GT(faces.size(), 100U) << "Should generate substantial mesh";
    }

    // =========================================================================
    // Test: Colors can be sampled from faces
    // =========================================================================

    TEST_F(ColorExport_Integration_Test, WebcamMountColor_SamplesFaceColors)
    {
        auto bundle = loadDocument("testdata/webcam_mount_color.3mf");

        auto [vertices, faces] = extractMesh(*bundle.core);
        ASSERT_GT(faces.size(), 0U);

        // Get the sampling program
        auto* samplingProgram =
            bundle.core->getProgramManager().getDualContouringSamplingProgram();
        ASSERT_NE(samplingProgram, nullptr);

        // Get primitives
        auto primitives = bundle.core->getPrimitives();
        ASSERT_NE(primitives, nullptr);

        // Sample colors
        auto colors =
            FaceColorSampler::sampleFaceColors(vertices, faces, *samplingProgram, *primitives);

        EXPECT_EQ(colors.size(), faces.size()) << "Should have one color per face";

        // Verify colors are in valid range [0, 1]
        for (auto const& color : colors)
        {
            EXPECT_GE(color.x(), 0.0f);
            EXPECT_LE(color.x(), 1.0f);
            EXPECT_GE(color.y(), 0.0f);
            EXPECT_LE(color.y(), 1.0f);
            EXPECT_GE(color.z(), 0.0f);
            EXPECT_LE(color.z(), 1.0f);
        }
    }

    // =========================================================================
    // Test: Colors vary based on normals
    // =========================================================================

    TEST_F(ColorExport_Integration_Test, WebcamMountColor_ColorsVaryByNormal)
    {
        auto bundle = loadDocument("testdata/webcam_mount_color.3mf");

        auto [vertices, faces] = extractMesh(*bundle.core);
        ASSERT_GT(faces.size(), 10U);

        auto* samplingProgram =
            bundle.core->getProgramManager().getDualContouringSamplingProgram();
        ASSERT_NE(samplingProgram, nullptr);

        auto primitives = bundle.core->getPrimitives();
        ASSERT_NE(primitives, nullptr);

        auto colors =
            FaceColorSampler::sampleFaceColors(vertices, faces, *samplingProgram, *primitives);

        // Since the model uses normals for coloring, we expect color variation
        // Count unique colors (with some tolerance for floating point)
        std::set<std::tuple<int, int, int>> uniqueColors;
        for (auto const& color : colors)
        {
            // Quantize to avoid floating point comparison issues
            int const r = static_cast<int>(color.x() * 100);
            int const g = static_cast<int>(color.y() * 100);
            int const b = static_cast<int>(color.z() * 100);
            uniqueColors.insert({r, g, b});
        }

        // With normal-based coloring, we expect significant color variation
        EXPECT_GT(uniqueColors.size(), 5U)
            << "Normal-based coloring should produce multiple distinct colors";
    }

    // =========================================================================
    // Test: Full export pipeline produces valid 3MF
    // =========================================================================

    TEST_F(ColorExport_Integration_Test, WebcamMountColor_ExportsColoredMesh)
    {
        auto bundle = loadDocument("testdata/webcam_mount_color.3mf");

        auto [vertices, faces] = extractMesh(*bundle.core);
        ASSERT_GT(faces.size(), 0U);

        auto* samplingProgram =
            bundle.core->getProgramManager().getDualContouringSamplingProgram();
        ASSERT_NE(samplingProgram, nullptr);

        auto primitives = bundle.core->getPrimitives();
        ASSERT_NE(primitives, nullptr);

        // Sample colors and convert to FaceColors
        auto faceColors = FaceColorSampler::sampleFaceColorsAsColor8(
            vertices, faces, *samplingProgram, *primitives);

        ASSERT_EQ(faceColors.size(), faces.size());

        // Create mesh from vertices and faces
        Mesh mesh(*m_context);
        for (auto const& face : faces)
        {
            mesh.addFace(vertices[face[0]], vertices[face[1]], vertices[face[2]]);
        }

        // Export to 3MF with colors
        auto const outputPath = m_outputDir / "webcam_mount_colored.3mf";
        MeshWriter3mf writer(nullptr);
        EXPECT_NO_THROW(
            writer.exportMeshWithColors(outputPath, mesh, "webcam_mount_colored", faceColors));

        // Verify file exists
        EXPECT_TRUE(std::filesystem::exists(outputPath));
        EXPECT_GT(std::filesystem::file_size(outputPath), 0U);
    }

    // =========================================================================
    // Test: Exported 3MF has color group and triangle properties
    // =========================================================================

    TEST_F(ColorExport_Integration_Test, WebcamMountColor_ExportedMeshHasColors)
    {
        auto bundle = loadDocument("testdata/webcam_mount_color.3mf");

        auto [vertices, faces] = extractMesh(*bundle.core);
        ASSERT_GT(faces.size(), 0U);

        auto* samplingProgram =
            bundle.core->getProgramManager().getDualContouringSamplingProgram();
        ASSERT_NE(samplingProgram, nullptr);

        auto primitives = bundle.core->getPrimitives();
        ASSERT_NE(primitives, nullptr);

        auto faceColors = FaceColorSampler::sampleFaceColorsAsColor8(
            vertices, faces, *samplingProgram, *primitives);

        Mesh mesh(*m_context);
        for (auto const& face : faces)
        {
            mesh.addFace(vertices[face[0]], vertices[face[1]], vertices[face[2]]);
        }

        auto const outputPath = m_outputDir / "webcam_mount_verify.3mf";
        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithColors(outputPath, mesh, "verify_mesh", faceColors);

        // Read back the exported file
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        // Verify mesh exists
        auto meshIterator = model->GetMeshObjects();
        ASSERT_TRUE(meshIterator->MoveNext());

        auto meshObject = meshIterator->GetCurrentMeshObject();
        EXPECT_EQ(meshObject->GetTriangleCount(), faces.size());

        // Verify color group exists
        auto colorGroupIterator = model->GetColorGroups();
        ASSERT_TRUE(colorGroupIterator->MoveNext())
            << "Exported 3MF should have a color group";

        auto colorGroup = colorGroupIterator->GetCurrentColorGroup();
        EXPECT_GT(colorGroup->GetCount(), 1U)
            << "Color group should contain multiple colors (normal-based coloring)";

        // Verify triangles have properties set
        std::vector<Lib3MF::sTriangleProperties> properties;
        meshObject->GetAllTriangleProperties(properties);
        EXPECT_EQ(properties.size(), faces.size());

        // Check that at least some triangles have non-zero resource IDs (color assigned)
        std::size_t trianglesWithColor = 0;
        for (auto const& prop : properties)
        {
            if (prop.m_ResourceID != 0)
            {
                ++trianglesWithColor;
            }
        }

        EXPECT_EQ(trianglesWithColor, faces.size())
            << "All triangles should have color properties assigned";
    }

    // =========================================================================
    // Test: Color variation in exported file
    // =========================================================================

    TEST_F(ColorExport_Integration_Test, WebcamMountColor_ExportedColorsVary)
    {
        auto bundle = loadDocument("testdata/webcam_mount_color.3mf");

        auto [vertices, faces] = extractMesh(*bundle.core);
        ASSERT_GT(faces.size(), 0U);

        auto* samplingProgram =
            bundle.core->getProgramManager().getDualContouringSamplingProgram();
        ASSERT_NE(samplingProgram, nullptr);

        auto primitives = bundle.core->getPrimitives();
        ASSERT_NE(primitives, nullptr);

        auto faceColors = FaceColorSampler::sampleFaceColorsAsColor8(
            vertices, faces, *samplingProgram, *primitives);

        Mesh mesh(*m_context);
        for (auto const& face : faces)
        {
            mesh.addFace(vertices[face[0]], vertices[face[1]], vertices[face[2]]);
        }

        auto const outputPath = m_outputDir / "webcam_mount_vary.3mf";
        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithColors(outputPath, mesh, "vary_mesh", faceColors);

        // Read back the exported file
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        auto colorGroupIterator = model->GetColorGroups();
        ASSERT_TRUE(colorGroupIterator->MoveNext());

        auto colorGroup = colorGroupIterator->GetCurrentColorGroup();
        auto const colorCount = colorGroup->GetCount();

        // Collect all colors from the group
        std::vector<Lib3MF::sColor> exportedColors;
        for (std::uint32_t i = 1; i <= colorCount; ++i)
        {
            exportedColors.push_back(colorGroup->GetColor(i));
        }

        // Verify we have multiple distinct colors
        std::set<std::tuple<uint8_t, uint8_t, uint8_t>> uniqueExportedColors;
        for (auto const& c : exportedColors)
        {
            uniqueExportedColors.insert({c.m_Red, c.m_Green, c.m_Blue});
        }

        EXPECT_GT(uniqueExportedColors.size(), 1U)
            << "Exported color palette should contain multiple distinct colors";

        // The colors should not all be white (default)
        bool hasNonWhite = false;
        for (auto const& [r, g, b] : uniqueExportedColors)
        {
            if (r != 255 || g != 255 || b != 255)
            {
                hasNonWhite = true;
                break;
            }
        }

        EXPECT_TRUE(hasNonWhite) << "Should have non-white colors in export";
    }

} // namespace gladius_tests::color_export

