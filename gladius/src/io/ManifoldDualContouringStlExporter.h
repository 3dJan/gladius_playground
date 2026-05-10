#pragma once

#include "IExporter.h"
#include "SurfaceExtractionOptions.h"
#include "3mf/ColorCompatibilityPlanner.h"
#include "3mf/MeshWriter3mf.h"

#include "../EventLogger.h"
#include "../Mesh.h"

#include <Eigen/Core>

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace gladius
{
    class ComputeCore;
    class Document;
}

namespace gladius::io
{
    /// @brief Output format for mesh export
    enum class MeshOutputFileFormat
    {
        STL,
        ThreeMF
    };

    class ManifoldDualContouringStlExporter : public IExporter
    {
      public:
        ManifoldDualContouringStlExporter();
        explicit ManifoldDualContouringStlExporter(events::SharedLogger logger);

        void setOptions(ManifoldDualContouringOptions options);
        
        /// @brief Set the output file format (default: STL)
        void setOutputFormat(MeshOutputFileFormat format);
        
        /// @brief Set the document for 3MF thumbnail generation
        void setDocument(Document const * doc);
        
        /// @brief Enable/disable color export (samples volumetric colors at face centroids)
        /// @param exportWithColors If true, sample and include per-face colors in 3MF output
        void setExportWithColors(bool exportWithColors);
        
        /// @brief Enable/disable sRGB conversion for color export
        /// @param convertToSrgb If true, convert linear RGB to sRGB (default: true)
        void setConvertToSrgb(bool convertToSrgb);

        /// @brief Set the color export mode
        /// @param mode The color mode to use
        void setColorMode(ColorMode mode);

        /// @brief Set the quantization behavior for printable-region compatibility
        void setQuantizationMode(QuantizationMode mode);

        /// @brief Set the maximum palette size (nullopt = automatic)
        void setMaxPaletteSize(std::optional<std::uint32_t> maxPaletteSize);

        /// @brief Set the target application for optional proprietary optimization
        void setTargetApplication(TargetApplication targetApplication);

        void beginExport(std::filesystem::path const & fileName, ComputeCore & generator) override;
        bool advanceExport(ComputeCore & generator) override;
        void finalize() override;
        [[nodiscard]] double getProgress() const override;

        [[nodiscard]] bool hasError() const;
        [[nodiscard]] std::string const & errorMessage() const;

      private:
        enum class State
        {
            Idle,
            Running,
            Completed,
            Failed
        };

        void performExport(ComputeCore & generator);
        void writeMeshToFile(ComputeCore & generator,
                             std::vector<Eigen::Vector3f> const & positions,
                             std::vector<std::uint32_t> const & indices,
                             std::vector<Eigen::Vector3f> const & normals);

        events::SharedLogger m_logger;
        ManifoldDualContouringOptions m_options{};
        std::filesystem::path m_targetFile;
        ComputeCore * m_computeCore{nullptr};
        std::atomic<State> m_state{State::Idle};
        std::atomic<double> m_progress{0.0};
        std::string m_errorMessage;
        
        // Output format and document for 3MF
        MeshOutputFileFormat m_outputFormat{MeshOutputFileFormat::STL};
        Document const * m_document{nullptr};
        bool m_exportWithColors{false};  ///< Whether to sample and export volumetric colors
        bool m_convertToSrgb{true};  ///< Whether to convert linear RGB to sRGB
        ColorMode m_colorMode{ColorMode::PerFace}; ///< The color mode to use
        QuantizationMode m_quantizationMode{QuantizationMode::Adaptive};
        std::optional<std::uint32_t> m_maxPaletteSize;
        TargetApplication m_targetApplication{TargetApplication::None};
        
        // Background thread for non-blocking export
        std::future<void> m_exportFuture;
    };
}
