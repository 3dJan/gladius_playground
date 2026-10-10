#pragma once
#include "ExportState.h"
#include "FileDialogService.h"
#include "io/MeshExportJob.h"
#include "io/MeshExportRequest.h"
#include "ColorToThicknessDialog.h"

#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <atomic>
#include <vector>

// Forward declaration
namespace gladius
{
    class Document;
}

namespace gladius::ui
{
    /// @brief Output file format for mesh export
    enum class MeshOutputFormat
    {
        STL = 0,
        ThreeMF = 1
    };

    class MeshExportDialog final
    {
      public:
        /// @brief Set the export state reference for blocking UI during export
        /// @param state Pointer to the ExportState (must remain valid)
        void setExportState(ExportState * state)
        {
            m_exportState = state;
        }

        /// @brief Set the document for 3MF thumbnail generation
        /// @param doc Pointer to the document (must remain valid during export)
        /// @brief Show the dialog with optional suggested filename
        /// @param suggestedFilename Optional initial filename to suggest
        void show(std::filesystem::path suggestedFilename = {});
        [[nodiscard]] bool isVisible() const noexcept
        {
            return m_visible;
        }

        void render(std::shared_ptr<Document> const & document);

      private:
        void pollExportJob();
        void renderConfiguration(std::shared_ptr<Document> const & document);
        void renderFileSelection();
        void renderMeshExtractionTab();
        void renderColorMaterialTab();
        void renderStatusArea();
        void derivePaletteFromMesh();
        void startExport(std::shared_ptr<Document> const & document);
        void resetState();
        void resetExportState();
        [[nodiscard]] io::ManifoldDualContouringOptions makeManifoldOptions() const;

        std::filesystem::path m_targetFile;
        bool m_visible{false};
        std::unique_ptr<io::AsyncMeshExportJob> m_exportJob;
        std::weak_ptr<Document> m_document;
        MeshOutputFormat m_outputFormat = MeshOutputFormat::ThreeMF;
        io::SurfaceExtractionMethod m_selectedMethod =
          io::SurfaceExtractionMethod::ManifoldDualContouring;
        std::size_t m_marchingCubesQuality = 1U;
        io::DualContouringQuality m_dualQualityPreset = io::DualContouringQuality::Balanced;
        bool m_dualForceUniform = false;
        io::ManifoldDualContouringQuality m_manifoldQualityPreset =
          io::ManifoldDualContouringQuality::UltraFine;
        bool m_manifoldEnableGpu = true;
        bool m_manifoldAllowCpuFallback = true;
        bool m_manifoldEnableCaching = true;
        // Minimum feature size
        float m_manifoldMinFeatureSize = 0.0F;
        // Hierarchical octree (watertight mesh generation) — always enabled
        bool m_manifoldEnableHierarchicalOctree = true;
        // Sharp feature post-processing — disabled (QEF handles sharp features)
        bool m_manifoldEnableSharpFeaturePostProcess = false;
        float m_manifoldSharpFeatureAngleThreshold = 0.5F;
        std::size_t m_manifoldSubdivisionIterations = 1U;
        bool m_manifoldProjectToSurface = true;
        // Mesh simplification (fast geometric QEM)
        bool m_manifoldEnableSimplification = true;
        float m_manifoldSimplificationTolerance = 0.05F;  ///< Tolerance in mm (world units)
        bool m_exportInProgress = false;
        std::string m_errorMessage;
        
        // Status display
        std::string m_statusMessage;
        bool m_statusIsError = false;
        bool m_exportCompleted = false;
        
        // File browse dialog
        AsyncFileDialog m_browseDialog;
        
        // Export state for blocking UI modifications during export
        ExportState * m_exportState = nullptr;
        
        // Color export options
        bool m_exportWithColors = true;  ///< Export with volumetric colors (3MF only)
        bool m_convertToSrgb = true;  ///< Convert linear RGB to sRGB for display
        io::ColorMode m_colorMode = io::ColorMode::PerFace; ///< Color export mode
        bool m_modelHasVolumetricColor = false;  ///< Cached: does model have color output?
        bool m_enableShellBasedExport = false; ///< Use shell-based export with LUTs when available
        bool m_useSurfaceColorSampling = true;  ///< Sample colors at surface instead of interior

        // Compatibility tuning options
        io::QuantizationMode m_quantizationMode = io::QuantizationMode::Adaptive;
        bool m_overridePaletteSize = false;  ///< Whether user has overridden palette size
        int m_maxPaletteSize = 16;           ///< Maximum palette colors (when overridden)
        io::TargetApplication m_targetApplication = io::TargetApplication::None;

        ColorToThicknessDialog m_colorToThicknessDialog;

        // Async palette derivation
        struct PaletteDeriveResult
        {
          std::vector<Eigen::Vector3f> palette;
          std::string error;
          bool success{false};
        };
        std::future<PaletteDeriveResult> m_paletteFuture;
        std::atomic<bool> m_paletteDeriveInProgress{false};
        bool m_paletteHandlerBound{false};

    };
} // namespace gladius::ui
