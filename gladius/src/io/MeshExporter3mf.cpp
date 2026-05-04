#include "MeshExporter3mf.h"

#include "../Document.h"
#include "../compute/ComputeCore.h"
#include "3mf/MeshColorExportPipeline.h"
#include "3mf/MeshSamplingGeometry.h"
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

            std::string meshName = "Mesh";

            auto samplingGeometry = io::MeshSamplingGeometry::fromTriangleSoupMesh(mesh);
            m_exportResult = io::exportMeshWithColorPipeline(m_fileName,
                                                             mesh,
                                                             meshName,
                                                             samplingGeometry,
                                                             *m_computeCore,
                                                             settings,
                                                             m_sourceDocument,
                                                             true,
                                                             m_logger);
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
