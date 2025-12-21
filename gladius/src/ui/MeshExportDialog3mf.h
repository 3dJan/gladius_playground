#pragma once
#include "../io/MeshExporter3mf.h"
#include "BaseExportDialog.h"
#include "ExportState.h"

#include <filesystem>

// Forward declaration
namespace gladius
{
    class Document;
}

namespace gladius::ui
{
    class MeshExportDialog3mf : public BaseExportDialog
    {
      public:
        MeshExportDialog3mf()
            : m_exporter(nullptr)
        {
        }

        /// @brief Set the export state reference for blocking UI during export
        void setExportState(ExportState * state)
        {
            m_exportState = state;
        }

        void beginExport(std::filesystem::path const & threeMfFilename,
                         ComputeCore & core) override;

        void beginExport(std::filesystem::path const & threeMfFilename,
                         ComputeCore & core,
                         Document const * document);

      protected:
        [[nodiscard]] std::string getWindowTitle() const override;
        [[nodiscard]] std::string getExportMessage() const override;
        [[nodiscard]] io::IExporter & getExporter() override;
        void onExportCompleted() override;
        void onExportCancelled() override;

      private:
        vdb::MeshExporter3mf m_exporter;
        ExportState * m_exportState = nullptr;
    };
} // namespace gladius::ui
