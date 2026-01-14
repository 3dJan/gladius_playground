#include "MeshExportDialog3mf.h"
#include "../Document.h"

namespace gladius::ui
{
    void MeshExportDialog3mf::beginExport(std::filesystem::path const & threeMfFilename,
                                          ComputeCore & core)
    {
        m_visible = true;
        m_exporter.setQualityLevel(1);
        m_exporter.beginExport(threeMfFilename, core);
        
        // Lock UI modifications during export
        if (m_exportState != nullptr)
        {
            m_exportState->beginExport("3MF mesh export");
        }
    }

    void MeshExportDialog3mf::beginExport(std::filesystem::path const & threeMfFilename,
                                          ComputeCore & core,
                                          Document const * document)
    {
        m_visible = true;
        m_exporter.setQualityLevel(1);
        m_exporter.beginExport(threeMfFilename, core, document);
        
        // Lock UI modifications during export
        if (m_exportState != nullptr)
        {
            m_exportState->beginExport("3MF mesh export");
        }
    }

    std::string MeshExportDialog3mf::getWindowTitle() const
    {
        return "Export in progress";
    }

    std::string MeshExportDialog3mf::getExportMessage() const
    {
        return "Exporting to 3MF file";
    }

    io::IExporter & MeshExportDialog3mf::getExporter()
    {
        return m_exporter;
    }

    void MeshExportDialog3mf::onExportCompleted()
    {
        // Unlock UI modifications
        if (m_exportState != nullptr)
        {
            m_exportState->endExport();
        }
    }

    void MeshExportDialog3mf::onExportCancelled()
    {
        // Unlock UI modifications
        if (m_exportState != nullptr)
        {
            m_exportState->endExport();
        }
    }
}
