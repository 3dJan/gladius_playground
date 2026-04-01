#pragma once
#include "../EventLogger.h"
#include "../compute/ComputeCore.h"
#include "../io/3mf/ColorCompatibilityPlanner.h"
#include "../io/3mf/MeshWriter3mf.h"
#include "../nodes/Assembly.h"
#include "LayerBasedMeshExporter.h"
#include "vdb.h"

#include <filesystem>
#include <optional>
#include <vector>

// Forward declaration
namespace gladius
{
    class Document;
}

namespace gladius::vdb
{
    /// Final export-state record produced by the mesh color export pipeline
    struct ColoredMeshExportResult
    {
        io::ExportRepresentation representation = io::ExportRepresentation::StandardTriangleColor;
        bool standardsOnly = true;
        bool transparencyIgnored = false;
        std::vector<std::string> warnings;
    };

    class MeshExporter3mf : public gladius::io::LayerBasedMeshExporter
    {
      public:
        explicit MeshExporter3mf(events::SharedLogger logger = nullptr);

        // Override to store compute core reference
        void beginExport(std::filesystem::path const & fileName, ComputeCore & generator) override;

        // Overload to accept document for thumbnail generation
        void beginExport(std::filesystem::path const & fileName,
                         ComputeCore & generator,
                         Document const * document);

        // Override finalize to implement 3MF-specific finalization
        void finalize() override;
        
        /// @brief Enable/disable color export (samples volumetric colors at face centroids)
        void setExportWithColors(bool exportWithColors);
        
        /// @brief Enable/disable sRGB conversion for color export
        void setConvertToSrgb(bool convertToSrgb);

        using ColorMode = gladius::io::ColorMode;

        /// @brief Set the color export mode
        void setColorMode(ColorMode mode);

        /// @brief Set the quantization behavior for printable-region compatibility
        void setQuantizationMode(io::QuantizationMode mode);

        /// @brief Set the maximum palette size (nullopt = automatic)
        void setMaxPaletteSize(std::optional<std::uint32_t> maxPaletteSize);

        /// @brief Set the target application for optional proprietary optimization
        void setTargetApplication(io::TargetApplication targetApplication);

        /// @brief Get the result of the last export (available after finalize)
        [[nodiscard]] ColoredMeshExportResult const& getExportResult() const;

      private:
        events::SharedLogger m_logger;
        ComputeCore * m_computeCore = nullptr;
        Document const * m_sourceDocument = nullptr;

        // Color export settings (captured as immutable snapshot at export time)
        bool m_exportWithColors = false;
        bool m_convertToSrgb = true;
        ColorMode m_colorMode = ColorMode::PerFace;
        io::QuantizationMode m_quantizationMode = io::QuantizationMode::Adaptive;
        std::optional<std::uint32_t> m_maxPaletteSize;
        io::TargetApplication m_targetApplication = io::TargetApplication::None;

        // Export result
        ColoredMeshExportResult m_exportResult;
    };
}
