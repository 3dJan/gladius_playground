/**
 * @file MeshWriter3mfColor_tests.cpp
 * @brief Unit tests for per-face color export in 3MF mesh writer
 */

#include "io/3mf/ColorQuantizer.h"
#include "io/3mf/ColorRegionizer.h"
#include "io/3mf/FaceColors.h"
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

    class MeshWriter3mfColorTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Create compute context for mesh creation
            m_context = std::make_unique<ComputeContext>();

            // Create a simple tetrahedron mesh (4 faces)
            m_mesh = std::make_unique<Mesh>(*m_context);

            // Tetrahedron vertices
            Vector3 const v0(0.0f, 0.0f, 0.0f);
            Vector3 const v1(10.0f, 0.0f, 0.0f);
            Vector3 const v2(5.0f, 10.0f, 0.0f);
            Vector3 const v3(5.0f, 5.0f, 10.0f);

            // Add 4 faces (CCW winding for outward normals)
            m_mesh->addFace(v0, v2, v1); // Bottom face
            m_mesh->addFace(v0, v1, v3); // Front face
            m_mesh->addFace(v1, v2, v3); // Right face
            m_mesh->addFace(v2, v0, v3); // Left face

                        // Create a unique output directory for test files.
                        // These gtests are also registered as individual CTest tests and may run in parallel.
                        auto const * testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
                        std::string const suffix =
                            testInfo ? (std::string(testInfo->test_suite_name()) + "_" + testInfo->name()) : "unknown";
                        m_outputDir = std::filesystem::temp_directory_path() /
                                                 ("gladius_color_tests_" + std::to_string(static_cast<long>(
#ifdef _WIN32
                                                     ::_getpid()
#else
                                                     ::getpid()
#endif
                                                 )) + "_" + suffix);
            std::filesystem::create_directories(m_outputDir);
        }

        void TearDown() override
        {
            // Clean up test files
            std::filesystem::remove_all(m_outputDir);
        }

        std::unique_ptr<ComputeContext> m_context;
        std::unique_ptr<Mesh> m_mesh;
        std::filesystem::path m_outputDir;
    };

    // ========================================================================
    // Color8 struct tests
    // ========================================================================

    TEST(Color8Test, DefaultConstruction_CreatesWhite)
    {
        Color8 const color;
        EXPECT_EQ(color.r, 255);
        EXPECT_EQ(color.g, 255);
        EXPECT_EQ(color.b, 255);
        EXPECT_EQ(color.a, 255);
    }

    TEST(Color8Test, ComponentConstruction_StoresValues)
    {
        Color8 const color(128, 64, 32, 200);
        EXPECT_EQ(color.r, 128);
        EXPECT_EQ(color.g, 64);
        EXPECT_EQ(color.b, 32);
        EXPECT_EQ(color.a, 200);
    }

    TEST(Color8Test, FromFloat_ClampsAndConverts)
    {
        // Normal values
        auto const c1 = Color8::fromFloat(0.5f, 0.25f, 0.75f, 1.0f);
        EXPECT_EQ(c1.r, 128);
        EXPECT_EQ(c1.g, 64);
        EXPECT_EQ(c1.b, 191);
        EXPECT_EQ(c1.a, 255);

        // Clamping at 0
        auto const c2 = Color8::fromFloat(-0.5f, 0.0f, 0.0f);
        EXPECT_EQ(c2.r, 0);
        EXPECT_EQ(c2.g, 0);
        EXPECT_EQ(c2.b, 0);

        // Clamping at 1
        auto const c3 = Color8::fromFloat(1.5f, 1.0f, 1.0f);
        EXPECT_EQ(c3.r, 255);
        EXPECT_EQ(c3.g, 255);
        EXPECT_EQ(c3.b, 255);
    }

    TEST(Color8Test, FromVector3f_ConvertsCorrectly)
    {
        Eigen::Vector3f const rgb(1.0f, 0.0f, 0.5f);
        auto const color = Color8::fromVector3f(rgb);
        EXPECT_EQ(color.r, 255);
        EXPECT_EQ(color.g, 0);
        EXPECT_EQ(color.b, 128);
        EXPECT_EQ(color.a, 255); // Default alpha
    }

    TEST(Color8Test, FromVector4f_ConvertsCorrectly)
    {
        Eigen::Vector4f const rgba(1.0f, 0.0f, 0.5f, 0.5f);
        auto const color = Color8::fromVector4f(rgba);
        EXPECT_EQ(color.r, 255);
        EXPECT_EQ(color.g, 0);
        EXPECT_EQ(color.b, 128);
        EXPECT_EQ(color.a, 128);
    }

    TEST(Color8Test, ToVector3f_ConvertsCorrectly)
    {
        Color8 const color(255, 0, 128, 255);
        Eigen::Vector3f const rgb = color.toVector3f();
        EXPECT_FLOAT_EQ(rgb.x(), 1.0f);
        EXPECT_FLOAT_EQ(rgb.y(), 0.0f);
        EXPECT_NEAR(rgb.z(), 0.5f, 0.01f);
    }

    TEST(Color8Test, Equality_ComparesAllComponents)
    {
        Color8 const c1(100, 100, 100, 100);
        Color8 const c2(100, 100, 100, 100);
        Color8 const c3(100, 100, 100, 101);

        EXPECT_TRUE(c1 == c2);
        EXPECT_FALSE(c1 == c3);
        EXPECT_TRUE(c1 != c3);
    }

    // ========================================================================
    // FaceColors struct tests
    // ========================================================================

    TEST(FaceColorsTest, DefaultConstruction_IsEmpty)
    {
        FaceColors const colors;
        EXPECT_TRUE(colors.empty());
        EXPECT_EQ(colors.size(), 0u);
    }

    TEST(FaceColorsTest, SizedConstruction_CreatesCorrectSize)
    {
        FaceColors const colors(10);
        EXPECT_EQ(colors.size(), 10u);
        EXPECT_FALSE(colors.empty());
    }

    TEST(FaceColorsTest, FromVector3f_ConvertsAllColors)
    {
        std::vector<Eigen::Vector3f> const rgbColors = {{1.0f, 0.0f, 0.0f},
                                                        {0.0f, 1.0f, 0.0f},
                                                        {0.0f, 0.0f, 1.0f}};

        auto const faceColors = FaceColors::fromVector3f(rgbColors);
        EXPECT_EQ(faceColors.size(), 3u);
        EXPECT_EQ(faceColors[0].r, 255);
        EXPECT_EQ(faceColors[0].g, 0);
        EXPECT_EQ(faceColors[1].g, 255);
        EXPECT_EQ(faceColors[2].b, 255);
    }

    // ========================================================================
    // MeshWriter3mf color export tests
    // ========================================================================

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithColors_CreatesValidFile)
    {
        // Create face colors - one color per face
        FaceColors faceColors(4);
        faceColors[0] = Color8(255, 0, 0, 255);   // Red
        faceColors[1] = Color8(0, 255, 0, 255);   // Green
        faceColors[2] = Color8(0, 0, 255, 255);   // Blue
        faceColors[3] = Color8(255, 255, 0, 255); // Yellow

        auto const outputPath = m_outputDir / "colored_tetrahedron.3mf";

        MeshWriter3mf writer(nullptr);
        EXPECT_NO_THROW(writer.exportMeshWithColors(outputPath, *m_mesh, "ColoredTetra", faceColors));

        // Verify file exists and has content
        EXPECT_TRUE(std::filesystem::exists(outputPath));
        EXPECT_GT(std::filesystem::file_size(outputPath), 0u);
    }

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithColors_CanBeReadBack)
    {
        // Create face colors
        FaceColors faceColors(4);
        faceColors[0] = Color8(255, 0, 0, 255);
        faceColors[1] = Color8(0, 255, 0, 255);
        faceColors[2] = Color8(0, 0, 255, 255);
        faceColors[3] = Color8(255, 255, 0, 255);

        auto const outputPath = m_outputDir / "colored_mesh_readback.3mf";

        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithColors(outputPath, *m_mesh, "ColoredMesh", faceColors);

        // Read back using lib3mf
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        // Get mesh objects
        auto meshIterator = model->GetMeshObjects();
        ASSERT_TRUE(meshIterator->MoveNext());

        auto meshObject = meshIterator->GetCurrentMeshObject();
        EXPECT_EQ(meshObject->GetTriangleCount(), 4u);

        // Check that color group exists
        auto colorGroupIterator = model->GetColorGroups();
        ASSERT_TRUE(colorGroupIterator->MoveNext());

        auto colorGroup = colorGroupIterator->GetCurrentColorGroup();
        // MeshWriter3mf reserves index 0 as a transparent placeholder color.
        EXPECT_EQ(colorGroup->GetCount(), 5u); // 4 unique colors + placeholder
    }

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithColors_DuplicateColorsAreDeduped)
    {
        // Create face colors with duplicates
        FaceColors faceColors(4);
        faceColors[0] = Color8(255, 0, 0, 255); // Red
        faceColors[1] = Color8(255, 0, 0, 255); // Red (duplicate)
        faceColors[2] = Color8(0, 0, 255, 255); // Blue
        faceColors[3] = Color8(255, 0, 0, 255); // Red (duplicate)

        auto const outputPath = m_outputDir / "deduped_colors.3mf";

        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithColors(outputPath, *m_mesh, "DedupedMesh", faceColors);

        // Read back and verify only 2 unique colors
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        auto colorGroupIterator = model->GetColorGroups();
        ASSERT_TRUE(colorGroupIterator->MoveNext());

        auto colorGroup = colorGroupIterator->GetCurrentColorGroup();
        // MeshWriter3mf reserves index 0 as a transparent placeholder color.
        EXPECT_EQ(colorGroup->GetCount(), 3u); // 2 unique colors + placeholder
    }

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithColors_MismatchedCountThrows)
    {
        // Create face colors with wrong count
        FaceColors faceColors(2); // Only 2 colors for 4-face mesh

        auto const outputPath = m_outputDir / "should_fail.3mf";

        MeshWriter3mf writer(nullptr);
        EXPECT_THROW(writer.exportMeshWithColors(outputPath, *m_mesh, "FailMesh", faceColors),
                     std::runtime_error);
    }

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithColors_TrianglePropertiesAreCorrect)
    {
        // Create distinct colors for each face
        FaceColors faceColors(4);
        faceColors[0] = Color8(255, 0, 0, 255);   // Red
        faceColors[1] = Color8(0, 255, 0, 255);   // Green
        faceColors[2] = Color8(0, 0, 255, 255);   // Blue
        faceColors[3] = Color8(255, 255, 0, 255); // Yellow

        auto const outputPath = m_outputDir / "triangle_props.3mf";

        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithColors(outputPath, *m_mesh, "PropsMesh", faceColors);

        // Read back and verify triangle properties
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        auto meshIterator = model->GetMeshObjects();
        ASSERT_TRUE(meshIterator->MoveNext());
        auto meshObject = meshIterator->GetCurrentMeshObject();

        // Verify each triangle has properties set
        std::vector<Lib3MF::sTriangleProperties> properties;
        meshObject->GetAllTriangleProperties(properties);
        EXPECT_EQ(properties.size(), 4u);

        // Each triangle should have the same color for all 3 vertices (flat shading)
        for (auto const& prop : properties)
        {
            EXPECT_EQ(prop.m_PropertyIDs[0], prop.m_PropertyIDs[1]);
            EXPECT_EQ(prop.m_PropertyIDs[1], prop.m_PropertyIDs[2]);
        }
    }

    // ========================================================================
    // ExportMeshWithRegions tests (discrete printable regions)
    // ========================================================================

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithRegions_CreatesMultipleBuildItems)
    {
        // 4-face tetrahedron with 3 palette entries → 3 regions
        FaceColors faceColors(4);
        faceColors[0] = Color8(255, 0, 0, 255);   // Red  → palette 0
        faceColors[1] = Color8(0, 255, 0, 255);   // Green → palette 1
        faceColors[2] = Color8(0, 0, 255, 255);   // Blue  → palette 2
        faceColors[3] = Color8(255, 0, 0, 255);   // Red  → palette 0 (duplicate)

        auto palette = ColorQuantizer::quantize(faceColors, 3);
        auto regions = ColorRegionizer::regionize(palette, PrintableRegionKind::BuildItem);

        ASSERT_EQ(regions.size(), 3u);

        auto const outputPath = m_outputDir / "regions_build_items.3mf";
        MeshWriter3mf writer(nullptr);
        EXPECT_NO_THROW(writer.exportMeshWithRegions(
            outputPath, *m_mesh, "RegionMesh", palette, regions));

        // Read back and verify multiple build items
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        auto buildItems = model->GetBuildItems();
        std::uint64_t buildItemCount = 0;
        while (buildItems->MoveNext())
        {
            ++buildItemCount;
        }
        EXPECT_EQ(buildItemCount, 3u) << "One build item per palette color";
    }

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithRegions_AllTrianglesAccountedFor)
    {
        // 4-face mesh with 2 colors → 2 regions, total faces must be 4
        FaceColors faceColors(4);
        faceColors[0] = Color8(255, 0, 0, 255);
        faceColors[1] = Color8(0, 255, 0, 255);
        faceColors[2] = Color8(255, 0, 0, 255);
        faceColors[3] = Color8(0, 255, 0, 255);

        auto palette = ColorQuantizer::quantize(faceColors, 2);
        auto regions = ColorRegionizer::regionize(palette, PrintableRegionKind::BuildItem);

        auto const outputPath = m_outputDir / "regions_all_tris.3mf";
        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithRegions(outputPath, *m_mesh, "FullMesh", palette, regions);

        // Read back and count total triangles across all mesh objects
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        auto meshes = model->GetMeshObjects();
        std::uint64_t totalTriangles = 0;
        while (meshes->MoveNext())
        {
            totalTriangles += meshes->GetCurrentMeshObject()->GetTriangleCount();
        }
        EXPECT_EQ(totalTriangles, 4u);
    }

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithRegions_EachMeshHasMaterialColor)
    {
        // 4-face mesh with 2 colors; each region mesh should have color properties
        FaceColors faceColors(4);
        faceColors[0] = Color8(255, 0, 0, 255);
        faceColors[1] = Color8(0, 0, 255, 255);
        faceColors[2] = Color8(255, 0, 0, 255);
        faceColors[3] = Color8(0, 0, 255, 255);

        auto palette = ColorQuantizer::quantize(faceColors, 2);
        auto regions = ColorRegionizer::regionize(palette, PrintableRegionKind::BuildItem);

        auto const outputPath = m_outputDir / "regions_materials.3mf";
        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithRegions(outputPath, *m_mesh, "MatMesh", palette, regions);

        // Read back and verify a base material group exists (regions use basematerials for slicer compat)
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        auto baseMaterials = model->GetBaseMaterialGroups();
        EXPECT_TRUE(baseMaterials->MoveNext()) << "Should have a base material group for region colors";
    }

    TEST_F(MeshWriter3mfColorTest, ExportMeshWithRegions_SingleColor_ProducesSingleBuildItem)
    {
        // All same color → 1 region
        FaceColors faceColors(4);
        faceColors[0] = Color8(200, 200, 200, 255);
        faceColors[1] = Color8(200, 200, 200, 255);
        faceColors[2] = Color8(200, 200, 200, 255);
        faceColors[3] = Color8(200, 200, 200, 255);

        auto palette = ColorQuantizer::quantize(faceColors, 1);
        auto regions = ColorRegionizer::regionize(palette, PrintableRegionKind::BuildItem);

        ASSERT_EQ(regions.size(), 1u);

        auto const outputPath = m_outputDir / "regions_single.3mf";
        MeshWriter3mf writer(nullptr);
        writer.exportMeshWithRegions(outputPath, *m_mesh, "SingleColor", palette, regions);

        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->ReadFromFile(outputPath.string());

        auto buildItems = model->GetBuildItems();
        std::uint64_t count = 0;
        while (buildItems->MoveNext())
        {
            ++count;
        }
        EXPECT_EQ(count, 1u);
    }

} // namespace gladius_tests

