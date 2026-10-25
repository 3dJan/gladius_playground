#pragma once

#include "IExporter.h"
#include "3mf/FilamentOpticalProperties.h"
#include "3mf/FrontlitThicknessSolver.h"
#include "3mf/ShellThicknessPartition.h"
#include "SurfaceExtractionOptions.h"
#include "../EventLogger.h"

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
    /// @brief Configuration for shell-based color export
    struct ShellExportConfig
    {
        ShellGenerationMode generationMode = ShellGenerationMode::LegacyManifoldDualContouring;
        FilamentStack filamentStack;                      ///< Ordered materials (bottom to top)
        std::vector<std::vector<float>> precomputedLuts;  ///< Thickness LUTs per layer
        int lutResolution = 16;                           ///< LUT grid resolution
        ThicknessConstraints thicknessConstraints;        ///< Thickness limits
        ManifoldDualContouringOptions mdcOptions;         ///< Mesh extraction options
        bool useSurfaceColorSampling = false;             ///< Use surface colors instead of interior
    };

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
        std::filesystem::path m_targetFile;

        std::atomic<State> m_state{State::Idle};
        std::atomic<double> m_progress{0.0};
        mutable std::mutex m_statusMutex;
        std::string m_statusMessage;
        std::string m_errorMessage;

        std::future<void> m_exportFuture;
    };

} // namespace gladius::io
