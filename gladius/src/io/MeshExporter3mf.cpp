#include "MeshExporter3mf.h"

#include "../Document.h"
#include "../compute/ComputeCore.h"
#include "../compute/ProgramManager.h"
#include "3mf/FaceColorSampler.h"
#include "3mf/MeshWriter3mf.h"
#include "MeshExporter.h"
#include "vdb.h"

#include <fmt/format.h>

namespace gladius::vdb
{
    MeshExporter3mf::MeshExporter3mf(events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
    }
    
    void MeshExporter3mf::setExportWithColors(bool exportWithColors)
    {
        m_exportWithColors = exportWithColors;
    }
    
    void MeshExporter3mf::setConvertToSrgb(bool convertToSrgb)
    {
        m_convertToSrgb = convertToSrgb;
    }

    void MeshExporter3mf::setColorMode(ColorMode mode)
    {
        m_colorMode = mode;
    }

    void MeshExporter3mf::beginExport(std::filesystem::path const & fileName,
                                      ComputeCore & generator)
    {
        m_computeCore = &generator;
        m_sourceDocument = nullptr;
        LayerBasedMeshExporter::beginExport(fileName, generator);
    }

    void MeshExporter3mf::beginExport(std::filesystem::path const & fileName,
                                      ComputeCore & generator,
                                      Document const * document)
    {
        m_computeCore = &generator;
        m_sourceDocument = document;
        LayerBasedMeshExporter::beginExport(fileName, generator);
    }

    void MeshExporter3mf::finalize()
    {
        if (!m_computeCore || !m_grid)
        {
            return;
        }

        try
        {
            // Convert grid to mesh using the existing function
            auto mesh = gridToMesh(m_grid, *m_computeCore->getComputeContext());

            // Export the mesh using MeshWriter3mf
            gladius::io::MeshWriter3mf writer(m_logger);
            std::string meshName = "Mesh";
            
            if (m_exportWithColors)
            {
                // Extract vertices and faces in the format required by FaceColorSampler
                std::size_t const numFaces = mesh.getNumberOfFaces();
                std::vector<Eigen::Vector3f> vertices;
                vertices.reserve(numFaces * 3);
                std::vector<std::array<std::uint32_t, 3>> faces;
                faces.reserve(numFaces);
                
                auto const& vertexBuffer = mesh.getVertices().getData();
                for (std::size_t i = 0; i < numFaces; ++i)
                {
                    std::uint32_t const baseIdx = static_cast<std::uint32_t>(vertices.size());
                    for (int v = 0; v < 3; ++v)
                    {
                        auto const& vert = vertexBuffer[i * 3 + v];
                        vertices.emplace_back(vert.x, vert.y, vert.z);
                    }
                    faces.push_back({baseIdx, baseIdx + 1, baseIdx + 2});
                }
                
                // Sample colors using GPU
                auto* samplingProgram = m_computeCore->getProgramManager().getDualContouringSamplingProgram();
                auto primitives = m_computeCore->getPrimitives();
                
                if (samplingProgram != nullptr && primitives != nullptr)
                {
                    if (m_colorMode == ColorMode::PerVertex)
                    {
                        auto vertexColors = io::FaceColorSampler::sampleVertexColors(
                            vertices, faces, *samplingProgram, *primitives, nullptr, m_convertToSrgb);
                        
                        writer.exportMeshWithVertexColors(m_fileName, mesh, meshName, vertexColors, m_sourceDocument, true);
                        
                        if (m_logger)
                        {
                            m_logger->addEvent(
                              {fmt::format("Successfully exported 3MF mesh with per-vertex colors to {}", 
                                           m_fileName.string()),
                               events::Severity::Info});
                        }
                    }
                    else
                    {
                        auto faceColors = io::FaceColorSampler::sampleFaceColorsAsColor8(
                            vertices, faces, *samplingProgram, *primitives, nullptr, m_convertToSrgb);
                        
                        writer.exportMeshWithColors(m_fileName, mesh, meshName, faceColors, m_sourceDocument, true);
                        
                        if (m_logger)
                        {
                            m_logger->addEvent(
                              {fmt::format("Successfully exported 3MF mesh with per-face colors to {}", 
                                           m_fileName.string()),
                               events::Severity::Info});
                        }
                    }
                }
                else
                {
                    // Fallback to non-colored export if sampling is unavailable
                    if (m_logger)
                    {
                        m_logger->addEvent(
                          {"Color sampling unavailable, exporting without colors",
                           events::Severity::Warning});
                    }
                    writer.exportMesh(m_fileName, mesh, meshName, m_sourceDocument, true);
                }
            }
            else
            {
                writer.exportMesh(m_fileName, mesh, meshName, m_sourceDocument, true);
                
                if (m_logger)
                {
                    m_logger->addEvent(
                      {fmt::format("Successfully exported 3MF mesh to {}", m_fileName.string()),
                       events::Severity::Info});
                }
            }
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Failed to export 3MF mesh: {}", e.what()),
                                    events::Severity::Error});
            }
            throw;
        }

        m_grid.reset();
    }

} // namespace gladius::vdb
