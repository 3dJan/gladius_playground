#include "io/3mf/IndexedTriangleMesh3mfWriter.h"
#include "io/3mf/MmuSegmentationWriter.h"
#include "io/3mf/SlicerConfigWriter.h"
#include "io/3mf/ThreeMfPostProcessor.h"

#include <lib3mf_abi.hpp>

#include <chrono>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace gladius::io
{
    namespace
    {
        void verifyFileWritten(std::filesystem::path const & filePath)
        {
            std::error_code error;
            if (!std::filesystem::exists(filePath, error) || error ||
                std::filesystem::file_size(filePath, error) == 0U || error)
            {
                throw std::runtime_error("3MF export did not produce a non-empty file: " +
                                         filePath.string());
            }
        }

        [[nodiscard]] std::filesystem::path makeTemporaryPath(
          std::filesystem::path const & filePath)
        {
            auto temporaryPath = filePath;
            temporaryPath += ".tmp." + std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count());
            return temporaryPath;
        }

        void writeModelToFile(Lib3MF::PModel const & model,
                              std::filesystem::path const & filePath)
        {
            auto const writer = model->QueryWriter("3mf");
            writer->WriteToFile(filePath.string());
            verifyFileWritten(filePath);
        }

        void publishTemporaryFile(std::filesystem::path const & temporaryPath,
                                  std::filesystem::path const & filePath)
        {
            std::error_code error;
            std::filesystem::rename(temporaryPath, filePath, error);
            if (error)
            {
                throw std::runtime_error("3MF export could not finalize the output file: " +
                                         error.message());
            }
        }

        void removeTemporaryFile(std::filesystem::path const & temporaryPath) noexcept
        {
            std::error_code error;
            std::filesystem::remove(temporaryPath, error);
        }
    } // namespace

    IndexedTriangleMesh3mfWriter::IndexedTriangleMesh3mfWriter(events::SharedLogger logger)
        : Writer3mfBase(std::move(logger))
    {
    }

    void IndexedTriangleMesh3mfWriter::exportMesh(
        std::filesystem::path const & filePath,
        IndexedTriangleMesh const & mesh,
        std::string const & meshName,
        IndexedTriangleMeshValidationPolicy policy)
    {
        if (filePath.empty())
        {
            throw std::invalid_argument("3MF export target path must not be empty");
        }
        if (auto const error = mesh.closedOrientedTopologyError(policy))
        {
            throw std::invalid_argument("3MF export requires a closed, consistently oriented mesh: " +
                                        *error);
        }

        auto temporaryPath = filePath;
        temporaryPath += ".tmp." + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());

        try
        {
            auto const model = m_wrapper->CreateModel();
            addDefaultMetadata(model);

            auto const meshObject = model->AddMeshObject();
            meshObject->SetName(meshName.empty() ? "Gladius Mesh" : meshName);
            for (auto const & position : mesh.positions)
            {
                meshObject->AddVertex({position.x(), position.y(), position.z()});
            }
            for (std::size_t offset = 0U; offset < mesh.indices.size(); offset += 3U)
            {
                meshObject->AddTriangle({mesh.indices[offset],
                                         mesh.indices[offset + 1U],
                                         mesh.indices[offset + 2U]});
            }

            auto const transform = m_wrapper->GetIdentityTransform();
            auto const buildItem = model->AddBuildItem(meshObject.get(), transform);
            buildItem->SetPartNumber(meshName.empty() ? "Gladius Mesh" : meshName);

            auto const writer = model->QueryWriter("3mf");
            writer->WriteToFile(temporaryPath.string());
            verifyFileWritten(temporaryPath);

            std::error_code error;
            std::filesystem::rename(temporaryPath, filePath, error);
            if (error)
            {
                throw std::runtime_error("3MF export could not finalize the output file: " +
                                         error.message());
            }

            if (m_logger)
            {
                m_logger->addEvent({"Successfully exported geometry-only 3MF to " +
                                      filePath.string(),
                                    events::Severity::Info});
            }
        }
        catch (std::exception const & error)
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
            if (m_logger)
            {
                m_logger->addEvent({"Failed to export 3MF mesh to " + filePath.string() + ": " +
                                      error.what(),
                                    events::Severity::Error});
            }
            throw;
        }
    }

    void IndexedTriangleMesh3mfWriter::exportMeshWithColors(
      std::filesystem::path const & filePath,
      IndexedTriangleMesh const & mesh,
      std::string const & meshName,
      FaceColors const & faceColors)
    {
        if (filePath.empty())
        {
            throw std::invalid_argument("3MF export target path must not be empty");
        }
        if (auto const error = mesh.closedOrientedTopologyError())
        {
            throw std::invalid_argument("3MF export requires a closed, consistently oriented mesh: " +
                                        *error);
        }
        auto const triangleCount = mesh.indices.size() / 3u;
        if (faceColors.size() != triangleCount)
        {
            throw std::invalid_argument("3MF face-color count does not match the mesh triangle count");
        }

        auto const temporaryPath = makeTemporaryPath(filePath);
        try
        {
            auto const model = m_wrapper->CreateModel();
            addDefaultMetadata(model);

            auto const meshObject = model->AddMeshObject();
            meshObject->SetName(meshName.empty() ? "Gladius Mesh" : meshName);
            for (auto const & position : mesh.positions)
            {
                meshObject->AddVertex({position.x(), position.y(), position.z()});
            }

            auto const colorGroup = model->AddColorGroup();
            auto const colorGroupId = colorGroup->GetUniqueResourceID();
            colorGroup->AddColor({0u, 0u, 0u, 0u});
            std::unordered_map<std::uint32_t, Lib3MF_uint32> colorProperties;
            colorProperties.reserve(faceColors.size());
            auto getColorProperty = [&colorGroup, &colorProperties](Color8 const & color)
            {
                std::uint32_t const key = (static_cast<std::uint32_t>(color.r) << 24u) |
                                          (static_cast<std::uint32_t>(color.g) << 16u) |
                                          (static_cast<std::uint32_t>(color.b) << 8u) |
                                          static_cast<std::uint32_t>(color.a);
                auto const found = colorProperties.find(key);
                if (found != colorProperties.end())
                {
                    return found->second;
                }
                Lib3MF_uint32 const propertyId = colorGroup->AddColor(
                  {color.r, color.g, color.b, color.a});
                colorProperties.emplace(key, propertyId);
                return propertyId;
            };

            std::vector<Lib3MF::sTriangleProperties> triangleProperties;
            triangleProperties.reserve(triangleCount);
            for (std::size_t faceIndex = 0u; faceIndex < triangleCount; ++faceIndex)
            {
                std::size_t const offset = faceIndex * 3u;
                meshObject->AddTriangle({mesh.indices[offset],
                                         mesh.indices[offset + 1u],
                                         mesh.indices[offset + 2u]});
                Lib3MF_uint32 const propertyId = getColorProperty(faceColors[faceIndex]);
                Lib3MF::sTriangleProperties properties{};
                properties.m_ResourceID = colorGroupId;
                properties.m_PropertyIDs[0] = propertyId;
                properties.m_PropertyIDs[1] = propertyId;
                properties.m_PropertyIDs[2] = propertyId;
                triangleProperties.push_back(properties);
            }
            meshObject->SetAllTriangleProperties(triangleProperties);

            auto const buildItem = model->AddBuildItem(meshObject.get(), m_wrapper->GetIdentityTransform());
            buildItem->SetPartNumber(meshName.empty() ? "Gladius Mesh" : meshName);
            writeModelToFile(model, temporaryPath);
            publishTemporaryFile(temporaryPath, filePath);
        }
        catch (...)
        {
            removeTemporaryFile(temporaryPath);
            throw;
        }
    }

    void IndexedTriangleMesh3mfWriter::exportMeshWithRegions(
      std::filesystem::path const & filePath,
      IndexedTriangleMesh const & mesh,
      std::string const & meshName,
      QuantizedPalette const & palette,
      std::vector<PrintableRegion> const & regions)
    {
        if (filePath.empty())
        {
            throw std::invalid_argument("3MF export target path must not be empty");
        }
        if (auto const error = mesh.validationError())
        {
            throw std::invalid_argument("Cannot export invalid indexed mesh: " + *error);
        }
        if (palette.colors.empty() || regions.empty())
        {
            throw std::invalid_argument("3MF region export requires a palette and printable regions");
        }

        auto const temporaryPath = makeTemporaryPath(filePath);
        try
        {
            auto const model = m_wrapper->CreateModel();
            addDefaultMetadata(model);
            auto const materials = model->AddBaseMaterialGroup();
            auto const materialGroupId = materials->GetUniqueResourceID();
            std::vector<Lib3MF_uint32> materialIds;
            materialIds.reserve(palette.colors.size());
            for (std::size_t index = 0u; index < palette.colors.size(); ++index)
            {
                auto const & color = palette.colors[index];
                materialIds.push_back(materials->AddMaterial(
                  "Color_" + std::to_string(index), {color.r, color.g, color.b, color.a}));
            }

            std::size_t exportedRegions = 0u;
            auto const triangleCount = mesh.indices.size() / 3u;
            for (auto const & region : regions)
            {
                if (region.triangleIndices.empty())
                {
                    continue;
                }
                if (region.paletteIndex >= materialIds.size())
                {
                    throw std::invalid_argument("3MF region references a missing palette color");
                }

                auto const regionName = meshName + "_" + std::to_string(region.regionId);
                auto const meshObject = model->AddMeshObject();
                meshObject->SetName(regionName);
                meshObject->SetObjectLevelProperty(materialGroupId, materialIds[region.paletteIndex]);
                std::unordered_map<std::uint32_t, Lib3MF_uint32> vertexMap;
                vertexMap.reserve(region.triangleIndices.size());
                auto getVertex = [&mesh, &meshObject, &vertexMap](std::uint32_t const index)
                {
                    auto const found = vertexMap.find(index);
                    if (found != vertexMap.end())
                    {
                        return found->second;
                    }
                    auto const & position = mesh.positions[index];
                    Lib3MF_uint32 const vertexId = meshObject->AddVertex(
                      {position.x(), position.y(), position.z()});
                    vertexMap.emplace(index, vertexId);
                    return vertexId;
                };
                std::vector<Lib3MF::sTriangleProperties> triangleProperties;
                triangleProperties.reserve(region.triangleIndices.size());
                for (auto const faceIndex : region.triangleIndices)
                {
                    if (faceIndex >= triangleCount)
                    {
                        throw std::invalid_argument("3MF region references a missing mesh triangle");
                    }
                    std::size_t const offset = static_cast<std::size_t>(faceIndex) * 3u;
                    auto const first = getVertex(mesh.indices[offset]);
                    auto const second = getVertex(mesh.indices[offset + 1u]);
                    auto const third = getVertex(mesh.indices[offset + 2u]);
                    meshObject->AddTriangle({first, second, third});
                    Lib3MF::sTriangleProperties properties{};
                    properties.m_ResourceID = materialGroupId;
                    properties.m_PropertyIDs[0] = materialIds[region.paletteIndex];
                    properties.m_PropertyIDs[1] = materialIds[region.paletteIndex];
                    properties.m_PropertyIDs[2] = materialIds[region.paletteIndex];
                    triangleProperties.push_back(properties);
                }
                meshObject->SetAllTriangleProperties(triangleProperties);
                auto const buildItem = model->AddBuildItem(
                  meshObject.get(), m_wrapper->GetIdentityTransform());
                buildItem->SetPartNumber(regionName);
                ++exportedRegions;
            }
            if (exportedRegions == 0u)
            {
                throw std::runtime_error("3MF export produced no printable regions");
            }

            writeModelToFile(model, temporaryPath);
            publishTemporaryFile(temporaryPath, filePath);
        }
        catch (...)
        {
            removeTemporaryFile(temporaryPath);
            throw;
        }
    }

    void IndexedTriangleMesh3mfWriter::exportMeshWithMmuSegmentation(
      std::filesystem::path const & filePath,
      IndexedTriangleMesh const & mesh,
      std::string const & meshName,
      QuantizedPalette const & palette)
    {
        if (filePath.empty())
        {
            throw std::invalid_argument("3MF export target path must not be empty");
        }
        if (auto const error = mesh.closedOrientedTopologyError())
        {
            throw std::invalid_argument("3MF export requires a closed, consistently oriented mesh: " +
                                        *error);
        }
        auto const triangleCount = mesh.indices.size() / 3u;
        if (palette.sourceToPaletteMap.size() != triangleCount)
        {
            throw std::invalid_argument("3MF MMU palette mapping does not match the mesh triangle count");
        }

        auto const temporaryPath = makeTemporaryPath(filePath);
        try
        {
            auto const model = m_wrapper->CreateModel();
            addDefaultMetadata(model);
            auto const meshObject = model->AddMeshObject();
            auto const objectName = meshName.empty() ? "Gladius Mesh" : meshName;
            meshObject->SetName(objectName);
            for (auto const & position : mesh.positions)
            {
                meshObject->AddVertex({position.x(), position.y(), position.z()});
            }
            for (std::size_t offset = 0u; offset < mesh.indices.size(); offset += 3u)
            {
                meshObject->AddTriangle({mesh.indices[offset],
                                         mesh.indices[offset + 1u],
                                         mesh.indices[offset + 2u]});
            }
            auto const objectId = meshObject->GetUniqueResourceID();
            auto const buildItem = model->AddBuildItem(meshObject.get(), m_wrapper->GetIdentityTransform());
            buildItem->SetPartNumber(objectName);

            writeModelToFile(model, temporaryPath);
            auto const mmuAttributes = MmuSegmentationWriter::encodeFaceExtruders(palette);
            SlicerVolumeInfo volume;
            volume.name = objectName;
            volume.lastTriangleId = triangleCount == 0u ? 0u : triangleCount - 1u;
            volume.defaultExtruder = 1;
            auto const config = SlicerConfigWriter::generate(static_cast<int>(objectId), volume);
            ThreeMfPostProcessor::injectMmuSegmentation(temporaryPath, mmuAttributes, config);
            verifyFileWritten(temporaryPath);
            publishTemporaryFile(temporaryPath, filePath);
        }
        catch (...)
        {
            removeTemporaryFile(temporaryPath);
            throw;
        }
    }
} // namespace gladius::io
