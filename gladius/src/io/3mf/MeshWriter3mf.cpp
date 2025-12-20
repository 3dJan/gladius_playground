/**
 * @file MeshWriter3mf.cpp
 * @brief Implementation of 3MF mesh exporter using core specification only
 */

#include "io/3mf/MeshWriter3mf.h"

#include "ComputeContext.h"
#include "Document.h"
#include "EventLogger.h"
#include "MeshResource.h"
#include "ResourceKey.h"
#include "VdbResource.h"
#include "nodes/Model.h"

#include <lib3mf_abi.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>
#include <tuple>
#include <unordered_map>

namespace
{
    [[nodiscard]] bool isFinite3(float x, float y, float z)
    {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    [[nodiscard]] double triangleArea2(float ax, float ay, float az,
                                       float bx, float by, float bz,
                                       float cx, float cy, float cz)
    {
        // 4 * area^2 = |(b-a) x (c-a)|^2
        double const abx = static_cast<double>(bx) - static_cast<double>(ax);
        double const aby = static_cast<double>(by) - static_cast<double>(ay);
        double const abz = static_cast<double>(bz) - static_cast<double>(az);

        double const acx = static_cast<double>(cx) - static_cast<double>(ax);
        double const acy = static_cast<double>(cy) - static_cast<double>(ay);
        double const acz = static_cast<double>(cz) - static_cast<double>(az);

        double const cxp = aby * acz - abz * acy;
        double const cyp = abz * acx - abx * acz;
        double const czp = abx * acy - aby * acx;
        return cxp * cxp + cyp * cyp + czp * czp;
    }

    void verifyFileWritten(std::filesystem::path const & filePath)
    {
        std::error_code ec;
        bool const exists = std::filesystem::exists(filePath, ec);
        if (ec || !exists)
        {
            throw std::runtime_error(
              fmt::format("3MF export failed to create output file: {}", filePath.string()));
        }

        auto const size = std::filesystem::file_size(filePath, ec);
        if (ec || size == 0U)
        {
            throw std::runtime_error(
              fmt::format("3MF export produced an empty output file: {}", filePath.string()));
        }
    }
}

namespace gladius::io
{

    MeshWriter3mf::MeshWriter3mf(events::SharedLogger logger)
        : Writer3mfBase(std::move(logger))
    {
    }

    void MeshWriter3mf::exportMesh(std::filesystem::path const & filePath,
                                   Mesh const & mesh,
                                   std::string const & meshName,
                                   Document const * sourceDocument,
                                   bool writeThumbnail)
    {
        if (!validateMesh(mesh))
        {
            throw std::runtime_error("Invalid mesh for export");
        }

        try
        {
            // Create new 3MF model
            auto model3mf = m_wrapper->CreateModel();

            // Add default metadata
            addDefaultMetadata(model3mf);

            // Copy metadata from source document if available
            if (sourceDocument)
            {
                copyMetadata(*sourceDocument, model3mf);
            }

            // Add mesh to model
            auto meshObject = addMeshToModel(model3mf, mesh, meshName);

            // Create build item
            createBuildItem(model3mf, meshObject, meshName);

            // Add thumbnail if requested and source document is available
            if (writeThumbnail && sourceDocument)
            {
                updateThumbnail(const_cast<Document &>(*sourceDocument), model3mf);
            }

            // Write to file
            auto writer = model3mf->QueryWriter("3mf");
            writer->WriteToFile(filePath.string());
            verifyFileWritten(filePath);

            if (m_logger)
            {
                m_logger->addEvent(
                  {fmt::format("Successfully exported mesh to {}", filePath.string()),
                   events::Severity::Info});
            }
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->addEvent(
                  {fmt::format("Failed to export mesh to {}: {}", filePath.string(), e.what()),
                   events::Severity::Error});
            }
            throw;
        }
    }

