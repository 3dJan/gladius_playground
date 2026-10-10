#include "io/IndexedTriangleMesh.h"
#include "io/3mf/IndexedTriangleMesh3mfWriter.h"
#include "io/3mf/Lib3mfLoader.h"

#include <gtest/gtest.h>
#include <lib3mf_abi.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <utility>

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
    } // namespace

    TEST(IndexedTriangleMesh3mfWriter, RoundTrip_ClosedTetrahedron_PreservesMeshGeometry)
    {
        auto const mesh = makeTetrahedron();
        auto const filePath = makeTemporaryPath();
        IndexedTriangleMesh3mfWriter writer;

        ASSERT_NO_THROW(writer.exportMesh(filePath, mesh, "Tetrahedron"));
        ASSERT_TRUE(std::filesystem::exists(filePath));

        auto const wrapper = loadLib3mfScoped();
        auto const model = wrapper->CreateModel();
        model->QueryReader("3mf")->ReadFromFile(filePath.string());

        auto const meshObjects = model->GetMeshObjects();
        ASSERT_TRUE(meshObjects->MoveNext());
        auto const exportedMesh = meshObjects->GetCurrentMeshObject();
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
} // namespace gladius::io::tests
