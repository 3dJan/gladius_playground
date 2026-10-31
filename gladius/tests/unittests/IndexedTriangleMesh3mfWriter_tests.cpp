#include "io/IndexedTriangleMesh.h"
#include "io/3mf/IndexedTriangleMesh3mfWriter.h"
#include "io/3mf/Lib3mfLoader.h"

#include <gtest/gtest.h>
#include <lodepng.h>
#include <lib3mf_abi.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gladius::io::tests
{
    namespace
    {
        IndexedTriangleMesh makeTetrahedron()
        {
            IndexedTriangleMesh mesh;
            mesh.positions = {
              Eigen::Vector3f{0.0F, 0.0F, 0.0F},
              Eigen::Vector3f{1.0F, 0.0F, 0.0F},
              Eigen::Vector3f{0.0F, 1.0F, 0.0F},
              Eigen::Vector3f{0.0F, 0.0F, 1.0F}};
            mesh.indices = {0U, 2U, 1U, 0U, 1U, 3U, 0U, 3U, 2U, 1U, 2U, 3U};
            return mesh;
        }

        std::filesystem::path makeTemporaryPath()
        {
            auto const uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
            return std::filesystem::temp_directory_path() /
                   ("gladius_indexed_mesh_" + std::to_string(uniqueSuffix) + ".3mf");
        }

        std::vector<Lib3MF_uint8> makeThumbnailPng()
        {
            std::vector<unsigned char> rgba{255u, 0u, 0u, 255u,
                                            0u, 255u, 0u, 255u,
                                            0u, 0u, 255u, 255u,
                                            255u, 255u, 0u, 255u};
            std::vector<unsigned char> encoded;
            auto const error = lodepng::encode(encoded, rgba, 2u, 2u);
            if (error != 0u)
            {
                throw std::runtime_error("Failed to encode test thumbnail: " +
                                         std::string{lodepng_error_text(error)});
            }
            return {encoded.begin(), encoded.end()};
        }

        void expectThumbnail(Lib3MF::PModel const & model)
        {
            ASSERT_TRUE(model->HasPackageThumbnailAttachment());
            auto const attachment = model->GetPackageThumbnailAttachment();
            ASSERT_TRUE(attachment);

            std::vector<Lib3MF_uint8> encoded;
            attachment->WriteToBuffer(encoded);
            std::vector<unsigned char> decoded;
            unsigned width = 0u;
            unsigned height = 0u;
            auto const error = lodepng::decode(decoded, width, height, encoded);
            ASSERT_EQ(error, 0u) << lodepng_error_text(error);
            EXPECT_EQ(width, 2u);
            EXPECT_EQ(height, 2u);
            EXPECT_EQ(decoded.size(), 16u);
        }
    } // namespace

    TEST(IndexedTriangleMesh3mfWriter, RoundTrip_ClosedTetrahedron_PreservesMeshGeometry)
    {
        auto const mesh = makeTetrahedron();
        auto const filePath = makeTemporaryPath();
        auto const thumbnailPng = makeThumbnailPng();
        IndexedTriangleMesh3mfWriter writer;

        ASSERT_NO_THROW(writer.exportMesh(filePath,
                          mesh,
                          "Tetrahedron",
                          IndexedTriangleMeshValidationPolicy::Strict,
                          thumbnailPng));
        ASSERT_TRUE(std::filesystem::exists(filePath));

        auto const wrapper = loadLib3mfScoped();
        auto const model = wrapper->CreateModel();
        model->QueryReader("3mf")->ReadFromFile(filePath.string());

        auto const meshObjects = model->GetMeshObjects();
        ASSERT_TRUE(meshObjects->MoveNext());
        auto const exportedMesh = meshObjects->GetCurrentMeshObject();
        expectThumbnail(model);
        EXPECT_EQ(exportedMesh->GetVertexCount(), mesh.positions.size());
        EXPECT_EQ(exportedMesh->GetTriangleCount(), mesh.indices.size() / 3U);
        EXPECT_EQ(model->GetBuildItems()->Count(), 1U);

                for (std::size_t index = 0U; index < mesh.positions.size(); ++index)
                {
                        auto const exportedPosition =
                            exportedMesh->GetVertex(static_cast<Lib3MF_uint32>(index));
                        EXPECT_FLOAT_EQ(exportedPosition.m_Coordinates[0], mesh.positions[index].x());
                        EXPECT_FLOAT_EQ(exportedPosition.m_Coordinates[1], mesh.positions[index].y());
                        EXPECT_FLOAT_EQ(exportedPosition.m_Coordinates[2], mesh.positions[index].z());
                }
                for (std::size_t offset = 0U; offset < mesh.indices.size(); offset += 3U)
                {
                        auto const exportedTriangle =
                            exportedMesh->GetTriangle(static_cast<Lib3MF_uint32>(offset / 3U));
                        EXPECT_EQ(exportedTriangle.m_Indices[0], mesh.indices[offset]);
                        EXPECT_EQ(exportedTriangle.m_Indices[1], mesh.indices[offset + 1U]);
                        EXPECT_EQ(exportedTriangle.m_Indices[2], mesh.indices[offset + 2U]);
                }

        std::error_code cleanupError;
        std::filesystem::remove(filePath, cleanupError);
    }

    TEST(IndexedTriangleMesh3mfWriter, Export_OpenMesh_RejectsBeforeCreatingFile)
    {
        IndexedTriangleMesh mesh;
        mesh.positions = {
          Eigen::Vector3f{0.0F, 0.0F, 0.0F},
          Eigen::Vector3f{1.0F, 0.0F, 0.0F},
          Eigen::Vector3f{0.0F, 1.0F, 0.0F}};
        mesh.indices = {0U, 1U, 2U};
        auto const filePath = makeTemporaryPath();
        IndexedTriangleMesh3mfWriter writer;

        EXPECT_THROW(writer.exportMesh(filePath, mesh), std::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(filePath));
    }

    TEST(IndexedTriangleMesh3mfWriter, ExportWithColors_RoundTripsTriangleProperties)
    {
        auto const mesh = makeTetrahedron();
        auto const filePath = makeTemporaryPath();
        auto const thumbnailPng = makeThumbnailPng();
        FaceColors colors(4u);
        colors[0] = Color8{255u, 0u, 0u};
        colors[1] = Color8{0u, 255u, 0u};
        colors[2] = Color8{0u, 0u, 255u};
        colors[3] = Color8{255u, 0u, 0u};
        IndexedTriangleMesh3mfWriter writer;

                ASSERT_NO_THROW(writer.exportMeshWithColors(
                    filePath, mesh, "Colored tetrahedron", colors, thumbnailPng));
        auto const wrapper = loadLib3mfScoped();
        auto const model = wrapper->CreateModel();
        model->QueryReader("3mf")->ReadFromFile(filePath.string());

        auto const meshes = model->GetMeshObjects();
        ASSERT_TRUE(meshes->MoveNext());
        auto const exportedMesh = meshes->GetCurrentMeshObject();
        expectThumbnail(model);
        std::vector<Lib3MF::sTriangleProperties> properties;
        exportedMesh->GetAllTriangleProperties(properties);
        ASSERT_EQ(properties.size(), 4u);
        EXPECT_EQ(properties[0].m_PropertyIDs[0], properties[0].m_PropertyIDs[1]);
        EXPECT_EQ(properties[0].m_PropertyIDs[1], properties[0].m_PropertyIDs[2]);

        auto const colorGroups = model->GetColorGroups();
        ASSERT_TRUE(colorGroups->MoveNext());
        EXPECT_EQ(colorGroups->GetCurrentColorGroup()->GetCount(), 4u);
        EXPECT_EQ(model->GetBuildItems()->Count(), 1u);
        std::error_code cleanupError;
        std::filesystem::remove(filePath, cleanupError);
    }

    TEST(IndexedTriangleMesh3mfWriter, ExportWithRegions_CreatesOneBuildItemPerPaletteColor)
    {
        auto const mesh = makeTetrahedron();
        auto const filePath = makeTemporaryPath();
        auto const thumbnailPng = makeThumbnailPng();
        FaceColors colors(4u);
        colors[0] = Color8{255u, 0u, 0u};
        colors[1] = Color8{0u, 255u, 0u};
        colors[2] = Color8{255u, 0u, 0u};
        colors[3] = Color8{0u, 255u, 0u};
        auto const palette = ColorQuantizer::quantize(colors, 2u);
        auto const regions = ColorRegionizer::regionize(palette, PrintableRegionKind::Component);
        IndexedTriangleMesh3mfWriter writer;

        ASSERT_EQ(regions.size(), 2u);
                ASSERT_NO_THROW(writer.exportMeshWithRegions(
                    filePath, mesh, "Region tetrahedron", palette, regions, thumbnailPng));
        auto const wrapper = loadLib3mfScoped();
        auto const model = wrapper->CreateModel();
        model->QueryReader("3mf")->ReadFromFile(filePath.string());

        expectThumbnail(model);
        EXPECT_EQ(model->GetMeshObjects()->Count(), 2u);
        EXPECT_EQ(model->GetBuildItems()->Count(), 2u);
        std::error_code cleanupError;
        std::filesystem::remove(filePath, cleanupError);
    }

    TEST(IndexedTriangleMesh3mfWriter, ExportWithMmuSegmentation_ProducesReadable3mf)
    {
        auto const mesh = makeTetrahedron();
        auto const filePath = makeTemporaryPath();
        auto const thumbnailPng = makeThumbnailPng();
        FaceColors colors(4u);
        colors[0] = Color8{255u, 0u, 0u};
        colors[1] = Color8{0u, 255u, 0u};
        colors[2] = Color8{0u, 0u, 255u};
        colors[3] = Color8{255u, 255u, 0u};
        auto const palette = ColorQuantizer::quantize(colors, 4u);
        IndexedTriangleMesh3mfWriter writer;

                ASSERT_NO_THROW(writer.exportMeshWithMmuSegmentation(
                    filePath, mesh, "MMU tetrahedron", palette, thumbnailPng));
        auto const wrapper = loadLib3mfScoped();
        auto const model = wrapper->CreateModel();
        EXPECT_NO_THROW(model->QueryReader("3mf")->ReadFromFile(filePath.string()));
        expectThumbnail(model);
        std::error_code cleanupError;
        std::filesystem::remove(filePath, cleanupError);
    }

    TEST(IndexedTriangleMesh3mfWriter, Export_WithoutThumbnail_LeavesAttachmentAbsent)
    {
        auto const mesh = makeTetrahedron();
        auto const filePath = makeTemporaryPath();
        IndexedTriangleMesh3mfWriter writer;

        ASSERT_NO_THROW(writer.exportMesh(filePath, mesh, "Tetrahedron"));
        auto const wrapper = loadLib3mfScoped();
        auto const model = wrapper->CreateModel();
        ASSERT_NO_THROW(model->QueryReader("3mf")->ReadFromFile(filePath.string()));
        EXPECT_FALSE(model->HasPackageThumbnailAttachment());

        std::error_code cleanupError;
        std::filesystem::remove(filePath, cleanupError);
    }

} // namespace gladius::io::tests
