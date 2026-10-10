#include "io/3mf/IndexedTriangleMesh3mfWriter.h"

#include <lib3mf_abi.hpp>

#include <chrono>
#include <filesystem>
#include <system_error>
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
} // namespace gladius::io