    void MeshWriter3mf::exportMeshes(
      std::filesystem::path const & filePath,
      std::vector<std::pair<std::shared_ptr<Mesh>, std::string>> const & meshes,
      Document const * sourceDocument,
      bool writeThumbnail)
    {
        if (meshes.empty())
        {
            throw std::runtime_error("No meshes provided for export");
        }

        try
        {
            // Create new 3MF model
            auto model3mf = m_wrapper->CreateModel();

            // Add default metadata
            addDefaultMetadata(model3mf);

            // Copy metadata from source document if available
            if (sourceDocument)
            {
                copyMetadata(*sourceDocument, model3mf);
            }

            // Add all meshes to model
            for (auto const & [mesh, name] : meshes)
            {
                if (!mesh || !validateMesh(*mesh))
                {
                    if (m_logger)
                    {
                        m_logger->addEvent({fmt::format("Skipping invalid mesh: {}", name),
                                            events::Severity::Warning});
                    }
                    continue;
                }

                auto meshObject = addMeshToModel(model3mf, *mesh, name);
                createBuildItem(model3mf, meshObject, name);
            }

            // Add thumbnail if requested and source document is available
            if (writeThumbnail && sourceDocument)
            {
                updateThumbnail(const_cast<Document &>(*sourceDocument), model3mf);
            }

            // Write to file
            auto writer = model3mf->QueryWriter("3mf");
            writer->WriteToFile(filePath.string());
            verifyFileWritten(filePath);

            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Successfully exported {} meshes to {}",
                                                meshes.size(),
                                                filePath.string()),
                                    events::Severity::Info});
            }
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->addEvent(
                  {fmt::format("Failed to export meshes to {}: {}", filePath.string(), e.what()),
                   events::Severity::Error});
            }
            throw;
        }
    }

    void MeshWriter3mf::exportMeshesWithMaterialColors(
        std::filesystem::path const & filePath,
        std::vector<std::tuple<std::shared_ptr<Mesh>, std::string, Eigen::Vector3f>> const & meshesWithColors,
        Document const * sourceDocument,
        bool writeThumbnail)
    {
        if (meshesWithColors.empty())
        {
            throw std::runtime_error("No meshes provided for export");
        }

        try
        {
            auto model3mf = m_wrapper->CreateModel();
            addDefaultMetadata(model3mf);

            if (sourceDocument)
            {
                copyMetadata(*sourceDocument, model3mf);
            }

            // Create a color group for solid per-mesh colors
            auto colorGroup = model3mf->AddColorGroup();
            Lib3MF_uint32 const colorGroupId = colorGroup->GetUniqueResourceID();
            // placeholder at index 0
            colorGroup->AddColor({0, 0, 0, 0});

            for (auto const & [mesh, name, color] : meshesWithColors)
            {
                if (!mesh || !validateMesh(*mesh))
                {
                    if (m_logger)
                    {
                        m_logger->addEvent(
                            {fmt::format("Skipping invalid mesh: {}", name), events::Severity::Warning});
                    }
                    continue;
                }

                // Convert Eigen color to 8-bit
                auto toU8 = [](float v) -> std::uint8_t
                {
                    return static_cast<std::uint8_t>(std::clamp(v * 255.0F + 0.5F, 0.0F, 255.0F));
                };
                Lib3MF::sColor lib3mfColor{toU8(color.x()), toU8(color.y()), toU8(color.z()), 255};
                Lib3MF_uint32 const colorPropertyId = colorGroup->AddColor(lib3mfColor);

                auto meshObject = addMeshToModel(model3mf, *mesh, name);

                // Apply uniform color to every triangle
                std::size_t const numFaces = mesh->getNumberOfFaces();
                std::vector<Lib3MF::sTriangleProperties> triProps(numFaces);
                for (std::size_t i = 0; i < numFaces; ++i)
                {
                    triProps[i].m_ResourceID = colorGroupId;
                    triProps[i].m_PropertyIDs[0] = colorPropertyId;
                    triProps[i].m_PropertyIDs[1] = colorPropertyId;
                    triProps[i].m_PropertyIDs[2] = colorPropertyId;
                }
                meshObject->SetAllTriangleProperties(triProps);

                createBuildItem(model3mf, meshObject, name);
            }

            if (writeThumbnail && sourceDocument)
            {
                updateThumbnail(const_cast<Document &>(*sourceDocument), model3mf);
            }

            auto writer = model3mf->QueryWriter("3mf");
            writer->WriteToFile(filePath.string());
            verifyFileWritten(filePath);

            if (m_logger)
            {
                m_logger->addEvent(
                    {fmt::format("Successfully exported {} shell meshes with colors to {}",
                                 meshesWithColors.size(),
                                 filePath.string()),
                     events::Severity::Info});
            }
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->addEvent(
                    {fmt::format("Failed to export meshes with colors to {}: {}",
                                 filePath.string(),
                                 e.what()),
                     events::Severity::Error});
            }
            throw;
        }
    }

    void MeshWriter3mf::exportMeshWithColors(std::filesystem::path const & filePath,
                                             Mesh const & mesh,
                                             std::string const & meshName,
                                             FaceColors const & faceColors,
                                             Document const * sourceDocument,
                                             bool writeThumbnail)
    {
        if (!validateMesh(mesh))
        {
            throw std::runtime_error("Invalid mesh for export");
        }

        if (faceColors.size() != mesh.getNumberOfFaces())
        {
            throw std::runtime_error(
              fmt::format("Face color count ({}) does not match face count ({})",
                          faceColors.size(),
                          mesh.getNumberOfFaces()));
        }

        try
        {
            // Create new 3MF model
            auto model3mf = m_wrapper->CreateModel();

            // Add default metadata
            addDefaultMetadata(model3mf);

            // Copy metadata from source document if available
            if (sourceDocument)
            {
                copyMetadata(*sourceDocument, model3mf);
            }

            // Add mesh with colors to model
            auto [meshObject, colorGroupId] =
              addMeshWithColorsToModel(model3mf, mesh, meshName, faceColors);

            // Create build item
            createBuildItem(model3mf, meshObject, meshName);

            // Add thumbnail if requested and source document is available
            if (writeThumbnail && sourceDocument)
            {
                updateThumbnail(const_cast<Document &>(*sourceDocument), model3mf);
            }

            // Write to file
            auto writer = model3mf->QueryWriter("3mf");
            writer->WriteToFile(filePath.string());
            verifyFileWritten(filePath);

            if (m_logger)
            {
                m_logger->addEvent(
                  {fmt::format("Successfully exported colored mesh to {} ({} faces, {} colors)",
                               filePath.string(),
                               mesh.getNumberOfFaces(),
                               faceColors.size()),
                   events::Severity::Info});
            }
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Failed to export colored mesh to {}: {}",
                                                filePath.string(),
                                                e.what()),
                                    events::Severity::Error});
            }
            throw;
        }
    }

    void MeshWriter3mf::exportMeshWithVertexColors(std::filesystem::path const & filePath,
                                                   Mesh const & mesh,
                                                   std::string const & meshName,
                                                   VertexColors const & vertexColors,
                                                   Document const * sourceDocument,
                                                   bool writeThumbnail)
    {
        if (!validateMesh(mesh))
        {
            throw std::runtime_error("Invalid mesh for export");
        }

        if (vertexColors.size() != mesh.getNumberOfFaces())
        {
            throw std::runtime_error(
              fmt::format("Vertex color count ({}) does not match face count ({})",
                          vertexColors.size(),
                          mesh.getNumberOfFaces()));
        }

        try
        {
            // Create new 3MF model
            auto model3mf = m_wrapper->CreateModel();

            // Add default metadata
            addDefaultMetadata(model3mf);

            // Copy metadata from source document if available
            if (sourceDocument)
            {
                copyMetadata(*sourceDocument, model3mf);
            }

            // Add mesh with vertex colors to model
            auto [meshObject, colorGroupId] =
              addMeshWithVertexColorsToModel(model3mf, mesh, meshName, vertexColors);

            // Create build item
            createBuildItem(model3mf, meshObject, meshName);

            // Add thumbnail if requested and source document is available
            if (writeThumbnail && sourceDocument)
            {
                updateThumbnail(const_cast<Document &>(*sourceDocument), model3mf);
            }

            // Write to file
            auto writer = model3mf->QueryWriter("3mf");
            writer->WriteToFile(filePath.string());
            verifyFileWritten(filePath);

            if (m_logger)
            {
                m_logger->addEvent(
                  {fmt::format("Successfully exported vertex-colored mesh to {} ({} faces)",
                               filePath.string(),
                               mesh.getNumberOfFaces()),
                   events::Severity::Info});
            }
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Failed to export vertex-colored mesh to {}: {}",
                                                filePath.string(),
                                                e.what()),
                                    events::Severity::Error});
            }
            throw;
        }
    }

    void MeshWriter3mf::exportMeshFromDocument(std::filesystem::path const & filePath,
                                               Document & document,
                                               ResourceKey const & resourceKey,
                                               bool writeThumbnail)
    {
        // Get mesh from document's resource manager
        auto & resourceManager = document.getResourceManager();
        auto & resource = resourceManager.getResource(resourceKey);
        auto const * meshResource = dynamic_cast<MeshResource const *>(&resource);
        if (!meshResource)
        {
            throw std::runtime_error(
              fmt::format("Resource is not a mesh: {}", resourceKey.getDisplayName()));
        }

        // Get the triangle mesh from the resource
        auto const & triangleMesh = meshResource->getMesh();

        // Convert vdb::TriangleMesh to gladius::Mesh
        auto computeContext = document.getComputeContext();
        if (!computeContext)
        {
            throw std::runtime_error("No compute context available for mesh conversion");
        }

        // Convert triangle mesh to Gladius mesh format
        Mesh gladiusMesh(*computeContext);

        // Convert vertices to faces
        for (size_t i = 0; i < triangleMesh.indices.size(); ++i)
        {
            auto const & triangle = triangleMesh.indices[i];

            if (triangle.x() >= triangleMesh.vertices.size() ||
                triangle.y() >= triangleMesh.vertices.size() ||
                triangle.z() >= triangleMesh.vertices.size())
            {
                if (m_logger)
                {
                    m_logger->addEvent({fmt::format("Invalid triangle indices in mesh: {}",
                                                    resourceKey.getDisplayName()),
                                        events::Severity::Warning});
                }
                continue;
            }

            auto const & v1 = triangleMesh.vertices[triangle.x()];
            auto const & v2 = triangleMesh.vertices[triangle.y()];
            auto const & v3 = triangleMesh.vertices[triangle.z()];

            Vector3 vertex1(v1.x(), v1.y(), v1.z());
            Vector3 vertex2(v2.x(), v2.y(), v2.z());
            Vector3 vertex3(v3.x(), v3.y(), v3.z());

            gladiusMesh.addFace(vertex1, vertex2, vertex3);
        }

        std::string meshName = resourceKey.getDisplayName();
        if (meshName.empty())
        {
            meshName = fmt::format("Mesh_Resource");
        }

        exportMesh(filePath, gladiusMesh, meshName, &document, writeThumbnail);
    }

    Lib3MF::PMeshObject MeshWriter3mf::addMeshToModel(Lib3MF::PModel model3mf,
                                                      Mesh const & mesh,
                                                      std::string const & meshName)
    {
        auto meshObject = model3mf->AddMeshObject();
        meshObject->SetName(meshName);

        size_t const numFaces = mesh.getNumberOfFaces();
        if (numFaces == 0)
        {
            throw std::runtime_error("Mesh has no faces to export");
        }

        // Track unique vertices to avoid duplicates
        std::vector<Vector3> uniqueVertices;
        std::map<std::tuple<float, float, float>, Lib3MF_uint32> vertexMap;

        auto const tolerance = 1e-6f;

        auto getOrCreateVertex = [&](Vector3 const & vertex) -> Lib3MF_uint32
        {
            // Round to tolerance to merge nearly identical vertices
            auto x = std::round(vertex.x() / tolerance) * tolerance;
            auto y = std::round(vertex.y() / tolerance) * tolerance;
            auto z = std::round(vertex.z() / tolerance) * tolerance;

            auto key = std::make_tuple(x, y, z);
            auto it = vertexMap.find(key);

            if (it != vertexMap.end())
            {
                return it->second;
            }

            // Add new vertex
            auto vertexIndex = meshObject->AddVertex({x, y, z});
            vertexMap[key] = vertexIndex;
            return vertexIndex;
        };

        auto const & vertexBuffer = mesh.getVertices();
        auto const vertexData = const_cast<Buffer<cl_float4> &>(vertexBuffer).getDataCopy();

        std::size_t trianglesAdded = 0U;
        std::size_t skippedNonFinite = 0U;
        std::size_t skippedZeroArea = 0U;
        std::size_t skippedCollapsed = 0U;
        std::size_t skippedLib3mf = 0U;

        // Add all triangles
        for (size_t i = 0; i < numFaces; ++i)
        {
            // Each face has 3 vertices, stored sequentially in the buffer
            size_t vertexOffset = i * 3;

            if (vertexOffset + 2 >= vertexData.size())
            {
                throw std::runtime_error(
                  fmt::format("Invalid vertex data: face {} requires vertices at indices {}-{}, "
                              "but buffer only has {} vertices",
                              i,
                              vertexOffset,
                              vertexOffset + 2,
                              vertexData.size()));
            }

            // Extract the three vertices of this triangle
            auto const & v1 = vertexData[vertexOffset];
            auto const & v2 = vertexData[vertexOffset + 1];
            auto const & v3 = vertexData[vertexOffset + 2];

            if (!isFinite3(v1.x, v1.y, v1.z) || !isFinite3(v2.x, v2.y, v2.z) ||
                !isFinite3(v3.x, v3.y, v3.z))
            {
                ++skippedNonFinite;
                continue;
            }

            // Skip near-degenerate triangles early (helps avoid collapsing triangles after rounding).
            // Threshold is intentionally very small; it mainly filters truly pathological cases.
            constexpr double MIN_AREA2 = 1e-24;
            if (triangleArea2(v1.x, v1.y, v1.z, v2.x, v2.y, v2.z, v3.x, v3.y, v3.z) <= MIN_AREA2)
            {
                ++skippedZeroArea;
                continue;
            }

            // Convert cl_float4 to Vector3
            Vector3 vertex1(v1.x, v1.y, v1.z);
            Vector3 vertex2(v2.x, v2.y, v2.z);
            Vector3 vertex3(v3.x, v3.y, v3.z);

            // Get vertex indices (creating vertices if needed)
            auto v1Index = getOrCreateVertex(vertex1);
            auto v2Index = getOrCreateVertex(vertex2);
            auto v3Index = getOrCreateVertex(vertex3);

            // Vertex de-duplication can collapse very tiny triangles. 3MF does not allow degenerate triangles.
            if (v1Index == v2Index || v2Index == v3Index || v1Index == v3Index)
            {
                ++skippedCollapsed;
                continue;
            }

            // Ensure counter-clockwise order for outward-facing normals
            // The 3MF spec requires counter-clockwise vertex order
            try
            {
                meshObject->AddTriangle({v1Index, v2Index, v3Index});
                ++trianglesAdded;
            }
            catch (Lib3MF::ELib3MFException const &)
            {
                // Skip invalid triangles rather than failing the entire export.
                ++skippedLib3mf;
                if (skippedLib3mf > 1000U)
                {
                    throw;
                }
                continue;
            }
            catch (std::exception const &)
            {
                ++skippedLib3mf;
                if (skippedLib3mf > 1000U)
                {
                    throw;
                }
                continue;
            }
        }

        if (trianglesAdded == 0U)
        {
            throw std::runtime_error(
              "3MF export failed: all triangles were invalid/degenerate after filtering");
        }

        if (std::getenv("GLADIUS_DEBUG_3MF_TOPOLOGY") != nullptr)
        {
            // Diagnostic: Check vertex sharing ratio
            std::size_t const totalVertexRefs = trianglesAdded * 3U;
            std::size_t const uniqueVertexCount = vertexMap.size();
            std::cout << "[3MF export] Vertex deduplication: " << totalVertexRefs
                      << " vertex refs -> " << uniqueVertexCount << " unique vertices"
                      << " (sharing ratio: "
                      << (static_cast<double>(totalVertexRefs) /
                          static_cast<double>(uniqueVertexCount))
                      << ", expected ~6 for closed manifold)" << std::endl;

            // Diagnostic: Count boundary edges in the lib3mf mesh
            std::unordered_map<std::uint64_t, std::uint32_t> edgeCounts;
            auto const triCount = meshObject->GetTriangleCount();
            edgeCounts.reserve(static_cast<std::size_t>(triCount) * 3U);

            auto makeKey = [](Lib3MF_uint32 a, Lib3MF_uint32 b) -> std::uint64_t
            {
                auto lo = std::min(a, b);
                auto hi = std::max(a, b);
                return (static_cast<std::uint64_t>(lo) << 32U) | hi;
            };

            for (Lib3MF_uint32 ti = 0; ti < triCount; ++ti)
            {
                auto tri = meshObject->GetTriangle(ti);
                ++edgeCounts[makeKey(tri.m_Indices[0], tri.m_Indices[1])];
                ++edgeCounts[makeKey(tri.m_Indices[1], tri.m_Indices[2])];
                ++edgeCounts[makeKey(tri.m_Indices[2], tri.m_Indices[0])];
            }

            std::size_t boundaryEdges = 0U;
            std::size_t nonManifoldEdges = 0U;
            for (auto const & [key, count] : edgeCounts)
            {
                (void)key;
                if (count == 1U)
                {
                    ++boundaryEdges;
                }
                else if (count > 2U)
                {
                    ++nonManifoldEdges;
                }
            }

            std::cout << "[3MF export] lib3mf mesh check: " << triCount
                      << " triangles, " << edgeCounts.size()
                      << " edges, boundaryEdges=" << boundaryEdges
                      << ", nonManifoldEdges=" << nonManifoldEdges << std::endl;
        }

        if (m_logger && (skippedNonFinite + skippedZeroArea + skippedCollapsed + skippedLib3mf) > 0U)
        {
            m_logger->addEvent(
              {fmt::format(
                 "3MF export filtered triangles for '{}': added={}, skippedNonFinite={}, skippedZeroArea={}, skippedCollapsed={}, skippedLib3mf={}",
                 meshName,
                 trianglesAdded,
                 skippedNonFinite,
                 skippedZeroArea,
                 skippedCollapsed,
                 skippedLib3mf),
               events::Severity::Warning});
        }

        if (m_logger)
        {
            m_logger->addEvent({fmt::format("Added mesh '{}' with {} vertices and {} triangles",
                                            meshName,
                                            meshObject->GetVertexCount(),
                                            meshObject->GetTriangleCount()),
                                events::Severity::Info});
        }

        return meshObject;
    }

    std::pair<Lib3MF::PMeshObject, Lib3MF_uint32>
    MeshWriter3mf::addMeshWithColorsToModel(Lib3MF::PModel model3mf,
                                            Mesh const & mesh,
                                            std::string const & meshName,
                                            FaceColors const & faceColors)
    {
        auto meshObject = model3mf->AddMeshObject();
        meshObject->SetName(meshName);

        size_t const numFaces = mesh.getNumberOfFaces();
        if (numFaces == 0)
        {
            throw std::runtime_error("Mesh has no faces to export");
        }

        // Create color group for per-face colors
        auto colorGroup = model3mf->AddColorGroup();
        Lib3MF_uint32 const colorGroupId = colorGroup->GetUniqueResourceID();

        // Add a placeholder color at index 0 to avoid lib3mf treating all-zero property IDs as "no properties"
        // This ensures all actual colors get indices >= 1
        colorGroup->AddColor({0, 0, 0, 0}); // Transparent black placeholder (index 0)

        // Build a map of unique colors to their property IDs to avoid duplicates
        std::map<std::uint32_t, Lib3MF_uint32> colorToPropertyId;

        auto getOrCreateColorProperty = [&](Color8 const & color) -> Lib3MF_uint32
        {
            // Pack color into a single uint32 for map lookup
            std::uint32_t colorKey = (static_cast<std::uint32_t>(color.r) << 24) |
                                     (static_cast<std::uint32_t>(color.g) << 16) |
                                     (static_cast<std::uint32_t>(color.b) << 8) |
                                     static_cast<std::uint32_t>(color.a);

            auto it = colorToPropertyId.find(colorKey);
            if (it != colorToPropertyId.end())
            {
                return it->second;
            }

            // Add new color to the color group
            Lib3MF::sColor lib3mfColor{color.r, color.g, color.b, color.a};
            Lib3MF_uint32 const propertyId = colorGroup->AddColor(lib3mfColor);
            colorToPropertyId[colorKey] = propertyId;
            return propertyId;
        };

        // Track unique vertices to avoid duplicates
        std::map<std::tuple<float, float, float>, Lib3MF_uint32> vertexMap;
        auto const tolerance = 1e-6f;

        auto getOrCreateVertex = [&](Vector3 const & vertex) -> Lib3MF_uint32
        {
            auto x = std::round(vertex.x() / tolerance) * tolerance;
            auto y = std::round(vertex.y() / tolerance) * tolerance;
            auto z = std::round(vertex.z() / tolerance) * tolerance;

            auto key = std::make_tuple(x, y, z);
            auto it = vertexMap.find(key);

            if (it != vertexMap.end())
            {
                return it->second;
            }

            auto vertexIndex = meshObject->AddVertex({x, y, z});
            vertexMap[key] = vertexIndex;
            return vertexIndex;
        };

        auto const & vertexBuffer = mesh.getVertices();
        auto const vertexData = const_cast<Buffer<cl_float4> &>(vertexBuffer).getDataCopy();

        // Prepare triangle properties for batch assignment
        std::vector<Lib3MF::sTriangleProperties> triangleProperties;
        triangleProperties.reserve(numFaces);

        std::size_t trianglesAdded = 0U;
        std::size_t skippedNonFinite = 0U;
        std::size_t skippedZeroArea = 0U;
        std::size_t skippedCollapsed = 0U;
        std::size_t skippedLib3mf = 0U;

        // Add all triangles with color properties
        for (size_t i = 0; i < numFaces; ++i)
        {
            size_t const vertexOffset = i * 3;

            if (vertexOffset + 2 >= vertexData.size())
            {
                throw std::runtime_error(
                  fmt::format("Invalid vertex data: face {} requires vertices at indices {}-{}, "
                              "but buffer only has {} vertices",
                              i,
                              vertexOffset,
                              vertexOffset + 2,
                              vertexData.size()));
            }

            // Extract the three vertices of this triangle
            auto const & v1 = vertexData[vertexOffset];
            auto const & v2 = vertexData[vertexOffset + 1];
            auto const & v3 = vertexData[vertexOffset + 2];

            if (!isFinite3(v1.x, v1.y, v1.z) || !isFinite3(v2.x, v2.y, v2.z) ||
                !isFinite3(v3.x, v3.y, v3.z))
            {
                ++skippedNonFinite;
                continue;
            }

            constexpr double MIN_AREA2 = 1e-24;
            if (triangleArea2(v1.x, v1.y, v1.z, v2.x, v2.y, v2.z, v3.x, v3.y, v3.z) <= MIN_AREA2)
            {
                ++skippedZeroArea;
                continue;
            }

            Vector3 const vertex1(v1.x, v1.y, v1.z);
            Vector3 const vertex2(v2.x, v2.y, v2.z);
            Vector3 const vertex3(v3.x, v3.y, v3.z);

            auto const v1Index = getOrCreateVertex(vertex1);
            auto const v2Index = getOrCreateVertex(vertex2);
            auto const v3Index = getOrCreateVertex(vertex3);

            if (v1Index == v2Index || v2Index == v3Index || v1Index == v3Index)
            {
                ++skippedCollapsed;
                continue;
            }

            try
            {
                meshObject->AddTriangle({v1Index, v2Index, v3Index});
                ++trianglesAdded;
            }
            catch (Lib3MF::ELib3MFException const &)
            {
                ++skippedLib3mf;
                if (skippedLib3mf > 1000U)
                {
                    throw;
                }
                continue;
            }
            catch (std::exception const &)
            {
                ++skippedLib3mf;
                if (skippedLib3mf > 1000U)
                {
                    throw;
                }
                continue;
            }

            // Get color property ID for this face (same color for all 3 vertices = flat shading)
            Lib3MF_uint32 const colorPropertyId = getOrCreateColorProperty(faceColors[i]);

            // Set the same color for all three vertices of the triangle
            Lib3MF::sTriangleProperties props{};
            props.m_ResourceID = colorGroupId;
            props.m_PropertyIDs[0] = colorPropertyId;
            props.m_PropertyIDs[1] = colorPropertyId;
            props.m_PropertyIDs[2] = colorPropertyId;
            triangleProperties.push_back(props);
        }

                if (trianglesAdded == 0U)
                {
                        throw std::runtime_error(
                            "3MF export failed: all triangles were invalid/degenerate after filtering");
                }

                if (m_logger && (skippedNonFinite + skippedZeroArea + skippedCollapsed + skippedLib3mf) > 0U)
                {
                        m_logger->addEvent(
                            {fmt::format(
                                 "3MF export filtered colored triangles for '{}': added={}, skippedNonFinite={}, skippedZeroArea={}, skippedCollapsed={}, skippedLib3mf={}",
                                 meshName,
                                 trianglesAdded,
                                 skippedNonFinite,
                                 skippedZeroArea,
                                 skippedCollapsed,
                                 skippedLib3mf),
                             events::Severity::Warning});
                }

        // Apply all triangle properties at once
        meshObject->SetAllTriangleProperties(triangleProperties);

        if (m_logger)
        {
            m_logger->addEvent(
              {fmt::format("Added colored mesh '{}' with {} vertices, {} triangles, {} unique colors",
                           meshName,
                           meshObject->GetVertexCount(),
                           meshObject->GetTriangleCount(),
                           colorToPropertyId.size()),
               events::Severity::Info});
        }

        return {meshObject, colorGroupId};
    }

    std::pair<Lib3MF::PMeshObject, Lib3MF_uint32>
    MeshWriter3mf::addMeshWithVertexColorsToModel(Lib3MF::PModel model3mf,
                                                  Mesh const & mesh,
                                                  std::string const & meshName,
                                                  VertexColors const & vertexColors)
    {
        auto meshObject = model3mf->AddMeshObject();
        meshObject->SetName(meshName);

        size_t const numFaces = mesh.getNumberOfFaces();
        if (numFaces == 0)
        {
            throw std::runtime_error("Mesh has no faces to export");
        }

        // Create color group for per-vertex colors
        auto colorGroup = model3mf->AddColorGroup();
        Lib3MF_uint32 const colorGroupId = colorGroup->GetUniqueResourceID();

        // Add a placeholder color at index 0 to avoid lib3mf treating all-zero property IDs as "no properties"
        // This ensures all actual colors get indices >= 1
        colorGroup->AddColor({0, 0, 0, 0}); // Transparent black placeholder (index 0)

        // Build a map of unique colors to their property IDs to avoid duplicates
        std::map<std::uint32_t, Lib3MF_uint32> colorToPropertyId;

        auto getOrCreateColorProperty = [&](Color8 const & color) -> Lib3MF_uint32
        {
            // Pack color into a single uint32 for map lookup
            std::uint32_t colorKey = (static_cast<std::uint32_t>(color.r) << 24) |
                                     (static_cast<std::uint32_t>(color.g) << 16) |
                                     (static_cast<std::uint32_t>(color.b) << 8) |
                                     static_cast<std::uint32_t>(color.a);

            auto it = colorToPropertyId.find(colorKey);
            if (it != colorToPropertyId.end())
            {
                return it->second;
            }

            // Add new color to the color group
            Lib3MF::sColor lib3mfColor{color.r, color.g, color.b, color.a};
            Lib3MF_uint32 const propertyId = colorGroup->AddColor(lib3mfColor);
            colorToPropertyId[colorKey] = propertyId;
            return propertyId;
        };

        // Track unique vertices to avoid duplicates
        std::map<std::tuple<float, float, float>, Lib3MF_uint32> vertexMap;
        auto const tolerance = 1e-6f;

        auto getOrCreateVertex = [&](Vector3 const & vertex) -> Lib3MF_uint32
        {
            auto x = std::round(vertex.x() / tolerance) * tolerance;
            auto y = std::round(vertex.y() / tolerance) * tolerance;
            auto z = std::round(vertex.z() / tolerance) * tolerance;

            auto key = std::make_tuple(x, y, z);
            auto it = vertexMap.find(key);

            if (it != vertexMap.end())
            {
                return it->second;
            }

            auto vertexIndex = meshObject->AddVertex({x, y, z});
            vertexMap[key] = vertexIndex;
            return vertexIndex;
        };

        auto const & vertexBuffer = mesh.getVertices();
        auto const vertexData = const_cast<Buffer<cl_float4> &>(vertexBuffer).getDataCopy();

        // Prepare triangle properties for batch assignment
        std::vector<Lib3MF::sTriangleProperties> triangleProperties;
        triangleProperties.reserve(numFaces);

        std::size_t trianglesAdded = 0U;
        std::size_t skippedNonFinite = 0U;
        std::size_t skippedZeroArea = 0U;
        std::size_t skippedCollapsed = 0U;
        std::size_t skippedLib3mf = 0U;

        // Add all triangles with per-vertex color properties
        for (size_t i = 0; i < numFaces; ++i)
        {
            size_t const vertexOffset = i * 3;

            if (vertexOffset + 2 >= vertexData.size())
            {
                throw std::runtime_error(
                  fmt::format("Invalid vertex data: face {} requires vertices at indices {}-{}, "
                              "but buffer only has {} vertices",
                              i,
                              vertexOffset,
                              vertexOffset + 2,
                              vertexData.size()));
            }

            // Extract the three vertices of this triangle
            auto const & v1 = vertexData[vertexOffset];
            auto const & v2 = vertexData[vertexOffset + 1];
            auto const & v3 = vertexData[vertexOffset + 2];

            if (!isFinite3(v1.x, v1.y, v1.z) || !isFinite3(v2.x, v2.y, v2.z) ||
                !isFinite3(v3.x, v3.y, v3.z))
            {
                ++skippedNonFinite;
                continue;
            }

            constexpr double MIN_AREA2 = 1e-24;
            if (triangleArea2(v1.x, v1.y, v1.z, v2.x, v2.y, v2.z, v3.x, v3.y, v3.z) <= MIN_AREA2)
            {
                ++skippedZeroArea;
                continue;
            }

            Vector3 const vertex1(v1.x, v1.y, v1.z);
            Vector3 const vertex2(v2.x, v2.y, v2.z);
            Vector3 const vertex3(v3.x, v3.y, v3.z);

            auto const v1Index = getOrCreateVertex(vertex1);
            auto const v2Index = getOrCreateVertex(vertex2);
            auto const v3Index = getOrCreateVertex(vertex3);

            if (v1Index == v2Index || v2Index == v3Index || v1Index == v3Index)
            {
                ++skippedCollapsed;
                continue;
            }

            try
            {
                meshObject->AddTriangle({v1Index, v2Index, v3Index});
                ++trianglesAdded;
            }
            catch (Lib3MF::ELib3MFException const &)
            {
                ++skippedLib3mf;
                if (skippedLib3mf > 1000U)
                {
                    throw;
                }
                continue;
            }
            catch (std::exception const &)
            {
                ++skippedLib3mf;
                if (skippedLib3mf > 1000U)
                {
                    throw;
                }
                continue;
            }

            // Get color property IDs for each vertex of this face
            auto const & faceVertexColors = vertexColors[i];
            Lib3MF_uint32 const colorProp0 = getOrCreateColorProperty(faceVertexColors[0]);
            Lib3MF_uint32 const colorProp1 = getOrCreateColorProperty(faceVertexColors[1]);
            Lib3MF_uint32 const colorProp2 = getOrCreateColorProperty(faceVertexColors[2]);

            // Set different colors for each vertex of the triangle
            Lib3MF::sTriangleProperties props{};
            props.m_ResourceID = colorGroupId;
            props.m_PropertyIDs[0] = colorProp0;
            props.m_PropertyIDs[1] = colorProp1;
            props.m_PropertyIDs[2] = colorProp2;
            triangleProperties.push_back(props);
        }

                if (trianglesAdded == 0U)
                {
                        throw std::runtime_error(
                            "3MF export failed: all triangles were invalid/degenerate after filtering");
                }

                if (m_logger && (skippedNonFinite + skippedZeroArea + skippedCollapsed + skippedLib3mf) > 0U)
                {
                        m_logger->addEvent(
                            {fmt::format(
                                 "3MF export filtered vertex-colored triangles for '{}': added={}, skippedNonFinite={}, skippedZeroArea={}, skippedCollapsed={}, skippedLib3mf={}",
                                 meshName,
                                 trianglesAdded,
                                 skippedNonFinite,
                                 skippedZeroArea,
                                 skippedCollapsed,
                                 skippedLib3mf),
                             events::Severity::Warning});
                }

        // Apply all triangle properties at once
        meshObject->SetAllTriangleProperties(triangleProperties);

        if (m_logger)
        {
            m_logger->addEvent(
              {fmt::format("Added vertex-colored mesh '{}' with {} vertices, {} triangles, {} unique colors",
                           meshName,
                           meshObject->GetVertexCount(),
                           meshObject->GetTriangleCount(),
                           colorToPropertyId.size()),
               events::Severity::Info});
        }

        return {meshObject, colorGroupId};
    }

    void MeshWriter3mf::createBuildItem(Lib3MF::PModel model3mf,
                                        Lib3MF::PMeshObject meshObject,
                                        std::string const & partNumber)
    {
        try
        {
            // Create identity transform (no scaling/rotation/translation)
            Lib3MF::sTransform transform = m_wrapper->GetIdentityTransform();

            auto buildItem = model3mf->AddBuildItem(meshObject.get(), transform);

            if (!partNumber.empty())
            {
                buildItem->SetPartNumber(partNumber);
            }

            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Created build item for mesh object (part: {})",
                                                partNumber.empty() ? "unnamed" : partNumber),
                                    events::Severity::Info});
            }
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Failed to create build item: {}", e.what()),
                                    events::Severity::Warning});
            }
            throw;
        }
    }

    bool MeshWriter3mf::validateMesh(Mesh const & mesh)
    {
        if (mesh.getNumberOfFaces() == 0)
        {
            if (m_logger)
            {
                m_logger->addEvent(
                  {"Mesh validation failed: No faces in mesh", events::Severity::Error});
            }
            return false;
        }

        if (mesh.getNumberOfFaces() < 4)
        {
            if (m_logger)
            {
                m_logger->addEvent(
                  {"Mesh validation warning: Mesh has fewer than 4 faces (may not form a solid)",
                   events::Severity::Warning});
            }
        }

        // Additional validation could be added here:
        // - Check for degenerate triangles
        // - Verify manifold topology
        // - Check for self-intersections
        // For now, we'll allow any mesh with at least one face

        return true;
    }

    // Convenience function implementation
    // Convenience function implementation
    void exportMeshTo3mfCore(std::filesystem::path const & filePath,
                             Mesh const & mesh,
                             std::string const & meshName,
                             Document const * sourceDocument,
                             events::SharedLogger logger)
    {
        MeshWriter3mf writer(logger);
        writer.exportMesh(filePath, mesh, meshName, sourceDocument, false);
    }

} // namespace gladius::io
