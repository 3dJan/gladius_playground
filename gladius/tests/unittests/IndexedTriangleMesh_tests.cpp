#include "io/IndexedTriangleMesh.h"
#include "io/IndexedTriangleMeshWriter.h"

#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace gladius::io::tests
{
    namespace
    {
        IndexedTriangleMesh makeSingleTriangle()
        {
            IndexedTriangleMesh mesh;
            mesh.positions = {
              Eigen::Vector3f{0.0F, 0.0F, 0.0F},
              Eigen::Vector3f{1.0F, 0.0F, 0.0F},
              Eigen::Vector3f{0.0F, 1.0F, 0.0F}};
            mesh.indices = {0U, 1U, 2U};
            return mesh;
        }

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

        std::uint32_t readUInt32LittleEndian(std::string const & bytes, std::size_t offset)
        {
            auto const byte0 = static_cast<std::uint8_t>(bytes[offset]);
            auto const byte1 = static_cast<std::uint8_t>(bytes[offset + 1U]);
            auto const byte2 = static_cast<std::uint8_t>(bytes[offset + 2U]);
            auto const byte3 = static_cast<std::uint8_t>(bytes[offset + 3U]);
            return static_cast<std::uint32_t>(byte0) |
                   (static_cast<std::uint32_t>(byte1) << 8U) |
                   (static_cast<std::uint32_t>(byte2) << 16U) |
                   (static_cast<std::uint32_t>(byte3) << 24U);
        }

        float readFloatLittleEndian(std::string const & bytes, std::size_t offset)
        {
            return std::bit_cast<float>(readUInt32LittleEndian(bytes, offset));
        }
    }

    TEST(IndexedTriangleMesh, Validation_ValidTriangleWithoutNormals_ReturnsNoError)
    {
        auto const mesh = makeSingleTriangle();

        EXPECT_TRUE(mesh.isValid());
        EXPECT_FALSE(mesh.validationError().has_value());
    }

    TEST(IndexedTriangleMesh, Validation_ValidTriangleWithNormals_ReturnsNoError)
    {
        auto mesh = makeSingleTriangle();
        mesh.normals.assign(3U, Eigen::Vector3f{0.0F, 0.0F, 1.0F});

        EXPECT_TRUE(mesh.isValid());
    }

    TEST(IndexedTriangleMesh, Validation_EmptyMesh_ReturnsError)
    {
        IndexedTriangleMesh const mesh;

        EXPECT_FALSE(mesh.isValid());
        EXPECT_EQ(mesh.validationError().value_or(""), "Mesh has no positions");
    }

    TEST(IndexedTriangleMesh, Validation_IncompleteTriangle_ReturnsError)
    {
        auto mesh = makeSingleTriangle();
        mesh.indices.pop_back();

        EXPECT_EQ(mesh.validationError().value_or(""),
                  "Mesh index count is not divisible by three");
    }

    TEST(IndexedTriangleMesh, Validation_OutOfRangeIndex_ReturnsError)
    {
        auto mesh = makeSingleTriangle();
        mesh.indices[2] = 3U;

        EXPECT_EQ(mesh.validationError().value_or(""),
                  "Mesh contains an out-of-range triangle index");
    }

    TEST(IndexedTriangleMesh, Validation_NonFinitePosition_ReturnsError)
    {
        auto mesh = makeSingleTriangle();
        mesh.positions[0].x() = std::numeric_limits<float>::quiet_NaN();

        EXPECT_EQ(mesh.validationError().value_or(""), "Mesh contains a non-finite position");
    }

    TEST(IndexedTriangleMesh, Validation_MismatchedNormalCount_ReturnsError)
    {
        auto mesh = makeSingleTriangle();
        mesh.normals.emplace_back(0.0F, 0.0F, 1.0F);

        EXPECT_EQ(mesh.validationError().value_or(""),
                  "Mesh normal count does not match its position count");
    }

    TEST(IndexedTriangleMesh, Validation_NonFiniteNormal_ReturnsError)
    {
        auto mesh = makeSingleTriangle();
        mesh.normals.assign(3U, Eigen::Vector3f{0.0F, 0.0F, 1.0F});
        mesh.normals[1].y() = std::numeric_limits<float>::infinity();

        EXPECT_EQ(mesh.validationError().value_or(""), "Mesh contains a non-finite normal");
    }

    TEST(IndexedTriangleMesh, Validation_RepeatedIndex_ReturnsError)
    {
        auto mesh = makeSingleTriangle();
        mesh.indices[2] = mesh.indices[1];

        EXPECT_EQ(mesh.validationError().value_or(""),
                  "Mesh contains a triangle with repeated indices");
    }

    TEST(IndexedTriangleMesh, Validation_CollinearTriangle_ReturnsError)
    {
        auto mesh = makeSingleTriangle();
        mesh.positions[2] = Eigen::Vector3f{2.0F, 0.0F, 0.0F};

        EXPECT_EQ(mesh.validationError().value_or(""), "Mesh contains a degenerate triangle");
    }

    TEST(IndexedTriangleMesh, Validation_AllowGeometricDegenerates_PreservesStructuralChecks)
    {
        auto mesh = makeSingleTriangle();
        mesh.positions[2] = Eigen::Vector3f{2.0F, 0.0F, 0.0F};
        auto const policy = IndexedTriangleMeshValidationPolicy::AllowGeometricDegenerates;

        EXPECT_TRUE(mesh.validationError().has_value());
        EXPECT_FALSE(mesh.validationError(policy).has_value());

        mesh.indices[2] = mesh.indices[1];
        EXPECT_EQ(mesh.validationError(policy).value_or(""),
                  "Mesh contains a triangle with repeated indices");
    }

    TEST(IndexedTriangleMesh, Topology_OpenTriangle_ReturnsError)
    {
        auto const mesh = makeSingleTriangle();

        EXPECT_NE(mesh.closedOrientedTopologyError(), std::nullopt);
    }

    TEST(IndexedTriangleMesh, Topology_ClosedOrientedTetrahedron_ReturnsNoError)
    {
        auto const mesh = makeTetrahedron();

        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());
    }

    TEST(IndexedTriangleMesh, Topology_AllowGeometricDegenerates_StillChecksEdgeOrientation)
    {
        auto mesh = makeTetrahedron();
        mesh.positions[3] = mesh.positions[0];
        auto const policy = IndexedTriangleMeshValidationPolicy::AllowGeometricDegenerates;

        EXPECT_TRUE(mesh.closedOrientedTopologyError().has_value());
        EXPECT_FALSE(mesh.closedOrientedTopologyError(policy).has_value());

        std::swap(mesh.indices[1], mesh.indices[2]);
        EXPECT_EQ(mesh.closedOrientedTopologyError(policy).value_or(""),
                  "Mesh has inconsistent face winding across a shared edge");
    }

    TEST(IndexedTriangleMesh, Topology_ReversedTetrahedronFace_ReturnsWindingError)
    {
        auto mesh = makeTetrahedron();
        std::swap(mesh.indices[1], mesh.indices[2]);

        EXPECT_EQ(mesh.closedOrientedTopologyError().value_or(""),
                  "Mesh has inconsistent face winding across a shared edge");
    }

    TEST(IndexedTriangleMesh, Diagnostics_ClosedTetrahedronReportsTopologyAndVolume)
    {
        IndexedTriangleMesh mesh;
        mesh.positions = {Eigen::Vector3f(1.0F, 1.0F, 1.0F),
                          Eigen::Vector3f(-1.0F, -1.0F, 1.0F),
                          Eigen::Vector3f(-1.0F, 1.0F, -1.0F),
                          Eigen::Vector3f(1.0F, -1.0F, -1.0F)};
        mesh.indices = {0U, 2U, 1U, 0U, 1U, 3U, 0U, 3U, 2U, 1U, 2U, 3U};

        auto const diagnostics = computeIndexedTriangleMeshDiagnostics(mesh);

        EXPECT_EQ(diagnostics.vertexCount, 4U);
        EXPECT_EQ(diagnostics.referencedVertexCount, 4U);
        EXPECT_EQ(diagnostics.triangleCount, 4U);
        EXPECT_EQ(diagnostics.connectedComponentCount, 1U);
        EXPECT_EQ(diagnostics.boundaryEdgeCount, 0U);
        EXPECT_EQ(diagnostics.nonManifoldEdgeCount, 0U);
        EXPECT_EQ(diagnostics.inconsistentWindingEdgeCount, 0U);
        EXPECT_EQ(diagnostics.degenerateTriangleCount, 0U);
        EXPECT_NEAR(std::abs(diagnostics.signedVolume), 8.0 / 3.0, 1e-6);
        EXPECT_FLOAT_EQ(diagnostics.boundsMin.x(), -1.0F);
        EXPECT_FLOAT_EQ(diagnostics.boundsMin.y(), -1.0F);
        EXPECT_FLOAT_EQ(diagnostics.boundsMin.z(), -1.0F);
        EXPECT_FLOAT_EQ(diagnostics.boundsMax.x(), 1.0F);
        EXPECT_FLOAT_EQ(diagnostics.boundsMax.y(), 1.0F);
        EXPECT_FLOAT_EQ(diagnostics.boundsMax.z(), 1.0F);
    }

    TEST(IndexedTriangleMesh, Diagnostics_TranslatedTetrahedronPreservesSignedVolume)
    {
        IndexedTriangleMesh mesh;
        mesh.positions = {Eigen::Vector3f(1.0F, 1.0F, 1.0F),
                          Eigen::Vector3f(-1.0F, -1.0F, 1.0F),
                          Eigen::Vector3f(-1.0F, 1.0F, -1.0F),
                          Eigen::Vector3f(1.0F, -1.0F, -1.0F)};
        mesh.indices = {0U, 2U, 1U, 0U, 1U, 3U, 0U, 3U, 2U, 1U, 2U, 3U};
        Eigen::Vector3f const translation(1'000'000.0F, -2'000'000.0F, 3'000'000.0F);
        for (auto& position : mesh.positions)
        {
            position += translation;
        }

        auto const diagnostics = computeIndexedTriangleMeshDiagnostics(mesh);

        EXPECT_NEAR(std::abs(diagnostics.signedVolume), 8.0 / 3.0, 1e-6);
    }

    TEST(IndexedTriangleMesh, Diagnostics_DisconnectedTrianglesReportsTwoComponents)
    {
        IndexedTriangleMesh mesh;
        mesh.positions = {Eigen::Vector3f(0.0F, 0.0F, 0.0F),
                          Eigen::Vector3f(1.0F, 0.0F, 0.0F),
                          Eigen::Vector3f(0.0F, 1.0F, 0.0F),
                          Eigen::Vector3f(3.0F, 0.0F, 0.0F),
                          Eigen::Vector3f(4.0F, 0.0F, 0.0F),
                          Eigen::Vector3f(3.0F, 1.0F, 0.0F),
                          Eigen::Vector3f(1000.0F, 1000.0F, 1000.0F)};
        mesh.indices = {0U, 1U, 2U, 3U, 4U, 5U};

        auto const diagnostics = computeIndexedTriangleMeshDiagnostics(mesh);

        EXPECT_EQ(diagnostics.vertexCount, 7U);
        EXPECT_EQ(diagnostics.referencedVertexCount, 6U);
        EXPECT_EQ(diagnostics.connectedComponentCount, 2U);
        EXPECT_EQ(diagnostics.boundaryEdgeCount, 6U);
        EXPECT_FLOAT_EQ(diagnostics.boundsMax.x(), 4.0F);
        EXPECT_FLOAT_EQ(diagnostics.boundsMax.y(), 1.0F);
        EXPECT_FLOAT_EQ(diagnostics.boundsMax.z(), 0.0F);
    }

    TEST(IndexedTriangleMesh, Diagnostics_NonManifoldEdgeReportsIncidentFaceCount)
    {
        IndexedTriangleMesh mesh;
        mesh.positions = {Eigen::Vector3f(0.0F, 0.0F, 0.0F),
                          Eigen::Vector3f(1.0F, 0.0F, 0.0F),
                          Eigen::Vector3f(0.0F, 1.0F, 0.0F),
                          Eigen::Vector3f(0.0F, -1.0F, 0.0F),
                          Eigen::Vector3f(0.0F, 0.0F, 1.0F)};
        mesh.indices = {0U, 1U, 2U, 1U, 0U, 3U, 0U, 1U, 4U};

        auto const diagnostics = computeIndexedTriangleMeshDiagnostics(mesh);

        EXPECT_EQ(diagnostics.nonManifoldEdgeCount, 1U);
        EXPECT_EQ(diagnostics.connectedComponentCount, 1U);
    }

    TEST(IndexedTriangleMesh, Diagnostics_InvalidIndicesAreCountedWithoutDereferencing)
    {
        IndexedTriangleMesh mesh;
        mesh.positions = {Eigen::Vector3f(0.0F, 0.0F, 0.0F),
                          Eigen::Vector3f(1.0F, 0.0F, 0.0F),
                          Eigen::Vector3f(0.0F, 1.0F, 0.0F)};
        mesh.indices = {0U, 1U, 3U};

        auto const diagnostics = computeIndexedTriangleMeshDiagnostics(mesh);

        EXPECT_EQ(diagnostics.invalidTriangleIndexCount, 1U);
        EXPECT_EQ(diagnostics.connectedComponentCount, 0U);
        EXPECT_EQ(diagnostics.boundaryEdgeCount, 0U);
    }

    TEST(IndexedTriangleMeshWriter, WriteBinaryStl_ValidTriangle_ProducesStandardBinaryLayout)
    {
        auto const mesh = makeSingleTriangle();
        std::ostringstream output(std::ios::out | std::ios::binary);

        ASSERT_NO_THROW(writeIndexedMeshToBinaryStl(output, mesh));
        auto const bytes = output.str();

        ASSERT_EQ(bytes.size(), 84U + 50U);
        EXPECT_EQ(readUInt32LittleEndian(bytes, 80U), 1U);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 84U), 0.0F);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 88U), 0.0F);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 92U), 1.0F);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 96U), 0.0F);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 108U), 1.0F);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 124U), 1.0F);
        EXPECT_EQ(bytes[132U], '\0');
        EXPECT_EQ(bytes[133U], '\0');
    }

    TEST(IndexedTriangleMeshWriter, WriteBinaryStl_InvalidMesh_ThrowsBeforeWriting)
    {
        auto mesh = makeSingleTriangle();
        mesh.indices[2] = 3U;
        std::ostringstream output(std::ios::out | std::ios::binary);

        EXPECT_THROW(writeIndexedMeshToBinaryStl(output, mesh), std::invalid_argument);
        EXPECT_TRUE(output.str().empty());
    }

    TEST(IndexedTriangleMeshWriter, WriteBinaryStl_AllowedDegenerateFacet_UsesFiniteFallbackNormal)
    {
        auto mesh = makeSingleTriangle();
        mesh.positions[2] = Eigen::Vector3f{2.0F, 0.0F, 0.0F};
        std::ostringstream output(std::ios::out | std::ios::binary);
        auto const policy = IndexedTriangleMeshValidationPolicy::AllowGeometricDegenerates;

        EXPECT_THROW(writeIndexedMeshToBinaryStl(output, mesh), std::invalid_argument);
        ASSERT_NO_THROW(writeIndexedMeshToBinaryStl(output, mesh, policy));
        auto const bytes = output.str();

        ASSERT_EQ(bytes.size(), 84U + 50U);
        EXPECT_EQ(readUInt32LittleEndian(bytes, 80U), 1U);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 84U), 0.0F);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 88U), 0.0F);
        EXPECT_FLOAT_EQ(readFloatLittleEndian(bytes, 92U), 1.0F);
    }

    TEST(IndexedTriangleMeshWriter, WriteBinaryStl_ValidMeshFile_CreatesBinaryFile)
    {
        auto const mesh = makeSingleTriangle();
        auto const uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
        auto const filePath = std::filesystem::temp_directory_path() /
                              ("gladius_indexed_mesh_" + std::to_string(uniqueSuffix) + ".stl");

        ASSERT_NO_THROW(writeIndexedMeshToBinaryStl(filePath, mesh));
        ASSERT_TRUE(std::filesystem::exists(filePath));
        EXPECT_EQ(std::filesystem::file_size(filePath), 84U + 50U);

        std::error_code error;
        std::filesystem::remove(filePath, error);
    }

    TEST(IndexedTriangleMeshWriter, WriteBinaryStl_InvalidMesh_PreservesExistingDestination)
    {
        auto mesh = makeSingleTriangle();
        mesh.indices[2] = 3U;
        auto const uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
        auto const filePath = std::filesystem::temp_directory_path() /
                              ("gladius_indexed_mesh_existing_" + std::to_string(uniqueSuffix) +
                               ".stl");
        constexpr std::string_view existingContent{"previous export"};
        {
            std::ofstream output(filePath, std::ios::binary);
            ASSERT_TRUE(output);
            output << existingContent;
        }

        EXPECT_THROW(writeIndexedMeshToBinaryStl(filePath, mesh), std::invalid_argument);
        std::ifstream input(filePath, std::ios::binary);
        ASSERT_TRUE(input);
        std::string const actualContent{std::istreambuf_iterator<char>{input},
                                        std::istreambuf_iterator<char>{}};
        EXPECT_EQ(actualContent, existingContent);

        std::error_code error;
        std::filesystem::remove(filePath, error);
    }
} // namespace gladius::io::tests