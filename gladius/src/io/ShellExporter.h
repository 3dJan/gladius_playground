#pragma once

#include "IExporter.h"
#include "3mf/ShellExportOptions.h"
#include "../EventLogger.h"

#include <lib3mf_abi.hpp>

#include <atomic>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace gladius
{
    class ComputeCore;
    class Document;
}

namespace gladius::io
{
    /// @brief Async exporter for shell-based color 3MF export
    ///
    /// Implements IExporter interface for background shell generation with
    /// per-shell progress reporting and cooperative cancellation.
    /// Follows the same pattern as ManifoldDualContouringStlExporter.
    class ShellExporter : public IExporter
    {
      public:
        ShellExporter();
        explicit ShellExporter(events::SharedLogger logger);

        /// @brief Configure export parameters
        void setConfig(ShellExportConfig config);

        /// @brief Set the document for metadata and thumbnail
        void setDocument(Document const* doc);

        /// @brief Set pre-rendered PNG bytes for the exported 3MF package thumbnail.
        void setThumbnailPng(std::vector<Lib3MF_uint8> thumbnailPng);

        // IExporter interface
        void beginExport(std::filesystem::path const& fileName, ComputeCore& generator) override;
        bool advanceExport(ComputeCore& generator) override;
        void finalize() override;
        [[nodiscard]] double getProgress() const override;

        /// @brief Check if export ended with error
        [[nodiscard]] bool hasError() const;

        /// @brief Get error message if hasError() returns true
        [[nodiscard]] std::string const& errorMessage() const;

        /// @brief Get current status message (e.g., "Generating shell 2/5...")
        [[nodiscard]] std::string getStatusMessage() const;

      private:
        enum class State
        {
            Idle,
            Running,
            Completed,
            Failed
        };

        void performExport(ComputeCore& generator);

        events::SharedLogger m_logger;
        ShellExportConfig m_config;
        Document const* m_document = nullptr;
        std::vector<Lib3MF_uint8> m_thumbnailPng;
        std::filesystem::path m_targetFile;

        std::atomic<State> m_state{State::Idle};
        std::atomic<double> m_progress{0.0};
        mutable std::mutex m_statusMutex;
        std::string m_statusMessage;
        std::string m_errorMessage;

        std::future<void> m_exportFuture;
    };

} // namespace gladius::io
