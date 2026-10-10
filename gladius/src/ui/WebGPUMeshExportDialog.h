#pragma once

#include "FileDialogService.h"

#include <filesystem>
#include <future>
#include <memory>
#include <string>

namespace gladius
{
    class Document;
}

namespace gladius::ui
{
    class ExportState;

    /// @brief Minimal backend-neutral UI for the currently supported WebGPU STL export path.
    class WebGPUMeshExportDialog final
    {
      public:
        ~WebGPUMeshExportDialog();

        void setExportState(ExportState * exportState) noexcept;
        void show(std::filesystem::path suggestedFilename);
        void render(std::shared_ptr<Document> const & document);

        [[nodiscard]] bool isBusy() const;
        [[nodiscard]] bool isVisible() const noexcept;

      private:
        void startExport(std::filesystem::path filename,
                         std::shared_ptr<Document> document,
                         bool exportThreeMf);
        void pollExport();

        AsyncFileDialog m_fileDialog;
        std::future<void> m_exportFuture;
        std::filesystem::path m_suggestedFilename;
        std::filesystem::path m_targetFilename;
        std::string m_statusMessage;
        std::string m_errorMessage;
        ExportState * m_exportState{nullptr};
        int m_formatIndex{0};
        int m_qualityIndex{1};
        bool m_pendingThreeMf{false};
        bool m_visible{false};
        bool m_exportInProgress{false};
    };
} // namespace gladius::ui
