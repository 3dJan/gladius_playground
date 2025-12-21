#pragma once
#include "../EventLogger.h"
#include "../compute/ComputeCore.h"
#include "../io/3mf/MeshWriter3mf.h"
#include "../nodes/Assembly.h"
#include "LayerBasedMeshExporter.h"
#include "vdb.h"

#include <filesystem>

// Forward declaration
namespace gladius
{
    class Document;
}

namespace gladius::vdb
{
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
        /// @param exportWithColors If true, sample and include per-face colors in 3MF output
        void setExportWithColors(bool exportWithColors);
        
        /// @brief Enable/disable sRGB conversion for color export
        /// @param convertToSrgb If true, convert linear RGB to sRGB (default: true)
        void setConvertToSrgb(bool convertToSrgb);

        using ColorMode = gladius::io::ColorMode;

        /// @brief Set the color export mode
        /// @param mode The color mode to use
        void setColorMode(ColorMode mode);

      private:
        events::SharedLogger m_logger;
        ComputeCore * m_computeCore = nullptr;
        Document const * m_sourceDocument = nullptr;
        bool m_exportWithColors = false;  ///< Whether to sample and export volumetric colors
        bool m_convertToSrgb = true;  ///< Whether to convert linear RGB to sRGB
        ColorMode m_colorMode = ColorMode::PerFace; ///< The color mode to use
    };
}
