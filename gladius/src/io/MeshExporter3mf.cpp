#include "MeshExporter3mf.h"

#include "../Document.h"
#include "../compute/ComputeCore.h"
#include "../compute/ProgramManager.h"
#include "3mf/ColorCompatibilityPlanner.h"
#include "3mf/ColorExportDispatcher.h"
#include "3mf/ColorQuantizer.h"
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

    void MeshExporter3mf::setQuantizationMode(io::QuantizationMode mode)
    {
        m_quantizationMode = mode;
    }

    void MeshExporter3mf::setMaxPaletteSize(std::optional<std::uint32_t> maxPaletteSize)
    {
        m_maxPaletteSize = maxPaletteSize;
    }

    void MeshExporter3mf::setTargetApplication(io::TargetApplication targetApplication)
    {
        m_targetApplication = targetApplication;
    }

    ColoredMeshExportResult const& MeshExporter3mf::getExportResult() const
    {
        return m_exportResult;
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

        // Reset export result
        m_exportResult = ColoredMeshExportResult{};

        try
        {
            // Convert grid to mesh using the existing function
            auto mesh = gridToMesh(m_grid, *m_computeCore->getComputeContext());

            // Build immutable settings snapshot
            io::MeshColorExportSettings const settings{
                m_exportWithColors,
                m_convertToSrgb,
                m_colorMode,
                m_quantizationMode,
                m_targetApplication,
                m_maxPaletteSize,
            };

            // Export the mesh using MeshWriter3mf
            gladius::io::MeshWriter3mf writer(m_logger);
            std::string meshName = "Mesh";
            
            if (settings.exportWithColors)
            {
                // Extract vertices and faces for color sampling
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
                    // Always sample face colors for compatibility planning
                    auto faceColors = io::FaceColorSampler::sampleFaceColorsAsColor8(
                        vertices, faces, *samplingProgram, *primitives, nullptr, settings.convertToSrgb);

                    // Run compatibility planner
                    auto const uniqueColors = io::ColorQuantizer::countUniqueOpaqueColors(faceColors);
                    bool const hasTransparency = io::ColorQuantizer::hasTransparency(faceColors);

                    auto const decision = io::ColorCompatibilityPlanner::decide(settings, uniqueColors, hasTransparency);

                    // Record export result
                    m_exportResult.representation = decision.finalRepresentation;
                    m_exportResult.standardsOnly = !decision.needsProprietaryTags;
                    m_exportResult.transparencyIgnored = hasTransparency;
                    m_exportResult.warnings = decision.warnings;

                    // Dispatch based on decision
                    auto vertexColorSupplier = [&]() -> io::VertexColors
                    {
                        return io::FaceColorSampler::sampleVertexColors(
                            vertices, faces, *samplingProgram, *primitives,
                            nullptr, settings.convertToSrgb);
                    };
                    auto multipointSupplier = [&]() -> std::vector<io::FaceColors>
                    {
                        return io::FaceColorSampler::sampleFaceColorsMultipoint(
                            vertices, faces, *samplingProgram, *primitives,
                            nullptr, settings.convertToSrgb);
                    };

                    io::dispatchColorExport(
                        writer, m_fileName, mesh, meshName,
                        faceColors, decision, settings, uniqueColors,
                        m_sourceDocument, true,
                        vertexColorSupplier, multipointSupplier);
                    
                    if (m_logger)
                    {
                        // Log warnings from the planner
                        for (auto const& warning : decision.warnings)
                        {
                            m_logger->addEvent({warning, events::Severity::Warning});
                        }

                        m_logger->addEvent(
                          {fmt::format("Successfully exported 3MF mesh with colors to {}", 
                                       m_fileName.string()),
                           events::Severity::Info});
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
